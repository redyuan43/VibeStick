#include <assert.h>
#include <stdio.h>

#include "vibe_power_runtime.h"

static bool no_active_work(void *context)
{
    (void)context;
    return false;
}

static bool no_false_wake(void *context)
{
    (void)context;
    return false;
}

int main(void)
{
    vibe_power_runtime_t runtime;
    const vibe_power_runtime_config_t config = {
        .dim_after_ms = 30000,
        .off_after_ms = 60000,
        .usb_unplug_hold_ms = 30000,
        .wake_stabilize_ms = 5000,
    };
    const vibe_power_runtime_dependencies_t dependencies = {
        .active_work = no_active_work,
        .false_wake_due = no_false_wake,
    };
    vibe_power_runtime_init(&runtime, &config, &dependencies, 80);

    vibe_power_runtime_command_t command = {
        .type = VIBE_POWER_COMMAND_ACTIVITY,
        .data.time_ms = 100,
    };
    assert(vibe_power_runtime_handle(&runtime, &command));
    vibe_power_runtime_tick(&runtime, 30100);
    assert(runtime.state.display_state == DISPLAY_POWER_DIMMED);
    vibe_power_runtime_tick(&runtime, 60100);
    assert(runtime.state.display_state == DISPLAY_POWER_OFF);

    command.type = VIBE_POWER_COMMAND_SET_WAKE_TIME;
    command.data.time_ms = 1000;
    assert(vibe_power_runtime_handle(&runtime, &command));
    command.type = VIBE_POWER_COMMAND_RECORD_BATTERY;
    command.data.battery.raw_level = 80;
    command.data.battery.now_ms = 1100;
    command.data.battery.woke_from_deep_sleep = true;
    command.data.battery.retained_valid = true;
    command.data.battery.retained_level = 90;
    assert(vibe_power_runtime_handle(&runtime, &command));
    assert(runtime.state.battery_display_level == 90);

    command.data.battery.raw_level = 50;
    command.data.battery.now_ms = 2000;
    assert(vibe_power_runtime_handle(&runtime, &command));
    assert(runtime.state.battery_display_level == 90);

    command.data.battery.raw_level = 50;
    command.data.battery.now_ms = 7000;
    assert(vibe_power_runtime_handle(&runtime, &command));
    assert(runtime.state.battery_display_level == 89);

    puts("vibestick power runtime tests passed");
    return 0;
}
