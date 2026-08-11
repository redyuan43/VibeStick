#include "vibe_settings.h"

#include <stddef.h>

static const uint8_t SLEEP_MINUTES[] = {
    1,
    2,
    5,
    10,
    VIBE_SETTINGS_SLEEP_DISABLED_MINUTES,
};

bool vibe_settings_sleep_minutes_valid(uint8_t minutes)
{
    for (size_t i = 0; i < sizeof(SLEEP_MINUTES) / sizeof(SLEEP_MINUTES[0]); ++i) {
        if (SLEEP_MINUTES[i] == minutes) {
            return true;
        }
    }
    return false;
}

uint8_t vibe_settings_sleep_minutes_sanitize(uint8_t minutes)
{
    return vibe_settings_sleep_minutes_valid(minutes)
               ? minutes
               : VIBE_SETTINGS_DEFAULT_SLEEP_MINUTES;
}

uint8_t vibe_settings_next_sleep_minutes(uint8_t minutes)
{
    minutes = vibe_settings_sleep_minutes_sanitize(minutes);
    for (size_t i = 0; i < sizeof(SLEEP_MINUTES) / sizeof(SLEEP_MINUTES[0]); ++i) {
        if (SLEEP_MINUTES[i] == minutes) {
            return SLEEP_MINUTES[(i + 1) %
                                 (sizeof(SLEEP_MINUTES) / sizeof(SLEEP_MINUTES[0]))];
        }
    }
    return VIBE_SETTINGS_DEFAULT_SLEEP_MINUTES;
}

int64_t vibe_settings_sleep_timeout_ms(uint8_t minutes)
{
    return (int64_t)vibe_settings_sleep_minutes_sanitize(minutes) * 60 * 1000;
}

vibe_settings_page_t vibe_settings_next_page(vibe_settings_page_t page)
{
    if (page < VIBE_SETTINGS_PAGE_MODE || page >= VIBE_SETTINGS_PAGE_COUNT) {
        return VIBE_SETTINGS_PAGE_MODE;
    }
    return (vibe_settings_page_t)((page + 1) % VIBE_SETTINGS_PAGE_COUNT);
}
