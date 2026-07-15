#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    bool available;
    uint8_t wake_source;
    uint8_t irq_status_gpio;
    uint8_t irq_status_power;
    uint8_t irq_status_button;
    uint8_t timer_config;
    uint32_t timer_seconds;
} vibe_board_boot_power_status_t;

esp_err_t vibe_board_init_power(void);
vibe_board_boot_power_status_t vibe_board_boot_power_status(void);
i2c_master_bus_handle_t vibe_board_i2c_bus(void);
esp_err_t vibe_board_battery_voltage_mv(int *voltage_mv);
esp_err_t vibe_board_battery_level(int *level_percent);
esp_err_t vibe_board_battery_charging(bool *charging);
esp_err_t vibe_board_usb_powered(bool *usb_powered);
esp_err_t vibe_board_speaker_set_enabled(bool enabled);
esp_err_t vibe_board_set_lcd_brightness(uint8_t brightness);
