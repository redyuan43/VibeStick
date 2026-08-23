#include "vibe_sticks3_asr_minimal.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "vibe_audio.h"
#include "vibe_board.h"
#include "vibe_board_profile.h"
#include "vibe_stick_config.h"
#include "vibe_sticks3_status.h"
#include "vibe_sticks3_upload.h"
#include "vibe_wifi_runtime.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HTTP_RX_BYTES 512
#define HTTP_TX_BYTES 2048
#define BUTTON_POLL_MS 20

static const char *TAG = "sticks3_min";
static vibe_wifi_runtime_t s_wifi;
static bool s_recording;
static char s_session[40];

static bool online(void) { return vibe_wifi_runtime_connected(&s_wifi); }

static void device_id(char *out, size_t len)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5]);
    }
}

static esp_err_t request(const char *path, const uint8_t *body, size_t len,
                         const char *type, int timeout_ms)
{
    if (!online()) return ESP_ERR_INVALID_STATE;
    char url[224] = {0};
    char id[18] = {0};
    device_id(id, sizeof(id));
    snprintf(url, sizeof(url), "http://%s:%d%s", VIBE_STICK_BRIDGE_HOST,
             VIBE_STICK_BRIDGE_PORT, path);
    const esp_http_client_config_t config = {
        .url = url, .timeout_ms = timeout_ms, .buffer_size = HTTP_RX_BYTES,
        .buffer_size_tx = HTTP_TX_BYTES,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "http");
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "X-Vibe-Stick-Device-Id", id);
    esp_http_client_set_header(client, "X-Vibe-Stick-Board", VIBE_BOARD_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Version", FIRMWARE_VERSION);
    esp_http_client_set_header(client, "X-Vibe-Stick-Event-Source", VIBE_BOARD_EVENT_SOURCE);
    esp_http_client_set_header(client, "Content-Type", type);
    esp_http_client_set_post_field(client, (const char *)body, len);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK && status >= 200 && status < 300 ? ESP_OK : ESP_FAIL;
}

static uint32_t crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & -(int32_t)(crc & 1));
    }
    return crc ^ UINT32_MAX;
}

static esp_err_t upload(const uint8_t *audio, size_t len, uint32_t chunk, void *context)
{
    (void)context;
    char path[176] = {0};
    snprintf(path, sizeof(path), VIBE_STICK_RECORDING_AUDIO_PATH
             "?session_id=%s&chunk_id=%lu&chunk_crc32=%08lx", s_session,
             (unsigned long)chunk, (unsigned long)crc32(audio, len));
    return request(path, audio, len, "application/octet-stream", 5000);
}

static void stop_recording(void)
{
    if (!s_recording) return;
    vibe_sticks3_status_set_recording_animation(false);
    vibe_sticks3_status_show(VIBE_STICKS3_STATUS_SENDING);
    (void)vibe_audio_stop();
    vibe_sticks3_upload_wait();
    size_t posts = 0, bytes = 0;
    vibe_sticks3_upload_totals(&posts, &bytes);
    bool failed = vibe_sticks3_upload_failed();
    char body[320] = {0};
    snprintf(body, sizeof(body),
             "{\"event\":\"sticks3_button\",\"source\":\"%s\",\"session_id\":\"%s\","
             "\"intent\":\"dictation\",\"mode\":\"PTT\",\"paste\":true,"
             "\"protocol_version\":2,\"total_chunks\":%u,\"total_bytes\":%u,"
             "\"upload_failed\":%s}", VIBE_BOARD_EVENT_SOURCE, s_session,
             (unsigned)posts, (unsigned)bytes, failed ? "true" : "false");
    esp_err_t err = request(VIBE_STICK_RECORDING_STOP_PATH, (uint8_t *)body,
                            strlen(body), "application/json", 30000);
    ESP_LOGI(TAG, "stop posts=%u bytes=%u failed=%d err=%s", (unsigned)posts,
             (unsigned)bytes, failed, esp_err_to_name(err));
    vibe_audio_clear();
    s_recording = false;
    s_session[0] = '\0';
    vibe_sticks3_status_show(!failed && err == ESP_OK ? VIBE_STICKS3_STATUS_DONE :
                                                        VIBE_STICKS3_STATUS_ERROR);
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_wifi_runtime_set_performance(&s_wifi, false));
}

static void start_recording(void)
{
    if (s_recording || !online()) {
        vibe_sticks3_status_show(VIBE_STICKS3_STATUS_WIFI);
        return;
    }
    snprintf(s_session, sizeof(s_session), "%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random());
    char id[18] = {0}, body[320] = {0};
    device_id(id, sizeof(id));
    snprintf(body, sizeof(body),
             "{\"event\":\"sticks3_button\",\"source\":\"%s\",\"audio_source\":\"%s\","
             "\"session_id\":\"%s\",\"device_id\":\"%s\",\"intent\":\"dictation\","
             "\"mode\":\"PTT\",\"protocol_version\":2}", VIBE_BOARD_EVENT_SOURCE,
             VIBE_BOARD_AUDIO_SOURCE, s_session, id);
    vibe_sticks3_status_show(VIBE_STICKS3_STATUS_SENDING);
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_wifi_runtime_set_performance(&s_wifi, true));
    if (request(VIBE_STICK_RECORDING_START_PATH, (uint8_t *)body, strlen(body),
                "application/json", 1500) != ESP_OK || vibe_audio_start() != ESP_OK ||
        !vibe_sticks3_upload_start(upload, NULL)) {
        (void)vibe_audio_stop();
        vibe_audio_clear();
        s_session[0] = '\0';
        vibe_sticks3_status_show(VIBE_STICKS3_STATUS_ERROR);
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_wifi_runtime_set_performance(&s_wifi, false));
        return;
    }
    s_recording = true;
    vibe_sticks3_status_show(VIBE_STICKS3_STATUS_RECORDING);
    vibe_sticks3_status_set_recording_animation(true);
}

static void button_task(void *arg)
{
    (void)arg;
    bool was_pressed = false;
    while (true) {
        bool pressed = gpio_get_level(VIBE_BOARD_PIN_BUTTON_FRONT) == 0;
        if (pressed && !was_pressed) {
            vTaskDelay(pdMS_TO_TICKS(35));
            if (gpio_get_level(VIBE_BOARD_PIN_BUTTON_FRONT) == 0) {
                s_recording ? stop_recording() : start_recording();
            }
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

static void wifi_changed(bool connected, const char *ip, void *context)
{
    (void)ip; (void)context;
    if (!s_recording) vibe_sticks3_status_show(connected ? VIBE_STICKS3_STATUS_READY :
                                                            VIBE_STICKS3_STATUS_WIFI);
}

void vibe_sticks3_asr_minimal_start(void)
{
    ESP_LOGI(TAG, "StickS3 minimal ASR %s", FIRMWARE_VERSION);
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(vibe_board_init_power());
    ESP_ERROR_CHECK(vibe_sticks3_status_init());
    ESP_ERROR_CHECK(vibe_audio_init());
    ESP_ERROR_CHECK(vibe_sticks3_upload_init());
    const vibe_wifi_profile_t profile = {
        .ssid = VIBE_STICK_WIFI_SSID, .password = VIBE_STICK_WIFI_PASSWORD,
    };
    const vibe_wifi_runtime_config_t wifi = {
        .configured_profiles = profile.ssid[0] ? &profile : NULL,
        .configured_profile_count = profile.ssid[0] ? 1 : 0,
        .idle_power_save = WIFI_PS_MIN_MODEM, .max_tx_power = VIBE_BOARD_WIFI_MAX_TX_POWER,
        .status_changed = wifi_changed,
    };
    ESP_ERROR_CHECK(vibe_wifi_runtime_init(&s_wifi, &wifi));
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << VIBE_BOARD_PIN_BUTTON_FRONT, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(button_task, "asr_button", 4096,
                                             NULL, 4, NULL, 0) == pdPASS ?
                    ESP_OK : ESP_ERR_NO_MEM);
}
