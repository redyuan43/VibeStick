#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIBE_TELEMETRY_PERIOD_MS 5000U
#define VIBE_TELEMETRY_ENDPOINT_PATH "/telemetry/v1/samples"
#define VIBE_TELEMETRY_FIRMWARE_NAME "vibestick-battery-telemetry"
#define VIBE_TELEMETRY_FIRMWARE_VERSION "0.1.0"
#define VIBE_TELEMETRY_DEVICE_ID_LEN 33
#define VIBE_TELEMETRY_BOOT_ID_LEN 33

typedef struct {
    char device_id[VIBE_TELEMETRY_DEVICE_ID_LEN];
    char boot_id[VIBE_TELEMETRY_BOOT_ID_LEN];
} vibe_telemetry_identity_t;

typedef struct {
    const char *board;
    const char *pmic;
    const char *device_id;
    const char *boot_id;
    uint32_t sequence;
    uint64_t uptime_ms;
    bool wifi_connected;
    bool battery_voltage_mv_valid;
    int battery_voltage_mv;
    bool battery_percent_valid;
    int battery_percent;
    bool usb_voltage_mv_valid;
    int usb_voltage_mv;
    bool charging_valid;
    bool charging;
    bool usb_powered_valid;
    bool usb_powered;
} vibe_telemetry_sample_t;

void vibe_telemetry_app_main(const char *board_name);
bool vibe_telemetry_json_write(const vibe_telemetry_sample_t *sample, char *out, size_t out_size);
void vibe_telemetry_make_hex_id(uint8_t bytes[16], char out[VIBE_TELEMETRY_DEVICE_ID_LEN]);
