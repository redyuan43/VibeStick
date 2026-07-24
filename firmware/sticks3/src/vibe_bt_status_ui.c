#include "vibe_bt_status_ui.h"

#include <stdio.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
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
#include "vibe_bt_ui_renderer.h"
#include "vibe_minijoy_pet_assets.h"

#define UI_TOP_HEIGHT 36
#define UI_BOTTOM_Y 151
#define UI_BOTTOM_HEIGHT (VIBE_BOARD_LCD_V_RES - UI_BOTTOM_Y)
#define UI_RENDER_BUFFER_PIXELS \
    (VIBE_BOARD_LCD_H_RES * UI_BOTTOM_HEIGHT)
#define UI_IDLE_OFF_MS 30000
#define UI_POWER_POLL_MS 2000
#define UI_LOADING_REFRESH_MS 40
#define UI_PET_BLINK_HOLD_MS 250
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
static uint16_t *s_render_buffer;
static bool s_display_on;
static bool s_minijoy_ready;
static bool s_confirm_window;
static vibe_bt_ui_status_t s_status = VIBE_BT_UI_WAITING;
static int64_t s_last_activity_ms;
static int64_t s_last_power_poll_ms;
static int64_t s_last_loading_refresh_ms;
static int64_t s_loading_started_ms;
static int64_t s_pet_happy_until_ms;
static int64_t s_pet_blink_until_ms;
static int64_t s_pet_next_blink_ms;
static vibe_minijoy_pet_frame_id_t s_pet_blink_frame =
    VIBE_MINIJOY_PET_FRAME_BLINK_BOTH;
static vibe_minijoy_pet_frame_id_t s_pet_current_frame =
    VIBE_MINIJOY_PET_FRAME_COUNT;
static int s_loading_heights[5];
static bool s_loading_heights_valid;
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

static uint16_t color_loading(void)
{
    return rgb565(244, 245, 247);
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

static bool begin_surface(vibe_bt_ui_surface_t *surface, int width, int height)
{
    return s_render_buffer && vibe_bt_ui_surface_init(
                                  surface, s_render_buffer,
                                  UI_RENDER_BUFFER_PIXELS, width, height);
}

static bool flush_surface(int x, int y, const vibe_bt_ui_surface_t *surface)
{
    return surface && draw_bitmap(x, y, surface->width, surface->height,
                                  surface->pixels);
}

static void draw_text_centered(vibe_bt_ui_surface_t *surface, int y,
                               const char *text, uint16_t color, int scale)
{
    vibe_bt_ui_surface_draw_text(
        surface,
        (surface->width - vibe_bt_ui_text_width(text, scale)) / 2, y, text,
        color, scale);
}

static const char *status_text(void)
{
    switch (s_status) {
    case VIBE_BT_UI_CONNECTING:
        return "SYNC";
    case VIBE_BT_UI_PAIRING:
        return "PAIR";
    case VIBE_BT_UI_CONNECTED:
        return "LINK";
    case VIBE_BT_UI_RECORDING:
        return "REC";
    case VIBE_BT_UI_OTA_CONNECTING:
        return "WIFI";
    case VIBE_BT_UI_OTA_CHECKING:
        return "CHECK";
    case VIBE_BT_UI_OTA_DOWNLOADING:
        return "UPDATE";
    case VIBE_BT_UI_OTA_CURRENT:
        return "CURRENT";
    case VIBE_BT_UI_OTA_FAILED:
        return "ERROR";
    case VIBE_BT_UI_ERROR:
        return "ERROR";
    case VIBE_BT_UI_WAITING:
    default:
        return "WAIT";
    }
}

static uint16_t status_color(void)
{
    if (s_status == VIBE_BT_UI_CONNECTING) {
        return rgb565(255, 196, 64);
    }
    return (s_status == VIBE_BT_UI_RECORDING ||
            s_status == VIBE_BT_UI_OTA_DOWNLOADING ||
            s_status == VIBE_BT_UI_OTA_FAILED ||
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

static void draw_battery_icon(vibe_bt_ui_surface_t *surface, int x, int y)
{
    uint16_t color = battery_color();
    vibe_bt_ui_surface_fill_rect(surface, x, y, 16, 1, color);
    vibe_bt_ui_surface_fill_rect(surface, x, y + 9, 16, 1, color);
    vibe_bt_ui_surface_fill_rect(surface, x, y, 1, 10, color);
    vibe_bt_ui_surface_fill_rect(surface, x + 15, y, 1, 10, color);
    vibe_bt_ui_surface_fill_rect(surface, x + 16, y + 3, 2, 4, color);
    if (s_battery_valid && s_battery_display > 0) {
        int fill_width = (s_battery_display * 12 + 99) / 100;
        vibe_bt_ui_surface_fill_rect(surface, x + 2, y + 2, fill_width, 6,
                                     color);
    }
    if (s_battery_charging || s_usb_powered) {
        uint16_t bolt = color_foreground();
        vibe_bt_ui_surface_fill_rect(surface, x + 8, y + 1, 2, 3, bolt);
        vibe_bt_ui_surface_fill_rect(surface, x + 6, y + 4, 4, 2, bolt);
        vibe_bt_ui_surface_fill_rect(surface, x + 6, y + 6, 2, 3, bolt);
    }
}

static void draw_top_bar(void)
{
    vibe_bt_ui_surface_t surface;
    if (!begin_surface(&surface, VIBE_BOARD_LCD_H_RES, UI_TOP_HEIGHT)) {
        return;
    }
    vibe_bt_ui_surface_clear(&surface, color_background());
    vibe_bt_ui_surface_draw_text(&surface, 8, 13, status_text(),
                                 status_color(), 1);

    char level_text[5] = "--%";
    if (s_battery_valid) {
        snprintf(level_text, sizeof(level_text), "%d%%", s_battery_display);
    }
    int level_width = vibe_bt_ui_text_width(level_text, 1);
    int level_x = VIBE_BOARD_LCD_H_RES - 7 - level_width;
    draw_battery_icon(&surface, level_x - 22, 11);
    vibe_bt_ui_surface_draw_text(&surface, level_x, 13, level_text,
                                 battery_color(), 1);
    vibe_bt_ui_surface_fill_rect(&surface, 8, 34,
                                 VIBE_BOARD_LCD_H_RES - 16, 1,
                                 rgb565(24, 42, 54));
    (void)flush_surface(0, 0, &surface);
}

static void draw_bottom_bar(void)
{
    vibe_bt_ui_surface_t surface;
    if (!begin_surface(&surface, VIBE_BOARD_LCD_H_RES, UI_BOTTOM_HEIGHT)) {
        return;
    }
    vibe_bt_ui_surface_clear(&surface, color_background());
    const bool ota_status = s_status >= VIBE_BT_UI_OTA_CONNECTING &&
                            s_status <= VIBE_BT_UI_OTA_FAILED;
    const char *joy_text =
        ota_status
            ? "OTA MODE"
            : (s_status == VIBE_BT_UI_CONNECTING
                   ? "HID MIC"
                   : (s_status == VIBE_BT_UI_RECORDING
                          ? "JOY MIC"
                          : (s_minijoy_ready ? "JOY OK" : "JOY OFF")));
    draw_text_centered(&surface, 166 - UI_BOTTOM_Y, joy_text,
                       ota_status || s_minijoy_ready ||
                               s_status == VIBE_BT_UI_RECORDING
                           ? color_foreground()
                           : color_warning(),
                       1);
    const char *action_text = "A MIC";
    if (s_status == VIBE_BT_UI_CONNECTING) {
        action_text = "PLEASE WAIT";
    } else if (s_status == VIBE_BT_UI_RECORDING) {
        action_text = "MIC LIVE";
    } else if (s_status == VIBE_BT_UI_OTA_CONNECTING) {
        action_text = "CONNECT";
    } else if (s_status == VIBE_BT_UI_OTA_CHECKING) {
        action_text = "CHECKING";
    } else if (s_status == VIBE_BT_UI_OTA_DOWNLOADING) {
        action_text = "UPDATING";
    } else if (s_status == VIBE_BT_UI_OTA_CURRENT) {
        action_text = "NO UPDATE";
    } else if (s_status == VIBE_BT_UI_OTA_FAILED) {
        action_text = "FAILED";
    } else if (s_confirm_window) {
        action_text = "ENTER";
    }
    draw_text_centered(&surface, 198 - UI_BOTTOM_Y, action_text,
                       s_status == VIBE_BT_UI_CONNECTING
                           ? rgb565(255, 196, 64)
                           : s_status == VIBE_BT_UI_RECORDING ||
                               s_status == VIBE_BT_UI_OTA_DOWNLOADING
                           ? color_warning()
                           : color_accent(),
                       2);
    (void)flush_surface(0, UI_BOTTOM_Y, &surface);
}

static bool draw_pet_frame(vibe_minijoy_pet_frame_id_t frame_id)
{
    vibe_bt_ui_surface_t surface;
    if (!begin_surface(&surface, UI_WAVE_WIDTH, UI_WAVE_HEIGHT)) {
        return false;
    }
    vibe_bt_ui_surface_clear(&surface, color_background());
    int pet_x = UI_PET_X - UI_WAVE_X;
    if (!vibe_bt_ui_surface_draw_pet(&surface, pet_x, 0, frame_id)) {
        ESP_LOGW(TAG, "pet frame decode failed id=%d", (int)frame_id);
        return false;
    }
    return flush_surface(UI_WAVE_X, UI_WAVE_Y, &surface);
}

static void schedule_next_blink(int64_t now_ms)
{
    s_pet_next_blink_ms =
        now_ms + UI_PET_BLINK_MIN_MS +
        (int64_t)(esp_random() % UI_PET_BLINK_RANGE_MS);
}

static vibe_minijoy_pet_frame_id_t pet_frame_for_status(int64_t now_ms)
{
    if (s_status == VIBE_BT_UI_ERROR ||
        s_status == VIBE_BT_UI_OTA_FAILED) {
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
    if (draw_pet_frame(frame)) {
        s_pet_current_frame = frame;
    }
}

static void draw_loading(int64_t now_ms)
{
    int heights[5];
    uint32_t elapsed_ms = s_loading_started_ms > 0 &&
                                  now_ms >= s_loading_started_ms
                              ? (uint32_t)(now_ms - s_loading_started_ms)
                              : 0;
    vibe_bt_ui_loading_heights(elapsed_ms, heights);
    if (s_loading_heights_valid &&
        memcmp(heights, s_loading_heights, sizeof(heights)) == 0) {
        return;
    }

    vibe_bt_ui_surface_t surface;
    if (!begin_surface(&surface, UI_WAVE_WIDTH, UI_WAVE_HEIGHT)) {
        return;
    }
    vibe_bt_ui_surface_clear(&surface, color_background());
    const int bar_width = 6;
    const int gap = 6;
    const int total_width = 5 * bar_width + 4 * gap;
    const int start_x = (UI_WAVE_WIDTH - total_width) / 2;
    for (int i = 0; i < 5; ++i) {
        int y = (UI_WAVE_HEIGHT - heights[i]) / 2;
        vibe_bt_ui_surface_fill_rounded_rect(
            &surface, start_x + i * (bar_width + gap), y, bar_width,
            heights[i], 3, color_loading());
    }
    if (flush_surface(UI_WAVE_X, UI_WAVE_Y, &surface)) {
        memcpy(s_loading_heights, heights, sizeof(heights));
        s_loading_heights_valid = true;
    }
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

static void clear_panel_background(void)
{
    for (int y = 0; y < VIBE_BOARD_LCD_V_RES; y += UI_BOTTOM_HEIGHT) {
        int height = VIBE_BOARD_LCD_V_RES - y;
        if (height > UI_BOTTOM_HEIGHT) {
            height = UI_BOTTOM_HEIGHT;
        }
        vibe_bt_ui_surface_t surface;
        if (!begin_surface(&surface, VIBE_BOARD_LCD_H_RES, height)) {
            return;
        }
        vibe_bt_ui_surface_clear(&surface, color_background());
        if (!flush_surface(0, y, &surface)) {
            return;
        }
    }
}

static void redraw_full(int64_t now_ms)
{
    if (!s_panel || !s_display_on) {
        return;
    }
    draw_top_bar();
    draw_bottom_bar();
    s_pet_current_frame = VIBE_MINIJOY_PET_FRAME_COUNT;
    if (s_status == VIBE_BT_UI_RECORDING) {
        s_loading_heights_valid = false;
        draw_loading(now_ms);
        s_last_loading_refresh_ms = now_ms;
    } else {
        update_pet(now_ms);
    }
}

static bool wake_display(void)
{
    if (!s_display_on && s_panel) {
        vibe_board_set_lcd_brightness(VIBE_BOARD_LCD_BACKLIGHT_DEFAULT);
        s_display_on = true;
        clear_panel_background();
        redraw_full(esp_timer_get_time() / 1000);
        esp_lcd_panel_disp_on_off(s_panel, true);
        return true;
    }
    return false;
}

esp_err_t vibe_bt_status_ui_init(void)
{
    s_transfer_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_transfer_done != NULL, ESP_ERR_NO_MEM, TAG,
                        "transfer semaphore");
    s_render_buffer = heap_caps_malloc(
        UI_RENDER_BUFFER_PIXELS * sizeof(s_render_buffer[0]),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_render_buffer != NULL, ESP_ERR_NO_MEM, TAG,
                        "render buffer");
    ESP_RETURN_ON_ERROR(vibe_board_set_lcd_brightness(0), TAG,
                        "backlight off");
    spi_bus_config_t bus_config = {
        .sclk_io_num = VIBE_BOARD_PIN_LCD_SCK,
        .mosi_io_num = VIBE_BOARD_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz =
            UI_RENDER_BUFFER_PIXELS * sizeof(s_render_buffer[0]),
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
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, false), TAG,
                        "display off");
    s_display_on = false;
    s_last_activity_ms = esp_timer_get_time() / 1000;
    refresh_power_status(s_last_activity_ms, false);
    s_display_on = true;
    clear_panel_background();
    redraw_full(s_last_activity_ms);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG,
                        "display on");
    ESP_RETURN_ON_ERROR(vibe_board_set_lcd_brightness(
                            VIBE_BOARD_LCD_BACKLIGHT_DEFAULT),
                        TAG, "backlight on");
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
        s_last_loading_refresh_ms = 0;
        s_loading_started_ms = current_ms;
        s_loading_heights_valid = false;
    }
    if (!wake_display()) {
        redraw_full(current_ms);
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
    if (!wake_display() && s_display_on &&
        s_status != VIBE_BT_UI_RECORDING) {
        draw_bottom_bar();
    }
}

void vibe_bt_status_ui_tick(int64_t now_ms)
{
    if (s_last_power_poll_ms == 0 ||
        now_ms - s_last_power_poll_ms >= UI_POWER_POLL_MS) {
        refresh_power_status(now_ms, false);
    }
    if (!s_display_on) {
        return;
    }
    if (s_status == VIBE_BT_UI_RECORDING) {
        if (s_last_loading_refresh_ms == 0 ||
            now_ms - s_last_loading_refresh_ms >=
                UI_LOADING_REFRESH_MS) {
            draw_loading(now_ms);
            s_last_loading_refresh_ms = now_ms;
        }
        return;
    }
    update_pet(now_ms);
    if (now_ms - s_last_activity_ms >= UI_IDLE_OFF_MS) {
        esp_lcd_panel_disp_on_off(s_panel, false);
        vibe_board_set_lcd_brightness(0);
        s_display_on = false;
        s_pet_current_frame = VIBE_MINIJOY_PET_FRAME_COUNT;
    }
}

esp_err_t vibe_bt_status_ui_prepare_deep_sleep(void)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "panel unavailable");
    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, false);
    esp_err_t backlight_err = vibe_board_set_lcd_brightness(0);
    s_display_on = false;
    s_pet_current_frame = VIBE_MINIJOY_PET_FRAME_COUNT;
    if (err != ESP_OK) {
        return err;
    }
    return backlight_err;
}
