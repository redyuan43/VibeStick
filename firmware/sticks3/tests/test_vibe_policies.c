#include "vibe_bridge_profile_policy.h"
#include "vibe_ota_policy.h"
#include "vibe_power_policy.h"
#include "vibe_recording_policy.h"
#include "vibe_wifi_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_ota_versions(void)
{
    int comparison = 0;
    assert(vibe_ota_compare_semantic_versions("v1.2.3", "1.2.2", &comparison));
    assert(comparison > 0);
    assert(vibe_ota_compare_semantic_versions("1.2.3", "1.2.3", &comparison));
    assert(comparison == 0);
    assert(vibe_ota_compare_semantic_versions("1.2.2", "1.2.3", &comparison));
    assert(comparison < 0);
    assert(vibe_ota_version_is_newer("1.2.4-rc1", "1.2.3"));
    assert(!vibe_ota_version_is_newer("1.2.3", "1.2.3"));
    assert(!vibe_ota_parse_semantic_version("1.2", (uint32_t[3]){0}));
    assert(!vibe_ota_parse_semantic_version("1.2.x", (uint32_t[3]){0}));

    vibe_ota_manifest_t manifest = {
        .available = true,
        .board = "sticks3",
        .version = "1.2.4",
        .build_id = "build-new",
    };
    assert(vibe_ota_update_decision(&manifest, "sticks3", "1.2.3", "build-old",
                                    "", "") == VIBE_OTA_DECISION_UPDATE);
    assert(vibe_ota_update_decision(&manifest, "stickc_plus", "1.2.3", "build-old",
                                    "", "") == VIBE_OTA_DECISION_BOARD_MISMATCH);
    strcpy(manifest.version, "1.2.3");
    assert(vibe_ota_update_decision(&manifest, "sticks3", "1.2.3", "build-old",
                                    "", "") == VIBE_OTA_DECISION_CURRENT_VERSION);
    strcpy(manifest.version, "1.2.4");
    strcpy(manifest.sha256, "same");
    assert(vibe_ota_update_decision(&manifest, "sticks3", "1.2.3", "build-old",
                                    "same", "") == VIBE_OTA_DECISION_CURRENT_IMAGE_SHA256);
}

static void test_followup_window(void)
{
    vibe_recording_followup_window_t window = {0};
    assert(vibe_recording_followup_arm(&window, "session-1", 100, 3000));
    assert(vibe_recording_followup_present(&window));
    assert(vibe_recording_followup_consume(&window, true, 3100));
    assert(vibe_recording_followup_present(&window));
    assert(!vibe_recording_followup_consume(&window, true, 3101));
    assert(!vibe_recording_followup_present(&window));
    assert(vibe_recording_followup_arm(&window, "session-2", 100, 3000));
    assert(!vibe_recording_followup_consume(&window, false, 200));
    assert(!vibe_recording_followup_present(&window));
}

static void test_power_policy(void)
{
    vibe_power_policy_input_t input = {
        .now_ms = 10,
        .last_activity_ms = 0,
        .dim_after_ms = 30,
        .off_after_ms = 60,
    };
    assert(vibe_power_policy_display_state(&input) ==
           VIBE_POWER_POLICY_DISPLAY_ACTIVE);
    input.now_ms = 30;
    assert(vibe_power_policy_display_state(&input) ==
           VIBE_POWER_POLICY_DISPLAY_DIMMED);
    input.now_ms = 60;
    assert(vibe_power_policy_display_state(&input) ==
           VIBE_POWER_POLICY_DISPLAY_OFF);
    input.active_work = true;
    assert(vibe_power_policy_display_state(&input) ==
           VIBE_POWER_POLICY_DISPLAY_ACTIVE);
    assert(vibe_power_policy_should_attempt_deep_sleep(false, false, false,
                                                        300, 0, 300) == false);
    assert(vibe_power_policy_should_attempt_deep_sleep(false, false, false,
                                                        300, 1, 299));
    assert(!vibe_power_policy_should_attempt_deep_sleep(true, false, false,
                                                         300, 1, 299));
}

static void test_bridge_identity_policy(void)
{
    char fallback[24] = {0};
    assert(vibe_bridge_health_name_supported("vibestick-bridge"));
    assert(vibe_bridge_health_name_supported("capswriter-m5-voice-bridge"));
    assert(!vibe_bridge_health_name_supported("other"));
    assert(vibe_bridge_identity_is_generic(""));
    assert(vibe_bridge_identity_is_generic("capswriter-m5-voice-bridge"));
    assert(!vibe_bridge_identity_is_generic("desk-a"));
    vibe_bridge_fallback_id("192.168.100.142", fallback, sizeof(fallback));
    assert(strcmp(fallback, "lan-192-168-100-142") == 0);

    bridge_discovered_profile_t stored[2] = {
        {
            .id = "bridge-a",
            .label = "A",
            .host = "192.168.1.10",
            .port = 8765,
            .token = "one",
        },
    };
    bridge_discovered_profile_t scanned[2] = {
        {
            .id = "bridge-a",
            .label = "A renamed",
            .host = "192.168.1.11",
            .port = 8765,
            .token = "one",
        },
        {
            .id = "bridge-b",
            .label = "B",
            .host = "192.168.1.12",
            .port = 8765,
            .token = "two",
        },
    };
    size_t stored_count = 1;
    size_t skipped = 0;
    assert(vibe_bridge_profiles_merge(stored, &stored_count, 2,
                                      scanned, 2, &skipped));
    assert(stored_count == 2);
    assert(skipped == 0);
    assert(strcmp(stored[0].label, "A renamed") == 0);
    assert(strcmp(stored[0].host, "192.168.1.11") == 0);
    assert(strcmp(stored[1].id, "bridge-b") == 0);

    bridge_profile_snapshot_t snapshot;
    vibe_bridge_profile_snapshot_from_discovered(&stored[1], &snapshot);
    assert(strcmp(snapshot.id, "bridge-b") == 0);
    bridge_profile_config_t view;
    vibe_bridge_profile_snapshot_view(&snapshot, &view);
    assert(strcmp(view.host, "192.168.1.12") == 0);
}

static void test_wifi_profile_policy(void)
{
    vibe_wifi_profile_t profiles[2] = {0};
    size_t count = 0;
    const vibe_wifi_profile_t empty = {0};
    const vibe_wifi_profile_t first = {
        .ssid = "330",
        .password = "first-password",
    };
    const vibe_wifi_profile_t updated = {
        .ssid = "330",
        .password = "updated-password",
    };
    const vibe_wifi_profile_t second = {
        .ssid = "HANYUAN",
        .password = "second-password",
    };
    const vibe_wifi_profile_t overflow = {
        .ssid = "EDGE",
        .password = "third-password",
    };

    assert(vibe_wifi_profiles_merge(profiles, &count, 2, &empty) ==
           VIBE_WIFI_PROFILE_INVALID);
    assert(vibe_wifi_profiles_merge(profiles, &count, 2, &first) ==
           VIBE_WIFI_PROFILE_ADDED);
    assert(count == 1);
    assert(strcmp(profiles[0].ssid, "330") == 0);
    assert(vibe_wifi_profiles_merge(profiles, &count, 2, &first) ==
           VIBE_WIFI_PROFILE_UNCHANGED);
    assert(vibe_wifi_profiles_merge(profiles, &count, 2, &updated) ==
           VIBE_WIFI_PROFILE_UPDATED);
    assert(strcmp(profiles[0].password, "updated-password") == 0);
    assert(vibe_wifi_profiles_merge(profiles, &count, 2, &second) ==
           VIBE_WIFI_PROFILE_ADDED);
    assert(count == 2);
    assert(vibe_wifi_profiles_merge(profiles, &count, 2, &overflow) ==
           VIBE_WIFI_PROFILE_FULL);
    assert(vibe_wifi_profiles_merge(profiles, &count, 2, &first) ==
           VIBE_WIFI_PROFILE_UPDATED);
    assert(strcmp(profiles[0].password, "first-password") == 0);

    const uint32_t expected[] = {1000, 2000, 4000, 8000, 30000, 30000};
    for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); index++) {
        assert(vibe_wifi_reconnect_delay_ms((unsigned int)index, 30000) ==
               expected[index]);
    }
    assert(vibe_wifi_reconnect_delay_ms(4, 10000) == 10000);
}

int main(void)
{
    test_ota_versions();
    test_followup_window();
    test_power_policy();
    test_bridge_identity_policy();
    test_wifi_profile_policy();
    puts("vibestick policy tests passed");
    return 0;
}
