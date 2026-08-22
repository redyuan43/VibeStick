#include "vibe_cardputer_input_profile.h"

#include <math.h>
#include <string.h>

#include "nvs.h"

#define CARD_INPUT_PROFILE_NAMESPACE "vibe_prefs"
#define CARD_INPUT_PROFILE_KEY "card_input_v1"
#define CARD_INPUT_PROFILE_MAGIC 0x56434950u
#define CARD_INPUT_PROFILE_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    vibe_card_input_profile_t profile;
} stored_profile_t;

void vibe_card_input_profile_default(vibe_card_input_profile_t *profile)
{
    if (!profile) return;
    *profile = (vibe_card_input_profile_t){
        .revision = 2,
        .opt_tap = VIBE_CARD_ROUTE_DEVICE_RECORDING_TOGGLE,
        .opt_double = VIBE_CARD_ROUTE_NONE,
        .opt_hold = VIBE_CARD_ROUTE_DEVICE_RECORDING_HOLD,
        .air_mouse = {
            .pointer_speed = 1.0f,
            .wheel_speed = 1.0f,
            .pointer_deadzone_dps = 2.5f,
            .wheel_deadzone_dps = 5.0f,
        },
    };
}

static bool route_valid(vibe_card_input_route_t route)
{
    return route >= VIBE_CARD_ROUTE_NONE &&
           route <= VIBE_CARD_ROUTE_DEVICE_LEGACY_DOUBLE;
}

bool vibe_card_input_profile_valid(const vibe_card_input_profile_t *profile)
{
    if (!profile || profile->revision == 0 || !route_valid(profile->opt_tap) ||
        !route_valid(profile->opt_double) || !route_valid(profile->opt_hold)) return false;
    const vibe_card_air_mouse_settings_t *mouse = &profile->air_mouse;
    return isfinite(mouse->pointer_speed) && mouse->pointer_speed >= 0.5f &&
           mouse->pointer_speed <= 2.5f && isfinite(mouse->wheel_speed) &&
           mouse->wheel_speed >= 0.5f && mouse->wheel_speed <= 2.0f &&
           isfinite(mouse->pointer_deadzone_dps) && mouse->pointer_deadzone_dps >= 1.0f &&
           mouse->pointer_deadzone_dps <= 6.0f && isfinite(mouse->wheel_deadzone_dps) &&
           mouse->wheel_deadzone_dps >= 2.0f && mouse->wheel_deadzone_dps <= 10.0f;
}

esp_err_t vibe_card_input_profile_load(vibe_card_input_profile_t *profile)
{
    if (!profile) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CARD_INPUT_PROFILE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    stored_profile_t stored = {0};
    size_t size = sizeof(stored);
    err = nvs_get_blob(handle, CARD_INPUT_PROFILE_KEY, &stored, &size);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    if (size != sizeof(stored) || stored.magic != CARD_INPUT_PROFILE_MAGIC ||
        stored.version != CARD_INPUT_PROFILE_VERSION || stored.size != sizeof(stored) ||
        !vibe_card_input_profile_valid(&stored.profile)) return ESP_ERR_INVALID_VERSION;
    *profile = stored.profile;
    return ESP_OK;
}

esp_err_t vibe_card_input_profile_save(const vibe_card_input_profile_t *profile)
{
    if (!vibe_card_input_profile_valid(profile)) return ESP_ERR_INVALID_ARG;
    stored_profile_t stored = {
        .magic = CARD_INPUT_PROFILE_MAGIC,
        .version = CARD_INPUT_PROFILE_VERSION,
        .size = sizeof(stored_profile_t),
        .profile = *profile,
    };
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CARD_INPUT_PROFILE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, CARD_INPUT_PROFILE_KEY, &stored, sizeof(stored));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

const char *vibe_card_input_route_name(vibe_card_input_route_t route)
{
    switch (route) {
    case VIBE_CARD_ROUTE_NONE: return "none";
    case VIBE_CARD_ROUTE_HOST: return "host";
    case VIBE_CARD_ROUTE_DEVICE_RECORDING_TOGGLE: return "device.recording.toggle";
    case VIBE_CARD_ROUTE_DEVICE_RECORDING_HOLD: return "device.recording.hold";
    case VIBE_CARD_ROUTE_DEVICE_LEGACY_DOUBLE: return "device.legacy_double";
    default: return "none";
    }
}

bool vibe_card_input_route_parse(const char *name, vibe_card_input_route_t *route)
{
    if (!name || !route) return false;
    for (int value = VIBE_CARD_ROUTE_NONE; value <= VIBE_CARD_ROUTE_DEVICE_LEGACY_DOUBLE; ++value) {
        if (strcmp(name, vibe_card_input_route_name(value)) == 0) {
            *route = (vibe_card_input_route_t)value;
            return true;
        }
    }
    return false;
}
