#include "vibe_cardputer_capture.h"

#include <stdatomic.h>
#include <string.h>

#include "vibe_cardputer_asr_board.h"
#include "vibe_ima_adpcm.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CAPTURE_FRAME_MS 60
#define CAPTURE_QUEUE_DEPTH 96
#define CAPTURE_STOP_TIMEOUT_MS 1200
#define CARDPUTER_I2S_PORT I2S_NUM_1
#define CARDPUTER_I2S_BCLK GPIO_NUM_41
#define CARDPUTER_I2S_LRCK GPIO_NUM_43
#define CARDPUTER_I2S_DOUT GPIO_NUM_42
#define CARDPUTER_I2S_DIN GPIO_NUM_46

typedef struct {
    size_t len;
    uint8_t data[VIBE_CARDPUTER_CAPTURE_ENCODED_FRAME_BYTES];
} capture_chunk_t;

static const char *TAG = "card_capture";
static const struct {
    uint8_t reg;
    uint8_t value;
} s_es8311_capture_registers[] = {
    {0x00, 0x80},
    {0x01, 0xBA},
    {0x02, 0x18},
    {0x0D, 0x01},
    {0x0E, 0x02},
    {0x14, 0x10},
    {0x17, 0xBF},
    {0x1C, 0x6A},
};

static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static i2s_chan_handle_t s_rx;
static bool s_rx_enabled;
static atomic_bool s_running;
static vibe_cardputer_capture_stats_t s_stats;
static esp_codec_dev_handle_t s_codec;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;

static void release_codec(void)
{
    if (s_codec) {
        esp_codec_dev_close(s_codec);
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
    }
    if (s_codec_if) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_data_if) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_gpio_if) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_ctrl_if) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
}

static void release_i2s(void)
{
    if (!s_rx) {
        return;
    }
    if (s_rx_enabled) {
        esp_err_t err = i2s_channel_disable(s_rx);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "disable microphone i2s: %s", esp_err_to_name(err));
        }
    }
    i2s_del_channel(s_rx);
    s_rx = NULL;
    s_rx_enabled = false;
}

static void release_capture(void)
{
    release_codec();
    release_i2s();
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        CARDPUTER_I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, NULL, &s_rx), TAG,
                        "create microphone i2s");

    i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VIBE_CARDPUTER_CAPTURE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = CARDPUTER_I2S_BCLK,
            .ws = CARDPUTER_I2S_LRCK,
            .dout = CARDPUTER_I2S_DOUT,
            .din = CARDPUTER_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &i2s_config), TAG,
                        "init microphone i2s");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable microphone i2s");
    s_rx_enabled = true;
    return ESP_OK;
}

static esp_err_t init_codec(void)
{
    i2c_master_bus_handle_t i2c_bus = vibe_cardputer_asr_i2c_bus();
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "i2c unavailable");

    audio_codec_i2c_cfg_t i2c_config = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    ESP_RETURN_ON_FALSE(s_ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "codec i2c");

    audio_codec_i2s_cfg_t i2s_config = {
        .port = CARDPUTER_I2S_PORT,
        .rx_handle = s_rx,
        .tx_handle = NULL,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_config);
    ESP_RETURN_ON_FALSE(s_data_if != NULL, ESP_ERR_NO_MEM, TAG, "codec i2s");
    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "codec gpio");

    es8311_codec_cfg_t es8311_config = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = false,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    s_codec_if = es8311_codec_new(&es8311_config);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_ERR_NO_MEM, TAG, "es8311");

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_codec = esp_codec_dev_new(&device_config);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_NO_MEM, TAG, "codec device");

    esp_codec_dev_sample_info_t sample_config = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 1,
        .channel_mask = I2S_STD_SLOT_LEFT,
        .sample_rate = VIBE_CARDPUTER_CAPTURE_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_RETURN_ON_FALSE(
        esp_codec_dev_open(s_codec, &sample_config) == ESP_CODEC_DEV_OK,
        ESP_FAIL, TAG, "open microphone codec");
    for (size_t index = 0;
         index < sizeof(s_es8311_capture_registers) /
                     sizeof(s_es8311_capture_registers[0]);
         ++index) {
        ESP_RETURN_ON_FALSE(
            esp_codec_dev_write_reg(s_codec, s_es8311_capture_registers[index].reg,
                                    s_es8311_capture_registers[index].value) ==
                ESP_CODEC_DEV_OK,
            ESP_FAIL, TAG, "configure microphone codec");
    }
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec, 36.0f) ==
                            ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set microphone gain");
    return ESP_OK;
}

static void capture_task(void *arg)
{
    (void)arg;
    capture_chunk_t chunk = {0};
    int16_t pcm[VIBE_CARDPUTER_CAPTURE_FRAME_SAMPLES] = {0};
    vibe_ima_adpcm_state_t encoder = {0};
    vibe_ima_adpcm_reset(&encoder);
    while (atomic_load(&s_running)) {
        if (esp_codec_dev_read(s_codec, pcm, sizeof(pcm)) !=
            ESP_CODEC_DEV_OK) {
            if (atomic_load(&s_running)) {
                ESP_LOGW(TAG, "microphone read failed");
            }
            continue;
        }
        if (!vibe_ima_adpcm_encode_block(
                &encoder, pcm, VIBE_CARDPUTER_CAPTURE_FRAME_SAMPLES,
                chunk.data, sizeof(chunk.data), &chunk.len)) {
            ESP_LOGW(TAG, "ADPCM encode failed");
            continue;
        }
        s_stats.chunks_read++;
        s_stats.bytes_read += sizeof(pcm);
        if (xQueueSend(s_queue, &chunk, 0) != pdTRUE) {
            s_stats.chunks_dropped++;
            s_stats.bytes_dropped += sizeof(pcm);
        } else {
            s_stats.chunks_queued++;
        }
    }

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        release_capture();
        s_task = NULL;
        xSemaphoreGive(s_mutex);
    } else {
        release_capture();
        s_task = NULL;
    }
    vTaskDelete(NULL);
}

esp_err_t vibe_cardputer_capture_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                            "capture mutex");
    }
    if (!s_queue) {
        s_queue = xQueueCreate(CAPTURE_QUEUE_DEPTH, sizeof(capture_chunk_t));
        ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG,
                            "capture queue");
    }
    return ESP_OK;
}

esp_err_t vibe_cardputer_capture_start(void)
{
    ESP_RETURN_ON_FALSE(s_mutex && s_queue, ESP_ERR_INVALID_STATE, TAG,
                        "capture not initialized");
    if (atomic_load(&s_running)) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_mutex, pdMS_TO_TICKS(250)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "capture busy");
    if (atomic_load(&s_running) || s_task || s_rx) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    vibe_cardputer_capture_clear();
    memset(&s_stats, 0, sizeof(s_stats));
    esp_err_t err = init_i2s();
    if (err == ESP_OK) {
        err = init_codec();
    }
    if (err != ESP_OK) {
        release_capture();
        xSemaphoreGive(s_mutex);
        return err;
    }
    atomic_store(&s_running, true);
    BaseType_t started = xTaskCreatePinnedToCore(
        capture_task, "mic_capture", 8192, NULL, 5, &s_task, 1);
    if (started != pdPASS) {
        atomic_store(&s_running, false);
        release_capture();
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG,
             "microphone started 16kHz ima-adpcm queue=%u buffer_ms=%u",
             CAPTURE_QUEUE_DEPTH, CAPTURE_QUEUE_DEPTH * CAPTURE_FRAME_MS);
    return ESP_OK;
}

esp_err_t vibe_cardputer_capture_stop(void)
{
    if (!atomic_load(&s_running) && !s_task) {
        return ESP_OK;
    }
    atomic_store(&s_running, false);
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        if (s_rx && s_rx_enabled) {
            esp_err_t err = i2s_channel_disable(s_rx);
            if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
                s_rx_enabled = false;
            }
        }
        xSemaphoreGive(s_mutex);
    }

    TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(CAPTURE_STOP_TIMEOUT_MS);
    while (s_task) {
        if (xTaskGetTickCount() >= deadline) {
            ESP_LOGE(TAG, "microphone stop timed out");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

bool vibe_cardputer_capture_is_recording(void)
{
    return atomic_load(&s_running) || s_task != NULL;
}

esp_err_t vibe_cardputer_capture_read_batch(uint8_t *buffer, size_t capacity,
                                            size_t *len, size_t max_chunks,
                                            uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(buffer && len &&
                            capacity >=
                                VIBE_CARDPUTER_CAPTURE_ENCODED_FRAME_BYTES,
                        ESP_ERR_INVALID_ARG, TAG, "invalid read buffer");
    if (max_chunks == 0) {
        max_chunks = 1;
    }
    *len = 0;
    capture_chunk_t chunk = {0};
    if (xQueueReceive(s_queue, &chunk, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(buffer, chunk.data, chunk.len);
    *len = chunk.len;

    for (size_t count = 1; count < max_chunks; ++count) {
        if (capacity - *len <
            VIBE_CARDPUTER_CAPTURE_ENCODED_FRAME_BYTES) {
            break;
        }
        TickType_t wait = atomic_load(&s_running)
                              ? pdMS_TO_TICKS(CAPTURE_FRAME_MS + 10)
                              : 0;
        if (xQueueReceive(s_queue, &chunk, wait) != pdTRUE) {
            break;
        }
        memcpy(buffer + *len, chunk.data, chunk.len);
        *len += chunk.len;
    }
    return ESP_OK;
}

size_t vibe_cardputer_capture_pending_chunks(void)
{
    return s_queue ? uxQueueMessagesWaiting(s_queue) : 0;
}

void vibe_cardputer_capture_stats(vibe_cardputer_capture_stats_t *stats)
{
    if (stats) {
        *stats = s_stats;
    }
}

void vibe_cardputer_capture_clear(void)
{
    if (!s_queue) {
        return;
    }
    capture_chunk_t chunk = {0};
    while (xQueueReceive(s_queue, &chunk, 0) == pdTRUE) {
    }
}
