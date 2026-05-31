idf_component_register(
    SRCS "main.c" "ulp_linky.cpp" "tic_parser.c" "voltage_manager.c" "voltage_state.c" "wifi_manager.c" "mqtt_manager.c" "fsm.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi mqtt HULP
)

if(LINKEY_QEMU_EMULATION)
    target_link_options(${COMPONENT_LIB} INTERFACE
        -Wl,--wrap=esp_pm_configure
        -Wl,--wrap=esp_read_mac
        -Wl,--wrap=hulp_ulp_isr_register
        -Wl,--wrap=hulp_ulp_interrupt_en
        -Wl,--wrap=init_ulp_linky
        -Wl,--wrap=voltage_init
        -Wl,--wrap=voltage_read_mv
        -Wl,--wrap=voltage_is_low
        -Wl,--wrap=voltage_is_low_dynamic
        -Wl,--wrap=voltage_reset_peak
        -Wl,--wrap=wifi_init
        -Wl,--wrap=wifi_start
        -Wl,--wrap=wifi_stop
        -Wl,--wrap=wifi_connect
        -Wl,--wrap=wifi_is_connected
    )
endif()
