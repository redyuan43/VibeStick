#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "vibe_keyboard.h"

typedef esp_err_t (*vibe_cardputer_message_request_fn)(
    const char *method,
    const char *path,
    const char *body,
    char *response,
    size_t response_len);

typedef esp_err_t (*vibe_cardputer_message_download_fn)(
    const char *path,
    const char *destination,
    size_t maximum_size);

typedef struct {
    vibe_cardputer_message_request_fn request;
    vibe_cardputer_message_download_fn download;
    void (*display_lock)(void);
    void (*display_unlock)(void);
    esp_err_t (*set_landscape)(bool landscape);
    void (*restore_home)(void);
    void (*activity)(void);
    bool (*audio_busy)(void);
} vibe_cardputer_messages_config_t;

esp_err_t vibe_cardputer_messages_init(
    const vibe_cardputer_messages_config_t *config);
esp_err_t vibe_cardputer_messages_start(void);
bool vibe_cardputer_messages_handle_key(const vibe_key_event_t *event);
bool vibe_cardputer_messages_active(void);
bool vibe_cardputer_messages_storage_ready(void);
void vibe_cardputer_messages_release_display_resources(void);
