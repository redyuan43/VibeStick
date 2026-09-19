#include "vibe_sticks3_asr_transport.h"
#include "vibe_sticks3_asr_local.h"
#include "vibe_audio.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "asr_transport";
static esp_http_client_handle_t s_client;
static char s_response[1536];
static size_t s_response_length;
static bool s_response_overflow;
static unsigned s_retries;
static unsigned s_retry_budget_ms = 5000;
static int s_http_status;

static esp_err_t response_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t n = (size_t)event->data_len;
        if (n >= sizeof(s_response) - s_response_length) {
            s_response_overflow = true;
            return ESP_FAIL;
        }
        memcpy(s_response + s_response_length, event->data, n);
        s_response_length += n;
        s_response[s_response_length] = '\0';
    }
    return ESP_OK;
}

esp_err_t sticks3_transport_open(void)
{
    sticks3_transport_close();
    s_retries = 0;
    esp_http_client_config_t config = {
        .url = "http://" STICKS3_ASR_BRIDGE_HOST ":8765/health",
        .timeout_ms = 1800, .buffer_size = 2048, .buffer_size_tx = 2048,
        .event_handler = response_event, .keep_alive_enable = true,
        .keep_alive_idle = 5, .keep_alive_interval = 2, .keep_alive_count = 3,
    };
    s_client = esp_http_client_init(&config);
    if (!s_client) return ESP_ERR_NO_MEM;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char id[18];
    snprintf(id, sizeof(id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_http_client_set_header(s_client, "X-Vibe-Stick-Device-Id", id);
    esp_http_client_set_header(s_client, "X-Vibe-Stick-Board", "sticks3");
    esp_http_client_set_header(s_client, "X-Vibe-Stick-Firmware-Version",
                               esp_app_get_description()->version);
    esp_http_client_set_header(s_client, "X-Vibe-Stick-Token", STICKS3_ASR_BRIDGE_TOKEN);
    esp_http_client_set_header(s_client, "Connection", "keep-alive");
    return ESP_OK;
}

void sticks3_transport_close(void)
{
    if (s_client) esp_http_client_cleanup(s_client);
    s_client = NULL;
}

static esp_err_t request(const char *path, const void *body, size_t length,
                         int timeout_ms, cJSON **response)
{
    if (!s_client) return ESP_ERR_INVALID_STATE;
    char url[256];
    snprintf(url, sizeof(url), "http://%s:8765%s", STICKS3_ASR_BRIDGE_HOST, path);
    s_response_length = 0; s_response_overflow = false; s_response[0] = '\0';
    esp_http_client_set_url(s_client, url);
    esp_http_client_set_timeout_ms(s_client, timeout_ms);
    esp_http_client_set_method(s_client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(s_client, body, (int)length);
    int64_t begin = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(s_client);
    int status = esp_http_client_get_status_code(s_client);
    s_http_status = err == ESP_OK ? status : 0;
    int elapsed = (int)((esp_timer_get_time() - begin) / 1000);
    if (err != ESP_OK || status < 200 || status >= 300 || s_response_overflow) {
        ESP_LOGW(TAG, "request failed status=%d err=%s elapsed_ms=%d", status,
                 esp_err_to_name(err), elapsed);
        esp_http_client_close(s_client);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    *response = cJSON_Parse(s_response);
    if (!*response || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(*response, "success"))) {
        cJSON_Delete(*response); *response = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (elapsed > 350) ESP_LOGW(TAG, "slow acknowledged request elapsed_ms=%d", elapsed);
    return ESP_OK;
}

esp_err_t sticks3_transport_start(const char *session)
{
    char body[320];
    unsigned requested_retry_ms = vibe_audio_buffer_ms() >= 10000 ? 8000 : 0;
    s_retry_budget_ms = 5000;
    snprintf(body, sizeof(body), "{\"session_id\":\"%s\",\"protocol_version\":2,"
             "\"transport_encoding\":\"ima-adpcm-v1\",\"intent\":\"dictation\",\"mode\":\"PTT\","
             "\"upload_retry_ms\":%u}", session, requested_retry_ms);
    esp_http_client_set_header(s_client, "Content-Type", "application/json");
    cJSON *root = NULL;
    esp_err_t err = request("/recording/start", body, strlen(body), 4000, &root);
    if (err == ESP_OK) {
        cJSON *recording = cJSON_GetObjectItemCaseSensitive(root, "recording");
        cJSON *retry = cJSON_GetObjectItemCaseSensitive(recording, "upload_retry_ms");
        if (cJSON_IsNumber(retry) && retry->valuedouble >= 5000 &&
            retry->valuedouble <= requested_retry_ms) s_retry_budget_ms = (unsigned)retry->valuedouble;
        ESP_LOGI(TAG, "upload retry budget_ms=%u buffer_ms=%u", s_retry_budget_ms,
                 (unsigned)vibe_audio_buffer_ms());
        cJSON *encoding = cJSON_GetObjectItemCaseSensitive(recording, "accepted_transport_encoding");
        cJSON *mode = cJSON_GetObjectItemCaseSensitive(recording, "capture_mode");
        if (!cJSON_IsString(encoding) || strcmp(encoding->valuestring, "ima-adpcm-v1") ||
            !cJSON_IsString(mode) || strcmp(mode->valuestring, "device_upload")) err = ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t sticks3_transport_audio(const char *session, const uint8_t *audio,
                                 size_t length, uint32_t chunk)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= audio[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & -(int32_t)(crc & 1));
    }
    char path[180];
    snprintf(path, sizeof(path), "/recording/audio?session_id=%s&chunk_id=%lu&chunk_crc32=%08lx",
             session, (unsigned long)chunk, (unsigned long)(crc ^ UINT32_MAX));
    esp_http_client_set_header(s_client, "Content-Type", VIBE_STICK_AUDIO_ADPCM_CONTENT_TYPE);
    esp_http_client_set_header(s_client, "X-Vibe-Stick-Audio-Encoding", "ima-adpcm-v1");
    esp_http_client_set_header(s_client, "X-Vibe-Stick-Audio-Sample-Rate", "16000");
    esp_http_client_set_header(s_client, "X-Vibe-Stick-Audio-Channels", "1");
    esp_http_client_set_header(s_client, "X-Vibe-Stick-Audio-Block-Samples", "960");
    /* Hold this exact batch until ACK; do not dequeue fresh audio on a retry.
     * Bound recovery below the ASR relay's 15s idle / 10s backlog limits.
     */
    int64_t deadline = esp_timer_get_time() + (int64_t)s_retry_budget_ms * 1000;
    for (unsigned attempt = 0; ; ++attempt) {
        int remaining_ms = (int)((deadline - esp_timer_get_time()) / 1000);
        if (remaining_ms <= 0) break;
        cJSON *root = NULL;
        esp_err_t err = request(path, audio, length, remaining_ms < 1800 ? remaining_ms : 1800, &root);
        if (err == ESP_OK) {
            cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "recording");
            cJSON *next = cJSON_GetObjectItemCaseSensitive(r, "expected_chunk_id");
            cJSON *id = cJSON_GetObjectItemCaseSensitive(r, "session_id");
            bool accepted = cJSON_IsNumber(next) && next->valuedouble == (double)chunk + 1 &&
                            cJSON_IsString(id) && !strcmp(id->valuestring, session);
            cJSON_Delete(root);
            if (accepted) return ESP_OK;
            ESP_LOGE(TAG, "unexpected acknowledgement chunk=%lu", (unsigned long)chunk);
            return ESP_ERR_INVALID_RESPONSE;
        }
        cJSON_Delete(root);
        /* Authentication, ended sessions and invalid ACKs are not transient. */
        if ((s_http_status >= 400 && s_http_status < 500 &&
             s_http_status != 408 && s_http_status != 429) || err == ESP_ERR_INVALID_RESPONSE) return err;
        if (esp_timer_get_time() + 250000 < deadline) {
            ++s_retries;
            ESP_LOGW(TAG, "retry chunk=%lu attempt=%u", (unsigned long)chunk, attempt + 2);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            break;
        }
    }
    return ESP_FAIL;
}

esp_err_t sticks3_transport_stop(const char *session, size_t posts,
                                size_t pcm_bytes, bool failed)
{
    char body[320];
    snprintf(body, sizeof(body), "{\"session_id\":\"%s\",\"protocol_version\":2,"
             "\"total_chunks\":%u,\"total_bytes\":%u,\"upload_failed\":%s,\"paste\":true}",
             session, (unsigned)posts, (unsigned)pcm_bytes, failed ? "true" : "false");
    esp_http_client_set_header(s_client, "Content-Type", "application/json");
    cJSON *root = NULL;
    esp_err_t err = request("/recording/stop", body, strlen(body), 45000, &root);
    if (err == ESP_OK) {
        cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "recording");
        cJSON *status = cJSON_GetObjectItemCaseSensitive(r, "status");
        if (!cJSON_IsString(status) || strcmp(status->valuestring, "pasted")) err = ESP_FAIL;
    }
    cJSON_Delete(root);
    return err;
}

unsigned sticks3_transport_retries(void) { return s_retries; }
