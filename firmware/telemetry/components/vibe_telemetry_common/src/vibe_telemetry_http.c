#include "vibe_telemetry.h"
#include "vibe_telemetry_config.h"

#include "esp_http_client.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "telemetry_http";

esp_err_t vibe_telemetry_post_sample(const char *json)
{
    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[256];
    int written = snprintf(url, sizeof(url), "%s%s", VIBE_TELEMETRY_BASE_URL, VIBE_TELEMETRY_ENDPOINT_PATH);
    if (written < 0 || written >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Name", VIBE_TELEMETRY_FIRMWARE_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Version", VIBE_TELEMETRY_FIRMWARE_VERSION);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Transport", "HTTP");
    esp_http_client_set_header(client, "X-Vibe-Stick-Token", VIBE_TELEMETRY_SHARED_SECRET);
    esp_http_client_set_post_field(client, json, (int)strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && (status < 200 || status > 299)) {
        ESP_LOGW(TAG, "telemetry POST returned HTTP %d", status);
        err = ESP_FAIL;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "telemetry POST failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}
