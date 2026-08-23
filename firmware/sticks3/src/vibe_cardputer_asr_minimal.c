#include "vibe_cardputer_asr_minimal.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "vibe_cardputer_asr_board.h"
#include "vibe_cardputer_capture.h"
#include "vibe_cardputer_opt.h"
#include "vibe_cardputer_upload.h"
#include "vibe_stick_config.h"
#include "vibe_wifi_runtime.h"

#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define MINIMAL_HTTP_RX_BYTES 512
#define MINIMAL_HTTP_TX_BYTES 2048
#define MINIMAL_RECORDING_START_TIMEOUT_MS 1500
#define MINIMAL_RECORDING_STOP_TIMEOUT_MS 30000
#define MINIMAL_RECORDING_UPLOAD_TIMEOUT_MS 5000
static const char *TAG = "card_asr_min";

typedef struct {
    char *data;
    size_t capacity;
    size_t used;
} response_capture_t;

typedef enum {
    MINIMAL_COMMAND_TOGGLE_RECORDING = 1,
} minimal_command_t;

static vibe_wifi_runtime_t s_wifi;
static QueueHandle_t s_commands;
static bool s_recording;
static char s_session_id[40];

static bool wifi_is_connected(void)
{
    return vibe_wifi_runtime_connected(&s_wifi);
}

static esp_err_t capture_response(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data ||
        !event->data || event->data_len <= 0) {
        return ESP_OK;
    }
    response_capture_t *capture = event->user_data;
    if (!capture->data || capture->capacity == 0 ||
        capture->used >= capture->capacity - 1) {
        return ESP_OK;
    }
    size_t remaining = capture->capacity - 1 - capture->used;
    size_t count = event->data_len < (int)remaining
                       ? (size_t)event->data_len
                       : remaining;
    memcpy(capture->data + capture->used, event->data, count);
    capture->used += count;
    capture->data[capture->used] = '\0';
    return ESP_OK;
}

static void device_id(char *value, size_t value_len)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(value, value_len, "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return;
    }
    snprintf(value, value_len, "cardputer-minimal");
}

static void set_headers(esp_http_client_handle_t client)
{
    char id[18] = {0};
    device_id(id, sizeof(id));
    esp_http_client_set_header(client, "X-Vibe-Stick-Device-Id", id);
    esp_http_client_set_header(client, "X-Vibe-Stick-Board",
                               VIBE_CARDPUTER_BOARD_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Name", FIRMWARE_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Version", FIRMWARE_VERSION);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Transport", TRANSPORT);
    esp_http_client_set_header(client, "X-Vibe-Stick-Event-Source",
                               VIBE_CARDPUTER_EVENT_SOURCE);
    if (VIBE_STICK_BRIDGE_TOKEN[0] != '\0') {
        esp_http_client_set_header(
            client, "X-Vibe-Stick-Token", VIBE_STICK_BRIDGE_TOKEN);
    }
}

static esp_err_t request(const char *method, const char *path,
                         const uint8_t *body, size_t body_len,
                         const char *content_type, char *response,
                         size_t response_len, int timeout_ms)
{
    if (!wifi_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    char url[224];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             VIBE_STICK_CARDPUTER_MINIMAL_BRIDGE_HOST,
             VIBE_STICK_CARDPUTER_MINIMAL_BRIDGE_PORT, path);
    response_capture_t capture = {
        .data = response,
        .capacity = response_len,
    };
    if (response && response_len > 0) {
        response[0] = '\0';
    }
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = MINIMAL_HTTP_RX_BYTES,
        .buffer_size_tx = MINIMAL_HTTP_TX_BYTES,
        .event_handler = capture_response,
        .user_data = &capture,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "http init");
    esp_http_client_set_method(
        client, strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    set_headers(client);
    if (content_type) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }
    if (body && body_len > 0) {
        esp_http_client_set_post_field(client, (const char *)body, body_len);
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && (status < 200 || status >= 300)) {
        ESP_LOGW(TAG, "%s %s returned %d", method, path, status);
        return ESP_FAIL;
    }
    return err;
}

static uint32_t crc32(const uint8_t *data, size_t len)
{
    uint32_t value = UINT32_MAX;
    for (size_t index = 0; index < len; ++index) {
        value ^= data[index];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            value = (value >> 1) ^ (0xedb88320u & -(int32_t)(value & 1u));
        }
    }
    return value ^ UINT32_MAX;
}

static esp_err_t upload_audio(const uint8_t *audio, size_t audio_len,
                              uint32_t chunk_id, void *context)
{
    (void)context;
    if (!audio || audio_len == 0 || s_session_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char path[176];
    snprintf(path, sizeof(path),
             VIBE_STICK_RECORDING_AUDIO_PATH
             "?session_id=%s&chunk_id=%lu&chunk_crc32=%08lx",
             s_session_id, (unsigned long)chunk_id,
             (unsigned long)crc32(audio, audio_len));
    return request("POST", path, audio, audio_len,
                   "application/octet-stream", NULL, 0,
                   MINIMAL_RECORDING_UPLOAD_TIMEOUT_MS);
}

static void make_session_id(void)
{
    snprintf(s_session_id, sizeof(s_session_id), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
}

static void start_recording(void)
{
    if (s_recording || !wifi_is_connected()) {
        ESP_LOGW(TAG, "start rejected recording=%d wifi=%d",
                 s_recording ? 1 : 0, wifi_is_connected() ? 1 : 0);
        return;
    }
    make_session_id();
    char id[18] = {0};
    device_id(id, sizeof(id));
    char body[320];
    snprintf(body, sizeof(body),
             "{\"event\":\"cardputer_opt\",\"source\":\"%s\","
             "\"audio_source\":\"%s\",\"session_id\":\"%s\","
             "\"device_id\":\"%s\",\"intent\":\"dictation\","
             "\"mode\":\"PTT\",\"protocol_version\":2}",
             VIBE_CARDPUTER_EVENT_SOURCE, VIBE_CARDPUTER_AUDIO_SOURCE,
             s_session_id, id);
    char response[MINIMAL_HTTP_RX_BYTES] = {0};
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        vibe_wifi_runtime_set_performance(&s_wifi, true));
    esp_err_t err = request("POST", VIBE_STICK_RECORDING_START_PATH,
                            (const uint8_t *)body, strlen(body),
                            "application/json", response, sizeof(response),
                            MINIMAL_RECORDING_START_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "recording start failed: %s", esp_err_to_name(err));
        s_session_id[0] = '\0';
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            vibe_wifi_runtime_set_performance(&s_wifi, false));
        return;
    }
    err = vibe_cardputer_capture_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "microphone start failed: %s", esp_err_to_name(err));
        s_session_id[0] = '\0';
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            vibe_wifi_runtime_set_performance(&s_wifi, false));
        return;
    }
    if (!vibe_cardputer_upload_start(upload_audio, NULL)) {
        ESP_LOGW(TAG, "upload task start failed");
        (void)vibe_cardputer_capture_stop();
        vibe_cardputer_capture_clear();
        s_session_id[0] = '\0';
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            vibe_wifi_runtime_set_performance(&s_wifi, false));
        return;
    }
    s_recording = true;
    ESP_LOGI(TAG, "recording started session=%s", s_session_id);
}

static void stop_recording(void)
{
    if (!s_recording) {
        return;
    }
    (void)vibe_cardputer_capture_stop();
    vibe_cardputer_upload_wait();
    size_t posts = 0;
    size_t bytes = 0;
    vibe_cardputer_upload_totals(&posts, &bytes);
    bool failed = vibe_cardputer_upload_failed();
    char body[320];
    snprintf(body, sizeof(body),
             "{\"event\":\"cardputer_opt\",\"source\":\"%s\","
             "\"session_id\":\"%s\",\"intent\":\"dictation\",\"mode\":\"PTT\","
             "\"paste\":true,\"protocol_version\":2,\"total_chunks\":%u,"
             "\"total_bytes\":%u,\"upload_failed\":%s}",
             VIBE_CARDPUTER_EVENT_SOURCE, s_session_id,
             (unsigned)posts, (unsigned)bytes, failed ? "true" : "false");
    char response[MINIMAL_HTTP_RX_BYTES] = {0};
    esp_err_t err = request("POST", VIBE_STICK_RECORDING_STOP_PATH,
                            (const uint8_t *)body, strlen(body),
                            "application/json", response, sizeof(response),
                            MINIMAL_RECORDING_STOP_TIMEOUT_MS);
    ESP_LOGI(TAG, "recording stopped posts=%u bytes=%u upload_failed=%d stop=%s",
             (unsigned)posts, (unsigned)bytes, failed ? 1 : 0,
             esp_err_to_name(err));
    vibe_cardputer_capture_clear();
    s_session_id[0] = '\0';
    s_recording = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        vibe_wifi_runtime_set_performance(&s_wifi, false));
}

static void opt_pressed(void *context)
{
    (void)context;
    if (!s_commands) {
        return;
    }
    const minimal_command_t command = MINIMAL_COMMAND_TOGGLE_RECORDING;
    (void)xQueueSend(s_commands, &command, 0);
}

static void command_task(void *arg)
{
    (void)arg;
    while (true) {
        minimal_command_t command = 0;
        if (xQueueReceive(s_commands, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (command == MINIMAL_COMMAND_TOGGLE_RECORDING) {
            if (s_recording) {
                stop_recording();
            } else {
                start_recording();
            }
        }
    }
}

void vibe_cardputer_asr_minimal_start(void)
{
    ESP_LOGI(TAG, "Cardputer minimal ASR firmware %s", FIRMWARE_VERSION);
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(vibe_cardputer_asr_board_init());
    ESP_ERROR_CHECK(vibe_cardputer_capture_init());
    ESP_ERROR_CHECK(vibe_cardputer_upload_init());
    s_commands = xQueueCreate(4, sizeof(minimal_command_t));
    ESP_ERROR_CHECK(s_commands ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(vibe_cardputer_opt_init(opt_pressed, NULL));

    const vibe_wifi_runtime_config_t wifi = {
        .idle_power_save = WIFI_PS_MIN_MODEM,
        .max_tx_power = VIBE_CARDPUTER_WIFI_MAX_TX_POWER,
    };
    ESP_ERROR_CHECK(vibe_wifi_runtime_init(&s_wifi, &wifi));
    BaseType_t started = xTaskCreatePinnedToCore(
        command_task, "asr_commands", 4096, NULL, 4, NULL, 0);
    ESP_ERROR_CHECK(started == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
