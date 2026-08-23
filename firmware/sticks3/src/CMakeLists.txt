idf_component_register(
    SRCS "main.c" "vibe_sticks3_asr_minimal.c" "vibe_sticks3_upload.c"
         "vibe_sticks3_status.c" "vibe_wifi_runtime.c" "vibe_wifi_policy.c"
         "vibe_board.c" "vibe_audio.c" "vibe_capture_profile.c"
         "../../shared/sticks3_battery_curve.c"
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

if(VIBE_RECORDING_DIAGNOSTICS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_STICK_RECORDING_DIAGNOSTICS_ENABLED=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_STICK_RECORDING_DIAGNOSTICS_ENABLED=0)
endif()
