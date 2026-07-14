#include "vibe_telemetry.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "telemetry_identity";

static void random_id(char out[VIBE_TELEMETRY_DEVICE_ID_LEN])
{
    uint8_t bytes[16];
    for (size_t i = 0; i < sizeof(bytes); i += 4) {
        uint32_t rnd = esp_random();
        memcpy(&bytes[i], &rnd, sizeof(rnd));
    }
    vibe_telemetry_make_hex_id(bytes, out);
}

static void fallback_device_id(char out[VIBE_TELEMETRY_DEVICE_ID_LEN])
{
    uint8_t mac[6] = {0};
    uint8_t bytes[16] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        memcpy(bytes, mac, sizeof(mac));
    } else {
        for (size_t i = 0; i < sizeof(bytes); ++i) {
            bytes[i] = (uint8_t)(esp_random() & 0xff);
        }
    }
    vibe_telemetry_make_hex_id(bytes, out);
}

esp_err_t vibe_telemetry_identity_load(vibe_telemetry_identity_t *identity)
{
    if (!identity) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs = 0;
    err = nvs_open("vibe_tel", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = VIBE_TELEMETRY_DEVICE_ID_LEN;
    err = nvs_get_str(nvs, "device_id", identity->device_id, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || len != VIBE_TELEMETRY_DEVICE_ID_LEN) {
        fallback_device_id(identity->device_id);
        err = nvs_set_str(nvs, "device_id", identity->device_id);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        ESP_LOGI(TAG, "created stable device_id %s", identity->device_id);
    }
    nvs_close(nvs);

    random_id(identity->boot_id);
    return err;
}
