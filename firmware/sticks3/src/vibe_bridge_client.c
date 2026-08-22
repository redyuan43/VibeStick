#include "vibe_bridge_client.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "vibe_bridge_client";

typedef struct {
    char *data;
    int capacity;
    int used;
} response_capture_t;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data ||
        !event->data || event->data_len <= 0) {
        return ESP_OK;
    }
    response_capture_t *capture =
        (response_capture_t *)event->user_data;
    if (!capture->data || capture->capacity <= 0 ||
        capture->used >= capture->capacity - 1) {
        return ESP_OK;
    }
    const int remaining = capture->capacity - 1 - capture->used;
    const int copy_len =
        event->data_len < remaining ? event->data_len : remaining;
    memcpy(capture->data + capture->used, event->data, copy_len);
    capture->used += copy_len;
    capture->data[capture->used] = '\0';
    return ESP_OK;
}

esp_err_t vibe_bridge_client_init(
    vibe_bridge_client_t *client,
    const vibe_bridge_client_config_t *config)
{
    if (!client || !config || config->rx_buffer_size <= 0 ||
        config->tx_buffer_size <= 0 || !config->set_headers) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(client, 0, sizeof(*client));
    client->config = *config;
    client->lock = xSemaphoreCreateMutex();
    return client->lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void vibe_bridge_client_lock(vibe_bridge_client_t *client)
{
    if (client && client->lock) {
        xSemaphoreTake(client->lock, portMAX_DELAY);
    }
}

void vibe_bridge_client_unlock(vibe_bridge_client_t *client)
{
    if (client && client->lock) {
        xSemaphoreGive(client->lock);
    }
}

esp_err_t vibe_bridge_client_request(
    vibe_bridge_client_t *bridge_client,
    const char *method,
    const char *host,
    int port,
    const char *token,
    const char *path,
    const char *body,
    char *response,
    int response_len,
    int timeout_ms)
{
    if (!bridge_client || !bridge_client->lock || !method || !host ||
        host[0] == '\0' || port <= 0 || !path || timeout_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * Each request owns its esp_http_client handle and response buffer.
     * Do not serialize short recording control requests behind the device
     * command long-poll; that delays microphone activation noticeably.
     */
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%d%s", host, port, path);
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
        .buffer_size = bridge_client->config.rx_buffer_size,
        .buffer_size_tx = bridge_client->config.tx_buffer_size,
        .event_handler = http_event_handler,
        .user_data = &capture,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(
        client,
        strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    bridge_client->config.set_headers(
        client, token, bridge_client->config.context);
    if (body) {
        esp_http_client_set_header(
            client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }
    esp_err_t err = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    if (err == ESP_OK && (status_code < 200 || status_code >= 300)) {
        ESP_LOGW(TAG, "http %s %s returned status=%d",
                 method, path, status_code);
        err = ESP_FAIL;
    }
    if (err == ESP_OK && response && response_len > 0 &&
        capture.used == 0) {
        ESP_LOGW(TAG, "http %s %s status=%d empty response",
                 method, path, status_code);
    }
    esp_http_client_cleanup(client);
    return err;
}
