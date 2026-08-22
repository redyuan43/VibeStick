#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef esp_err_t (*vibe_ota_manifest_request_fn)(
    const char *path,
    char *response,
    size_t response_len,
    int timeout_ms,
    void *context);
typedef void (*vibe_ota_build_url_fn)(
    const char *path_or_url,
    char *url,
    size_t url_len,
    void *context);
typedef esp_err_t (*vibe_ota_prepare_client_fn)(
    esp_http_client_handle_t client,
    void *context);
typedef esp_err_t (*vibe_ota_prepare_download_fn)(void *context);
typedef bool (*vibe_ota_runtime_predicate_fn)(void *context);
typedef void (*vibe_ota_runtime_overlay_fn)(
    const char *title,
    const char *hint,
    bool visible,
    void *context);
typedef void (*vibe_ota_runtime_action_fn)(void *context);

typedef struct {
    const char *board;
    const char *version;
    const char *build_id;
    const char *manifest_path;
    const char *binary_path;
    int rx_buffer_size;
    int tx_buffer_size;
    int read_buffer_bytes;
    int download_timeout_ms;
    int no_progress_timeout_ms;
    int periodic_check_ms;
    int battery_check_ms;
    uint32_t task_stack_bytes;
    UBaseType_t task_priority;
    BaseType_t task_core;
    bool enabled;
} vibe_ota_runtime_config_t;

typedef struct {
    vibe_ota_manifest_request_fn request_manifest;
    vibe_ota_build_url_fn build_url;
    vibe_ota_prepare_download_fn prepare_download;
    vibe_ota_prepare_client_fn prepare_client;
    vibe_ota_runtime_predicate_fn online;
    vibe_ota_runtime_predicate_fn busy;
    vibe_ota_runtime_predicate_fn external_powered;
    vibe_ota_runtime_predicate_fn display_active;
    vibe_ota_runtime_predicate_fn settings_active;
    vibe_ota_runtime_overlay_fn overlay;
    vibe_ota_runtime_action_fn before_start;
    void *context;
} vibe_ota_runtime_dependencies_t;

typedef enum {
    VIBE_OTA_COMMAND_CHECK,
} vibe_ota_runtime_command_t;

typedef struct {
    bool active;
    int64_t last_check_ms;
} vibe_ota_runtime_snapshot_t;

typedef struct {
    vibe_ota_runtime_config_t config;
    vibe_ota_runtime_dependencies_t dependencies;
    TaskHandle_t task;
    atomic_bool active;
    int64_t last_check_ms;
} vibe_ota_runtime_t;

esp_err_t vibe_ota_runtime_init(
    vibe_ota_runtime_t *runtime,
    const vibe_ota_runtime_config_t *config,
    const vibe_ota_runtime_dependencies_t *dependencies);
bool vibe_ota_runtime_handle(
    vibe_ota_runtime_t *runtime,
    vibe_ota_runtime_command_t command);
void vibe_ota_runtime_tick(vibe_ota_runtime_t *runtime,
                           int64_t now_ms);
void vibe_ota_runtime_snapshot(
    const vibe_ota_runtime_t *runtime,
    vibe_ota_runtime_snapshot_t *snapshot);
