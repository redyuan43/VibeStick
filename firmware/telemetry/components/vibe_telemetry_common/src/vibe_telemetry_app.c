#include "vibe_telemetry.h"

#include "telemetry_board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>

esp_err_t vibe_telemetry_identity_load(vibe_telemetry_identity_t *identity);
esp_err_t vibe_telemetry_post_sample(const char *json);
esp_err_t vibe_telemetry_wifi_start(void);
bool vibe_telemetry_wifi_is_connected(void);

static const char *TAG = "telemetry_app";

void vibe_telemetry_app_main(const char *board_name)
{
    vibe_telemetry_identity_t identity = {0};
    ESP_ERROR_CHECK(vibe_telemetry_identity_load(&identity));
    ESP_ERROR_CHECK(telemetry_board_init());
    ESP_ERROR_CHECK(vibe_telemetry_wifi_start());

    uint32_t sequence = 0;
    while (true) {
        telemetry_battery_sample_t battery = {0};
        esp_err_t battery_err = telemetry_board_read_battery(&battery);
        if (battery_err != ESP_OK) {
            ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(battery_err));
        }

        vibe_telemetry_sample_t sample = {
            .board = board_name,
            .pmic = telemetry_board_pmic_name(),
            .device_id = identity.device_id,
            .boot_id = identity.boot_id,
            .sequence = sequence++,
            .uptime_ms = (uint64_t)(esp_timer_get_time() / 1000ULL),
            .wifi_connected = vibe_telemetry_wifi_is_connected(),
            .battery_voltage_mv_valid = battery.voltage_mv_valid,
            .battery_voltage_mv = battery.voltage_mv,
            .battery_percent_valid = battery.percent_valid,
            .battery_percent = battery.percent,
            .usb_voltage_mv_valid = battery.usb_voltage_mv_valid,
            .usb_voltage_mv = battery.usb_voltage_mv,
            .charging_valid = battery.charging_valid,
            .charging = battery.charging,
            .usb_powered_valid = battery.usb_powered_valid,
            .usb_powered = battery.usb_powered,
        };

        char json[512];
        if (vibe_telemetry_json_write(&sample, json, sizeof(json))) {
            ESP_LOGI(TAG, "sample seq=%" PRIu32 " %s", sample.sequence, json);
            if (sample.wifi_connected) {
                (void)vibe_telemetry_post_sample(json);
            }
        } else {
            ESP_LOGE(TAG, "failed to format telemetry JSON");
        }
        vTaskDelay(pdMS_TO_TICKS(VIBE_TELEMETRY_PERIOD_MS));
    }
}
