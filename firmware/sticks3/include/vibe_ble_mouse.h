#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t vibe_ble_mouse_start(void);
void vibe_ble_mouse_stop(void);
bool vibe_ble_mouse_connected(void);
esp_err_t vibe_ble_mouse_report(int8_t dx, int8_t dy, bool left_pressed);
