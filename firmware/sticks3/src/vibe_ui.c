#include "vibe_ui.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "driver/ledc.h"
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
#include "freertos/task.h"
#include "lvgl.h"
#include "vibe_board.h"
#include "vibe_board_profile.h"
#include "vibe_stick_anim_assets.h"
#include "vibe_stick_pet_assets.h"

#define LCD_HOST VIBE_BOARD_LCD_HOST
#define LCD_H_RES VIBE_BOARD_LCD_H_RES
#define LCD_V_RES VIBE_BOARD_LCD_V_RES
#define LCD_X_GAP VIBE_BOARD_LCD_X_GAP
#define LCD_Y_GAP VIBE_BOARD_LCD_Y_GAP
#define LCD_PIXEL_CLOCK_HZ VIBE_BOARD_LCD_PIXEL_CLOCK_HZ
#if defined(VIBE_BOARD_CARDPUTER_ADV)
#define LCD_BACKLIGHT_PWM_HZ 256
#define LCD_BACKLIGHT_PWM_TIMER LEDC_TIMER_3
#define LCD_BACKLIGHT_PWM_CHANNEL LEDC_CHANNEL_7
#define LCD_BACKLIGHT_PWM_RESOLUTION LEDC_TIMER_9_BIT
#else
#define LCD_BACKLIGHT_PWM_HZ 5000
#define LCD_BACKLIGHT_PWM_TIMER LEDC_TIMER_0
#define LCD_BACKLIGHT_PWM_CHANNEL LEDC_CHANNEL_0
#define LCD_BACKLIGHT_PWM_RESOLUTION LEDC_TIMER_8_BIT
#endif
#define LCD_BACKLIGHT_DEFAULT VIBE_BOARD_LCD_BACKLIGHT_DEFAULT
#define LVGL_DRAW_BUF_LINES 24
#define LVGL_TICK_PERIOD_MS 10
#define LVGL_TASK_STACK_BYTES 12288
#define BATTERY_FILL_MAX_WIDTH 20
#define BATTERY_LOW_THRESHOLD_PERCENT 20
#define BATTERY_HIGH_THRESHOLD_PERCENT 50
#define VIBE_UI_MODE_VISUAL_MS 1800
#define VIBE_UI_MODE_FRAME_MS 260
#define VIBE_UI_PET_ACTIVE_MS 300
#define VIBE_UI_PET_IDLE_MS 1000
#define VIBE_UI_PET_IDLE_BOB_STEPS 16
#ifndef VIBE_STICK_ANIM_PREVIEW
#define VIBE_STICK_ANIM_PREVIEW 0
#endif

#define FONT_UI (&lv_font_montserrat_10)

static const char *TAG = "vibe_ui";

typedef struct {
    const vibe_stick_pet_frame_id_t *frames;
    int frame_count;
    int frame_ms;
    int key;
} pet_sequence_t;

typedef struct {
    vibe_ui_dependencies_t dependencies;
    SemaphoreHandle_t lock;
    lv_display_t *display;
    esp_lcd_panel_handle_t panel;
    esp_timer_handle_t tick_timer;
    TaskHandle_t task;
    atomic_bool rendering_suspended;
    bool tick_running;
    bool ready;
    bool recording_visible;
    uint8_t current_backlight;
    vibe_ui_view_model_t view;
    lv_obj_t *wifi_label;
    lv_obj_t *battery_icon;
    lv_obj_t *battery_fill;
    lv_obj_t *battery_cap;
    lv_obj_t *battery_bolt;
    lv_obj_t *mode_label;
    lv_obj_t *intent_label;
    lv_obj_t *bridge_label;
    lv_obj_t *ip_label;
    lv_obj_t *pet_image;
    lv_obj_t *mode_switch_layer;
    lv_obj_t *mode_switch_title;
    lv_obj_t *mode_switch_hint;
    lv_obj_t *recording_overlay;
    lv_obj_t *recording_wave_group;
    lv_obj_t *recording_wave_bars[5];
    lv_obj_t *recording_title;
    lv_obj_t *recording_hint;
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    lv_obj_t *card_home_hint;
    lv_obj_t *card_setup_layer;
    lv_obj_t *card_setup_title;
    lv_obj_t *card_setup_field_label;
    lv_obj_t *card_setup_value;
    lv_obj_t *card_setup_hint;
#endif
    uint8_t *pet_pixels;
    vibe_stick_pet_frame_id_t pet_current_frame;
    int64_t pet_next_frame_ms;
    int pet_sequence_index;
    int pet_sequence_key;
    int pet_bob_step;
    int pet_idle_bob_steps_remaining;
    int pet_y_offset;
    bool pet_y_offset_valid;
    lv_timer_t *pet_timer;
    const vibe_stick_pet_frame_id_t *mode_frames;
    int mode_frame_count;
    int mode_frame_index;
    int64_t mode_next_frame_ms;
    int64_t mode_until_ms;
    bool mode_persistent;
#if VIBE_STICK_ANIM_PREVIEW
    int anim_asset_index;
    int anim_frame_index;
    volatile bool anim_switch_requested;
#endif
} vibe_ui_impl_t;

_Static_assert(sizeof(vibe_ui_impl_t) <= VIBE_UI_STORAGE_BYTES,
               "increase VIBE_UI_STORAGE_BYTES");

static vibe_ui_impl_t *impl(vibe_ui_t *ui)
{
    return (vibe_ui_impl_t *)ui->bytes;
}

static const vibe_ui_impl_t *const_impl(const vibe_ui_t *ui)
{
    return (const vibe_ui_impl_t *)ui->bytes;
}

static const lv_point_precise_t s_battery_bolt_points[] = {
    {3, 0}, {1, 3}, {3, 3}, {2, 7}, {6, 2}, {4, 2},
};

static const vibe_stick_pet_frame_id_t s_pet_idle_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE,
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_BLINK_LEFT,
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_BLINK_RIGHT,
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_BLINK_BOTH,
};
static const vibe_stick_pet_frame_id_t s_pet_running_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_TYPING,
    VIBE_STICK_PET_FRAME_CLOUDLING_THINKING,
    VIBE_STICK_PET_FRAME_CLOUDLING_BUILDING,
    VIBE_STICK_PET_FRAME_CLOUDLING_CONDUCTING,
};
static const vibe_stick_pet_frame_id_t s_pet_approval_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_ATTENTION,
    VIBE_STICK_PET_FRAME_CLOUDLING_REACT_DRAG,
};
static const vibe_stick_pet_frame_id_t s_pet_done_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_NOTIFICATION,
    VIBE_STICK_PET_FRAME_CLOUDLING_JUGGLING,
};
static const vibe_stick_pet_frame_id_t s_pet_error_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_ERROR,
    VIBE_STICK_PET_FRAME_CLOUDLING_ATTENTION,
};
static const vibe_stick_pet_frame_id_t s_pet_sleep_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_TO_DOZING,
    VIBE_STICK_PET_FRAME_CLOUDLING_DOZING,
    VIBE_STICK_PET_FRAME_CLOUDLING_DOZING_TO_SLEEPING,
    VIBE_STICK_PET_FRAME_CLOUDLING_SLEEPING,
    VIBE_STICK_PET_FRAME_CLOUDLING_SLEEPING_TO_IDLE,
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_TO_SLEEPING,
};
static const vibe_stick_pet_frame_id_t s_mode_ptt_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_ATTENTION,
    VIBE_STICK_PET_FRAME_CLOUDLING_TYPING,
    VIBE_STICK_PET_FRAME_CLOUDLING_MINI_TYPING,
};
static const vibe_stick_pet_frame_id_t s_mode_lift_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_CARRYING,
    VIBE_STICK_PET_FRAME_CLOUDLING_REACT_DRAG,
    VIBE_STICK_PET_FRAME_CLOUDLING_CARRYING,
};
static const vibe_stick_pet_frame_id_t s_mode_dict_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_TYPING,
    VIBE_STICK_PET_FRAME_CLOUDLING_MINI_TYPING,
    VIBE_STICK_PET_FRAME_CLOUDLING_TYPING,
};
static const vibe_stick_pet_frame_id_t s_mode_fortune_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_THINKING,
    VIBE_STICK_PET_FRAME_CLOUDLING_JUGGLING,
    VIBE_STICK_PET_FRAME_CLOUDLING_THINKING,
};
static const vibe_stick_pet_frame_id_t s_mode_almanac_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_READING,
    VIBE_STICK_PET_FRAME_CLOUDLING_NOTIFICATION,
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_READING,
};

static lv_color_t provider_accent(vibe_provider_id_t provider)
{
    return provider == VIBE_PROVIDER_CLAUDE
               ? lv_color_hex(0xd97757)
               : lv_color_hex(0x4d82ff);
}

void vibe_ui_lock(vibe_ui_t *ui)
{
    vibe_ui_impl_t *state = impl(ui);
    if (state->lock) {
        xSemaphoreTake(state->lock, portMAX_DELAY);
    }
}

void vibe_ui_unlock(vibe_ui_t *ui)
{
    vibe_ui_impl_t *state = impl(ui);
    if (state->lock) {
        xSemaphoreGive(state->lock);
    }
}

static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    vibe_ui_t *ui = arg;
    while (true) {
        vibe_ui_lock(ui);
        const uint32_t wait_ms = lv_timer_handler();
        vibe_ui_unlock(ui);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

static bool notify_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                               esp_lcd_panel_io_event_data_t *event_data,
                               void *user_ctx)
{
    (void)panel_io;
    (void)event_data;
    lv_display_flush_ready(user_ctx);
    return false;
}

static void flush_cb(lv_display_t *display,
                     const lv_area_t *area,
                     uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel =
        lv_display_get_user_data(display);
    const int32_t width = area->x2 - area->x1 + 1;
    const int32_t height = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, width * height);
    esp_lcd_panel_draw_bitmap(
        panel, area->x1, area->y1,
        area->x2 + 1, area->y2 + 1, px_map);
}

void vibe_ui_set_backlight(vibe_ui_t *ui, uint8_t brightness)
{
    vibe_ui_impl_t *state = impl(ui);
#if VIBE_BOARD_HAS_GPIO_BACKLIGHT
    uint32_t duty = brightness;
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    if (brightness != 0) {
        const uint32_t offset = (16U * 259U) >> 8;
        duty = brightness * (257U - offset) + offset * 255U;
        duty += 1U << 6;
        duty >>= 7;
    }
#endif
    ledc_set_duty(
        LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_PWM_CHANNEL, duty);
    ledc_update_duty(
        LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_PWM_CHANNEL);
#else
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        vibe_board_set_lcd_brightness(brightness));
#endif
    state->current_backlight = brightness;
    if (state->dependencies.brightness_changed) {
        state->dependencies.brightness_changed(
            brightness, state->dependencies.context);
    }
}

uint8_t vibe_ui_backlight(const vibe_ui_t *ui)
{
    return const_impl(ui)->current_backlight;
}

static void init_backlight(vibe_ui_t *ui)
{
#if VIBE_BOARD_HAS_GPIO_BACKLIGHT
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LCD_BACKLIGHT_PWM_TIMER,
        .duty_resolution = LCD_BACKLIGHT_PWM_RESOLUTION,
        .freq_hz = LCD_BACKLIGHT_PWM_HZ,
        .clk_cfg = LEDC_USE_XTAL_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const ledc_channel_config_t channel = {
        .gpio_num = VIBE_BOARD_LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_BACKLIGHT_PWM_CHANNEL,
        .timer_sel = LCD_BACKLIGHT_PWM_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
#endif
    vibe_ui_set_backlight(ui, LCD_BACKLIGHT_DEFAULT);
}

esp_err_t vibe_ui_init(vibe_ui_t *ui,
                       const vibe_ui_dependencies_t *dependencies)
{
    ESP_RETURN_ON_FALSE(ui && dependencies, ESP_ERR_INVALID_ARG,
                        TAG, "UI config");
    memset(ui, 0, sizeof(*ui));
    vibe_ui_impl_t *state = impl(ui);
    state->dependencies = *dependencies;
    state->pet_current_frame = VIBE_STICK_PET_FRAME_COUNT;
    state->pet_sequence_key = -1;
    state->lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(state->lock, ESP_ERR_NO_MEM, TAG, "UI lock");
    atomic_init(&state->rendering_suspended, false);
    init_backlight(ui);

    const spi_bus_config_t bus_config = {
        .sclk_io_num = VIBE_BOARD_PIN_LCD_SCK,
        .mosi_io_num = VIBE_BOARD_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz =
            LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color_t),
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
        TAG, "spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = VIBE_BOARD_PIN_LCD_DC,
        .cs_gpio_num = VIBE_BOARD_PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_flush_ready,
        .flags.sio_mode = VIBE_BOARD_LCD_SPI_SIO_MODE,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)LCD_HOST,
            &io_config, &io),
        TAG, "panel io");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = VIBE_BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7789(
            io, &panel_config, &state->panel),
        TAG, "panel");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_reset(state->panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_init(state->panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_invert_color(state->panel, true),
        TAG, "panel invert");
#if VIBE_BOARD_LCD_ROTATION == 1
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_swap_xy(state->panel, true),
        TAG, "panel swap xy");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_mirror(state->panel, true, false),
        TAG, "panel mirror");
#endif
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_set_gap(
            state->panel, LCD_X_GAP, LCD_Y_GAP),
        TAG, "panel gap");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(state->panel, true),
        TAG, "panel on");

    lv_init();
    state->display = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_user_data(state->display, state->panel);
    lv_display_set_flush_cb(state->display, flush_cb);
    const size_t buffer_size =
        LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color_t);
    void *buffer = heap_caps_malloc(
        buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(buffer, ESP_ERR_NO_MEM, TAG, "LVGL buffer");
    lv_display_set_buffers(
        state->display, buffer, NULL, buffer_size,
        LV_DISPLAY_RENDER_MODE_PARTIAL);
    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = notify_flush_ready,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_register_event_callbacks(
            io, &callbacks, state->display),
        TAG, "panel callbacks");
    const esp_timer_create_args_t tick_args = {
        .callback = tick_cb,
        .name = "lvgl_tick",
    };
    ESP_RETURN_ON_ERROR(
        esp_timer_create(&tick_args, &state->tick_timer),
        TAG, "tick timer");
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(
            state->tick_timer, LVGL_TICK_PERIOD_MS * 1000),
        TAG, "tick start");
    state->tick_running = true;
    const BaseType_t created = xTaskCreatePinnedToCore(
        lvgl_task, "lvgl", LVGL_TASK_STACK_BYTES,
        ui, 3, &state->task, 1);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static lv_obj_t *make_label(lv_obj_t *parent,
                            const char *text,
                            const lv_font_t *font,
                            lv_color_t color,
                            int32_t width,
                            lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static lv_obj_t *make_plain_obj(lv_obj_t *parent,
                                int32_t width,
                                int32_t height,
                                lv_color_t color,
                                lv_opa_t opacity,
                                int32_t radius)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, opacity, 0);
    lv_obj_set_style_radius(object, radius, 0);
    return object;
}

static void set_battery(vibe_ui_impl_t *state)
{
    int battery = state->view.battery;
    if (battery < 0) battery = 0;
    if (battery > 100) battery = 100;
    int width =
        battery > 0 ? battery * BATTERY_FILL_MAX_WIDTH / 100 : 0;
    if (width < 1 && battery > 0) width = 1;
    lv_color_t color = lv_color_hex(0x6b7280);
    if (state->view.battery_valid) {
        color = battery < BATTERY_LOW_THRESHOLD_PERCENT
                    ? lv_color_hex(0xef4444)
                : battery < BATTERY_HIGH_THRESHOLD_PERCENT
                    ? lv_color_hex(0xfacc15)
                    : lv_color_hex(0x32d583);
    }
    lv_obj_set_style_border_color(state->battery_icon, color, 0);
    lv_obj_set_style_bg_color(state->battery_fill, color, 0);
    lv_obj_set_style_bg_color(state->battery_cap, color, 0);
    lv_obj_set_width(state->battery_fill, width);
    if (state->view.battery_charging ||
        state->view.usb_powered) {
        lv_obj_clear_flag(state->battery_bolt, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(state->battery_bolt, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool set_pet_frame(vibe_ui_impl_t *state,
                          vibe_stick_pet_frame_id_t frame)
{
    if (!state->pet_image || !state->pet_pixels ||
        state->pet_current_frame == frame) {
        return true;
    }
    if (!vibe_stick_pet_decode_frame(
            frame, state->pet_pixels,
            VIBE_STICK_PET_PIXEL_BYTES)) {
        return false;
    }
    state->pet_current_frame = frame;
    lv_obj_invalidate(state->pet_image);
    return true;
}

static void set_pet_offset(vibe_ui_impl_t *state, int offset)
{
    if (!state->pet_image ||
        (state->pet_y_offset_valid &&
         state->pet_y_offset == offset)) {
        return;
    }
    lv_obj_align(state->pet_image, LV_ALIGN_CENTER, 0, offset);
    state->pet_y_offset = offset;
    state->pet_y_offset_valid = true;
}

static pet_sequence_t pet_sequence(const vibe_ui_impl_t *state)
{
    const char *alert = state->view.alert_type;
    const char *status = state->view.provider_status;
    if (strcmp(alert, "APPROVAL") == 0 ||
        strcmp(alert, "WAITING_APPROVAL") == 0 ||
        strcmp(alert, "PENDING_APPROVAL") == 0 ||
        strcmp(alert, "NEEDS_APPROVAL") == 0 ||
        strcmp(status, "APPROVAL") == 0) {
        return (pet_sequence_t){
            s_pet_approval_frames,
            sizeof(s_pet_approval_frames) /
                sizeof(s_pet_approval_frames[0]),
            650, 2};
    }
    if (strcmp(alert, "ERROR") == 0 ||
        strcmp(alert, "FAILED") == 0 ||
        strcmp(alert, "FAILURE") == 0 ||
        strcmp(status, "ERROR") == 0) {
        return (pet_sequence_t){
            s_pet_error_frames,
            sizeof(s_pet_error_frames) /
                sizeof(s_pet_error_frames[0]),
            750, 4};
    }
    if (strcmp(alert, "DONE") == 0 ||
        strcmp(alert, "COMPLETED") == 0 ||
        strcmp(alert, "SUCCESS") == 0 ||
        strcmp(status, "DONE") == 0) {
        return (pet_sequence_t){
            s_pet_done_frames,
            sizeof(s_pet_done_frames) /
                sizeof(s_pet_done_frames[0]),
            750, 3};
    }
    if (strcmp(status, "RUNNING") == 0) {
        return (pet_sequence_t){
            s_pet_running_frames,
            sizeof(s_pet_running_frames) /
                sizeof(s_pet_running_frames[0]),
            550, 1};
    }
    if (strcmp(status, "OFFLINE") == 0 ||
        strcmp(status, "UNIMPLEMENTED") == 0) {
        return (pet_sequence_t){
            s_pet_sleep_frames,
            sizeof(s_pet_sleep_frames) /
                sizeof(s_pet_sleep_frames[0]),
            900, 5};
    }
    return (pet_sequence_t){
        s_pet_idle_frames,
        sizeof(s_pet_idle_frames) / sizeof(s_pet_idle_frames[0]),
        220, 0};
}

static bool visual_active(const vibe_ui_impl_t *state,
                          int64_t now_ms)
{
    return (state->mode_persistent ||
            state->mode_until_ms > now_ms) &&
           state->mode_frames &&
           state->mode_frame_count > 0;
}

static void finish_visual_locked(vibe_ui_impl_t *state)
{
    if (state->mode_switch_layer) {
        lv_obj_add_flag(
            state->mode_switch_layer, LV_OBJ_FLAG_HIDDEN);
    }
    state->mode_until_ms = 0;
    state->mode_frames = NULL;
    state->mode_frame_count = 0;
    state->mode_frame_index = 0;
    state->mode_next_frame_ms = 0;
    state->mode_persistent = false;
    state->pet_sequence_key = -1;
    state->pet_next_frame_ms = 0;
}

static void update_pet(vibe_ui_impl_t *state)
{
    if (!state->view.display_active &&
        !state->recording_visible) {
        return;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (visual_active(state, now_ms)) {
        if (now_ms >= state->mode_next_frame_ms) {
            set_pet_frame(
                state,
                state->mode_frames[state->mode_frame_index]);
            state->mode_frame_index =
                (state->mode_frame_index + 1) %
                state->mode_frame_count;
            state->mode_next_frame_ms =
                now_ms + VIBE_UI_MODE_FRAME_MS;
        }
        lv_timer_set_period(state->pet_timer, VIBE_UI_PET_ACTIVE_MS);
        set_pet_offset(state, 24);
        return;
    }
    if (state->mode_until_ms != 0) {
        finish_visual_locked(state);
    }
#if VIBE_STICK_ANIM_PREVIEW
    if (state->anim_switch_requested) {
        state->anim_switch_requested = false;
        const int asset_count = vibe_stick_anim_asset_count();
        if (asset_count > 0) {
            state->anim_asset_index =
                (state->anim_asset_index + 1) % asset_count;
            state->anim_frame_index = 0;
            state->pet_next_frame_ms = 0;
        }
    }
    if (now_ms >= state->pet_next_frame_ms) {
        const int count =
            vibe_stick_anim_frame_count(state->anim_asset_index);
        if (count > 0 &&
            vibe_stick_anim_decode_frame(
                state->anim_asset_index,
                state->anim_frame_index,
                state->pet_pixels,
                VIBE_STICK_ANIM_PIXEL_BYTES)) {
            lv_obj_invalidate(state->pet_image);
            state->anim_frame_index =
                (state->anim_frame_index + 1) % count;
            state->pet_next_frame_ms =
                now_ms + 1000 / vibe_stick_anim_fps();
        }
    }
    set_pet_offset(state, 14);
    return;
#endif
    if (state->view.pet_fast_resume_pending &&
        now_ms < state->view.pet_animation_resume_ms) {
        return;
    }
    state->view.pet_fast_resume_pending = false;
    const int bob_offsets[] = {0, -2, -4, -2, 0, 2, 4, 2};
    const pet_sequence_t sequence = pet_sequence(state);
    bool refresh = false;
    if (sequence.key != state->pet_sequence_key) {
        state->pet_sequence_key = sequence.key;
        state->pet_sequence_index =
            sequence.key == 0
                ? (int)(esp_random() %
                        (uint32_t)sequence.frame_count)
                : 0;
        if (sequence.key == 0) {
            state->pet_idle_bob_steps_remaining =
                VIBE_UI_PET_IDLE_BOB_STEPS;
        }
        state->pet_next_frame_ms = 0;
        refresh = true;
    } else if (now_ms >= state->pet_next_frame_ms) {
        state->pet_sequence_index =
            sequence.key == 0
                ? (int)(esp_random() %
                        (uint32_t)sequence.frame_count)
                : (state->pet_sequence_index + 1) %
                      sequence.frame_count;
        refresh = true;
    }
    if (refresh) {
        set_pet_frame(
            state, sequence.frames[state->pet_sequence_index]);
        state->pet_next_frame_ms =
            now_ms +
            (sequence.key == 0
                 ? 1000 + (int)(esp_random() % 4001)
                 : sequence.frame_ms);
    }
    if (sequence.key != 0 ||
        state->pet_idle_bob_steps_remaining > 0) {
        lv_timer_set_period(state->pet_timer, VIBE_UI_PET_ACTIVE_MS);
        set_pet_offset(
            state, 14 + bob_offsets[state->pet_bob_step]);
        state->pet_bob_step =
            (state->pet_bob_step + 1) %
            (int)(sizeof(bob_offsets) / sizeof(bob_offsets[0]));
        if (sequence.key == 0) {
            state->pet_idle_bob_steps_remaining--;
        }
    } else {
        lv_timer_set_period(state->pet_timer, VIBE_UI_PET_IDLE_MS);
        set_pet_offset(state, 14);
    }
}

static void pet_timer_cb(lv_timer_t *timer)
{
    update_pet(lv_timer_get_user_data(timer));
}

static void wave_height_cb(void *object, int32_t height)
{
    lv_obj_set_height(object, height);
}

static void stop_wave(vibe_ui_impl_t *state)
{
    static const int heights[5] = {14, 22, 32, 22, 14};
    for (int index = 0; index < 5; ++index) {
        if (state->recording_wave_bars[index]) {
            lv_anim_delete(
                state->recording_wave_bars[index], NULL);
            lv_obj_set_height(
                state->recording_wave_bars[index], heights[index]);
        }
    }
}

static void start_wave(vibe_ui_impl_t *state)
{
    static const int min_heights[5] = {10, 14, 18, 14, 10};
    static const int max_heights[5] = {24, 34, 48, 34, 24};
    stop_wave(state);
    for (int index = 0; index < 5; ++index) {
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(
            &animation, state->recording_wave_bars[index]);
        lv_anim_set_values(
            &animation, min_heights[index], max_heights[index]);
        lv_anim_set_duration(&animation, 460);
        lv_anim_set_playback_duration(&animation, 460);
        lv_anim_set_delay(&animation, index * 70);
        lv_anim_set_repeat_count(
            &animation, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&animation, wave_height_cb);
        lv_anim_start(&animation);
    }
}

esp_err_t vibe_ui_create(vibe_ui_t *ui)
{
    vibe_ui_impl_t *state = impl(ui);
    ESP_RETURN_ON_FALSE(
        state->display, ESP_ERR_INVALID_STATE, TAG, "display");
    vibe_ui_lock(ui);
    lv_obj_t *screen =
        lv_display_get_screen_active(state->display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x050608), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    state->wifi_label = make_label(
        screen, "WiFi", FONT_UI, lv_color_hex(0xf3f4f6),
        38, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(state->wifi_label, LV_ALIGN_TOP_LEFT, 9, 9);
    state->battery_icon = make_plain_obj(
        screen, 26, 13, lv_color_hex(0), LV_OPA_TRANSP, 3);
    lv_obj_set_style_border_width(state->battery_icon, 1, 0);
    lv_obj_align(state->battery_icon, LV_ALIGN_TOP_RIGHT, -9, 9);
    state->battery_fill = make_plain_obj(
        state->battery_icon, 1, 9, lv_color_hex(0xf3f4f6),
        LV_OPA_COVER, 2);
    lv_obj_align(state->battery_fill, LV_ALIGN_LEFT_MID, 2, 0);
    state->battery_bolt = lv_line_create(state->battery_icon);
    lv_line_set_points(
        state->battery_bolt, s_battery_bolt_points,
        sizeof(s_battery_bolt_points) /
            sizeof(s_battery_bolt_points[0]));
    lv_obj_set_style_line_width(state->battery_bolt, 1, 0);
    lv_obj_set_style_line_color(
        state->battery_bolt, lv_color_hex(0xffffff), 0);
    lv_obj_align(state->battery_bolt, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(state->battery_bolt, LV_OBJ_FLAG_HIDDEN);
    state->battery_cap = make_plain_obj(
        screen, 2, 7, lv_color_hex(0xf3f4f6),
        LV_OPA_COVER, 1);
    lv_obj_align_to(
        state->battery_cap, state->battery_icon,
        LV_ALIGN_OUT_RIGHT_MID, 1, 0);
    state->mode_label = make_label(
        screen, "PTT", FONT_UI, lv_color_hex(0x8a9099),
        28, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(state->mode_label, LV_ALIGN_TOP_MID, -18, 9);
    state->intent_label = make_label(
        screen, "DICT", FONT_UI, lv_color_hex(0x8a9099),
        32, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(state->intent_label, LV_ALIGN_TOP_MID, 12, 9);
    state->bridge_label = make_label(
        screen, "B CapsWriter", FONT_UI, lv_color_hex(0x686e78),
        128, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(state->bridge_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    state->ip_label = make_label(
        screen, "IP --", FONT_UI, lv_color_hex(0x686e78),
        128, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(state->ip_label, LV_ALIGN_BOTTOM_MID, 0, -7);
    state->pet_image = lv_image_create(screen);
#if VIBE_STICK_ANIM_PREVIEW
    state->pet_pixels = heap_caps_malloc(
        VIBE_STICK_ANIM_PIXEL_BYTES, MALLOC_CAP_8BIT);
    if (state->pet_pixels && vibe_stick_anim_assets_init() &&
        vibe_stick_anim_decode_frame(
            0, 0, state->pet_pixels,
            VIBE_STICK_ANIM_PIXEL_BYTES)) {
        vibe_stick_anim_set_image_data(state->pet_pixels);
        lv_image_set_src(state->pet_image, &vibe_stick_anim_image);
        state->anim_frame_index = 1;
    }
#else
    state->pet_pixels = heap_caps_malloc(
        VIBE_STICK_PET_PIXEL_BYTES, MALLOC_CAP_8BIT);
    if (state->pet_pixels &&
        vibe_stick_pet_decode_frame(
            VIBE_STICK_PET_FRAME_CLOUDLING_IDLE,
            state->pet_pixels, VIBE_STICK_PET_PIXEL_BYTES)) {
        vibe_stick_pet_set_image_data(state->pet_pixels);
        state->pet_current_frame =
            VIBE_STICK_PET_FRAME_CLOUDLING_IDLE;
        lv_image_set_src(state->pet_image, &vibe_stick_pet_image);
    } else {
        lv_obj_add_flag(state->pet_image, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    set_pet_offset(state, 14);
    state->pet_timer = lv_timer_create(
        pet_timer_cb, VIBE_UI_PET_ACTIVE_MS, state);
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    state->card_home_hint = make_label(
        screen,
        "OPT TALK\nFN+M AIR MOUSE\nFN+S CONNECTION",
        FONT_UI, lv_color_hex(0x8a9099), 120,
        LV_TEXT_ALIGN_CENTER);
    lv_obj_set_height(state->card_home_hint, 50);
    lv_obj_align(state->card_home_hint, LV_ALIGN_CENTER, 0, 55);
#endif
    state->mode_switch_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(state->mode_switch_layer);
    lv_obj_set_size(
        state->mode_switch_layer, LCD_H_RES, LCD_V_RES);
    lv_obj_add_flag(
        state->mode_switch_layer, LV_OBJ_FLAG_HIDDEN);
    state->mode_switch_title = make_label(
        state->mode_switch_layer, "DICTATION",
        &lv_font_montserrat_20, lv_color_hex(0xf4f5f7),
        124, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(
        state->mode_switch_title, LV_ALIGN_CENTER, 0, -50);
    state->mode_switch_hint = make_label(
        state->mode_switch_layer, "SIDE 2X  DICT",
        &lv_font_montserrat_14, lv_color_hex(0x9aa1ad),
        124, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(
        state->mode_switch_hint, LV_ALIGN_CENTER, 0, -26);
    state->recording_overlay = lv_obj_create(screen);
    lv_obj_set_size(
        state->recording_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_radius(state->recording_overlay, 0, 0);
    lv_obj_set_style_bg_color(
        state->recording_overlay, lv_color_hex(0x050608), 0);
    lv_obj_set_style_bg_opa(
        state->recording_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(state->recording_overlay, 0, 0);
    lv_obj_add_flag(
        state->recording_overlay, LV_OBJ_FLAG_HIDDEN);
    state->recording_wave_group =
        lv_obj_create(state->recording_overlay);
    lv_obj_remove_style_all(state->recording_wave_group);
    lv_obj_set_size(state->recording_wave_group, 82, 58);
    lv_obj_set_flex_flow(
        state->recording_wave_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        state->recording_wave_group,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(
        state->recording_wave_group, 6, 0);
    lv_obj_align(
        state->recording_wave_group, LV_ALIGN_CENTER, 0, -34);
    static const int heights[5] = {14, 22, 32, 22, 14};
    for (int index = 0; index < 5; ++index) {
        state->recording_wave_bars[index] = make_plain_obj(
            state->recording_wave_group, 6, heights[index],
            lv_color_hex(0xf4f5f7), LV_OPA_COVER, 3);
    }
    state->recording_title = make_label(
        state->recording_overlay, "LISTENING",
        FONT_UI, lv_color_hex(0xf4f5f7), 120,
        LV_TEXT_ALIGN_CENTER);
    lv_obj_align(
        state->recording_title, LV_ALIGN_CENTER, 0, 22);
    state->recording_hint = make_label(
        state->recording_overlay, "RELEASE SPACE TO SEND",
        FONT_UI, lv_color_hex(0x8b9098), 120,
        LV_TEXT_ALIGN_CENTER);
    lv_obj_align(
        state->recording_hint, LV_ALIGN_BOTTOM_MID, 0, -22);
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    state->card_setup_layer = lv_obj_create(screen);
    lv_obj_set_size(
        state->card_setup_layer, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_radius(state->card_setup_layer, 0, 0);
    lv_obj_set_style_bg_color(
        state->card_setup_layer, lv_color_hex(0x050608), 0);
    lv_obj_set_style_bg_opa(
        state->card_setup_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(state->card_setup_layer, 0, 0);
    lv_obj_add_flag(
        state->card_setup_layer, LV_OBJ_FLAG_HIDDEN);
    state->card_setup_title = make_label(
        state->card_setup_layer, "CONNECTION",
        FONT_UI, lv_color_hex(0xf4f5f7), 115,
        LV_TEXT_ALIGN_LEFT);
    lv_obj_align(
        state->card_setup_title, LV_ALIGN_TOP_LEFT, 10, 8);
    state->card_setup_field_label = make_label(
        state->card_setup_layer, "WI-FI SSID",
        FONT_UI, lv_color_hex(0x93c5fd), 115,
        LV_TEXT_ALIGN_LEFT);
    lv_obj_align(
        state->card_setup_field_label,
        LV_ALIGN_TOP_LEFT, 10, 34);
    state->card_setup_value = make_label(
        state->card_setup_layer, "_",
        FONT_UI, lv_color_hex(0xf4f5f7), 115,
        LV_TEXT_ALIGN_LEFT);
    lv_obj_align(
        state->card_setup_value, LV_ALIGN_TOP_LEFT, 10, 61);
    state->card_setup_hint = make_label(
        state->card_setup_layer,
        "Tab NEXT\nEnter SAVE\nFn+` CANCEL",
        FONT_UI, lv_color_hex(0x8a9099), 115,
        LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(
        state->card_setup_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(state->card_setup_hint, 58);
    lv_obj_align(
        state->card_setup_hint, LV_ALIGN_BOTTOM_LEFT, 10, -9);
#endif
    state->ready = true;
    vibe_ui_unlock(ui);
    return ESP_OK;
}

bool vibe_ui_ready(const vibe_ui_t *ui)
{
    return ui && const_impl(ui)->ready;
}

void vibe_ui_render(vibe_ui_t *ui,
                    const vibe_ui_view_model_t *view_model)
{
    if (!ui || !view_model || !vibe_ui_ready(ui)) {
        return;
    }
    vibe_ui_impl_t *state = impl(ui);
    vibe_ui_lock(ui);
    state->view = *view_model;
    lv_label_set_text(
        state->wifi_label,
        view_model->wifi_connected ? "WiFi" : "OFF");
    lv_obj_set_style_text_color(
        state->wifi_label,
        view_model->wifi_connected
            ? lv_color_hex(0xf3f4f6)
            : lv_color_hex(0x686e78),
        0);
    char ip[24] = {0};
    snprintf(
        ip, sizeof(ip), "IP %s",
        view_model->wifi_connected &&
                view_model->wifi_ip[0] != '\0'
            ? view_model->wifi_ip
            : "--");
    lv_label_set_text(state->ip_label, ip);
    set_battery(state);
    lv_label_set_text(
        state->mode_label, view_model->mode_label);
    lv_obj_set_style_text_color(
        state->mode_label,
        view_model->lift_mode
            ? provider_accent(view_model->provider)
            : lv_color_hex(0x8a9099),
        0);
    lv_label_set_text(
        state->intent_label, view_model->intent_label);
    lv_obj_set_style_text_color(
        state->intent_label,
        view_model->cyber_intent
            ? provider_accent(view_model->provider)
            : lv_color_hex(0x8a9099),
        0);
    lv_label_set_text(
        state->bridge_label, view_model->bridge_label);
    lv_obj_set_style_text_color(
        state->bridge_label,
        view_model->bridge_available
            ? lv_color_hex(0x8a9099)
            : lv_color_hex(0xc98484),
        0);
    update_pet(state);
    vibe_ui_unlock(ui);
}

void vibe_ui_register_activity(vibe_ui_t *ui)
{
    if (!ui || !vibe_ui_ready(ui)) return;
    vibe_ui_impl_t *state = impl(ui);
    vibe_ui_lock(ui);
    state->pet_idle_bob_steps_remaining =
        VIBE_UI_PET_IDLE_BOB_STEPS;
    if (state->pet_timer) {
        lv_timer_set_period(
            state->pet_timer, VIBE_UI_PET_ACTIVE_MS);
    }
    vibe_ui_unlock(ui);
}

void vibe_ui_complete_fast_resume(vibe_ui_t *ui)
{
    if (ui) {
        impl(ui)->view.pet_fast_resume_pending = false;
    }
}

void vibe_ui_request_preview_switch(vibe_ui_t *ui)
{
#if VIBE_STICK_ANIM_PREVIEW
    if (!ui || !vibe_ui_ready(ui)) return;
    impl(ui)->anim_switch_requested = true;
#else
    (void)ui;
#endif
}

static void visual_frames(
    vibe_ui_visual_t visual,
    const vibe_stick_pet_frame_id_t **frames,
    int *count)
{
    switch (visual) {
    case VIBE_UI_VISUAL_PTT:
        *frames = s_mode_ptt_frames;
        *count = sizeof(s_mode_ptt_frames) /
                 sizeof(s_mode_ptt_frames[0]);
        break;
    case VIBE_UI_VISUAL_LIFT:
        *frames = s_mode_lift_frames;
        *count = sizeof(s_mode_lift_frames) /
                 sizeof(s_mode_lift_frames[0]);
        break;
    case VIBE_UI_VISUAL_FORTUNE:
        *frames = s_mode_fortune_frames;
        *count = sizeof(s_mode_fortune_frames) /
                 sizeof(s_mode_fortune_frames[0]);
        break;
    case VIBE_UI_VISUAL_ALMANAC:
        *frames = s_mode_almanac_frames;
        *count = sizeof(s_mode_almanac_frames) /
                 sizeof(s_mode_almanac_frames[0]);
        break;
    case VIBE_UI_VISUAL_APPROVAL:
        *frames = s_pet_approval_frames;
        *count = sizeof(s_pet_approval_frames) /
                 sizeof(s_pet_approval_frames[0]);
        break;
    case VIBE_UI_VISUAL_DONE:
        *frames = s_pet_done_frames;
        *count = sizeof(s_pet_done_frames) /
                 sizeof(s_pet_done_frames[0]);
        break;
    case VIBE_UI_VISUAL_ERROR:
        *frames = s_pet_error_frames;
        *count = sizeof(s_pet_error_frames) /
                 sizeof(s_pet_error_frames[0]);
        break;
    case VIBE_UI_VISUAL_DICTATION:
    default:
        *frames = s_mode_dict_frames;
        *count = sizeof(s_mode_dict_frames) /
                 sizeof(s_mode_dict_frames[0]);
        break;
    }
}

void vibe_ui_show_visual(vibe_ui_t *ui,
                         const char *title,
                         const char *hint,
                         vibe_ui_visual_t visual,
                         uint32_t accent_rgb,
                         bool persistent)
{
    if (!ui || !title || !hint || !vibe_ui_ready(ui)) return;
    vibe_ui_impl_t *state = impl(ui);
    vibe_ui_lock(ui);
    visual_frames(
        visual, &state->mode_frames, &state->mode_frame_count);
    state->mode_frame_index = 0;
    state->mode_next_frame_ms = 0;
    state->mode_persistent = persistent;
    state->mode_until_ms =
        persistent ? 0
                   : esp_timer_get_time() / 1000 +
                         VIBE_UI_MODE_VISUAL_MS;
    lv_label_set_text(state->mode_switch_title, title);
    lv_label_set_text(state->mode_switch_hint, hint);
    lv_obj_set_style_text_color(
        state->mode_switch_title, lv_color_hex(accent_rgb), 0);
    lv_obj_clear_flag(
        state->mode_switch_layer, LV_OBJ_FLAG_HIDDEN);
    update_pet(state);
    vibe_ui_unlock(ui);
}

void vibe_ui_finish_visual(vibe_ui_t *ui)
{
    if (!ui) return;
    vibe_ui_lock(ui);
    finish_visual_locked(impl(ui));
    vibe_ui_unlock(ui);
}

void vibe_ui_show_recording(vibe_ui_t *ui,
                            const char *title,
                            const char *hint,
                            bool visible)
{
    if (!ui || !vibe_ui_ready(ui)) return;
    vibe_ui_impl_t *state = impl(ui);
    vibe_ui_lock(ui);
    if (visible) {
        if (title) lv_label_set_text(state->recording_title, title);
        if (hint) {
            lv_label_set_text(state->recording_hint, hint);
            if (hint[0] == '\0') {
                lv_obj_add_flag(
                    state->recording_hint, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(
                    state->recording_hint, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_clear_flag(
            state->recording_overlay, LV_OBJ_FLAG_HIDDEN);
        start_wave(state);
    } else {
        stop_wave(state);
        lv_obj_add_flag(
            state->recording_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    state->recording_visible = visible;
    vibe_ui_unlock(ui);
}

bool vibe_ui_recording_visible(const vibe_ui_t *ui)
{
    return ui && const_impl(ui)->recording_visible;
}

esp_err_t vibe_ui_set_rendering_suspended(vibe_ui_t *ui,
                                          bool suspended)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    vibe_ui_impl_t *state = impl(ui);
    if (!state->panel || !state->tick_timer ||
        atomic_load(&state->rendering_suspended) == suspended) {
        return ESP_OK;
    }
    vibe_ui_lock(ui);
    if (suspended) {
        atomic_store(&state->rendering_suspended, true);
        if (state->pet_timer) lv_timer_pause(state->pet_timer);
        if (state->tick_running) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                esp_timer_stop(state->tick_timer));
            state->tick_running = false;
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_lcd_panel_disp_on_off(state->panel, false));
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_lcd_panel_disp_on_off(state->panel, true));
        if (!state->tick_running) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                esp_timer_start_periodic(
                    state->tick_timer,
                    LVGL_TICK_PERIOD_MS * 1000));
            state->tick_running = true;
        }
        if (state->pet_timer) lv_timer_resume(state->pet_timer);
        atomic_store(&state->rendering_suspended, false);
        lv_obj_invalidate(
            lv_display_get_screen_active(state->display));
    }
    vibe_ui_unlock(ui);
    if (!suspended && state->task) {
        xTaskNotifyGive(state->task);
    }
    return ESP_OK;
}

bool vibe_ui_rendering_suspended(const vibe_ui_t *ui)
{
    return ui &&
           atomic_load(&const_impl(ui)->rendering_suspended);
}

esp_err_t vibe_ui_set_landscape(vibe_ui_t *ui, bool landscape)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    vibe_ui_impl_t *state = impl(ui);
    ESP_RETURN_ON_FALSE(
        state->panel && state->display,
        ESP_ERR_INVALID_STATE, TAG, "display unavailable");
    if (landscape) {
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_swap_xy(state->panel, true),
            TAG, "panel swap xy");
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_mirror(state->panel, true, false),
            TAG, "panel mirror");
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_set_gap(state->panel, 40, 53),
            TAG, "panel gap");
        lv_display_set_resolution(state->display, 240, 135);
    } else {
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_swap_xy(state->panel, false),
            TAG, "home panel swap xy");
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_mirror(state->panel, false, false),
            TAG, "home panel mirror");
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_set_gap(
                state->panel, LCD_X_GAP, LCD_Y_GAP),
            TAG, "home panel gap");
        lv_display_set_resolution(
            state->display, LCD_H_RES, LCD_V_RES);
    }
    lv_obj_invalidate(lv_screen_active());
    return ESP_OK;
}

void vibe_ui_card_setup(vibe_ui_t *ui,
                        const char *title,
                        const char *field,
                        const char *value,
                        const char *hint,
                        bool error,
                        bool visible)
{
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    if (!ui || !vibe_ui_ready(ui)) return;
    vibe_ui_impl_t *state = impl(ui);
    vibe_ui_lock(ui);
    if (title) lv_label_set_text(state->card_setup_title, title);
    if (field) {
        lv_label_set_text(state->card_setup_field_label, field);
    }
    if (value) lv_label_set_text(state->card_setup_value, value);
    if (hint) {
        lv_label_set_text(state->card_setup_hint, hint);
        lv_obj_set_style_text_color(
            state->card_setup_hint,
            error ? lv_color_hex(0xfca5a5)
                  : lv_color_hex(0x8a9099),
            0);
    }
    if (visible) {
        lv_obj_move_foreground(state->card_setup_layer);
        lv_obj_clear_flag(
            state->card_setup_layer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(
            state->card_setup_layer, LV_OBJ_FLAG_HIDDEN);
    }
    vibe_ui_unlock(ui);
#else
    (void)ui;
    (void)title;
    (void)field;
    (void)value;
    (void)hint;
    (void)error;
    (void)visible;
#endif
}
