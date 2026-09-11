#include <stdbool.h>
#include <string.h>

#include "factory_config.h"
#include "factory_config_schema.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "FACTORY_CFG";

#define FACTORY_SECRET_MAX_LEN 64

static bool factory_config_initialized;
static char factory_mqtt_password[FACTORY_SECRET_MAX_LEN + 1];
static char factory_ble_pop[FACTORY_SECRET_MAX_LEN + 1];

static void copy_default(char *dst, size_t dst_size, const char *value,
                         const char *fallback)
{
    if (value && value[0]) {
        strlcpy(dst, value, dst_size);
    } else {
        strlcpy(dst, fallback, dst_size);
    }
}

static void read_factory_string(nvs_handle_t nvs, const char *key,
                                char *dst, size_t dst_size)
{
    size_t required_size = dst_size;
    esp_err_t err = nvs_get_str(nvs, key, dst, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read factory key %s (%s)",
                 key, esp_err_to_name(err));
    }
}

static void factory_config_init(void)
{
    if (factory_config_initialized) {
        return;
    }

    copy_default(factory_mqtt_password, sizeof(factory_mqtt_password),
                 CONFIG_LINKEY_MQTT_PASSWORD,
                 LINKEY_FACTORY_FALLBACK_MQTT_PASSWORD);
    copy_default(factory_ble_pop, sizeof(factory_ble_pop),
                 CONFIG_LINKEY_PROV_POP,
                 LINKEY_FACTORY_FALLBACK_BLE_POP);

    esp_err_t err = nvs_flash_init_partition(LINKEY_FACTORY_PARTITION_LABEL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Factory partition unavailable, using build defaults (%s)",
                 esp_err_to_name(err));
        factory_config_initialized = true;
        return;
    }

    nvs_handle_t nvs;
    err = nvs_open_from_partition(LINKEY_FACTORY_PARTITION_LABEL,
                                  LINKEY_FACTORY_NAMESPACE,
                                  NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Factory config unavailable, using build defaults (%s)",
                 esp_err_to_name(err));
        factory_config_initialized = true;
        return;
    }

    read_factory_string(nvs, LINKEY_FACTORY_KEY_MQTT_PASSWORD,
                        factory_mqtt_password, sizeof(factory_mqtt_password));
    read_factory_string(nvs, LINKEY_FACTORY_KEY_BLE_POP,
                        factory_ble_pop, sizeof(factory_ble_pop));
    nvs_close(nvs);

    factory_config_initialized = true;
}

const char *factory_config_get_mqtt_password(void)
{
    factory_config_init();
    return factory_mqtt_password;
}

const char *factory_config_get_ble_pop(void)
{
    factory_config_init();
    return factory_ble_pop;
}
