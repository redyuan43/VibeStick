#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t vibe_cardputer_asr_board_init(void);
i2c_master_bus_handle_t vibe_cardputer_asr_i2c_bus(void);
