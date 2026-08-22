#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "vibe_recording_controller.h"

int main(void)
{
    vibe_recording_controller_t controller;
    vibe_recording_controller_config_t config = {
        .cyber_intents = true,
    };
    vibe_recording_controller_dependencies_t dependencies = {0};
    vibe_recording_controller_init(
        &controller, &config, &dependencies);

    assert(strcmp(vibe_recording_controller_trigger_label(
                      &controller, false),
                  "PTT") == 0);
    assert(strcmp(vibe_recording_controller_intent_label(&controller),
                  "DICT") == 0);

    vibe_recording_command_t command = {
        .type = VIBE_RECORDING_COMMAND_CYCLE_INTENT,
    };
    assert(vibe_recording_controller_handle(&controller, &command));
    assert(controller.intent == RECORDING_INTENT_CYBER_FORTUNE);
    assert(vibe_recording_controller_is_cyber(&controller));
    assert(strcmp(vibe_recording_controller_intent_key(&controller),
                  "cyber_fortune") == 0);

    command.type = VIBE_RECORDING_COMMAND_BEGIN_SESSION;
    command.data.begin_session.session_id = "session-1";
    command.data.begin_session.notify_bridge = true;
    assert(vibe_recording_controller_handle(&controller, &command));

    command.type = VIBE_RECORDING_COMMAND_NOTE_CHUNK;
    command.data.chunk_bytes = 640;
    assert(vibe_recording_controller_handle(&controller, &command));

    vibe_recording_snapshot_t snapshot;
    vibe_recording_controller_snapshot(&controller, &snapshot);
    assert(snapshot.session_active);
    assert(strcmp(snapshot.session_id, "session-1") == 0);
    assert(snapshot.chunk_id == 1);
    assert(snapshot.uploaded_bytes == 640);
    assert(snapshot.bridge_stop_required);

    command.type = VIBE_RECORDING_COMMAND_RESET_SESSION;
    assert(vibe_recording_controller_handle(&controller, &command));
    vibe_recording_controller_snapshot(&controller, &snapshot);
    assert(!snapshot.session_active);
    assert(snapshot.session_id[0] == '\0');

    config.cyber_intents = false;
    vibe_recording_controller_init(
        &controller, &config, &dependencies);
    command.type = VIBE_RECORDING_COMMAND_CYCLE_INTENT;
    assert(vibe_recording_controller_handle(&controller, &command));
    assert(controller.intent == RECORDING_INTENT_DICTATION);

    puts("vibestick recording controller tests passed");
    return 0;
}
