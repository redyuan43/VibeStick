#pragma once

#include <stddef.h>
#include <stdint.h>

#define VIBE_MINIJOY_PET_WIDTH 80
#define VIBE_MINIJOY_PET_HEIGHT 80

typedef enum {
    VIBE_MINIJOY_PET_FRAME_IDLE,
    VIBE_MINIJOY_PET_FRAME_BLINK_LEFT,
    VIBE_MINIJOY_PET_FRAME_BLINK_RIGHT,
    VIBE_MINIJOY_PET_FRAME_BLINK_BOTH,
    VIBE_MINIJOY_PET_FRAME_ATTENTION,
    VIBE_MINIJOY_PET_FRAME_HAPPY,
    VIBE_MINIJOY_PET_FRAME_ERROR,
    VIBE_MINIJOY_PET_FRAME_COUNT,
} vibe_minijoy_pet_frame_id_t;

typedef struct {
    const uint8_t *data;
    size_t size;
} vibe_minijoy_pet_rle_frame_t;

const vibe_minijoy_pet_rle_frame_t *vibe_minijoy_pet_frame(
    vibe_minijoy_pet_frame_id_t frame_id);
