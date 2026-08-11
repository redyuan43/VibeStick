#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "bridge_nvs_cleanup";
static const char *BRIDGE_NAMESPACE = "vibe_bridge";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_handle_t handle;
    err = nvs_open(BRIDGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open namespace %s failed: %s",
                 BRIDGE_NAMESPACE, esp_err_to_name(err));
        return;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bridge NVS cleanup failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "CLEANUP_COMPLETE namespace=%s", BRIDGE_NAMESPACE);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
