#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#define VIBE_STICK_SVG_SPINNER_WIDTH 112
#define VIBE_STICK_SVG_SPINNER_HEIGHT 112
#define VIBE_STICK_SVG_SPINNER_PIXEL_BYTES 25088
#define VIBE_STICK_SVG_SPINNER_FRAME_COUNT 60

extern lv_image_dsc_t vibe_stick_svg_spinner_image;

bool vibe_stick_svg_spinner_decode_frame(int frame_id, uint8_t *dest, size_t dest_size);
void vibe_stick_svg_spinner_set_image_data(uint8_t *data);
