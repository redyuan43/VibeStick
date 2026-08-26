#include "vibe_audio.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "vibe_board.h"
#include "vibe_board_profile.h"
#include "vibe_capture_profile.h"
#include "vibe_ima_adpcm.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if VIBE_BOARD_HAS_PDM_MIC
#include "driver/i2s_pdm.h"
#endif

#if VIBE_BOARD_HAS_ES8311
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#endif

#define AUDIO_FRAME_MS 60
#define AUDIO_FRAME_SAMPLES ((VIBE_STICK_AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS) / 1000)
#define AUDIO_CHUNK_BYTES (AUDIO_FRAME_SAMPLES * VIBE_STICK_AUDIO_CHANNELS * \
                           (VIBE_STICK_AUDIO_BITS_PER_SAMPLE / 8))
#define AUDIO_CAPTURE_MAX_OVERSAMPLING 2
#define AUDIO_CAPTURE_MAX_SAMPLES (AUDIO_FRAME_SAMPLES * AUDIO_CAPTURE_MAX_OVERSAMPLING)
#if defined(VIBE_BOARD_CARDPUTER_ADV)
#define AUDIO_PCM_QUEUE_DEPTH 24
#else
#define AUDIO_PCM_QUEUE_DEPTH 12
#endif
#define AUDIO_ADPCM_QUEUE_DEPTH 96
#define AUDIO_READ_WAIT_MS (AUDIO_FRAME_MS + 50)
#define TASK_EXIT_WAIT_MS 1200
#define VIBE_STICK_SOUND_FRAME_SAMPLES 160
#define VIBE_STICK_SOUND_FADE_MS 8
#define VIBE_STICK_TWO_PI 6.28318530717958647692f
#define VIBE_STICK_TONE_DUTY ((1 << 9) - 1)
#define VIBE_STICK_AUDIO_CORE 1

#if VIBE_BOARD_HAS_ES8311
/* The StickS3's amplified speaker needs a softer profile than the Plus buzzer. */
#define VIBE_STICK_SOUND_VOLUME 0.28f
#define VIBE_STICK_SOUND_OUTPUT_VOLUME 70
#define VIBE_STICK_BEEP_MS 90
#define VIBE_STICK_RECORDING_CHIRP_MS 65
#define VIBE_STICK_RECORDING_CHIRP_GAP_MS 16
#define VIBE_STICK_FOLLOWUP_BUZZ_MS 42
#define VIBE_STICK_FOLLOWUP_BUZZ_GAP_MS 20
#define VIBE_STICK_ESCAPE_GLITCH_SHORT_MS 40
#define VIBE_STICK_ESCAPE_GLITCH_LONG_MS 80
#define VIBE_STICK_ESCAPE_GLITCH_GAP_MS 24
#define VIBE_STICK_DONE_LOW_HZ 700
#define VIBE_STICK_DONE_HIGH_HZ 1000
#define VIBE_STICK_APPROVAL_LOW_HZ 520
#define VIBE_STICK_APPROVAL_HIGH_HZ 700
#define VIBE_STICK_RECORDING_START_HIGH_HZ 1800
#define VIBE_STICK_RECORDING_START_LOW_HZ 1200
#define VIBE_STICK_RECORDING_STOP_HZ 1500
#define VIBE_STICK_FOLLOWUP_ENTER_LOW_HZ 1400
#define VIBE_STICK_FOLLOWUP_ENTER_HIGH_HZ 1700
#define VIBE_STICK_FOLLOWUP_ESCAPE_HIGH_HZ 1700
#define VIBE_STICK_FOLLOWUP_ESCAPE_MID_HZ 1450
#else
#define VIBE_STICK_SOUND_VOLUME 0.40f
#define VIBE_STICK_SOUND_OUTPUT_VOLUME 85
#define VIBE_STICK_BEEP_MS 200
#define VIBE_STICK_RECORDING_CHIRP_MS 90
#define VIBE_STICK_RECORDING_CHIRP_GAP_MS 18
#define VIBE_STICK_FOLLOWUP_BUZZ_MS 55
#define VIBE_STICK_FOLLOWUP_BUZZ_GAP_MS 22
#define VIBE_STICK_ESCAPE_GLITCH_SHORT_MS 45
#define VIBE_STICK_ESCAPE_GLITCH_LONG_MS 105
#define VIBE_STICK_ESCAPE_GLITCH_GAP_MS 28
#define VIBE_STICK_DONE_LOW_HZ 880
#define VIBE_STICK_DONE_HIGH_HZ 1320
#define VIBE_STICK_APPROVAL_LOW_HZ 600
#define VIBE_STICK_APPROVAL_HIGH_HZ 800
#define VIBE_STICK_RECORDING_START_HIGH_HZ 3600
#define VIBE_STICK_RECORDING_START_LOW_HZ 1800
#define VIBE_STICK_RECORDING_STOP_HZ 4000
#define VIBE_STICK_FOLLOWUP_ENTER_LOW_HZ 2600
#define VIBE_STICK_FOLLOWUP_ENTER_HIGH_HZ 3200
#define VIBE_STICK_FOLLOWUP_ESCAPE_HIGH_HZ 3600
#define VIBE_STICK_FOLLOWUP_ESCAPE_MID_HZ 3200
#endif

typedef struct {
    size_t len;
    uint8_t data[AUDIO_CHUNK_BYTES];
} audio_chunk_t;

static const char *TAG = "vibe_audio";

static atomic_bool s_running;
static atomic_uchar s_output_volume = VIBE_STICK_SOUND_OUTPUT_VOLUME;
static bool s_initialized;
static SemaphoreHandle_t s_audio_mutex;
static QueueHandle_t s_audio_queue;
static vibe_audio_transport_t s_audio_transport = VIBE_AUDIO_TRANSPORT_PCM16;
static size_t s_audio_queue_item_size;
static size_t s_audio_queue_depth;
static TaskHandle_t s_audio_task;
static i2s_chan_handle_t s_tx_handle;
static i2s_chan_handle_t s_rx_handle;
static bool s_tx_enabled;
static bool s_rx_enabled;
static const vibe_capture_profile_t *s_capture_profile;
static vibe_capture_processor_t s_capture_processor;
static int16_t s_capture_buffer[AUDIO_CAPTURE_MAX_SAMPLES];
#if !VIBE_BOARD_HAS_GPIO_TONE_SPEAKER
static bool s_streaming;
#endif
static vibe_audio_stats_t s_audio_stats;

static size_t audio_wire_frame_bytes(vibe_audio_transport_t transport)
{
    return transport == VIBE_AUDIO_TRANSPORT_IMA_ADPCM
               ? VIBE_STICK_AUDIO_ADPCM_FRAME_BYTES
               : AUDIO_CHUNK_BYTES;
}

static size_t audio_queue_depth(vibe_audio_transport_t transport)
{
    return transport == VIBE_AUDIO_TRANSPORT_IMA_ADPCM
               ? AUDIO_ADPCM_QUEUE_DEPTH
               : AUDIO_PCM_QUEUE_DEPTH;
}

static esp_err_t create_audio_queue(vibe_audio_transport_t transport)
{
    const size_t frame_bytes = audio_wire_frame_bytes(transport);
    const size_t item_size = offsetof(audio_chunk_t, data) + frame_bytes;
    const size_t depth = audio_queue_depth(transport);
    QueueHandle_t queue = xQueueCreate(depth, item_size);
    ESP_RETURN_ON_FALSE(queue != NULL, ESP_ERR_NO_MEM, TAG, "audio queue");
    s_audio_queue = queue;
    s_audio_queue_item_size = item_size;
    s_audio_queue_depth = depth;
    s_audio_transport = transport;
    return ESP_OK;
}

#if VIBE_BOARD_HAS_ES8311
static esp_codec_dev_handle_t s_codec;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static int s_applied_output_volume = -1;
#endif

#if VIBE_BOARD_HAS_ES8311
static esp_err_t apply_output_volume(void)
{
    ESP_RETURN_ON_FALSE(s_codec, ESP_ERR_INVALID_STATE, TAG, "codec missing");
    uint8_t volume = atomic_load(&s_output_volume);
    if (s_applied_output_volume == volume) return ESP_OK;
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(s_codec, volume) ==
                            ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "speaker volume");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_mute(s_codec, volume == 0) ==
                            ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "speaker mute");
    s_applied_output_volume = volume;
    return ESP_OK;
}
#endif

#if VIBE_BOARD_HAS_ES8311
static esp_err_t apply_capture_es8311_profile(const vibe_capture_profile_t *profile)
{
    ESP_RETURN_ON_FALSE(profile != NULL, ESP_ERR_INVALID_ARG, TAG, "capture profile");
    ESP_RETURN_ON_FALSE(profile->es8311_registers != NULL &&
                            profile->es8311_register_count > 0,
                        ESP_ERR_INVALID_STATE, TAG, "es8311 capture profile");
    for (size_t i = 0; i < profile->es8311_register_count; ++i) {
        ESP_RETURN_ON_FALSE(
            esp_codec_dev_write_reg(s_codec, profile->es8311_registers[i].reg,
                                    profile->es8311_registers[i].value) ==
                ESP_CODEC_DEV_OK,
            ESP_FAIL, TAG, "capture codec register 0x%02x",
            profile->es8311_registers[i].reg);
    }
    return ESP_OK;
}

#if defined(VIBE_BOARD_CARDPUTER_ADV)
static esp_err_t apply_cardputer_speaker_profile(void)
{
    static const vibe_capture_es8311_register_t speaker_profile[] = {
        {0x00, 0x80},
        {0x01, 0xB5},
        {0x02, 0x18},
        {0x0D, 0x01},
        {0x12, 0x00},
        {0x13, 0x10},
        {0x32, 0xBF},
        {0x37, 0x08},
    };
    for (size_t i = 0; i < sizeof(speaker_profile) / sizeof(speaker_profile[0]); ++i) {
        ESP_RETURN_ON_FALSE(
            esp_codec_dev_write_reg(s_codec, speaker_profile[i].reg,
                                    speaker_profile[i].value) ==
                ESP_CODEC_DEV_OK,
            ESP_FAIL, TAG, "cardputer codec register 0x%02x",
            speaker_profile[i].reg);
    }
    return ESP_OK;
}
#endif

static esp_err_t init_i2s_std(bool enable_tx, bool enable_rx, uint32_t sample_rate,
                              bool input_only_right)
{
    const vibe_capture_profile_t *profile = vibe_capture_profile_current();
    ESP_RETURN_ON_FALSE(profile != NULL, ESP_ERR_INVALID_STATE, TAG, "capture profile");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(VIBE_BOARD_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg,
                                        enable_tx ? &s_tx_handle : NULL,
                                        enable_rx ? &s_rx_handle : NULL),
                        TAG, "create i2s");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)profile->mclk_pin,
            .bclk = (gpio_num_t)profile->bclk_pin,
            .ws = (gpio_num_t)profile->lrck_pin,
            .dout = (gpio_num_t)profile->data_out_pin,
            .din = (gpio_num_t)profile->data_in_pin,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (enable_rx && input_only_right) {
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
    }

    if (s_tx_handle) {
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG, "init i2s tx");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable i2s tx");
        s_tx_enabled = true;
    }
    if (s_rx_handle) {
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg), TAG, "init i2s rx");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "enable i2s rx");
        s_rx_enabled = true;
    }
    return ESP_OK;
}

static esp_err_t init_codec(esp_codec_dev_type_t dev_type, esp_codec_dec_work_mode_t work_mode,
                            uint32_t sample_rate, bool input_only_right)
{
    i2c_master_bus_handle_t i2c_bus = vibe_board_i2c_bus();
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "i2c unavailable");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = VIBE_BOARD_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "codec i2c");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = VIBE_BOARD_I2S_PORT,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_data_if != NULL, ESP_ERR_NO_MEM, TAG, "codec i2s");

    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "codec gpio");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = work_mode,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = VIBE_BOARD_ES8311_USE_MCLK,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    s_codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_ERR_NO_MEM, TAG, "es8311");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = dev_type,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_NO_MEM, TAG, "codec dev");

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = VIBE_STICK_AUDIO_CHANNELS,
        .channel_mask = input_only_right ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT,
        .sample_rate = sample_rate,
        .mclk_multiple = 0,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open codec");
    if (dev_type & ESP_CODEC_DEV_TYPE_IN) {
        ESP_RETURN_ON_ERROR(apply_capture_es8311_profile(s_capture_profile), TAG,
                            "capture codec profile");
        if (s_capture_profile->input_gain_db != 0) {
            ESP_RETURN_ON_FALSE(
                esp_codec_dev_set_in_gain(
                    s_codec, (float)s_capture_profile->input_gain_db) ==
                    ESP_CODEC_DEV_OK,
                ESP_FAIL, TAG, "capture input gain");
        }
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    } else if (dev_type & ESP_CODEC_DEV_TYPE_OUT) {
        ESP_RETURN_ON_ERROR(apply_cardputer_speaker_profile(), TAG,
                            "cardputer speaker profile");
#endif
    }
    if (dev_type & ESP_CODEC_DEV_TYPE_OUT) {
        s_applied_output_volume = -1;
        ESP_RETURN_ON_ERROR(apply_output_volume(), TAG, "apply speaker volume");
    }
    return ESP_OK;
}
#endif

#if VIBE_BOARD_HAS_PDM_MIC
static esp_err_t init_i2s_pdm_rx(const vibe_capture_profile_t *profile)
{
    ESP_RETURN_ON_FALSE(profile != NULL, ESP_ERR_INVALID_ARG, TAG, "capture profile");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(VIBE_BOARD_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle), TAG, "create pdm rx");

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(vibe_capture_input_sample_rate(profile)),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = (gpio_num_t)profile->pdm_clk_pin,
            .din = (gpio_num_t)profile->pdm_data_pin,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    if (profile->input_only_right) {
        pdm_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_RIGHT;
    }
    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_rx_handle, &pdm_cfg), TAG, "init pdm rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "enable pdm rx");
    s_rx_enabled = true;
    return ESP_OK;
}
#endif

#if VIBE_BOARD_HAS_ES8311
static void deinit_codec(void)
{
    if (s_codec) {
        esp_codec_dev_close(s_codec);
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
        s_applied_output_volume = -1;
        s_tx_enabled = false;
        s_rx_enabled = false;
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
#endif

static void deinit_i2s(void)
{
    if (s_tx_handle) {
        if (s_tx_enabled) {
            esp_err_t err = i2s_channel_disable(s_tx_handle);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "disable i2s tx failed: %s", esp_err_to_name(err));
            }
        }
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
        s_tx_enabled = false;
    }
    if (s_rx_handle) {
        if (s_rx_enabled) {
            esp_err_t err = i2s_channel_disable(s_rx_handle);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "disable i2s rx failed: %s", esp_err_to_name(err));
            }
        }
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
        s_rx_enabled = false;
    }
}

static void release_session_resources(void)
{
#if VIBE_BOARD_HAS_ES8311
    deinit_codec();
#endif
    deinit_i2s();
}

static void signal_capture_stop_locked(void)
{
    if (s_rx_handle && s_rx_enabled) {
        esp_err_t err = i2s_channel_disable(s_rx_handle);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            s_rx_enabled = false;
        } else {
            ESP_LOGW(TAG, "stop i2s rx failed: %s", esp_err_to_name(err));
        }
    }
}

typedef struct {
    int freq_hz;
    int duration_ms;
} sound_segment_t;

#if VIBE_BOARD_HAS_ES8311
static float sound_envelope(int sample_index, int total_samples)
{
    const int fade_samples = (VIBE_STICK_AUDIO_SAMPLE_RATE * VIBE_STICK_SOUND_FADE_MS) / 1000;
    if (fade_samples <= 0) {
        return 1.0f;
    }
    if (sample_index < fade_samples) {
        return (float)sample_index / (float)fade_samples;
    }
    int remaining = total_samples - sample_index - 1;
    if (remaining < fade_samples) {
        return (float)remaining / (float)fade_samples;
    }
    return 1.0f;
}

static esp_err_t write_sound_segment(const sound_segment_t *segment)
{
    const int total_samples = (VIBE_STICK_AUDIO_SAMPLE_RATE * segment->duration_ms) / 1000;
    int samples_written = 0;
    int16_t frame[VIBE_STICK_SOUND_FRAME_SAMPLES];

    while (samples_written < total_samples) {
        int frame_samples = total_samples - samples_written;
        if (frame_samples > VIBE_STICK_SOUND_FRAME_SAMPLES) {
            frame_samples = VIBE_STICK_SOUND_FRAME_SAMPLES;
        }

        for (int i = 0; i < frame_samples; ++i) {
            int sample_index = samples_written + i;
            if (segment->freq_hz <= 0) {
                frame[i] = 0;
                continue;
            }
            float phase = VIBE_STICK_TWO_PI * (float)segment->freq_hz *
                          (float)sample_index / (float)VIBE_STICK_AUDIO_SAMPLE_RATE;
            float value = sinf(phase) * sound_envelope(sample_index, total_samples) *
                          VIBE_STICK_SOUND_VOLUME * 32767.0f;
            frame[i] = (int16_t)value;
        }

        int bytes = frame_samples * (int)sizeof(frame[0]);
        ESP_RETURN_ON_FALSE(esp_codec_dev_write(s_codec, frame, bytes) == ESP_CODEC_DEV_OK,
                            ESP_FAIL, TAG, "speaker write");
        samples_written += frame_samples;
    }
    return ESP_OK;
}

static esp_err_t play_sound_segments(const sound_segment_t *segments, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        ESP_RETURN_ON_ERROR(write_sound_segment(&segments[i]), TAG, "sound segment");
    }
    sound_segment_t tail = {.freq_hz = 0, .duration_ms = 20};
    return write_sound_segment(&tail);
}
#endif

#if VIBE_BOARD_HAS_GPIO_TONE_SPEAKER
#define VIBE_STICK_TONE_SPEED_MODE LEDC_HIGH_SPEED_MODE
#define VIBE_STICK_TONE_CHANNEL LEDC_CHANNEL_0
#define VIBE_STICK_TONE_TIMER LEDC_TIMER_0
#define VIBE_STICK_TONE_RESOLUTION LEDC_TIMER_10_BIT

static esp_err_t init_tone_output(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = VIBE_STICK_TONE_SPEED_MODE,
        .duty_resolution = VIBE_STICK_TONE_RESOLUTION,
        .timer_num = VIBE_STICK_TONE_TIMER,
        .freq_hz = 4000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "tone timer");

    ledc_channel_config_t channel = {
        .gpio_num = VIBE_BOARD_PIN_SPEAKER,
        .speed_mode = VIBE_STICK_TONE_SPEED_MODE,
        .channel = VIBE_STICK_TONE_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = VIBE_STICK_TONE_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "tone channel");
    ESP_RETURN_ON_ERROR(gpio_set_drive_capability(VIBE_BOARD_PIN_SPEAKER, GPIO_DRIVE_CAP_3),
                        TAG, "tone gpio drive");
    return gpio_set_level(VIBE_BOARD_PIN_SPEAKER, 0);
}

static esp_err_t mute_tone_output(void)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(VIBE_STICK_TONE_SPEED_MODE, VIBE_STICK_TONE_CHANNEL, 0),
                        TAG, "tone duty off");
    ESP_RETURN_ON_ERROR(ledc_update_duty(VIBE_STICK_TONE_SPEED_MODE, VIBE_STICK_TONE_CHANNEL),
                        TAG, "tone update off");
    return gpio_set_level(VIBE_BOARD_PIN_SPEAKER, 0);
}

static esp_err_t play_ledc_tone(uint16_t freq_hz, uint16_t duration_ms)
{
    if (freq_hz == 0 || duration_ms == 0) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        return ESP_OK;
    }

    ledc_timer_config_t timer = {
        .speed_mode = VIBE_STICK_TONE_SPEED_MODE,
        .duty_resolution = VIBE_STICK_TONE_RESOLUTION,
        .timer_num = VIBE_STICK_TONE_TIMER,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "tone timer set");
    ESP_RETURN_ON_ERROR(ledc_set_duty(VIBE_STICK_TONE_SPEED_MODE,
                                      VIBE_STICK_TONE_CHANNEL,
                                      VIBE_STICK_TONE_DUTY),
                        TAG, "tone duty on");
    ESP_RETURN_ON_ERROR(ledc_update_duty(VIBE_STICK_TONE_SPEED_MODE, VIBE_STICK_TONE_CHANNEL),
                        TAG, "tone update on");
    ESP_LOGD(TAG, "ledc tone freq=%u duty=%u duration=%u",
             (unsigned)freq_hz, (unsigned)VIBE_STICK_TONE_DUTY, (unsigned)duration_ms);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    return mute_tone_output();
}

static esp_err_t play_tone_segments(const sound_segment_t *segments, size_t count)
{
    ESP_RETURN_ON_ERROR(init_tone_output(), TAG, "tone output");
    for (size_t i = 0; i < count; ++i) {
        ESP_RETURN_ON_ERROR(play_ledc_tone(segments[i].freq_hz, segments[i].duration_ms),
                            TAG, "tone segment");
    }
    ESP_RETURN_ON_ERROR(mute_tone_output(), TAG, "tone mute");
    return vibe_board_speaker_set_enabled(false);
}
#endif

static const sound_segment_t *sound_segments_for(agent_sound_t sound, size_t *count)
{
    static const sound_segment_t done[] = {
        {.freq_hz = VIBE_STICK_DONE_LOW_HZ, .duration_ms = VIBE_STICK_BEEP_MS},
        {.freq_hz = 0, .duration_ms = 40},
        {.freq_hz = VIBE_STICK_DONE_HIGH_HZ, .duration_ms = VIBE_STICK_BEEP_MS},
    };
    static const sound_segment_t error[] = {
        {.freq_hz = 240, .duration_ms = VIBE_STICK_BEEP_MS},
        {.freq_hz = 0, .duration_ms = 60},
        {.freq_hz = 240, .duration_ms = VIBE_STICK_BEEP_MS},
        {.freq_hz = 0, .duration_ms = 60},
        {.freq_hz = 240, .duration_ms = VIBE_STICK_BEEP_MS},
    };
    static const sound_segment_t approval[] = {
        {.freq_hz = VIBE_STICK_APPROVAL_LOW_HZ, .duration_ms = VIBE_STICK_BEEP_MS},
        {.freq_hz = 0, .duration_ms = 60},
        {.freq_hz = VIBE_STICK_APPROVAL_HIGH_HZ, .duration_ms = VIBE_STICK_BEEP_MS},
    };
    static const sound_segment_t recording_start[] = {
        {.freq_hz = VIBE_STICK_RECORDING_START_HIGH_HZ,
         .duration_ms = VIBE_STICK_RECORDING_CHIRP_MS},
        {.freq_hz = 0, .duration_ms = VIBE_STICK_RECORDING_CHIRP_GAP_MS},
        {.freq_hz = VIBE_STICK_RECORDING_START_LOW_HZ,
         .duration_ms = VIBE_STICK_RECORDING_CHIRP_MS},
    };
    static const sound_segment_t recording_stop[] = {
        {.freq_hz = VIBE_STICK_RECORDING_STOP_HZ, .duration_ms = VIBE_STICK_BEEP_MS},
    };
    static const sound_segment_t followup_enter[] = {
        {.freq_hz = VIBE_STICK_FOLLOWUP_ENTER_LOW_HZ,
         .duration_ms = VIBE_STICK_FOLLOWUP_BUZZ_MS},
        {.freq_hz = 0, .duration_ms = VIBE_STICK_FOLLOWUP_BUZZ_GAP_MS},
        {.freq_hz = VIBE_STICK_FOLLOWUP_ENTER_HIGH_HZ,
         .duration_ms = VIBE_STICK_FOLLOWUP_BUZZ_MS},
    };
    static const sound_segment_t followup_escape[] = {
        {.freq_hz = VIBE_STICK_FOLLOWUP_ESCAPE_HIGH_HZ,
         .duration_ms = VIBE_STICK_ESCAPE_GLITCH_SHORT_MS},
        {.freq_hz = 0, .duration_ms = VIBE_STICK_ESCAPE_GLITCH_GAP_MS},
        {.freq_hz = 760, .duration_ms = VIBE_STICK_ESCAPE_GLITCH_LONG_MS},
        {.freq_hz = 0, .duration_ms = VIBE_STICK_ESCAPE_GLITCH_GAP_MS},
        {.freq_hz = VIBE_STICK_FOLLOWUP_ESCAPE_MID_HZ,
         .duration_ms = VIBE_STICK_ESCAPE_GLITCH_SHORT_MS},
        {.freq_hz = 0, .duration_ms = VIBE_STICK_ESCAPE_GLITCH_GAP_MS},
        {.freq_hz = 520, .duration_ms = VIBE_STICK_ESCAPE_GLITCH_LONG_MS},
    };

    switch (sound) {
    case VIBE_STICK_SOUND_DONE:
        *count = sizeof(done) / sizeof(done[0]);
        return done;
    case VIBE_STICK_SOUND_ERROR:
        *count = sizeof(error) / sizeof(error[0]);
        return error;
    case VIBE_STICK_SOUND_APPROVAL:
        *count = sizeof(approval) / sizeof(approval[0]);
        return approval;
    case VIBE_STICK_SOUND_RECORDING_START:
        *count = sizeof(recording_start) / sizeof(recording_start[0]);
        return recording_start;
    case VIBE_STICK_SOUND_RECORDING_STOP:
        *count = sizeof(recording_stop) / sizeof(recording_stop[0]);
        return recording_stop;
    case VIBE_STICK_SOUND_FOLLOWUP_ENTER:
        *count = sizeof(followup_enter) / sizeof(followup_enter[0]);
        return followup_enter;
    case VIBE_STICK_SOUND_FOLLOWUP_ESCAPE:
        *count = sizeof(followup_escape) / sizeof(followup_escape[0]);
        return followup_escape;
    default:
        *count = 0;
        return NULL;
    }
}

static esp_err_t read_audio_chunk(audio_chunk_t *chunk)
{
    ESP_RETURN_ON_FALSE(chunk != NULL && s_capture_profile != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "capture unavailable");
    const size_t capture_samples =
        AUDIO_FRAME_SAMPLES * s_capture_profile->oversampling;
    ESP_RETURN_ON_FALSE(capture_samples <= AUDIO_CAPTURE_MAX_SAMPLES,
                        ESP_ERR_INVALID_SIZE, TAG, "capture profile too large");
    const size_t capture_bytes = capture_samples * sizeof(s_capture_buffer[0]);
    size_t bytes_read = 0;

#if VIBE_BOARD_HAS_ES8311
    if (esp_codec_dev_read(s_codec, s_capture_buffer, (int)capture_bytes) !=
        ESP_CODEC_DEV_OK) {
        if (!atomic_load(&s_running)) {
            chunk->len = 0;
            return ESP_OK;
        }
        return ESP_FAIL;
    }
    bytes_read = capture_bytes;
#else
    esp_err_t err = i2s_channel_read(s_rx_handle, s_capture_buffer, capture_bytes,
                                     &bytes_read, pdMS_TO_TICKS(AUDIO_READ_WAIT_MS));
    if (err == ESP_ERR_TIMEOUT) {
        chunk->len = 0;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "audio read");
#endif

    if (!s_capture_profile->process_samples) {
        ESP_RETURN_ON_FALSE(bytes_read <= sizeof(chunk->data), ESP_ERR_INVALID_SIZE, TAG,
                            "direct capture too large");
        memcpy(chunk->data, s_capture_buffer, bytes_read);
        chunk->len = bytes_read;
        return ESP_OK;
    }

    const size_t output_samples = vibe_capture_process_pcm16_mono(
        &s_capture_processor, s_capture_profile, s_capture_buffer,
        bytes_read / sizeof(s_capture_buffer[0]), (int16_t *)chunk->data,
        AUDIO_FRAME_SAMPLES);
    chunk->len = output_samples * sizeof(int16_t);
    return ESP_OK;
}

static void audio_task(void *arg)
{
    (void)arg;
    audio_chunk_t pcm_chunk = {0};
    audio_chunk_t wire_chunk = {0};
    vibe_ima_adpcm_state_t encoder = {0};
    vibe_ima_adpcm_reset(&encoder);

    while (atomic_load(&s_running)) {
        esp_err_t err = read_audio_chunk(&pcm_chunk);
        if (err != ESP_OK) {
            if (atomic_load(&s_running)) {
                ESP_LOGW(TAG, "audio read failed: %s", esp_err_to_name(err));
            }
            continue;
        }
        if (pcm_chunk.len == 0) {
            continue;
        }
        if (s_audio_transport == VIBE_AUDIO_TRANSPORT_IMA_ADPCM) {
            if (pcm_chunk.len != AUDIO_CHUNK_BYTES ||
                !vibe_ima_adpcm_encode_block(
                    &encoder, (const int16_t *)pcm_chunk.data,
                    AUDIO_FRAME_SAMPLES, wire_chunk.data,
                    VIBE_STICK_AUDIO_ADPCM_FRAME_BYTES, &wire_chunk.len)) {
                ESP_LOGW(TAG, "ADPCM encode failed pcm_bytes=%u",
                         (unsigned)pcm_chunk.len);
                s_audio_stats.chunks_dropped++;
                s_audio_stats.bytes_dropped += pcm_chunk.len;
                continue;
            }
        } else {
            wire_chunk = pcm_chunk;
        }
        s_audio_stats.chunks_read++;
        s_audio_stats.bytes_read += pcm_chunk.len;
        if (xQueueSend(s_audio_queue, &wire_chunk, 0) != pdTRUE) {
            s_audio_stats.chunks_dropped++;
            s_audio_stats.bytes_dropped += pcm_chunk.len;
        } else {
            s_audio_stats.chunks_queued++;
            s_audio_stats.bytes_queued += pcm_chunk.len;
        }
    }

    ESP_LOGI(TAG, "recording stopped read_chunks=%u queued_chunks=%u dropped_chunks=%u dropped_bytes=%u pending=%u",
             (unsigned)s_audio_stats.chunks_read,
             (unsigned)s_audio_stats.chunks_queued,
             (unsigned)s_audio_stats.chunks_dropped,
             (unsigned)s_audio_stats.bytes_dropped,
             (unsigned)uxQueueMessagesWaiting(s_audio_queue));
    if (s_audio_mutex && xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        release_session_resources();
        s_audio_task = NULL;
        xSemaphoreGive(s_audio_mutex);
    } else {
        release_session_resources();
        s_audio_task = NULL;
    }
    vTaskDelete(NULL);
}

esp_err_t vibe_audio_init(void)
{
    if (!s_audio_mutex) {
        s_audio_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_audio_mutex != NULL, ESP_ERR_NO_MEM, TAG, "audio mutex");
    }
    if (!s_audio_queue) {
        ESP_RETURN_ON_ERROR(create_audio_queue(VIBE_AUDIO_TRANSPORT_PCM16),
                            TAG, "create PCM queue");
    }
#if VIBE_BOARD_HAS_GPIO_TONE_SPEAKER
    ESP_RETURN_ON_ERROR(init_tone_output(), TAG, "tone init");
#endif
    s_initialized = true;
    return ESP_OK;
}

esp_err_t vibe_audio_set_transport(vibe_audio_transport_t transport)
{
    ESP_RETURN_ON_FALSE(
        transport == VIBE_AUDIO_TRANSPORT_PCM16 ||
            transport == VIBE_AUDIO_TRANSPORT_IMA_ADPCM,
        ESP_ERR_INVALID_ARG, TAG, "invalid audio transport");
    ESP_RETURN_ON_FALSE(s_initialized && s_audio_mutex, ESP_ERR_INVALID_STATE,
                        TAG, "audio not initialized");
    ESP_RETURN_ON_FALSE(!vibe_audio_is_recording(), ESP_ERR_INVALID_STATE, TAG,
                        "recording active");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(250)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "audio busy");

    if (s_audio_transport == transport && s_audio_queue) {
        xSemaphoreGive(s_audio_mutex);
        return ESP_OK;
    }
    if (s_audio_queue) {
        vQueueDelete(s_audio_queue);
        s_audio_queue = NULL;
    }
    esp_err_t err = create_audio_queue(transport);
    if (err != ESP_OK && transport != VIBE_AUDIO_TRANSPORT_PCM16) {
        const esp_err_t requested_err = err;
        ESP_LOGW(TAG, "ADPCM queue unavailable, restoring PCM queue");
        if (create_audio_queue(VIBE_AUDIO_TRANSPORT_PCM16) != ESP_OK) {
            xSemaphoreGive(s_audio_mutex);
            return ESP_ERR_NO_MEM;
        }
        err = requested_err;
    }
    xSemaphoreGive(s_audio_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "audio transport=%s queue=%u buffer_ms=%u item_bytes=%u",
                 vibe_audio_transport_encoding(),
                 (unsigned)s_audio_queue_depth,
                 (unsigned)(s_audio_queue_depth * AUDIO_FRAME_MS),
                 (unsigned)s_audio_queue_item_size);
    }
    return err;
}

vibe_audio_transport_t vibe_audio_transport(void)
{
    return s_audio_transport;
}

const char *vibe_audio_transport_encoding(void)
{
    return s_audio_transport == VIBE_AUDIO_TRANSPORT_IMA_ADPCM
               ? VIBE_STICK_AUDIO_ADPCM_ENCODING
               : "pcm16";
}

const char *vibe_audio_transport_content_type(void)
{
    return s_audio_transport == VIBE_AUDIO_TRANSPORT_IMA_ADPCM
               ? VIBE_STICK_AUDIO_ADPCM_CONTENT_TYPE
               : "application/octet-stream";
}

size_t vibe_audio_wire_frame_bytes(void)
{
    return audio_wire_frame_bytes(s_audio_transport);
}

size_t vibe_audio_pcm_bytes_for_wire(size_t wire_bytes)
{
    if (s_audio_transport != VIBE_AUDIO_TRANSPORT_IMA_ADPCM) {
        return wire_bytes;
    }
    if (wire_bytes % VIBE_STICK_AUDIO_ADPCM_FRAME_BYTES != 0) {
        return 0;
    }
    return (wire_bytes / VIBE_STICK_AUDIO_ADPCM_FRAME_BYTES) *
           AUDIO_CHUNK_BYTES;
}

esp_err_t vibe_audio_prepare_deep_sleep(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(!vibe_audio_is_recording(), ESP_ERR_INVALID_STATE,
                        TAG, "recording active");
    ESP_RETURN_ON_FALSE(s_audio_mutex != NULL, ESP_ERR_INVALID_STATE,
                        TAG, "audio mutex missing");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(250)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "audio busy");

    release_session_resources();
#if VIBE_BOARD_HAS_GPIO_TONE_SPEAKER
    esp_err_t err = mute_tone_output();
#else
    esp_err_t err = vibe_board_speaker_set_enabled(false);
#endif
    xSemaphoreGive(s_audio_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "audio peripherals prepared for deep sleep");
    }
    return err;
}

esp_err_t vibe_audio_start(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (atomic_load(&s_running)) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(s_audio_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "audio mutex missing");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(250)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "audio busy");
    if (atomic_load(&s_running) || s_audio_task != NULL || s_tx_handle != NULL || s_rx_handle != NULL) {
        xSemaphoreGive(s_audio_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    vibe_audio_clear();
    memset(&s_audio_stats, 0, sizeof(s_audio_stats));
    s_capture_profile = vibe_capture_profile_current();
    if (!s_capture_profile ||
        s_capture_profile->output_sample_rate != VIBE_STICK_AUDIO_SAMPLE_RATE ||
        s_capture_profile->oversampling > AUDIO_CAPTURE_MAX_OVERSAMPLING) {
        xSemaphoreGive(s_audio_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    vibe_capture_processor_reset(&s_capture_processor);

    esp_err_t err = ESP_OK;
#if VIBE_BOARD_HAS_ES8311
    err = init_i2s_std(false, true,
                       vibe_capture_input_sample_rate(s_capture_profile),
                       s_capture_profile->input_only_right);
    if (err == ESP_OK) {
        err = init_codec(ESP_CODEC_DEV_TYPE_IN, ESP_CODEC_DEV_WORK_MODE_ADC,
                         vibe_capture_input_sample_rate(s_capture_profile),
                         s_capture_profile->input_only_right);
    }
#else
    err = init_i2s_pdm_rx(s_capture_profile);
#endif
    if (err != ESP_OK) {
        release_session_resources();
        xSemaphoreGive(s_audio_mutex);
        return err;
    }

    atomic_store(&s_running, true);
    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "vibe_audio", 8192, NULL, 5,
                                            &s_audio_task, VIBE_STICK_AUDIO_CORE);
    if (ok != pdPASS) {
        atomic_store(&s_running, false);
        release_session_resources();
        xSemaphoreGive(s_audio_mutex);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_audio_mutex);
    ESP_LOGI(TAG,
             "recording started board=%s input_hz=%u output_hz=%u oversampling=%u "
             "gain=%u filter=%u input_gain=%u direct=%u transport=%s "
             "queue=%u buffer_ms=%u",
             s_capture_profile->board_name,
             (unsigned)vibe_capture_input_sample_rate(s_capture_profile),
             (unsigned)s_capture_profile->output_sample_rate,
             (unsigned)s_capture_profile->oversampling,
             (unsigned)s_capture_profile->magnification,
             (unsigned)s_capture_profile->noise_filter_level,
             (unsigned)s_capture_profile->input_gain_db,
             s_capture_profile->process_samples ? 0U : 1U,
             vibe_audio_transport_encoding(), (unsigned)s_audio_queue_depth,
             (unsigned)(s_audio_queue_depth * AUDIO_FRAME_MS));
    return ESP_OK;
}

esp_err_t vibe_audio_stop(void)
{
    if (!atomic_load(&s_running) && s_audio_task == NULL) {
        return ESP_OK;
    }
    atomic_store(&s_running, false);
    if (s_audio_mutex && xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        signal_capture_stop_locked();
        xSemaphoreGive(s_audio_mutex);
    } else {
        ESP_LOGW(TAG, "audio stop could not lock resources for initial unblock");
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TASK_EXIT_WAIT_MS);
    while (s_audio_task != NULL) {
        if (xTaskGetTickCount() >= deadline) {
            ESP_LOGE(TAG, "audio task stop timed out; forcing bounded cleanup");
            if (s_audio_mutex &&
                xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
                if (s_audio_task != NULL) {
                    vTaskDelete(s_audio_task);
                    s_audio_task = NULL;
                }
                release_session_resources();
                xSemaphoreGive(s_audio_mutex);
            }
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

bool vibe_audio_is_recording(void)
{
    return atomic_load(&s_running) || s_audio_task != NULL;
}

esp_err_t vibe_audio_play_sound(agent_sound_t sound)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
#if VIBE_BOARD_HAS_GPIO_TONE_SPEAKER
    size_t segment_count = 0;
    const sound_segment_t *segments = sound_segments_for(sound, &segment_count);
    ESP_RETURN_ON_FALSE(segments != NULL && segment_count > 0, ESP_ERR_INVALID_ARG, TAG, "invalid tone");

    esp_err_t err = play_tone_segments(segments, segment_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tone playback failed: %s", esp_err_to_name(err));
    }
    return err;
#else
    ESP_RETURN_ON_FALSE(s_audio_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "audio mutex missing");
    if (vibe_audio_is_recording()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_audio_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (vibe_audio_is_recording() || s_tx_handle != NULL || s_rx_handle != NULL) {
        xSemaphoreGive(s_audio_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    size_t segment_count = 0;
    const sound_segment_t *segments = sound_segments_for(sound, &segment_count);
    if (!segments || segment_count == 0) {
        xSemaphoreGive(s_audio_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = vibe_board_speaker_set_enabled(true);
#if VIBE_BOARD_HAS_ES8311
    if (err == ESP_OK) {
        err = init_i2s_std(true, false, VIBE_STICK_AUDIO_SAMPLE_RATE, false);
    }
    if (err == ESP_OK) {
        err = init_codec(ESP_CODEC_DEV_TYPE_OUT, ESP_CODEC_DEV_WORK_MODE_DAC,
                         VIBE_STICK_AUDIO_SAMPLE_RATE, false);
    }
    if (err == ESP_OK) {
        err = play_sound_segments(segments, segment_count);
    }
    release_session_resources();
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_speaker_set_enabled(false));
#else
    if (err == ESP_OK) {
        err = play_tone_segments(segments, segment_count);
    }
#endif
    xSemaphoreGive(s_audio_mutex);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sound playback failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "sound played id=%d", (int)sound);
    }
    return err;
#endif
}

esp_err_t vibe_audio_play_stream_begin(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
#if VIBE_BOARD_HAS_GPIO_TONE_SPEAKER
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_RETURN_ON_FALSE(s_audio_mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "audio mutex missing");
    ESP_RETURN_ON_FALSE(!vibe_audio_is_recording() && !s_streaming,
                        ESP_ERR_INVALID_STATE, TAG, "audio busy");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(250)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "audio lock");
    if (vibe_audio_is_recording() || s_tx_handle != NULL || s_rx_handle != NULL) {
        xSemaphoreGive(s_audio_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = vibe_board_speaker_set_enabled(true);
#if VIBE_BOARD_HAS_ES8311
    if (err == ESP_OK) {
        err = init_i2s_std(true, false, VIBE_STICK_AUDIO_SAMPLE_RATE, false);
    }
    if (err == ESP_OK) {
        err = init_codec(ESP_CODEC_DEV_TYPE_OUT, ESP_CODEC_DEV_WORK_MODE_DAC,
                         VIBE_STICK_AUDIO_SAMPLE_RATE, false);
    }
#else
    err = ESP_ERR_NOT_SUPPORTED;
#endif
    if (err != ESP_OK) {
        release_session_resources();
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_speaker_set_enabled(false));
        xSemaphoreGive(s_audio_mutex);
        return err;
    }
    s_streaming = true;
    return ESP_OK;
#endif
}

esp_err_t vibe_audio_play_stream_write(const uint8_t *pcm, size_t len)
{
    ESP_RETURN_ON_FALSE(pcm != NULL && len > 0 && (len % 2) == 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid stream pcm");
#if VIBE_BOARD_HAS_GPIO_TONE_SPEAKER
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_RETURN_ON_FALSE(s_streaming && s_codec != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "stream not open");
#if VIBE_BOARD_HAS_ES8311
    ESP_RETURN_ON_ERROR(apply_output_volume(), TAG, "update stream volume");
#endif
    return esp_codec_dev_write(s_codec, (void *)pcm, (int)len) ==
                   ESP_CODEC_DEV_OK
               ? ESP_OK
               : ESP_FAIL;
#endif
}

esp_err_t vibe_audio_set_output_volume(uint8_t percent)
{
    ESP_RETURN_ON_FALSE(percent <= 100, ESP_ERR_INVALID_ARG, TAG,
                        "invalid output volume");
    atomic_store(&s_output_volume, percent);
    return ESP_OK;
}

uint8_t vibe_audio_get_output_volume(void)
{
    return atomic_load(&s_output_volume);
}

esp_err_t vibe_audio_play_stream_end(void)
{
#if VIBE_BOARD_HAS_GPIO_TONE_SPEAKER
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_RETURN_ON_FALSE(s_streaming, ESP_ERR_INVALID_STATE, TAG,
                        "stream not open");
    s_streaming = false;
    release_session_resources();
    esp_err_t err = vibe_board_speaker_set_enabled(false);
    xSemaphoreGive(s_audio_mutex);
    return err;
#endif
}

esp_err_t vibe_audio_play_pcm16_mono(const uint8_t *pcm, size_t len)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(pcm != NULL && len > 0 && (len % 2) == 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid pcm");
#if VIBE_BOARD_HAS_GPIO_TONE_SPEAKER
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_RETURN_ON_FALSE(s_audio_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "audio mutex missing");
    if (vibe_audio_is_recording()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (vibe_audio_is_recording() || s_tx_handle != NULL || s_rx_handle != NULL) {
        xSemaphoreGive(s_audio_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = vibe_board_speaker_set_enabled(true);
#if VIBE_BOARD_HAS_ES8311
    if (err == ESP_OK) {
        err = init_i2s_std(true, false, VIBE_STICK_AUDIO_SAMPLE_RATE, false);
    }
    if (err == ESP_OK) {
        err = init_codec(ESP_CODEC_DEV_TYPE_OUT, ESP_CODEC_DEV_WORK_MODE_DAC,
                         VIBE_STICK_AUDIO_SAMPLE_RATE, false);
    }
    if (err == ESP_OK) {
        size_t offset = 0;
        while (offset < len) {
            size_t chunk = len - offset;
            if (chunk > 2048) {
                chunk = 2048;
            }
            if (esp_codec_dev_write(s_codec, (void *)(pcm + offset), (int)chunk) != ESP_CODEC_DEV_OK) {
                err = ESP_FAIL;
                break;
            }
            offset += chunk;
        }
    }
    release_session_resources();
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_speaker_set_enabled(false));
#else
    err = ESP_ERR_NOT_SUPPORTED;
#endif
    xSemaphoreGive(s_audio_mutex);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pcm playback failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "pcm played bytes=%u", (unsigned)len);
    }
    return err;
#endif
}

esp_err_t vibe_audio_read(uint8_t *buffer, size_t capacity, size_t *len, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(buffer != NULL && len != NULL, ESP_ERR_INVALID_ARG, TAG, "null read args");
    ESP_RETURN_ON_FALSE(capacity >= vibe_audio_wire_frame_bytes(),
                        ESP_ERR_INVALID_ARG, TAG, "buffer too small");
    audio_chunk_t chunk = {0};
    if (xQueueReceive(s_audio_queue, &chunk, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        *len = 0;
        return ESP_ERR_TIMEOUT;
    }
    memcpy(buffer, chunk.data, chunk.len);
    *len = chunk.len;
    return ESP_OK;
}

esp_err_t vibe_audio_read_batch(uint8_t *buffer, size_t capacity, size_t *len,
                                size_t max_chunks, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(buffer != NULL && len != NULL, ESP_ERR_INVALID_ARG, TAG, "null batch args");
    const size_t frame_bytes = vibe_audio_wire_frame_bytes();
    ESP_RETURN_ON_FALSE(capacity >= frame_bytes, ESP_ERR_INVALID_ARG,
                        TAG, "batch buffer too small");
    if (max_chunks == 0) {
        max_chunks = 1;
    }

    *len = 0;
    audio_chunk_t chunk = {0};
    if (xQueueReceive(s_audio_queue, &chunk, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (chunk.len > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buffer, chunk.data, chunk.len);
    *len = chunk.len;

    for (size_t chunks = 1; chunks < max_chunks; ++chunks) {
        if (capacity - *len < frame_bytes) {
            break;
        }
        const TickType_t next_wait = atomic_load(&s_running)
                                         ? pdMS_TO_TICKS(AUDIO_FRAME_MS + 10)
                                         : 0;
        if (xQueueReceive(s_audio_queue, &chunk, next_wait) != pdTRUE) {
            break;
        }
        if (chunk.len > capacity - *len) {
            break;
        }
        memcpy(buffer + *len, chunk.data, chunk.len);
        *len += chunk.len;
    }
    return ESP_OK;
}

size_t vibe_audio_pending_chunks(void)
{
    return s_audio_queue ? uxQueueMessagesWaiting(s_audio_queue) : 0;
}

void vibe_audio_stats(vibe_audio_stats_t *stats)
{
    if (!stats) {
        return;
    }
    *stats = s_audio_stats;
}

const uint8_t *vibe_audio_data(size_t *len)
{
    if (len) {
        *len = 0;
    }
    return NULL;
}

void vibe_audio_clear(void)
{
    if (!s_audio_queue) {
        return;
    }
    audio_chunk_t chunk = {0};
    while (xQueueReceive(s_audio_queue, &chunk, 0) == pdTRUE) {
    }
}
