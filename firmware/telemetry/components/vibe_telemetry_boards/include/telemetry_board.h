#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    bool voltage_mv_valid;
    int voltage_mv;
    bool percent_valid;
    int percent;
    bool usb_voltage_mv_valid;
    int usb_voltage_mv;
    bool charging_valid;
    bool charging;
    bool usb_powered_valid;
    bool usb_powered;
} telemetry_battery_sample_t;

typedef struct {
    int h_res;
    int v_res;
    int x_gap;
    int y_gap;
    int spi_host;
    int pin_mosi;
    int pin_sclk;
    int pin_cs;
    int pin_dc;
    int pin_rst;
    int pin_bl;
    bool backlight_active_high;
} telemetry_display_config_t;

esp_err_t telemetry_board_init(void);
esp_err_t telemetry_board_read_battery(telemetry_battery_sample_t *out);
const char *telemetry_board_pmic_name(void);
esp_err_t telemetry_display_init_white(const telemetry_display_config_t *config);
