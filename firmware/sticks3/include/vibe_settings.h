#pragma once

#include <stdbool.h>
#include <stdint.h>

#define VIBE_SETTINGS_DEFAULT_SLEEP_MINUTES 5

typedef enum {
    VIBE_SETTINGS_PAGE_MODE = 0,
    VIBE_SETTINGS_PAGE_SLEEP,
    VIBE_SETTINGS_PAGE_VERSION,
    VIBE_SETTINGS_PAGE_COUNT,
} vibe_settings_page_t;

bool vibe_settings_sleep_minutes_valid(uint8_t minutes);
uint8_t vibe_settings_sleep_minutes_sanitize(uint8_t minutes);
uint8_t vibe_settings_next_sleep_minutes(uint8_t minutes);
int64_t vibe_settings_sleep_timeout_ms(uint8_t minutes);
vibe_settings_page_t vibe_settings_next_page(vibe_settings_page_t page);
