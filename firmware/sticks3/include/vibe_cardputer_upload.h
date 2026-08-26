#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef esp_err_t (*vibe_cardputer_upload_post_fn)(const uint8_t *audio,
                                                    size_t audio_len,
                                                    uint32_t chunk_id,
                                                    void *context);

esp_err_t vibe_cardputer_upload_init(void);
bool vibe_cardputer_upload_start(vibe_cardputer_upload_post_fn post_chunk,
                                 void *context);
void vibe_cardputer_upload_wait(void);
bool vibe_cardputer_upload_failed(void);
void vibe_cardputer_upload_totals(size_t *posts, size_t *bytes);
size_t vibe_cardputer_upload_wire_bytes(void);
