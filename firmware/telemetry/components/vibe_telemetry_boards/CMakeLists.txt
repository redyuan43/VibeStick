if(CONFIG_IDF_TARGET_ESP32S3)
    set(board_src "src/board_sticks3.c")
else()
    set(board_src "src/board_stickc_plus_11.c")
endif()

idf_component_register(
    SRCS "../../../shared/sticks3_battery_curve.c" "src/board_display.c" ${board_src}
    INCLUDE_DIRS "include" "../../../shared"
    REQUIRES driver esp_lcd
)
