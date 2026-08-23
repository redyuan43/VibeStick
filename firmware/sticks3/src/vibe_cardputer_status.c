#include "vibe_cardputer_status.h"

#include <stdbool.h>
#include <string.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LCD_HOST SPI3_HOST
#define LCD_WIDTH 135
#define LCD_HEIGHT 240
#define LCD_X_GAP 52
#define LCD_Y_GAP 40
#define LCD_BAND_HEIGHT 48
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_PIN_MOSI GPIO_NUM_35
#define LCD_PIN_SCK GPIO_NUM_36
#define LCD_PIN_DC GPIO_NUM_34
#define LCD_PIN_CS GPIO_NUM_37
#define LCD_PIN_RST GPIO_NUM_33
#define LCD_PIN_BACKLIGHT GPIO_NUM_38
#define LCD_BACKLIGHT_TIMER LEDC_TIMER_3
#define LCD_BACKLIGHT_CHANNEL LEDC_CHANNEL_7
#define LCD_BACKLIGHT_RESOLUTION LEDC_TIMER_9_BIT
#define LCD_BACKLIGHT_HZ 256
#define TEXT_SCALE 4

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_transfer_done;
static bool s_ready;
static vibe_cardputer_status_t s_status;
static uint16_t s_pixels[LCD_WIDTH * LCD_BAND_HEIGHT];

static uint16_t swap_rgb565(uint16_t color)
{
    return (uint16_t)((color << 8) | (color >> 8));
}

static bool panel_transfer_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *event,
                                void *context)
{
    (void)io;
    (void)event;
    (void)context;
    BaseType_t wake_task = pdFALSE;
    xSemaphoreGiveFromISR(s_transfer_done, &wake_task);
    return wake_task == pdTRUE;
}

static esp_err_t draw_band(int y, int height)
{
    while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {
    }
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + height,
                                  s_pixels),
        "card_status", "draw status");
    return xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(250)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static const uint8_t *glyph(char character)
{
    static const uint8_t a[] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    static const uint8_t c[] = {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e};
    static const uint8_t d[] = {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e};
    static const uint8_t e[] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
    static const uint8_t f[] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10};
    static const uint8_t i[] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f};
    static const uint8_t n[] = {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11};
    static const uint8_t o[] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static const uint8_t r[] = {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11};
    static const uint8_t s[] = {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
    static const uint8_t w[] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a};
    static const uint8_t y[] = {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04};

    switch (character) {
    case 'A':
        return a;
    case 'C':
        return c;
    case 'D':
        return d;
    case 'E':
        return e;
    case 'F':
        return f;
    case 'I':
        return i;
    case 'N':
        return n;
    case 'O':
        return o;
    case 'R':
        return r;
    case 'S':
        return s;
    case 'W':
        return w;
    case 'Y':
        return y;
    default:
        return NULL;
    }
}

static void fill(uint16_t color)
{
    const uint16_t wire_color = swap_rgb565(color);
    for (size_t index = 0; index < sizeof(s_pixels) / sizeof(s_pixels[0]);
         ++index) {
        s_pixels[index] = wire_color;
    }
}

static void draw_text(const char *text, uint16_t color)
{
    const size_t length = strlen(text);
    const int glyph_width = 5 * TEXT_SCALE;
    const int advance = glyph_width + TEXT_SCALE;
    const int text_width = (int)length * advance - TEXT_SCALE;
    const int x_start = (LCD_WIDTH - text_width) / 2;
    const int y_start = (LCD_BAND_HEIGHT - 7 * TEXT_SCALE) / 2;
    const uint16_t wire_color = swap_rgb565(color);

    for (size_t letter = 0; letter < length; ++letter) {
        const uint8_t *rows = glyph(text[letter]);
        if (!rows) {
            continue;
        }
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((rows[row] & (1U << (4 - column))) == 0) {
                    continue;
                }
                const int pixel_x = x_start + (int)letter * advance +
                                    column * TEXT_SCALE;
                const int pixel_y = y_start + row * TEXT_SCALE;
                for (int y = pixel_y; y < pixel_y + TEXT_SCALE; ++y) {
                    for (int x = pixel_x; x < pixel_x + TEXT_SCALE; ++x) {
                        s_pixels[y * LCD_WIDTH + x] = wire_color;
                    }
                }
            }
        }
    }
}

static void status_style(vibe_cardputer_status_t status, const char **label,
                         uint16_t *background)
{
    switch (status) {
    case VIBE_CARDPUTER_STATUS_READY:
        *label = "READY";
        *background = 0x1565;
        return;
    case VIBE_CARDPUTER_STATUS_RECORDING:
        *label = "REC";
        *background = 0xb71c;
        return;
    case VIBE_CARDPUTER_STATUS_SENDING:
        *label = "SEND";
        *background = 0xb26a;
        return;
    case VIBE_CARDPUTER_STATUS_DONE:
        *label = "DONE";
        *background = 0x168a;
        return;
    case VIBE_CARDPUTER_STATUS_ERROR:
        *label = "ERROR";
        *background = 0x7f1d;
        return;
    case VIBE_CARDPUTER_STATUS_WIFI:
    default:
        *label = "WIFI";
        *background = 0x3747;
        return;
    }
}

static void render(vibe_cardputer_status_t status)
{
    const char *label = NULL;
    uint16_t background = 0;
    status_style(status, &label, &background);

    for (int y = 0; y < LCD_HEIGHT; y += LCD_BAND_HEIGHT) {
        int height = LCD_HEIGHT - y;
        if (height > LCD_BAND_HEIGHT) {
            height = LCD_BAND_HEIGHT;
        }
        fill(background);
        if (y == 96) {
            draw_text(label, 0xffff);
        }
        if (draw_band(y, height) != ESP_OK) {
            return;
        }
    }
}

static void init_backlight(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LCD_BACKLIGHT_TIMER,
        .duty_resolution = LCD_BACKLIGHT_RESOLUTION,
        .freq_hz = LCD_BACKLIGHT_HZ,
        .clk_cfg = LEDC_USE_XTAL_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const ledc_channel_config_t channel = {
        .gpio_num = LCD_PIN_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_BACKLIGHT_CHANNEL,
        .timer_sel = LCD_BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    const uint32_t brightness = 150;
    const uint32_t offset = (16U * 259U) >> 8;
    uint32_t duty = brightness * (257U - offset) + offset * 255U;
    duty = (duty + (1U << 6)) >> 7;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_CHANNEL,
                                  duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE,
                                     LCD_BACKLIGHT_CHANNEL));
}

esp_err_t vibe_cardputer_status_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_transfer_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_lock && s_transfer_done, ESP_ERR_NO_MEM,
                        "card_status", "display synchronization");
    init_backlight();

    const spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_SCK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = sizeof(s_pixels),
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
        "card_status", "display spi");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .on_color_trans_done = panel_transfer_done,
        .flags.sio_mode = true,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                 &io_config, &io),
        "card_status", "display io");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &s_panel),
                        "card_status", "display panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), "card_status",
                        "display reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), "card_status",
                        "display init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true),
                        "card_status", "display invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, LCD_Y_GAP),
                        "card_status", "display gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true),
                        "card_status", "display on");
    s_ready = true;
    s_status = VIBE_CARDPUTER_STATUS_ERROR;
    vibe_cardputer_status_show(VIBE_CARDPUTER_STATUS_WIFI);
    return ESP_OK;
}

void vibe_cardputer_status_show(vibe_cardputer_status_t status)
{
    if (!s_ready || status == s_status) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }
    render(status);
    s_status = status;
    xSemaphoreGive(s_lock);
}
