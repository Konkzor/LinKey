idf_component_register(
    SRCS "main.c" "ulp_linky.cpp" "tic_parser.c" "voltage_manager.c" "voltage_state.c" "wifi_manager.c" "mqtt_manager.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi mqtt HULP
)
