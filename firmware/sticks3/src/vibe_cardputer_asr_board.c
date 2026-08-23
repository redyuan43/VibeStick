#include "vibe_cardputer_asr_board.h"

#include "driver/gpio.h"
#include "esp_check.h"

#define CARDPUTER_I2C_PORT I2C_NUM_0
#define CARDPUTER_I2C_SDA GPIO_NUM_8
#define CARDPUTER_I2C_SCL GPIO_NUM_9

static i2c_master_bus_handle_t s_i2c_bus;

esp_err_t vibe_cardputer_asr_board_init(void)
{
    if (s_i2c_bus) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t config = {
        .i2c_port = CARDPUTER_I2C_PORT,
        .sda_io_num = CARDPUTER_I2C_SDA,
        .scl_io_num = CARDPUTER_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &s_i2c_bus),
                        "card_asr_board", "init shared i2c");
    return ESP_OK;
}

i2c_master_bus_handle_t vibe_cardputer_asr_i2c_bus(void)
{
    return s_i2c_bus;
}
