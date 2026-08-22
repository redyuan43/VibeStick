#include <assert.h>
#include <stdio.h>

#include "vibe_motion_controller.h"

int main(void)
{
    vibe_motion_controller_t controller;
    const vibe_motion_controller_config_t config = {
        .calibration_timeout_ms = 15000,
        .wake_confirm_ms = 500,
        .network_timeout_ms = 15000,
    };
    const vibe_motion_controller_dependencies_t dependencies = {0};
    vibe_motion_controller_init(
        &controller, &config, &dependencies);

    vibe_motion_controller_command_t command = {
        .type = VIBE_MOTION_COMMAND_BEGIN_CALIBRATION,
        .data.calibration = {
            .now_ms = 100,
            .had_previous = true,
        },
    };
    assert(vibe_motion_controller_handle(&controller, &command));
    vibe_motion_controller_tick(&controller, 15099);
    assert(!controller.state.calibration_timeout_due);
    vibe_motion_controller_tick(&controller, 15100);
    assert(controller.state.calibration_timeout_due);

    command.type = VIBE_MOTION_COMMAND_BEGIN_WAKE_CONFIRM;
    command.data.now_ms = 200;
    assert(vibe_motion_controller_handle(&controller, &command));
    vibe_motion_controller_tick(&controller, 700);
    assert(controller.state.wake_confirm_due);

    command.type = VIBE_MOTION_COMMAND_BEGIN_NETWORK_WAIT;
    command.data.now_ms = 1000;
    assert(vibe_motion_controller_handle(&controller, &command));
    assert(controller.state.start_pending);
    vibe_motion_controller_tick(&controller, 16000);
    assert(controller.state.network_timeout_due);

    command.type = VIBE_MOTION_COMMAND_CANCEL_PENDING;
    assert(vibe_motion_controller_handle(&controller, &command));
    assert(!controller.state.start_pending);
    assert(!controller.state.wake_network_pending);

    puts("vibestick motion controller tests passed");
    return 0;
}
