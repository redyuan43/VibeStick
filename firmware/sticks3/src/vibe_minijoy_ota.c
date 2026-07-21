#include "vibe_minijoy_ota.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "vibe_ota_policy.h"
#include "vibe_stick_config.h"

#define MINIJOY_OTA_BOARD "stickc_plus_minijoy_bt"
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define HTTP_TIMEOUT_MS 30000
#define OTA_NO_PROGRESS_TIMEOUT_MS 10000
#define OTA_BUFFER_BYTES 4096
#define FIRMWARE_BUILD_ID __DATE__ " " __TIME__

#ifndef VIBE_STICK_WIFI_PROFILES
#define VIBE_STICK_WIFI_PROFILES \
    { { VIBE_STICK_WIFI_SSID, VIBE_STICK_WIFI_PASSWORD } }
#endif

typedef struct {
    const char *ssid;
    const char *password;
} wifi_profile_t;

static const wifi_profile_t k_wifi_profiles[] = VIBE_STICK_WIFI_PROFILES;
static const char *TAG = "minijoy_ota";
static EventGroupHandle_t s_wifi_events;

static void report(vibe_minijoy_ota_status_fn callback, void *context,
                   vibe_minijoy_ota_status_t status)
{
    if (callback) {
        callback(status, context);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (!s_wifi_events) {
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
    }
}

static esp_err_t connect_wifi(void)
{
    bool has_profile = false;
    for (size_t i = 0;
         i < sizeof(k_wifi_profiles) / sizeof(k_wifi_profiles[0]); ++i) {
        if (k_wifi_profiles[i].ssid && k_wifi_profiles[i].ssid[0] != '\0') {
            has_profile = true;
            break;
        }
    }
    ESP_RETURN_ON_FALSE(has_profile, ESP_ERR_INVALID_STATE, TAG,
                        "no Wi-Fi profile configured");

    s_wifi_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_events != NULL, ESP_ERR_NO_MEM, TAG,
                        "Wi-Fi event group");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta() != NULL,
                        ESP_ERR_NO_MEM, TAG, "Wi-Fi netif");
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Wi-Fi init");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   wifi_event_handler, NULL),
        TAG, "Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   wifi_event_handler, NULL),
        TAG, "IP event handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "Wi-Fi station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");

    for (size_t i = 0;
         i < sizeof(k_wifi_profiles) / sizeof(k_wifi_profiles[0]); ++i) {
        const wifi_profile_t *profile = &k_wifi_profiles[i];
        if (!profile->ssid || profile->ssid[0] == '\0') {
            continue;
        }
        wifi_config_t config = {0};
        strlcpy((char *)config.sta.ssid, profile->ssid,
                sizeof(config.sta.ssid));
        strlcpy((char *)config.sta.password,
                profile->password ? profile->password : "",
                sizeof(config.sta.password));
        config.sta.threshold.authmode =
            config.sta.password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        xEventGroupClearBits(s_wifi_events,
                             WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG,
                            "Wi-Fi config");
        ESP_LOGI(TAG, "connecting Wi-Fi profile=%u ssid=%s",
                 (unsigned)i, profile->ssid);
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "Wi-Fi connect");
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT, pdTRUE,
            pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Wi-Fi connected ssid=%s", profile->ssid);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Wi-Fi profile failed ssid=%s", profile->ssid);
    }
    return ESP_ERR_TIMEOUT;
}

static void set_http_headers(esp_http_client_handle_t client)
{
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Name",
                               FIRMWARE_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Version",
                               FIRMWARE_VERSION);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Transport",
                               "OTA-MAINTENANCE");
    esp_http_client_set_header(client, "X-Vibe-Stick-Board",
                               MINIJOY_OTA_BOARD);
    if (VIBE_STICK_BRIDGE_TOKEN[0] != '\0') {
        esp_http_client_set_header(client, "X-Vibe-Stick-Token",
                                   VIBE_STICK_BRIDGE_TOKEN);
    }
}

static esp_err_t read_manifest(vibe_ota_manifest_t *manifest)
{
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d%s?board=%s",
             VIBE_STICK_BRIDGE_HOST, VIBE_STICK_BRIDGE_PORT,
             VIBE_STICK_OTA_MANIFEST_PATH, MINIJOY_OTA_BOARD);
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG,
                        "manifest HTTP client");
    set_http_headers(client);

    char response[1024] = {0};
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) != 200) {
            err = ESP_ERR_HTTP_FETCH_HEADER;
        }
    }
    int length = 0;
    if (err == ESP_OK) {
        length = esp_http_client_read_response(client, response,
                                               sizeof(response) - 1);
        if (length <= 0) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            response[length] = '\0';
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_RETURN_ON_ERROR(err, TAG, "manifest request");

    cJSON *root = cJSON_Parse(response);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG,
                        "manifest JSON");
    memset(manifest, 0, sizeof(*manifest));
    const cJSON *available = cJSON_GetObjectItemCaseSensitive(root, "available");
    const cJSON *board = cJSON_GetObjectItemCaseSensitive(root, "board");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *build_id = cJSON_GetObjectItemCaseSensitive(root, "build_id");
    const cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    const cJSON *elf_sha256 =
        cJSON_GetObjectItemCaseSensitive(root, "elf_sha256");
    const cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    manifest->available = cJSON_IsTrue(available);
    if (cJSON_IsString(board)) {
        strlcpy(manifest->board, board->valuestring, sizeof(manifest->board));
    }
    if (cJSON_IsString(version)) {
        strlcpy(manifest->version, version->valuestring,
                sizeof(manifest->version));
    }
    if (cJSON_IsString(build_id)) {
        strlcpy(manifest->build_id, build_id->valuestring,
                sizeof(manifest->build_id));
    }
    if (cJSON_IsString(sha256)) {
        strlcpy(manifest->sha256, sha256->valuestring,
                sizeof(manifest->sha256));
    }
    if (cJSON_IsString(elf_sha256)) {
        strlcpy(manifest->elf_sha256, elf_sha256->valuestring,
                sizeof(manifest->elf_sha256));
    }
    if (cJSON_IsString(url_item)) {
        strlcpy(manifest->url, url_item->valuestring, sizeof(manifest->url));
    }
    if (cJSON_IsNumber(size)) {
        manifest->size = size->valueint;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static void partition_sha256_hex(const esp_partition_t *partition,
                                 char output[65])
{
    uint8_t digest[32] = {0};
    output[0] = '\0';
    if (!partition ||
        esp_partition_get_sha256(partition, digest) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < sizeof(digest); ++i) {
        snprintf(output + i * 2, 65 - i * 2, "%02x", digest[i]);
    }
}

static vibe_ota_decision_t update_decision(const vibe_ota_manifest_t *manifest)
{
    char running_sha256[65];
    partition_sha256_hex(esp_ota_get_running_partition(), running_sha256);
    return vibe_ota_update_decision(
        manifest, MINIJOY_OTA_BOARD, FIRMWARE_VERSION, FIRMWARE_BUILD_ID,
        running_sha256, esp_app_get_elf_sha256_str());
}

static esp_err_t download_update(const vibe_ota_manifest_t *manifest)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(partition != NULL, ESP_ERR_NOT_FOUND, TAG,
                        "OTA partition");
    ESP_RETURN_ON_FALSE(manifest->size > 0 &&
                            manifest->size <= (int)partition->size,
                        ESP_ERR_INVALID_SIZE, TAG, "OTA image size");

    char path[128];
    if (manifest->url[0] != '\0') {
        strlcpy(path, manifest->url, sizeof(path));
    } else {
        snprintf(path, sizeof(path), "%s?board=%s", VIBE_STICK_OTA_BIN_PATH,
                 MINIJOY_OTA_BOARD);
    }
    char url[320];
    if (strncmp(path, "http://", 7) == 0 ||
        strncmp(path, "https://", 8) == 0) {
        strlcpy(url, path, sizeof(url));
    } else {
        snprintf(url, sizeof(url), "http://%s:%d%s", VIBE_STICK_BRIDGE_HOST,
                 VIBE_STICK_BRIDGE_PORT, path);
    }
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = OTA_BUFFER_BYTES,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG,
                        "OTA HTTP client");
    set_http_headers(client);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        int64_t content_length = esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) != 200) {
            err = ESP_ERR_HTTP_FETCH_HEADER;
        } else if (content_length > 0 && content_length != manifest->size) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    esp_ota_handle_t ota_handle = 0;
    if (err == ESP_OK) {
        err = esp_ota_begin(partition, manifest->size, &ota_handle);
    }
    uint8_t *buffer = NULL;
    if (err == ESP_OK) {
        buffer = malloc(OTA_BUFFER_BYTES);
        if (!buffer) err = ESP_ERR_NO_MEM;
    }
    int total = 0;
    int64_t last_progress_ms = esp_timer_get_time() / 1000;
    while (err == ESP_OK && total < manifest->size) {
        int remaining = manifest->size - total;
        int requested = remaining < OTA_BUFFER_BYTES ? remaining
                                                     : OTA_BUFFER_BYTES;
        int received = esp_http_client_read(client, (char *)buffer,
                                            requested);
        if (received < 0) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (received == 0) {
            int64_t current_ms = esp_timer_get_time() / 1000;
            if (esp_http_client_is_complete_data_received(client) ||
                current_ms - last_progress_ms >=
                    OTA_NO_PROGRESS_TIMEOUT_MS) {
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        err = esp_ota_write(ota_handle, buffer, (size_t)received);
        total += received;
        last_progress_ms = esp_timer_get_time() / 1000;
    }
    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && total != manifest->size) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = esp_ota_end(ota_handle);
    } else if (ota_handle != 0) {
        esp_ota_abort(ota_handle);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "OTA download");

    if (manifest->sha256[0] != '\0') {
        char downloaded_sha256[65];
        partition_sha256_hex(partition, downloaded_sha256);
        ESP_RETURN_ON_FALSE(downloaded_sha256[0] != '\0' &&
                                strcmp(downloaded_sha256,
                                       manifest->sha256) == 0,
                            ESP_ERR_INVALID_CRC, TAG, "OTA SHA-256 mismatch");
    }
    ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(partition), TAG,
                        "select OTA partition");
    ESP_LOGI(TAG, "OTA complete version=%s bytes=%d; restarting",
             manifest->version, total);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t vibe_minijoy_ota_run(vibe_minijoy_ota_status_fn status_callback,
                               void *context)
{
    report(status_callback, context, VIBE_MINIJOY_OTA_CONNECTING);
    esp_err_t err = connect_wifi();
    if (err != ESP_OK) {
        report(status_callback, context, VIBE_MINIJOY_OTA_FAILED);
        return err;
    }
    report(status_callback, context, VIBE_MINIJOY_OTA_CHECKING);
    vibe_ota_manifest_t manifest;
    err = read_manifest(&manifest);
    if (err != ESP_OK) {
        report(status_callback, context, VIBE_MINIJOY_OTA_FAILED);
        return err;
    }
    vibe_ota_decision_t decision = update_decision(&manifest);
    if (decision != VIBE_OTA_DECISION_UPDATE) {
        ESP_LOGI(TAG, "OTA not selected decision=%d version=%s",
                 (int)decision, manifest.version);
        report(status_callback, context, VIBE_MINIJOY_OTA_CURRENT);
        return ESP_OK;
    }
    report(status_callback, context, VIBE_MINIJOY_OTA_DOWNLOADING);
    err = download_update(&manifest);
    if (err != ESP_OK) {
        report(status_callback, context, VIBE_MINIJOY_OTA_FAILED);
    }
    return err;
}
