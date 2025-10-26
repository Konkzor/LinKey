idf_component_register(
    SRCS "main.c" "ulp_linky.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi mqtt HULP
)
