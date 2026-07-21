#include "vibe_bt_status_ui.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "vibe_board.h"
#include "vibe_board_profile.h"

#define UI_STRIP_LINES 8
#define UI_IDLE_OFF_MS 10000
#define UI_SCALE 2

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_transfer_done;
static uint16_t s_strip[VIBE_BOARD_LCD_H_RES * UI_STRIP_LINES];
static bool s_display_on;
static bool s_minijoy_ready;
static vibe_bt_ui_status_t s_status = VIBE_BT_UI_WAITING;
static int64_t s_last_activity_ms;

static bool transfer_done_callback(esp_lcd_panel_io_handle_t panel_io,
                                   esp_lcd_panel_io_event_data_t *event,
                                   void *context)
{
    (void)panel_io;
    (void)event;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)context, &task_woken);
    return task_woken == pdTRUE;
}

static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    uint16_t value = ((uint16_t)(red & 0xf8) << 8) |
                     ((uint16_t)(green & 0xfc) << 3) | (blue >> 3);
    return (uint16_t)((value << 8) | (value >> 8));
}

static void wait_transfer(void)
{
    xSemaphoreTake(s_transfer_done, portMAX_DELAY);
}

static void fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (!s_panel || width <= 0 || height <= 0) {
        return;
    }
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > VIBE_BOARD_LCD_H_RES) {
        width = VIBE_BOARD_LCD_H_RES - x;
    }
    if (y + height > VIBE_BOARD_LCD_V_RES) {
        height = VIBE_BOARD_LCD_V_RES - y;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    for (int row = 0; row < height; row += UI_STRIP_LINES) {
        int lines = height - row;
        if (lines > UI_STRIP_LINES) {
            lines = UI_STRIP_LINES;
        }
        for (int i = 0; i < width * lines; ++i) {
            s_strip[i] = color;
        }
        if (esp_lcd_panel_draw_bitmap(s_panel, x, y + row, x + width,
                                      y + row + lines, s_strip) == ESP_OK) {
            wait_transfer();
        }
    }
}

static uint8_t glyph_column(char character, int column)
{
    static const struct {
        char character;
        uint8_t columns[5];
    } glyphs[] = {
        {'A', {0x7e, 0x11, 0x11, 0x11, 0x7e}},
        {'B', {0x7f, 0x49, 0x49, 0x49, 0x36}},
        {'C', {0x3e, 0x41, 0x41, 0x41, 0x22}},
        {'D', {0x7f, 0x41, 0x41, 0x22, 0x1c}},
        {'E', {0x7f, 0x49, 0x49, 0x49, 0x41}},
        {'F', {0x7f, 0x09, 0x09, 0x09, 0x01}},
        {'G', {0x3e, 0x41, 0x49, 0x49, 0x7a}},
        {'I', {0x00, 0x41, 0x7f, 0x41, 0x00}},
        {'J', {0x20, 0x40, 0x41, 0x3f, 0x01}},
        {'K', {0x7f, 0x08, 0x14, 0x22, 0x41}},
        {'L', {0x7f, 0x40, 0x40, 0x40, 0x40}},
        {'M', {0x7f, 0x02, 0x0c, 0x02, 0x7f}},
        {'N', {0x7f, 0x04, 0x08, 0x10, 0x7f}},
        {'O', {0x3e, 0x41, 0x41, 0x41, 0x3e}},
        {'P', {0x7f, 0x09, 0x09, 0x09, 0x06}},
        {'R', {0x7f, 0x09, 0x19, 0x29, 0x46}},
        {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
        {'T', {0x01, 0x01, 0x7f, 0x01, 0x01}},
        {'V', {0x1f, 0x20, 0x40, 0x20, 0x1f}},
        {'W', {0x3f, 0x40, 0x38, 0x40, 0x3f}},
        {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    };
    for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i) {
        if (glyphs[i].character == character) {
            return glyphs[i].columns[column];
        }
    }
    return 0;
}

static void draw_text(int x, int y, const char *text, uint16_t color)
{
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor == ' ') {
            x += 6 * UI_SCALE;
            continue;
        }
        for (int column = 0; column < 5; ++column) {
            uint8_t bits = glyph_column(*cursor, column);
            for (int row = 0; row < 7; ++row) {
                if (bits & (1u << row)) {
                    fill_rect(x + column * UI_SCALE, y + row * UI_SCALE,
                              UI_SCALE, UI_SCALE, color);
                }
            }
        }
        x += 6 * UI_SCALE;
    }
}

static void redraw(void)
{
    if (!s_panel || !s_display_on) {
        return;
    }
    uint16_t background = rgb565(7, 12, 18);
    uint16_t foreground = rgb565(225, 235, 242);
    uint16_t accent = rgb565(0, 190, 255);
    uint16_t warning = rgb565(255, 76, 76);
    fill_rect(0, 0, VIBE_BOARD_LCD_H_RES, VIBE_BOARD_LCD_V_RES, background);
    draw_text(11, 20, "VIBESTICK", foreground);
    const char *status_text = "WAIT";
    uint16_t status_color = accent;
    switch (s_status) {
    case VIBE_BT_UI_PAIRING:
        status_text = "PAIR";
        break;
    case VIBE_BT_UI_CONNECTED:
        status_text = "LINK";
        break;
    case VIBE_BT_UI_RECORDING:
        status_text = "REC";
        status_color = warning;
        break;
    case VIBE_BT_UI_ERROR:
        status_text = "ERROR";
        status_color = warning;
        break;
    case VIBE_BT_UI_WAITING:
    default:
        break;
    }
    draw_text(25, 90, status_text, status_color);
    draw_text(34, 155, s_minijoy_ready ? "JOY OK" : "JOY OFF",
              s_minijoy_ready ? foreground : warning);
}

esp_err_t vibe_bt_status_ui_init(void)
{
    s_transfer_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_transfer_done != NULL, ESP_ERR_NO_MEM, "bt_ui",
                        "transfer semaphore");
    ESP_RETURN_ON_ERROR(vibe_board_set_lcd_brightness(
                            VIBE_BOARD_LCD_BACKLIGHT_DEFAULT),
                        "bt_ui", "backlight");
    spi_bus_config_t bus_config = {
        .sclk_io_num = VIBE_BOARD_PIN_LCD_SCK,
        .mosi_io_num = VIBE_BOARD_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(s_strip),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(VIBE_BOARD_LCD_HOST, &bus_config,
                                            SPI_DMA_CH_AUTO),
                        "bt_ui", "SPI bus");
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = VIBE_BOARD_PIN_LCD_DC,
        .cs_gpio_num = VIBE_BOARD_PIN_LCD_CS,
        .pclk_hz = VIBE_BOARD_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .on_color_trans_done = transfer_done_callback,
        .user_ctx = s_transfer_done,
        .flags.sio_mode = VIBE_BOARD_LCD_SPI_SIO_MODE,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                            (esp_lcd_spi_bus_handle_t)VIBE_BOARD_LCD_HOST,
                            &io_config, &io),
                        "bt_ui", "panel IO");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = VIBE_BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &s_panel),
                        "bt_ui", "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), "bt_ui", "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), "bt_ui", "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), "bt_ui",
                        "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(
                            s_panel, VIBE_BOARD_LCD_X_GAP,
                            VIBE_BOARD_LCD_Y_GAP),
                        "bt_ui", "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), "bt_ui",
                        "display on");
    s_display_on = true;
    s_last_activity_ms = esp_timer_get_time() / 1000;
    redraw();
    return ESP_OK;
}

void vibe_bt_status_ui_set(vibe_bt_ui_status_t status, bool minijoy_ready)
{
    bool changed = status != s_status || minijoy_ready != s_minijoy_ready;
    s_status = status;
    s_minijoy_ready = minijoy_ready;
    if (changed) {
        s_last_activity_ms = esp_timer_get_time() / 1000;
        if (!s_display_on && s_panel) {
            vibe_board_set_lcd_brightness(VIBE_BOARD_LCD_BACKLIGHT_DEFAULT);
            esp_lcd_panel_disp_on_off(s_panel, true);
            s_display_on = true;
        }
        redraw();
    }
}

void vibe_bt_status_ui_activity(void)
{
    s_last_activity_ms = esp_timer_get_time() / 1000;
    if (!s_display_on && s_panel) {
        vibe_board_set_lcd_brightness(VIBE_BOARD_LCD_BACKLIGHT_DEFAULT);
        esp_lcd_panel_disp_on_off(s_panel, true);
        s_display_on = true;
        redraw();
    }
}

void vibe_bt_status_ui_tick(int64_t now_ms)
{
    if (s_display_on && now_ms - s_last_activity_ms >= UI_IDLE_OFF_MS) {
        vibe_board_set_lcd_brightness(0);
        esp_lcd_panel_disp_on_off(s_panel, false);
        s_display_on = false;
    }
}
