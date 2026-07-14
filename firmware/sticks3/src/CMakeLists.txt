idf_component_register(
    SRCS "main.c" "vibe_board.c" "vibe_audio.c" "vibe_motion.c" "vibe_stick_anim_assets.c" "../../shared/sticks3_battery_curve.c" "../generated/vibe_stick_pet_assets.c"
    INCLUDE_DIRS "../include" "../generated" "../../shared"
    REQUIRES esp_wifi esp_http_client esp_event esp_netif nvs_flash json driver esp_timer esp_lcd esp_codec_dev app_update esp_partition esp_app_format lwip
)

if(VIBE_BOARD STREQUAL "stickc_plus")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_BOARD_STICKC_PLUS=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_BOARD_STICKS3=1)
endif()
