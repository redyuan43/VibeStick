idf_component_register(
    SRCS "main.c" "vibe_air_mouse.c" "vibe_cardputer_air_mouse.c" "vibe_cardputer_input_profile.c" "vibe_cardputer_messages.c" "vibe_cardputer_volume.c" "vibe_pointer_client.c" "vibe_board.c" "vibe_audio.c" "vibe_motion.c" "vibe_input.c" "vibe_keyboard.c" "vibe_recording_upload.c" "vibe_stick_anim_assets.c" "vibe_settings.c" "vibe_ota_policy.c" "vibe_recording_policy.c" "vibe_power_policy.c" "vibe_bridge_profile_policy.c" "vibe_wifi_policy.c" "vibe_wav.c" "../../shared/sticks3_battery_curve.c" "../generated/vibe_stick_pet_assets.c"
    INCLUDE_DIRS "../include" "../generated" "../../shared"
    REQUIRES esp_wifi esp_http_client esp_event esp_netif nvs_flash json driver esp_adc esp_timer esp_lcd esp_codec_dev app_update esp_partition esp_app_format fatfs sdmmc mbedtls lwip
)

if(VIBE_BOARD STREQUAL "stickc_plus")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_BOARD_STICKC_PLUS=1)
elseif(VIBE_BOARD STREQUAL "stickc_plus_se")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_BOARD_STICKC_PLUS_SE=1)
elseif(VIBE_BOARD STREQUAL "cardputer_adv")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_BOARD_CARDPUTER_ADV=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_BOARD_STICKS3=1)
endif()

if(VIBE_SERIAL_DEBUG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_STICK_SERIAL_DEBUG_ENABLED=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_STICK_SERIAL_DEBUG_ENABLED=0)
endif()
