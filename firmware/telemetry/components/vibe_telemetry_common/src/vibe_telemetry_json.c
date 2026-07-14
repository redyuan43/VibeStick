#include "vibe_telemetry.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool append_field(char **cursor, size_t *remaining, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(*cursor, *remaining, fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= *remaining) {
        return false;
    }
    *cursor += written;
    *remaining -= (size_t)written;
    return true;
}

static const char *json_bool(bool value)
{
    return value ? "true" : "false";
}

bool vibe_telemetry_json_write(const vibe_telemetry_sample_t *sample, char *out, size_t out_size)
{
    if (!sample || !out || out_size == 0 || !sample->board || !sample->pmic ||
        !sample->device_id || !sample->boot_id) {
        return false;
    }

    char *cursor = out;
    size_t remaining = out_size;
    bool ok = true;

    ok = ok && append_field(&cursor, &remaining,
        "{\"schema_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"board\":\"%s\",\"pmic\":\"%s\","
        "\"firmware_version\":\"" VIBE_TELEMETRY_FIRMWARE_VERSION "\","
        "\"sequence\":%lu,\"uptime_ms\":%llu,\"sample_interval_ms\":%u,"
        "\"wifi_connected\":%s,\"battery_mv\":",
        sample->device_id,
        sample->boot_id,
        sample->board,
        sample->pmic,
        (unsigned long)sample->sequence,
        (unsigned long long)sample->uptime_ms,
        (unsigned int)VIBE_TELEMETRY_PERIOD_MS,
        json_bool(sample->wifi_connected));

    if (sample->battery_voltage_mv_valid) {
        ok = ok && append_field(&cursor, &remaining, "%d", sample->battery_voltage_mv);
    } else {
        ok = ok && append_field(&cursor, &remaining, "null");
    }

    ok = ok && append_field(&cursor, &remaining, ",\"battery_percent\":");
    if (sample->battery_percent_valid) {
        ok = ok && append_field(&cursor, &remaining, "%d", sample->battery_percent);
    } else {
        ok = ok && append_field(&cursor, &remaining, "null");
    }

    ok = ok && append_field(&cursor, &remaining, ",\"usb_mv\":");
    if (sample->usb_voltage_mv_valid) {
        ok = ok && append_field(&cursor, &remaining, "%d", sample->usb_voltage_mv);
    } else {
        ok = ok && append_field(&cursor, &remaining, "null");
    }

    ok = ok && append_field(&cursor, &remaining, ",\"charging\":");
    if (sample->charging_valid) {
        ok = ok && append_field(&cursor, &remaining, "%s", json_bool(sample->charging));
    } else {
        ok = ok && append_field(&cursor, &remaining, "null");
    }

    ok = ok && append_field(&cursor, &remaining, ",\"usb_powered\":");
    if (sample->usb_powered_valid) {
        ok = ok && append_field(&cursor, &remaining, "%s", json_bool(sample->usb_powered));
    } else {
        ok = ok && append_field(&cursor, &remaining, "null");
    }

    ok = ok && append_field(&cursor, &remaining, "}");
    if (!ok && out_size > 0) {
        out[0] = '\0';
    }
    return ok;
}

void vibe_telemetry_make_hex_id(uint8_t bytes[16], char out[VIBE_TELEMETRY_DEVICE_ID_LEN])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) {
        out[i * 2] = hex[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[32] = '\0';
}
