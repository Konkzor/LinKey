idf_component_register(
    SRCS "main.c" "ulp_linky.cpp" "wifi_manager.c" "mqtt_manager.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi mqtt HULP
)
