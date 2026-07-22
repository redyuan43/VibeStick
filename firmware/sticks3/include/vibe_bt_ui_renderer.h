#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vibe_minijoy_pet_assets.h"

typedef struct {
    uint16_t *pixels;
    int width;
    int height;
    int stride;
} vibe_bt_ui_surface_t;

bool vibe_bt_ui_surface_init(vibe_bt_ui_surface_t *surface, uint16_t *pixels,
                             size_t capacity_pixels, int width, int height);
void vibe_bt_ui_surface_clear(vibe_bt_ui_surface_t *surface, uint16_t color);
void vibe_bt_ui_surface_fill_rect(vibe_bt_ui_surface_t *surface, int x, int y,
                                  int width, int height, uint16_t color);
int vibe_bt_ui_text_width(const char *text, int scale);
void vibe_bt_ui_surface_draw_text(vibe_bt_ui_surface_t *surface, int x, int y,
                                  const char *text, uint16_t color, int scale);
bool vibe_bt_ui_surface_draw_pet(vibe_bt_ui_surface_t *surface, int x, int y,
                                 vibe_minijoy_pet_frame_id_t frame_id);
uint8_t vibe_bt_ui_smooth_audio_level(uint8_t current, uint8_t target);
void vibe_bt_ui_wave_heights(uint8_t level, int heights[5]);
