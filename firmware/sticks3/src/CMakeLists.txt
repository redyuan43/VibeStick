idf_component_register(
    SRCS "main.c" "vibe_board.c" "vibe_audio.c" "../generated/vibe_stick_cn_16.c" "../generated/vibe_stick_ui_assets.c"
    INCLUDE_DIRS "../include" "../generated"
    REQUIRES esp_wifi esp_http_client esp_event esp_netif nvs_flash json driver esp_timer esp_lcd esp_codec_dev
)

if(VIBE_BOARD STREQUAL "stickc_plus")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_BOARD_STICKC_PLUS=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIBE_BOARD_STICKS3=1)
endif()
