#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef void (*vibe_bridge_client_headers_fn)(
    esp_http_client_handle_t client,
    const char *token,
    void *context);

typedef struct {
    int rx_buffer_size;
    int tx_buffer_size;
    vibe_bridge_client_headers_fn set_headers;
    void *context;
} vibe_bridge_client_config_t;

typedef struct {
    vibe_bridge_client_config_t config;
    SemaphoreHandle_t lock;
} vibe_bridge_client_t;

esp_err_t vibe_bridge_client_init(
    vibe_bridge_client_t *client,
    const vibe_bridge_client_config_t *config);
void vibe_bridge_client_lock(vibe_bridge_client_t *client);
void vibe_bridge_client_unlock(vibe_bridge_client_t *client);
esp_err_t vibe_bridge_client_request(
    vibe_bridge_client_t *client,
    const char *method,
    const char *host,
    int port,
    const char *token,
    const char *path,
    const char *body,
    char *response,
    int response_len,
    int timeout_ms);
