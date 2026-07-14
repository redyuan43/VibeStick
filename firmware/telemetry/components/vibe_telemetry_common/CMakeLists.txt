idf_component_register(
    SRCS
        "src/vibe_telemetry_app.c"
        "src/vibe_telemetry_http.c"
        "src/vibe_telemetry_identity.c"
        "src/vibe_telemetry_json.c"
        "src/vibe_telemetry_wifi.c"
    INCLUDE_DIRS "include" "../../secrets"
    REQUIRES esp_event esp_http_client esp_netif esp_timer esp_wifi nvs_flash vibe_telemetry_boards
)
