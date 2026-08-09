set(linkey_requires nvs_flash esp_wifi mqtt HULP)
set(linkey_sources
    "main.c"
    "ulp_linky.cpp"
    "tic_parser.c"
    "voltage_manager.c"
    "voltage_state.c"
    "wifi_manager.c"
    "mqtt_manager.c"
    "factory_config.c"
    "fsm.c"
)

if(NOT LINKEY_QEMU_EMULATION)
    list(APPEND linkey_requires wifi_provisioning bt)
    list(APPEND linkey_sources "provisioning_manager.c")
endif()

if(CONFIG_LINKEY_DEBUG_LOGS)
    list(APPEND linkey_requires qrcode)
endif()

if(CONFIG_LINKEY_MQTT_AUTODISCOVERY)
    list(APPEND linkey_requires mdns)
endif()

idf_component_register(
    SRCS ${linkey_sources}
    INCLUDE_DIRS "."
    REQUIRES ${linkey_requires}
)

if(LINKEY_QEMU_EMULATION)
    target_link_options(${COMPONENT_LIB} INTERFACE
        -Wl,--wrap=esp_pm_configure
        -Wl,--wrap=gpio_get_level
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
        -Wl,--wrap=wifi_has_config
        -Wl,--wrap=wifi_is_connected
    )
endif()
