if(VIBE_BOARD STREQUAL "stickc_plus_minijoy_bt")
    idf_component_register(
        SRCS "main_minijoy_bt.c" "vibe_board.c" "vibe_audio.c" "vibe_input.c"
             "vibe_minijoyc.c" "vibe_bt_composite.c" "vibe_bt_status_ui.c"
             "../../shared/sticks3_battery_curve.c"
             "../generated/vibe_minijoy_pet_assets.c"
        INCLUDE_DIRS "../include" "../generated" "../../shared"
        REQUIRES nvs_flash driver esp_timer esp_lcd esp_codec_dev esp_hid bt
    )
else()
    idf_component_register(
        SRCS "main.c" "vibe_board.c" "vibe_audio.c" "vibe_motion.c" "vibe_input.c" "vibe_recording_upload.c" "vibe_stick_anim_assets.c" "vibe_ota_policy.c" "vibe_recording_policy.c" "vibe_power_policy.c" "vibe_bridge_profile_policy.c" "vibe_wifi_policy.c" "vibe_wav.c" "../../shared/sticks3_battery_curve.c" "../generated/vibe_stick_pet_assets.c"
        INCLUDE_DIRS "../include" "../generated" "../../shared"
        # ESP-IDF's early component-requirement expansion runs before VIBE_BOARD
        # is available, so keep the dedicated target's public BT include paths in
        # this superset. Disabled/unused components are discarded by the linker
        # for the normal firmware targets.
        REQUIRES esp_wifi esp_http_client esp_event esp_netif nvs_flash json driver esp_timer esp_lcd esp_codec_dev app_update esp_partition esp_app_format lwip esp_hid bt
    )
endif()

if(VIBE_BOARD STREQUAL "stickc_plus_minijoy_bt")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        VIBE_BOARD_STICKC_PLUS=1
        VIBE_BOARD_MINIJOYC_BT=1)
elseif(VIBE_BOARD STREQUAL "stickc_plus")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_BOARD_STICKC_PLUS=1)
elseif(VIBE_BOARD STREQUAL "stickc_plus_se")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_BOARD_STICKC_PLUS_SE=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_BOARD_STICKS3=1)
endif()

if(VIBE_SERIAL_DEBUG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_STICK_SERIAL_DEBUG_ENABLED=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_STICK_SERIAL_DEBUG_ENABLED=0)
endif()
