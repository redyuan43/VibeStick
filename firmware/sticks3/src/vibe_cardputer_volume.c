#include "vibe_cardputer_volume.h"

#include <stdatomic.h>
#include <stdio.h>

#include "esp_check.h"
#include "esp_log.h"
#include "lvgl.h"
#include "nvs.h"
#include "vibe_audio.h"

#if defined(VIBE_BOARD_CARDPUTER_ADV)

#define VOLUME_NAMESPACE "vibe_prefs"
#define VOLUME_KEY "spk_vol"
#define VOLUME_DEFAULT 70
#define VOLUME_STEP 10
#define VOLUME_VISIBLE_MS 1500

static const char *TAG = "card_volume";
static vibe_cardputer_volume_config_t s_config;
static atomic_uchar s_volume;
static lv_obj_t *s_overlay;
static lv_obj_t *s_label;
static lv_obj_t *s_bar;
static lv_timer_t *s_hide_timer;

static void display_lock(void)
{
    if (s_config.display_lock) s_config.display_lock();
}

static void display_unlock(void)
{
    if (s_config.display_unlock) s_config.display_unlock();
}

static void hide_volume_timer(lv_timer_t *timer)
{
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
}

static void ensure_overlay(void)
{
    if (s_overlay) return;
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, 120, 58);
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_hex(0x38bdf8), 0);
    lv_obj_set_style_border_width(s_overlay, 1, 0);
    lv_obj_set_style_radius(s_overlay, 7, 0);
    lv_obj_set_style_pad_all(s_overlay, 8, 0);

    s_label = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0xf3f4f6), 0);
    lv_obj_align(s_label, LV_ALIGN_TOP_MID, 0, -1);

    s_bar = lv_bar_create(s_overlay);
    lv_obj_set_size(s_bar, 96, 8);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x374151), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x38bdf8),
                              LV_PART_INDICATOR);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_hide_timer = lv_timer_create(hide_volume_timer, VOLUME_VISIBLE_MS, NULL);
    lv_timer_pause(s_hide_timer);
}

static void show_volume(uint8_t volume)
{
    display_lock();
    ensure_overlay();
    char text[20];
    snprintf(text, sizeof(text), volume == 0 ? "MUTE  0%%" : "VOL  %u%%",
             (unsigned)volume);
    lv_label_set_text(s_label, text);
    lv_bar_set_value(s_bar, volume, LV_ANIM_OFF);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    lv_timer_reset(s_hide_timer);
    lv_timer_resume(s_hide_timer);
    display_unlock();
}

static void save_volume(uint8_t volume)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(VOLUME_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_u8(handle, VOLUME_KEY, volume);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "volume save failed: %s", esp_err_to_name(err));
    }
}

esp_err_t vibe_cardputer_volume_init(
    const vibe_cardputer_volume_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "missing config");
    s_config = *config;
    uint8_t volume = VOLUME_DEFAULT;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(VOLUME_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        esp_err_t read_err = nvs_get_u8(handle, VOLUME_KEY, &volume);
        nvs_close(handle);
        if (read_err != ESP_OK || volume > 100) volume = VOLUME_DEFAULT;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "volume restore failed: %s", esp_err_to_name(err));
    }
    atomic_store(&s_volume, volume);
    ESP_RETURN_ON_ERROR(vibe_audio_set_output_volume(volume), TAG,
                        "set initial volume");
    ESP_LOGI(TAG, "restored volume=%u%%", (unsigned)volume);
    return ESP_OK;
}

bool vibe_cardputer_volume_handle_key(const vibe_key_event_t *event)
{
    if (!event || !event->fn || event->row != 0 ||
        (event->column != 11 && event->column != 12)) {
        return false;
    }
    if (!event->pressed) return true;
    int volume = atomic_load(&s_volume);
    volume += event->column == 12 ? VOLUME_STEP : -VOLUME_STEP;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    atomic_store(&s_volume, (uint8_t)volume);
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_audio_set_output_volume((uint8_t)volume));
    if (s_config.activity) s_config.activity();
    show_volume((uint8_t)volume);
    save_volume((uint8_t)volume);
    ESP_LOGI(TAG, "volume=%d%%", volume);
    return true;
}

#else

esp_err_t vibe_cardputer_volume_init(
    const vibe_cardputer_volume_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

bool vibe_cardputer_volume_handle_key(const vibe_key_event_t *event)
{
    (void)event;
    return false;
}

#endif
