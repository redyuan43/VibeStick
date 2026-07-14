#include "vibe_telemetry.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_hex_id(void)
{
    uint8_t bytes[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    char id[VIBE_TELEMETRY_DEVICE_ID_LEN];
    vibe_telemetry_make_hex_id(bytes, id);
    assert(strcmp(id, "00112233445566778899aabbccddeeff") == 0);
}

static void test_json_with_values(void)
{
    vibe_telemetry_sample_t sample = {
        .board = "sticks3",
        .pmic = "m5pm1",
        .device_id = "device",
        .boot_id = "boot",
        .sequence = 7,
        .uptime_ms = 12345,
        .wifi_connected = true,
        .battery_voltage_mv_valid = true,
        .battery_voltage_mv = 4012,
        .battery_percent_valid = true,
        .battery_percent = 79,
        .usb_voltage_mv_valid = true,
        .usb_voltage_mv = 5021,
        .charging_valid = true,
        .charging = false,
        .usb_powered_valid = true,
        .usb_powered = true,
    };
    char json[512];
    assert(vibe_telemetry_json_write(&sample, json, sizeof(json)));
    assert(strstr(json, "\"schema_version\":1") != NULL);
    assert(strstr(json, "\"sequence\":7") != NULL);
    assert(strstr(json, "\"battery_mv\":4012") != NULL);
    assert(strstr(json, "\"battery_percent\":79") != NULL);
    assert(strstr(json, "\"usb_mv\":5021") != NULL);
    assert(strstr(json, "\"charging\":false") != NULL);
    assert(strstr(json, "\"usb_powered\":true") != NULL);
}

static void test_json_nulls_on_read_failure(void)
{
    vibe_telemetry_sample_t sample = {
        .board = "stickc_plus_11",
        .pmic = "axp192",
        .device_id = "device",
        .boot_id = "boot",
        .sequence = 8,
        .uptime_ms = 5000,
        .wifi_connected = false,
    };
    char json[512];
    assert(vibe_telemetry_json_write(&sample, json, sizeof(json)));
    assert(strstr(json, "\"battery_mv\":null") != NULL);
    assert(strstr(json, "\"battery_percent\":null") != NULL);
    assert(strstr(json, "\"usb_mv\":null") != NULL);
    assert(strstr(json, "\"charging\":null") != NULL);
    assert(strstr(json, "\"usb_powered\":null") != NULL);
}

int main(void)
{
    test_hex_id();
    test_json_with_values();
    test_json_nulls_on_read_failure();
    puts("telemetry common tests passed");
    return 0;
}
