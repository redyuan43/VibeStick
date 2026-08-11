#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define VIBE_POINTER_BUTTON_LEFT 0x01
#define VIBE_POINTER_BUTTON_RIGHT 0x02

typedef esp_err_t (*vibe_pointer_post_fn)(const char *path,
                                          const char *body,
                                          char *response,
                                          size_t response_len,
                                          int timeout_ms,
                                          void *context);
typedef bool (*vibe_pointer_online_fn)(void *context);

typedef struct {
    const char *path;
    int timeout_ms;
    int heartbeat_ms;
    vibe_pointer_post_fn post;
    vibe_pointer_online_fn online;
    void *context;
} vibe_pointer_client_config_t;

esp_err_t vibe_pointer_client_init(
    const vibe_pointer_client_config_t *config);
void vibe_pointer_client_report(int16_t dx, int16_t dy, int16_t wheel);
void vibe_pointer_client_set_button(uint8_t mask, bool pressed);
void vibe_pointer_client_release_all(void);
uint8_t vibe_pointer_client_buttons(void);
