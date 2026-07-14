#include "telemetry_board.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "telemetry_display";

esp_err_t telemetry_display_init_white(const telemetry_display_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "missing display config");

    if (config->pin_bl >= 0) {
        gpio_config_t bl = {
            .pin_bit_mask = 1ULL << config->pin_bl,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight gpio");
        gpio_set_level(config->pin_bl, config->backlight_active_high ? 1 : 0);
    }

    spi_bus_config_t bus = {
        .mosi_io_num = config->pin_mosi,
        .miso_io_num = -1,
        .sclk_io_num = config->pin_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = config->h_res * 40 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(config->spi_host, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = config->pin_dc,
        .cs_gpio_num = config->pin_cs,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)config->spi_host, &io_config, &io), TAG, "panel io");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = config->pin_rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &panel), TAG, "st7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, config->x_gap, config->y_gap), TAG, "panel gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "display on");

    const int lines = 20;
    size_t pixels = (size_t)config->h_res * lines;
    uint16_t *white = heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(white, ESP_ERR_NO_MEM, TAG, "white buffer");
    for (size_t i = 0; i < pixels; ++i) {
        white[i] = 0xffff;
    }
    for (int y = 0; y < config->v_res; y += lines) {
        int y_end = y + lines;
        if (y_end > config->v_res) {
            y_end = config->v_res;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, config->h_res, y_end, white));
    }
    free(white);
    ESP_LOGI(TAG, "screen set to fixed always-on white profile");
    return ESP_OK;
}
