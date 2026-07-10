#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#define VIBE_STICK_ANIM_WIDTH 112
#define VIBE_STICK_ANIM_HEIGHT 112
#define VIBE_STICK_ANIM_PIXEL_BYTES (VIBE_STICK_ANIM_WIDTH * VIBE_STICK_ANIM_HEIGHT * 2)

extern lv_image_dsc_t vibe_stick_anim_image;

bool vibe_stick_anim_assets_init(void);
int vibe_stick_anim_asset_count(void);
int vibe_stick_anim_frame_count(int asset_id);
int vibe_stick_anim_fps(void);
const char *vibe_stick_anim_asset_name(int asset_id);
bool vibe_stick_anim_decode_frame(int asset_id, int frame_id, uint8_t *dest, size_t dest_size);
void vibe_stick_anim_set_image_data(uint8_t *data);
