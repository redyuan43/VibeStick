#include <assert.h>
#include <stdio.h>

#include "vibe_input_router.h"

int main(void)
{
    assert(vibe_input_router_command(VIBE_INPUT_SIGNAL_FRONT_SINGLE) ==
           VIBE_STICK_EVENT_RECORDING_TOGGLE);
    assert(vibe_input_router_command(VIBE_INPUT_SIGNAL_FRONT_DOUBLE) ==
           VIBE_STICK_EVENT_DOUBLE_CLICK);
    assert(vibe_input_router_command(VIBE_INPUT_SIGNAL_FRONT_LONG_START) ==
           VIBE_STICK_EVENT_LONG_START);
    assert(vibe_input_router_command(VIBE_INPUT_SIGNAL_FRONT_LONG_STOP) ==
           VIBE_STICK_EVENT_LONG_STOP);
    assert(vibe_input_router_command(VIBE_INPUT_SIGNAL_SIDE_MODE) ==
           VIBE_STICK_EVENT_RECORDING_MODE_TOGGLE);
    assert(vibe_input_router_command(VIBE_INPUT_SIGNAL_SIDE_CALIBRATE) ==
           VIBE_STICK_EVENT_MOTION_CALIBRATE);
    assert(vibe_input_router_command(VIBE_INPUT_SIGNAL_SIDE_SCAN) ==
           VIBE_STICK_EVENT_BRIDGE_SCAN_FULL);
    assert(vibe_input_router_command(VIBE_INPUT_SIGNAL_MOTION_LIFTED) ==
           VIBE_STICK_EVENT_MOTION_START);
    assert(vibe_input_router_command(VIBE_INPUT_SIGNAL_MOTION_FLAT) ==
           VIBE_STICK_EVENT_MOTION_STOP);
    assert(vibe_input_router_command((vibe_input_signal_t)99) ==
           VIBE_APP_COMMAND_NONE);
    puts("vibestick input router tests passed");
    return 0;
}
