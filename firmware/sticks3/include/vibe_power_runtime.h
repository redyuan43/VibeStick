#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIBE_POWER_RUNTIME_MAX_SAMPLES 5

typedef enum {
    DISPLAY_POWER_ACTIVE,
    DISPLAY_POWER_DIMMED,
    DISPLAY_POWER_OFF,
} vibe_power_display_state_t;

typedef bool (*vibe_power_runtime_predicate_fn)(void *context);

typedef struct {
    int64_t dim_after_ms;
    int64_t off_after_ms;
    int64_t usb_unplug_hold_ms;
    int64_t wake_stabilize_ms;
} vibe_power_runtime_config_t;

typedef struct {
    vibe_power_runtime_predicate_fn active_work;
    vibe_power_runtime_predicate_fn false_wake_due;
    void *context;
} vibe_power_runtime_dependencies_t;

typedef enum {
    VIBE_POWER_COMMAND_ACTIVITY,
    VIBE_POWER_COMMAND_SET_WAKE_TIME,
    VIBE_POWER_COMMAND_SET_EXTERNAL_POWER_REMOVED_TIME,
    VIBE_POWER_COMMAND_SET_FULL_LATCH,
    VIBE_POWER_COMMAND_RECORD_BATTERY,
    VIBE_POWER_COMMAND_SET_VOLTAGE,
    VIBE_POWER_COMMAND_SET_CHARGING,
    VIBE_POWER_COMMAND_SET_USB_POWERED,
    VIBE_POWER_COMMAND_SET_BACKLIGHT,
} vibe_power_runtime_command_type_t;

typedef struct {
    vibe_power_runtime_command_type_t type;
    union {
        int64_t time_ms;
        bool flag;
        int value;
        uint8_t brightness;
        struct {
            int raw_level;
            int64_t now_ms;
            bool woke_from_deep_sleep;
            bool retained_valid;
            int retained_level;
        } battery;
    } data;
} vibe_power_runtime_command_t;

typedef struct {
    int64_t last_activity_ms;
    int64_t last_backlight_fade_ms;
    int64_t last_power_status_poll_ms;
    int64_t deep_sleep_wake_ms;
    int64_t external_power_removed_ms;
    int battery_samples[VIBE_POWER_RUNTIME_MAX_SAMPLES];
    size_t battery_sample_count;
    size_t battery_sample_index;
    bool battery_display_valid;
    bool battery_full_latched;
    int battery_display_level;
    int battery_raw_level;
    int battery_voltage_mv;
    bool charging;
    bool usb_powered;
    uint8_t current_backlight;
    vibe_power_display_state_t display_state;
} vibe_power_runtime_snapshot_t;

typedef struct {
    vibe_power_runtime_config_t config;
    vibe_power_runtime_dependencies_t dependencies;
    vibe_power_runtime_snapshot_t state;
} vibe_power_runtime_t;

void vibe_power_runtime_init(
    vibe_power_runtime_t *runtime,
    const vibe_power_runtime_config_t *config,
    const vibe_power_runtime_dependencies_t *dependencies,
    uint8_t initial_backlight);
bool vibe_power_runtime_handle(
    vibe_power_runtime_t *runtime,
    const vibe_power_runtime_command_t *command);
void vibe_power_runtime_tick(vibe_power_runtime_t *runtime,
                             int64_t now_ms);
void vibe_power_runtime_snapshot(
    const vibe_power_runtime_t *runtime,
    vibe_power_runtime_snapshot_t *snapshot);
