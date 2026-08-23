#pragma once

#include "esp_err.h"

esp_err_t vibe_cardputer_battery_init(void);
esp_err_t vibe_cardputer_battery_level(int *level_percent);
