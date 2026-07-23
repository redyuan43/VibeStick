#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool paired;
    bool pairing;
    bool hid_connected;
    bool hfp_connected;
    bool audio_connected;
    bool wideband;
} vibe_bt_composite_state_t;

typedef size_t (*vibe_bt_pcm_read_fn)(uint8_t *buffer, size_t length,
                                      void *context);
typedef void (*vibe_bt_state_callback_t)(
    const vibe_bt_composite_state_t *state, void *context);

esp_err_t vibe_bt_composite_init(vibe_bt_state_callback_t state_callback,
                                 void *context);
void vibe_bt_composite_set_pcm_reader(vibe_bt_pcm_read_fn reader,
                                      void *context);
esp_err_t vibe_bt_composite_begin_pairing(void);
esp_err_t vibe_bt_composite_end_pairing(void);
esp_err_t vibe_bt_composite_clear_bonds(void);
esp_err_t vibe_bt_composite_request_reconnect(void);
vibe_bt_composite_state_t vibe_bt_composite_state(void);
esp_err_t vibe_bt_composite_send_right_shift(bool pressed);
esp_err_t vibe_bt_composite_send_enter(bool pressed);
esp_err_t vibe_bt_composite_send_mouse(int8_t dx, int8_t dy,
                                       bool left_pressed);
