#include "vibe_ota_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "vibe_ota_policy.h"

static const char *TAG = "vibe_ota_runtime";

static void copy_json_string(cJSON *root,
                             const char *key,
                             char *target,
                             size_t target_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(target, item->valuestring, target_len);
    }
}

static bool parse_manifest(const char *json,
                           vibe_ota_manifest_t *manifest)
{
    if (!json || !manifest) {
        return false;
    }
    memset(manifest, 0, sizeof(*manifest));
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *available =
        cJSON_GetObjectItemCaseSensitive(root, "available");
    manifest->available =
        cJSON_IsBool(available) && cJSON_IsTrue(available);
    copy_json_string(root, "board", manifest->board,
                     sizeof(manifest->board));
    copy_json_string(root, "version", manifest->version,
                     sizeof(manifest->version));
    copy_json_string(root, "build_id", manifest->build_id,
                     sizeof(manifest->build_id));
    copy_json_string(root, "sha256", manifest->sha256,
                     sizeof(manifest->sha256));
    copy_json_string(root, "elf_sha256", manifest->elf_sha256,
                     sizeof(manifest->elf_sha256));
    copy_json_string(root, "url", manifest->url,
                     sizeof(manifest->url));
    cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (cJSON_IsNumber(size)) {
        manifest->size = size->valueint;
    }
    cJSON_Delete(root);
    return manifest->available;
}

static bool manifest_is_new(
    const vibe_ota_runtime_t *runtime,
    const vibe_ota_manifest_t *manifest)
{
    char current_sha256[65] = {0};
    if (manifest && manifest->sha256[0] != '\0') {
        uint8_t running_sha256[32] = {0};
        const esp_partition_t *running =
            esp_ota_get_running_partition();
        if (running &&
            esp_partition_get_sha256(
                running, running_sha256) == ESP_OK) {
            for (size_t index = 0;
                 index < sizeof(running_sha256); ++index) {
                snprintf(current_sha256 + index * 2,
                         sizeof(current_sha256) - index * 2,
                         "%02x", running_sha256[index]);
            }
        }
    }
    const vibe_ota_decision_t decision =
        vibe_ota_update_decision(
            manifest,
            runtime->config.board,
            runtime->config.version,
            runtime->config.build_id,
            current_sha256,
            esp_app_get_elf_sha256_str());
    if (decision == VIBE_OTA_DECISION_UPDATE) {
        return true;
    }
    ESP_LOGI(TAG,
             "manifest not selected decision=%d candidate=%s",
             (int)decision,
             manifest ? manifest->version : "");
    return false;
}

static esp_err_t perform_update(
    vibe_ota_runtime_t *runtime,
    const vibe_ota_manifest_t *manifest)
{
    const esp_partition_t *partition =
        esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(
        partition, ESP_ERR_NOT_FOUND, TAG, "no OTA partition");
    if (manifest->size <= 0 ||
        manifest->size > (int)partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    char default_path[96] = {0};
    const char *path_or_url = manifest->url;
    if (!path_or_url || path_or_url[0] == '\0') {
        snprintf(default_path, sizeof(default_path),
                 "%s?board=%s",
                 runtime->config.binary_path,
                 runtime->config.board);
        path_or_url = default_path;
    }
    ESP_RETURN_ON_ERROR(
        runtime->dependencies.prepare_download(
            runtime->dependencies.context),
        TAG, "prepare OTA target");
    char url[256] = {0};
    runtime->dependencies.build_url(
        path_or_url, url, sizeof(url),
        runtime->dependencies.context);

    const esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = runtime->config.rx_buffer_size,
        .buffer_size_tx = runtime->config.tx_buffer_size,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client =
        esp_http_client_init(&http_config);
    ESP_RETURN_ON_FALSE(
        client, ESP_ERR_NO_MEM, TAG, "OTA http init");
    esp_err_t err = runtime->dependencies.prepare_client(
        client, runtime->dependencies.context);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    const int64_t content_length =
        esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200 ||
        (content_length > 0 &&
         content_length > (int64_t)partition->size)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(
        partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }
    uint8_t *buffer = heap_caps_malloc(
        runtime->config.read_buffer_bytes, MALLOC_CAP_8BIT);
    if (!buffer) {
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    const int64_t started_ms = esp_timer_get_time() / 1000;
    int64_t last_progress_ms = started_ms;
    while (true) {
        const int data_read = esp_http_client_read(
            client, (char *)buffer,
            runtime->config.read_buffer_bytes);
        if (data_read < 0) {
            err = ESP_FAIL;
            break;
        }
        if (data_read == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                err = ESP_OK;
                break;
            }
            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - started_ms >=
                    runtime->config.download_timeout_ms ||
                now_ms - last_progress_ms >=
                    runtime->config.no_progress_timeout_ms) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        err = esp_ota_write(ota_handle, buffer, data_read);
        if (err != ESP_OK) {
            break;
        }
        total_read += data_read;
        last_progress_ms = esp_timer_get_time() / 1000;
        if (last_progress_ms - started_ms >=
            runtime->config.download_timeout_ms) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
    }

    const bool complete =
        esp_http_client_is_complete_data_received(client);
    heap_caps_free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || !complete ||
        (manifest->size > 0 &&
         total_read != manifest->size)) {
        esp_ota_abort(ota_handle);
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(
        esp_ota_end(ota_handle), TAG, "esp_ota_end");
    ESP_RETURN_ON_ERROR(
        esp_ota_set_boot_partition(partition),
        TAG, "esp_ota_set_boot_partition");
    ESP_LOGI(TAG, "OTA complete bytes=%d; restarting",
             total_read);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static void check_task(void *arg)
{
    vibe_ota_runtime_t *runtime = arg;
    char *response = calloc(1, 768);
    vibe_ota_manifest_t manifest = {0};
    bool overlay_shown = false;
    if (!response) {
        goto cleanup;
    }

    char path[96] = {0};
    snprintf(path, sizeof(path), "%s?board=%s",
             runtime->config.manifest_path,
             runtime->config.board);
    esp_err_t err = runtime->dependencies.request_manifest(
        path, response, 768, 5000,
        runtime->dependencies.context);
    if (err == ESP_OK &&
        parse_manifest(response, &manifest) &&
        manifest_is_new(runtime, &manifest)) {
        runtime->dependencies.overlay(
            "OTA update", "", true,
            runtime->dependencies.context);
        overlay_shown = true;
        err = perform_update(runtime, &manifest);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OTA update failed: %s",
                     esp_err_to_name(err));
            runtime->dependencies.overlay(
                "OTA failed", "", true,
                runtime->dependencies.context);
            vTaskDelay(pdMS_TO_TICKS(1200));
        }
    }
    if (overlay_shown) {
        runtime->dependencies.overlay(
            NULL, NULL, false,
            runtime->dependencies.context);
    }
    free(response);
cleanup:
    atomic_store(&runtime->active, false);
    runtime->task = NULL;
    vTaskDelete(NULL);
}

esp_err_t vibe_ota_runtime_init(
    vibe_ota_runtime_t *runtime,
    const vibe_ota_runtime_config_t *config,
    const vibe_ota_runtime_dependencies_t *dependencies)
{
    if (!runtime || !config || !dependencies ||
        !config->board || !config->version ||
        !config->build_id || !config->manifest_path ||
        !config->binary_path ||
        !dependencies->request_manifest ||
        !dependencies->build_url ||
        !dependencies->prepare_download ||
        !dependencies->prepare_client ||
        !dependencies->online || !dependencies->busy ||
        !dependencies->external_powered ||
        !dependencies->display_active ||
        !dependencies->settings_active ||
        !dependencies->overlay) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->config = *config;
    runtime->dependencies = *dependencies;
    atomic_init(&runtime->active, false);
    return ESP_OK;
}

bool vibe_ota_runtime_handle(
    vibe_ota_runtime_t *runtime,
    vibe_ota_runtime_command_t command)
{
    if (!runtime || command != VIBE_OTA_COMMAND_CHECK ||
        !runtime->config.enabled ||
        !runtime->dependencies.online(
            runtime->dependencies.context) ||
        runtime->dependencies.busy(
            runtime->dependencies.context) ||
        atomic_exchange(&runtime->active, true)) {
        return false;
    }
    if (runtime->dependencies.before_start) {
        runtime->dependencies.before_start(
            runtime->dependencies.context);
    }
    const BaseType_t created = xTaskCreatePinnedToCore(
        check_task, "ota_check",
        runtime->config.task_stack_bytes,
        runtime, runtime->config.task_priority,
        &runtime->task, runtime->config.task_core);
    if (created != pdPASS) {
        runtime->task = NULL;
        atomic_store(&runtime->active, false);
        return false;
    }
    return true;
}

void vibe_ota_runtime_tick(vibe_ota_runtime_t *runtime,
                           int64_t now_ms)
{
    if (!runtime || !runtime->config.enabled) {
        return;
    }
    const bool external =
        runtime->dependencies.external_powered(
            runtime->dependencies.context);
    const int interval_ms =
        external ? runtime->config.periodic_check_ms
                 : runtime->config.battery_check_ms;
    const bool power_allowed =
        external ||
        runtime->dependencies.display_active(
            runtime->dependencies.context);
    if (!power_allowed ||
        runtime->dependencies.settings_active(
            runtime->dependencies.context) ||
        now_ms - runtime->last_check_ms < interval_ms) {
        return;
    }
    runtime->last_check_ms = now_ms;
    (void)vibe_ota_runtime_handle(
        runtime, VIBE_OTA_COMMAND_CHECK);
}

void vibe_ota_runtime_snapshot(
    const vibe_ota_runtime_t *runtime,
    vibe_ota_runtime_snapshot_t *snapshot)
{
    if (!runtime || !snapshot) {
        return;
    }
    snapshot->active = atomic_load(&runtime->active);
    snapshot->last_check_ms = runtime->last_check_ms;
}
