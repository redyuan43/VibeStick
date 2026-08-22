#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "vibe_app_state.h"

#define VIBE_UI_STORAGE_BYTES 1536

typedef union {
    max_align_t alignment;
    unsigned char bytes[VIBE_UI_STORAGE_BYTES];
} vibe_ui_t;

typedef enum {
    VIBE_UI_VISUAL_PTT,
    VIBE_UI_VISUAL_LIFT,
    VIBE_UI_VISUAL_DICTATION,
    VIBE_UI_VISUAL_FORTUNE,
    VIBE_UI_VISUAL_ALMANAC,
    VIBE_UI_VISUAL_APPROVAL,
    VIBE_UI_VISUAL_DONE,
    VIBE_UI_VISUAL_ERROR,
} vibe_ui_visual_t;

typedef struct {
    bool wifi_connected;
    bool battery_valid;
    int battery;
    bool battery_charging;
    bool usb_powered;
    bool lift_mode;
    bool cyber_intent;
    bool bridge_available;
    bool display_active;
    bool pet_fast_resume_pending;
    int64_t pet_animation_resume_ms;
    vibe_provider_id_t provider;
    char wifi_ip[16];
    char mode_label[8];
    char intent_label[8];
    char bridge_label[40];
    char provider_status[24];
    char alert_type[24];
} vibe_ui_view_model_t;

typedef void (*vibe_ui_brightness_changed_fn)(
    uint8_t brightness,
    void *context);
typedef bool (*vibe_ui_external_powered_fn)(void *context);

typedef struct {
    vibe_ui_brightness_changed_fn brightness_changed;
    vibe_ui_external_powered_fn external_powered;
    void *context;
} vibe_ui_dependencies_t;

esp_err_t vibe_ui_init(vibe_ui_t *ui,
                       const vibe_ui_dependencies_t *dependencies);
esp_err_t vibe_ui_create(vibe_ui_t *ui);
void vibe_ui_lock(vibe_ui_t *ui);
void vibe_ui_unlock(vibe_ui_t *ui);
bool vibe_ui_ready(const vibe_ui_t *ui);
void vibe_ui_render(vibe_ui_t *ui,
                    const vibe_ui_view_model_t *view_model);
void vibe_ui_register_activity(vibe_ui_t *ui);
void vibe_ui_complete_fast_resume(vibe_ui_t *ui);
void vibe_ui_request_preview_switch(vibe_ui_t *ui);
void vibe_ui_show_visual(vibe_ui_t *ui,
                         const char *title,
                         const char *hint,
                         vibe_ui_visual_t visual,
                         uint32_t accent_rgb,
                         bool persistent);
void vibe_ui_finish_visual(vibe_ui_t *ui);
void vibe_ui_show_recording(vibe_ui_t *ui,
                            const char *title,
                            const char *hint,
                            bool visible);
bool vibe_ui_recording_visible(const vibe_ui_t *ui);
void vibe_ui_set_backlight(vibe_ui_t *ui, uint8_t brightness);
uint8_t vibe_ui_backlight(const vibe_ui_t *ui);
esp_err_t vibe_ui_set_rendering_suspended(vibe_ui_t *ui,
                                          bool suspended);
bool vibe_ui_rendering_suspended(const vibe_ui_t *ui);
esp_err_t vibe_ui_set_landscape(vibe_ui_t *ui, bool landscape);
void vibe_ui_card_setup(vibe_ui_t *ui,
                        const char *title,
                        const char *field,
                        const char *value,
                        const char *hint,
                        bool error,
                        bool visible);
