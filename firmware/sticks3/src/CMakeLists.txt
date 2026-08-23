idf_component_register(
    SRCS "main.c" "vibe_cardputer_asr_minimal.c"
         "vibe_cardputer_asr_board.c" "vibe_cardputer_capture.c"
         "vibe_cardputer_opt.c" "vibe_cardputer_status.c"
         "vibe_cardputer_upload.c"
         "vibe_wifi_runtime.c" "vibe_wifi_policy.c"
    INCLUDE_DIRS "../include"
    REQUIRES esp_wifi esp_http_client esp_event esp_netif nvs_flash
             driver esp_timer esp_lcd esp_codec_dev lwip
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_BOARD_CARDPUTER_ADV=1)

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
