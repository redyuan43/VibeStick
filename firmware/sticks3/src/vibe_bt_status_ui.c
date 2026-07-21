#include "vibe_bt_status_ui.h"

#include <stdio.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "vibe_board.h"
#include "vibe_board_profile.h"
#include "vibe_minijoy_pet_assets.h"

#define UI_STRIP_LINES 8
#define UI_IDLE_OFF_MS 30000
#define UI_POWER_POLL_MS 2000
#define UI_WAVE_REFRESH_MS 100
#define UI_PET_BLINK_HOLD_MS 180
#define UI_PET_BLINK_MIN_MS 4000
#define UI_PET_BLINK_RANGE_MS 4001
#define UI_PET_HAPPY_MS 900
#define UI_BATTERY_SAMPLE_COUNT 5
#define UI_PET_X ((VIBE_BOARD_LCD_H_RES - VIBE_MINIJOY_PET_WIDTH) / 2)
#define UI_PET_Y 54
#define UI_WAVE_X 24
#define UI_WAVE_Y 54
#define UI_WAVE_WIDTH 87
#define UI_WAVE_HEIGHT 80

static const char *TAG = "bt_ui";
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_transfer_done;
static uint16_t s_strip[VIBE_BOARD_LCD_H_RES * UI_STRIP_LINES];
static bool s_display_on;
static bool s_minijoy_ready;
static bool s_confirm_window;
static vibe_bt_ui_status_t s_status = VIBE_BT_UI_WAITING;
static int64_t s_last_activity_ms;
static int64_t s_last_power_poll_ms;
static int64_t s_last_wave_refresh_ms;
static int64_t s_pet_happy_until_ms;
static int64_t s_pet_blink_until_ms;
static int64_t s_pet_next_blink_ms;
static vibe_minijoy_pet_frame_id_t s_pet_blink_frame =
    VIBE_MINIJOY_PET_FRAME_BLINK_BOTH;
static vibe_minijoy_pet_frame_id_t s_pet_current_frame =
    VIBE_MINIJOY_PET_FRAME_COUNT;
static uint8_t s_wave_phase;
static int s_battery_samples[UI_BATTERY_SAMPLE_COUNT];
static size_t s_battery_sample_count;
static size_t s_battery_sample_index;
static bool s_battery_valid;
static bool s_battery_charging;
static bool s_usb_powered;
static bool s_power_error_logged;
static int s_battery_display;

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

static uint16_t color_background(void)
{
    return rgb565(5, 6, 8);
}

static uint16_t color_foreground(void)
{
    return rgb565(225, 235, 242);
}

static uint16_t color_accent(void)
{
    return rgb565(0, 190, 255);
}

static uint16_t color_warning(void)
{
    return rgb565(255, 76, 76);
}

static void wait_transfer(void)
{
    xSemaphoreTake(s_transfer_done, portMAX_DELAY);
}

static bool draw_bitmap(int x, int y, int width, int height,
                        const uint16_t *pixels)
{
    if (esp_lcd_panel_draw_bitmap(s_panel, x, y, x + width, y + height,
                                  pixels) != ESP_OK) {
        return false;
    }
    wait_transfer();
    return true;
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
        (void)draw_bitmap(x, y + row, width, lines, s_strip);
    }
}

static uint8_t glyph_column(char character, int column)
{
    static const struct {
        char character;
        uint8_t columns[5];
    } glyphs[] = {
        {'0', {0x3e, 0x51, 0x49, 0x45, 0x3e}},
        {'1', {0x00, 0x42, 0x7f, 0x40, 0x00}},
        {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
        {'3', {0x21, 0x41, 0x45, 0x4b, 0x31}},
        {'4', {0x18, 0x14, 0x12, 0x7f, 0x10}},
        {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
        {'6', {0x3c, 0x4a, 0x49, 0x49, 0x30}},
        {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
        {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
        {'9', {0x06, 0x49, 0x49, 0x29, 0x1e}},
        {'%', {0x63, 0x13, 0x08, 0x64, 0x63}},
        {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
        {'A', {0x7e, 0x11, 0x11, 0x11, 0x7e}},
        {'B', {0x7f, 0x49, 0x49, 0x49, 0x36}},
        {'C', {0x3e, 0x41, 0x41, 0x41, 0x22}},
        {'D', {0x7f, 0x41, 0x41, 0x22, 0x1c}},
        {'E', {0x7f, 0x49, 0x49, 0x49, 0x41}},
        {'F', {0x7f, 0x09, 0x09, 0x09, 0x01}},
        {'G', {0x3e, 0x41, 0x49, 0x49, 0x7a}},
        {'H', {0x7f, 0x08, 0x08, 0x08, 0x7f}},
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
        {'U', {0x3f, 0x40, 0x40, 0x40, 0x3f}},
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

static int text_width(const char *text, int scale)
{
    size_t length = strlen(text);
    return length == 0 ? 0 : (int)(length * 6 * scale - scale);
}

static void draw_text(int x, int y, const char *text, uint16_t color,
                      int scale)
{
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor != ' ') {
            for (int column = 0; column < 5; ++column) {
                uint8_t bits = glyph_column(*cursor, column);
                for (int row = 0; row < 7; ++row) {
                    if (bits & (1u << row)) {
                        fill_rect(x + column * scale, y + row * scale,
                                  scale, scale, color);
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

static void draw_text_centered(int y, const char *text, uint16_t color,
                               int scale)
{
    draw_text((VIBE_BOARD_LCD_H_RES - text_width(text, scale)) / 2, y,
              text, color, scale);
}

static const char *status_text(void)
{
    switch (s_status) {
    case VIBE_BT_UI_PAIRING:
        return "PAIR";
    case VIBE_BT_UI_CONNECTED:
        return "LINK";
    case VIBE_BT_UI_RECORDING:
        return "REC";
    case VIBE_BT_UI_ERROR:
        return "ERROR";
    case VIBE_BT_UI_WAITING:
    default:
        return "WAIT";
    }
}

static uint16_t status_color(void)
{
    return (s_status == VIBE_BT_UI_RECORDING ||
            s_status == VIBE_BT_UI_ERROR)
               ? color_warning()
               : color_accent();
}

static uint16_t battery_color(void)
{
    if (!s_battery_valid || s_battery_display < 20) {
        return color_warning();
    }
    if (s_battery_display < 50) {
        return rgb565(255, 196, 64);
    }
    return rgb565(76, 220, 128);
}

static void draw_battery_icon(int x, int y)
{
    uint16_t color = battery_color();
    fill_rect(x, y, 16, 1, color);
    fill_rect(x, y + 9, 16, 1, color);
    fill_rect(x, y, 1, 10, color);
    fill_rect(x + 15, y, 1, 10, color);
    fill_rect(x + 16, y + 3, 2, 4, color);
    if (s_battery_valid && s_battery_display > 0) {
        int fill_width = (s_battery_display * 12 + 99) / 100;
        fill_rect(x + 2, y + 2, fill_width, 6, color);
    }
    if (s_battery_charging || s_usb_powered) {
        uint16_t bolt = color_foreground();
        fill_rect(x + 8, y + 1, 2, 3, bolt);
        fill_rect(x + 6, y + 4, 4, 2, bolt);
        fill_rect(x + 6, y + 6, 2, 3, bolt);
    }
}

static void draw_top_bar(void)
{
    fill_rect(0, 0, VIBE_BOARD_LCD_H_RES, 36, color_background());
    draw_text(8, 13, status_text(), status_color(), 1);

    char level_text[5] = "--%";
    if (s_battery_valid) {
        snprintf(level_text, sizeof(level_text), "%d%%", s_battery_display);
    }
    int level_width = text_width(level_text, 1);
    int level_x = VIBE_BOARD_LCD_H_RES - 7 - level_width;
    draw_battery_icon(level_x - 22, 11);
    draw_text(level_x, 13, level_text, battery_color(), 1);
    fill_rect(8, 34, VIBE_BOARD_LCD_H_RES - 16, 1, rgb565(24, 42, 54));
}

static void draw_bottom_bar(void)
{
    fill_rect(0, 151, VIBE_BOARD_LCD_H_RES,
              VIBE_BOARD_LCD_V_RES - 151, color_background());
    const char *joy_text = s_status == VIBE_BT_UI_RECORDING
                               ? "JOY MIC"
                               : (s_minijoy_ready ? "JOY OK" : "JOY OFF");
    draw_text_centered(166, joy_text,
                       s_minijoy_ready || s_status == VIBE_BT_UI_RECORDING
                           ? color_foreground()
                           : color_warning(),
                       1);
    const char *action_text = s_status == VIBE_BT_UI_RECORDING
                                  ? "MIC LIVE"
                                  : (s_confirm_window ? "ENTER" : "A MIC");
    draw_text_centered(198, action_text,
                       s_status == VIBE_BT_UI_RECORDING ? color_warning()
                                                        : color_accent(),
                       2);
}

typedef struct {
    size_t strip_pixels;
    int row;
} pet_stream_t;

static bool write_pet_pixel(pet_stream_t *stream, uint8_t lo, uint8_t hi)
{
    s_strip[stream->strip_pixels++] = ((uint16_t)lo << 8) | hi;
    if (stream->strip_pixels == VIBE_MINIJOY_PET_WIDTH * UI_STRIP_LINES) {
        if (!draw_bitmap(UI_PET_X, UI_PET_Y + stream->row,
                         VIBE_MINIJOY_PET_WIDTH, UI_STRIP_LINES, s_strip)) {
            return false;
        }
        stream->row += UI_STRIP_LINES;
        stream->strip_pixels = 0;
    }
    return true;
}

static bool draw_pet_frame(vibe_minijoy_pet_frame_id_t frame_id)
{
    const vibe_minijoy_pet_rle_frame_t *frame =
        vibe_minijoy_pet_frame(frame_id);
    if (!frame) {
        return false;
    }

    pet_stream_t stream = {0};
    size_t input = 0;
    size_t pixels = 0;
    while (input < frame->size &&
           pixels < VIBE_MINIJOY_PET_WIDTH * VIBE_MINIJOY_PET_HEIGHT) {
        uint8_t control = frame->data[input++];
        size_t count = (control & 0x7f) + 1;
        if (pixels + count >
            VIBE_MINIJOY_PET_WIDTH * VIBE_MINIJOY_PET_HEIGHT) {
            return false;
        }
        if (control & 0x80) {
            if (input + 2 > frame->size) {
                return false;
            }
            uint8_t lo = frame->data[input++];
            uint8_t hi = frame->data[input++];
            for (size_t i = 0; i < count; ++i) {
                if (!write_pet_pixel(&stream, lo, hi)) {
                    return false;
                }
                pixels++;
            }
        } else {
            if (input + count * 2 > frame->size) {
                return false;
            }
            for (size_t i = 0; i < count; ++i) {
                uint8_t lo = frame->data[input++];
                uint8_t hi = frame->data[input++];
                if (!write_pet_pixel(&stream, lo, hi)) {
                    return false;
                }
                pixels++;
            }
        }
    }
    bool complete = pixels == VIBE_MINIJOY_PET_WIDTH * VIBE_MINIJOY_PET_HEIGHT &&
                    stream.strip_pixels == 0;
    if (!complete) {
        ESP_LOGW(TAG, "pet frame decode failed id=%d pixels=%u",
                 (int)frame_id, (unsigned)pixels);
    }
    return complete;
}

static void schedule_next_blink(int64_t now_ms)
{
    s_pet_next_blink_ms =
        now_ms + UI_PET_BLINK_MIN_MS +
        (int64_t)(esp_random() % UI_PET_BLINK_RANGE_MS);
}

static vibe_minijoy_pet_frame_id_t pet_frame_for_status(int64_t now_ms)
{
    if (s_status == VIBE_BT_UI_ERROR) {
        return VIBE_MINIJOY_PET_FRAME_ERROR;
    }
    if (s_status != VIBE_BT_UI_CONNECTED) {
        return VIBE_MINIJOY_PET_FRAME_ATTENTION;
    }
    if (now_ms < s_pet_happy_until_ms) {
        return VIBE_MINIJOY_PET_FRAME_HAPPY;
    }
    if (now_ms < s_pet_blink_until_ms) {
        return s_pet_blink_frame;
    }
    if (s_pet_next_blink_ms == 0) {
        schedule_next_blink(now_ms);
    } else if (now_ms >= s_pet_next_blink_ms) {
        static const vibe_minijoy_pet_frame_id_t blink_frames[] = {
            VIBE_MINIJOY_PET_FRAME_BLINK_LEFT,
            VIBE_MINIJOY_PET_FRAME_BLINK_RIGHT,
            VIBE_MINIJOY_PET_FRAME_BLINK_BOTH,
        };
        s_pet_blink_frame = blink_frames[esp_random() % 3];
        s_pet_blink_until_ms = now_ms + UI_PET_BLINK_HOLD_MS;
        schedule_next_blink(now_ms);
        return s_pet_blink_frame;
    }
    return VIBE_MINIJOY_PET_FRAME_IDLE;
}

static void update_pet(int64_t now_ms)
{
    if (!s_display_on || s_status == VIBE_BT_UI_RECORDING) {
        return;
    }
    vibe_minijoy_pet_frame_id_t frame = pet_frame_for_status(now_ms);
    if (frame == s_pet_current_frame) {
        return;
    }
    fill_rect(UI_PET_X, UI_PET_Y, VIBE_MINIJOY_PET_WIDTH,
              VIBE_MINIJOY_PET_HEIGHT, color_background());
    if (draw_pet_frame(frame)) {
        s_pet_current_frame = frame;
    }
}

static void draw_waveform(uint8_t level)
{
    fill_rect(UI_WAVE_X, UI_WAVE_Y, UI_WAVE_WIDTH, UI_WAVE_HEIGHT,
              color_background());
    const int offsets[] = {-8, 3, 10, -2, 6};
    const int bar_width = 9;
    const int gap = 7;
    const int total_width = 5 * bar_width + 4 * gap;
    const int start_x = UI_WAVE_X + (UI_WAVE_WIDTH - total_width) / 2;
    int base_height = 8 + level * 56 / 100;
    for (int i = 0; i < 5; ++i) {
        int motion = ((s_wave_phase + i * 2) % 5 - 2) * 2;
        int height = base_height + offsets[i] + motion;
        if (height < 6) {
            height = 6;
        } else if (height > 68) {
            height = 68;
        }
        int y = UI_WAVE_Y + (UI_WAVE_HEIGHT - height) / 2;
        fill_rect(start_x + i * (bar_width + gap), y, bar_width, height,
                  i == 2 ? color_warning() : color_accent());
    }
    s_wave_phase = (s_wave_phase + 1) % 5;
}

static int median_battery_sample(void)
{
    int samples[UI_BATTERY_SAMPLE_COUNT];
    memcpy(samples, s_battery_samples,
           s_battery_sample_count * sizeof(samples[0]));
    for (size_t i = 1; i < s_battery_sample_count; ++i) {
        int value = samples[i];
        size_t j = i;
        while (j > 0 && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            j--;
        }
        samples[j] = value;
    }
    return samples[s_battery_sample_count / 2];
}

static void update_battery_level(int raw_level)
{
    if (raw_level < 0) {
        raw_level = 0;
    } else if (raw_level > 100) {
        raw_level = 100;
    }
    s_battery_samples[s_battery_sample_index] = raw_level;
    s_battery_sample_index =
        (s_battery_sample_index + 1) % UI_BATTERY_SAMPLE_COUNT;
    if (s_battery_sample_count < UI_BATTERY_SAMPLE_COUNT) {
        s_battery_sample_count++;
    }
    int target = median_battery_sample();
    if (!s_battery_valid) {
        s_battery_display = target;
        s_battery_valid = true;
    } else if (target > s_battery_display) {
        s_battery_display++;
    } else if (target < s_battery_display) {
        s_battery_display--;
    }
}

static void refresh_power_status(int64_t now_ms, bool force_draw)
{
    int old_level = s_battery_display;
    bool old_valid = s_battery_valid;
    bool old_charging = s_battery_charging;
    bool old_usb_powered = s_usb_powered;
    int raw_level = 0;
    int voltage_mv = -1;
    bool charging = false;
    bool usb_powered = false;
    esp_err_t level_err = vibe_board_battery_level(&raw_level);
    if (level_err == ESP_OK) {
        update_battery_level(raw_level);
        s_power_error_logged = false;
    } else if (!s_power_error_logged) {
        ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(level_err));
        s_power_error_logged = true;
    }
    if (vibe_board_battery_voltage_mv(&voltage_mv) != ESP_OK) {
        voltage_mv = -1;
    }
    if (vibe_board_battery_charging(&charging) == ESP_OK) {
        s_battery_charging = charging;
    }
    if (vibe_board_usb_powered(&usb_powered) == ESP_OK) {
        s_usb_powered = usb_powered;
    }
    bool changed = old_valid != s_battery_valid ||
                   old_level != s_battery_display ||
                   old_charging != s_battery_charging ||
                   old_usb_powered != s_usb_powered;
    if (changed) {
        ESP_LOGI(TAG,
                 "power battery_raw=%d battery_display=%d battery_mv=%d charging=%d usb=%d",
                 level_err == ESP_OK ? raw_level : -1,
                 s_battery_valid ? s_battery_display : -1, voltage_mv,
                 s_battery_charging, s_usb_powered);
    }
    s_last_power_poll_ms = now_ms;
    if (s_display_on && (force_draw || changed)) {
        draw_top_bar();
    }
}

static void redraw_full(int64_t now_ms, uint8_t audio_level)
{
    if (!s_panel || !s_display_on) {
        return;
    }
    fill_rect(0, 0, VIBE_BOARD_LCD_H_RES, VIBE_BOARD_LCD_V_RES,
              color_background());
    draw_top_bar();
    draw_bottom_bar();
    s_pet_current_frame = VIBE_MINIJOY_PET_FRAME_COUNT;
    if (s_status == VIBE_BT_UI_RECORDING) {
        draw_waveform(audio_level);
        s_last_wave_refresh_ms = now_ms;
    } else {
        update_pet(now_ms);
    }
}

static bool wake_display(void)
{
    if (!s_display_on && s_panel) {
        vibe_board_set_lcd_brightness(VIBE_BOARD_LCD_BACKLIGHT_DEFAULT);
        esp_lcd_panel_disp_on_off(s_panel, true);
        s_display_on = true;
        redraw_full(esp_timer_get_time() / 1000, 0);
        return true;
    }
    return false;
}

esp_err_t vibe_bt_status_ui_init(void)
{
    s_transfer_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_transfer_done != NULL, ESP_ERR_NO_MEM, TAG,
                        "transfer semaphore");
    ESP_RETURN_ON_ERROR(vibe_board_set_lcd_brightness(
                            VIBE_BOARD_LCD_BACKLIGHT_DEFAULT),
                        TAG, "backlight");
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
                        TAG, "SPI bus");
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
                        TAG, "panel IO");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = VIBE_BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &s_panel),
                        TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG,
                        "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, VIBE_BOARD_LCD_X_GAP,
                                              VIBE_BOARD_LCD_Y_GAP),
                        TAG, "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG,
                        "display on");
    s_display_on = true;
    s_last_activity_ms = esp_timer_get_time() / 1000;
    refresh_power_status(s_last_activity_ms, false);
    redraw_full(s_last_activity_ms, 0);
    return ESP_OK;
}

void vibe_bt_status_ui_set(vibe_bt_ui_status_t status, bool minijoy_ready)
{
    vibe_bt_ui_status_t previous_status = s_status;
    bool changed = status != s_status || minijoy_ready != s_minijoy_ready;
    s_status = status;
    s_minijoy_ready = minijoy_ready;
    if (!changed) {
        return;
    }

    int64_t current_ms = esp_timer_get_time() / 1000;
    s_last_activity_ms = current_ms;
    if (status == VIBE_BT_UI_CONNECTED &&
        previous_status != VIBE_BT_UI_CONNECTED) {
        s_pet_happy_until_ms = current_ms + UI_PET_HAPPY_MS;
        s_pet_next_blink_ms = 0;
    }
    if (status == VIBE_BT_UI_RECORDING) {
        s_last_wave_refresh_ms = 0;
        s_wave_phase = 0;
    }
    if (!wake_display()) {
        redraw_full(current_ms, 0);
    }
}

void vibe_bt_status_ui_activity(void)
{
    s_last_activity_ms = esp_timer_get_time() / 1000;
    (void)wake_display();
}

void vibe_bt_status_ui_set_confirm_window(bool active)
{
    if (active == s_confirm_window) {
        return;
    }
    s_confirm_window = active;
    if (active) {
        s_last_activity_ms = esp_timer_get_time() / 1000;
    }
    if (!wake_display() && s_display_on) {
        draw_bottom_bar();
    }
}

void vibe_bt_status_ui_tick(int64_t now_ms, uint8_t audio_level)
{
    if (s_last_power_poll_ms == 0 ||
        now_ms - s_last_power_poll_ms >= UI_POWER_POLL_MS) {
        refresh_power_status(now_ms, false);
    }
    if (!s_display_on) {
        return;
    }
    if (s_status == VIBE_BT_UI_RECORDING) {
        if (s_last_wave_refresh_ms == 0 ||
            now_ms - s_last_wave_refresh_ms >= UI_WAVE_REFRESH_MS) {
            draw_waveform(audio_level);
            s_last_wave_refresh_ms = now_ms;
        }
        return;
    }
    update_pet(now_ms);
    if (now_ms - s_last_activity_ms >= UI_IDLE_OFF_MS) {
        vibe_board_set_lcd_brightness(0);
        esp_lcd_panel_disp_on_off(s_panel, false);
        s_display_on = false;
        s_pet_current_frame = VIBE_MINIJOY_PET_FRAME_COUNT;
    }
}
