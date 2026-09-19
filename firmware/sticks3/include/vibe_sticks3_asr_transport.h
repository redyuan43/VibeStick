#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* One recording owns one HTTP client. Responses are consumed before reuse. */
esp_err_t sticks3_transport_open(void);
void sticks3_transport_close(void);
esp_err_t sticks3_transport_start(const char *session);
esp_err_t sticks3_transport_audio(const char *session, const uint8_t *audio,
                                 size_t length, uint32_t chunk);
esp_err_t sticks3_transport_stop(const char *session, size_t posts,
                                size_t pcm_bytes, bool failed);
unsigned sticks3_transport_retries(void);
