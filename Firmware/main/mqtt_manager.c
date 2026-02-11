#include <string.h>
#include <stdio.h>
#include "mqtt_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "debug.h"

static const char *TAG = "MQTT_MGR";

// MQTT credentials from Kconfig
#define MQTT_BROKER_URI CONFIG_LINKEY_MQTT_BROKER_URI
#define MQTT_USERNAME   CONFIG_LINKEY_MQTT_USERNAME
#define MQTT_PASSWORD   CONFIG_LINKEY_MQTT_PASSWORD

// MQTT topics
#define MQTT_TOPIC_STATE  CONFIG_LINKEY_MQTT_TOPIC_PREFIX "/state"
#define MQTT_TOPIC_STATUS CONFIG_LINKEY_MQTT_TOPIC_PREFIX "/status"

// Home Assistant discovery prefix
#define HA_DISCOVERY_PREFIX "homeassistant"

// Device info for Home Assistant
#define DEVICE_NAME         CONFIG_LINKEY_DEVICE_NAME
#define DEVICE_MODEL        "Linkey"
#define DEVICE_MANUFACTURER "Konkzor"

// Firmware and hardware versions (major.minor)
#define FW_VERSION_MAJOR    1
#define FW_VERSION_MINOR    0
#define HW_VERSION_MAJOR    1
#define HW_VERSION_MINOR    0

// Stringify helper
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define FW_VERSION STR(FW_VERSION_MAJOR) "." STR(FW_VERSION_MINOR)
#define HW_VERSION STR(HW_VERSION_MAJOR) "." STR(HW_VERSION_MINOR)

// Cached MAC address string
static char device_mac_str[13] = {0};  // 12 hex chars + null

// Get device MAC address as lowercase hex string
static const char* get_device_mac_str(void)
{
    if (device_mac_str[0] == 0) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(device_mac_str, sizeof(device_mac_str), "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return device_mac_str;
}

// Publish Home Assistant device discovery config (single payload for all sensors)
// Topic: homeassistant/device/linkey_<mac>/config
static void mqtt_publish_ha_discovery(esp_mqtt_client_handle_t client)
{
    const char *mac = get_device_mac_str();
    char topic[128];
    char payload[1600];
    int offset = 0;

    // Device discovery topic
    snprintf(topic, sizeof(topic), "%s/device/linkey_%s/config",
             HA_DISCOVERY_PREFIX, mac);

    // Build single JSON payload with device info, origin, and all components
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "{"
        // Device info
        "\"dev\":{"
            "\"ids\":\"linkey_%s\","
            "\"name\":\"%s\","
            "\"mf\":\"%s\","
            "\"mdl\":\"%s\","
            "\"sw\":\"%s\","
            "\"hw\":\"%s\","
            "\"sn\":\"%s\""
        "},",
        mac, DEVICE_NAME, DEVICE_MANUFACTURER, DEVICE_MODEL,
        FW_VERSION, HW_VERSION, mac);

    // Origin info
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"o\":{\"name\":\"linkey\",\"sw\":\"%s\"},", FW_VERSION);

    // Components (using HA abbreviated keys)
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"cmps\":{"

        // IINST sensor (current) - instantaneous measurement
        "\"iinst\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Current\","
            "\"dev_cla\":\"current\","
            "\"unit_of_meas\":\"A\","
            "\"stat_cla\":\"measurement\","
            "\"ic\":\"mdi:current-ac\","
            "\"val_tpl\":\"{{ value_json.iinst }}\","
            "\"uniq_id\":\"linkey_%s_iinst\""
        "},",
        mac);

    // BASE sensor (energy) - cumulative meter, only increases
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"base\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Energy Index\","
            "\"dev_cla\":\"energy\","
            "\"unit_of_meas\":\"Wh\","
            "\"stat_cla\":\"total_increasing\","
            "\"ic\":\"mdi:counter\","
            "\"val_tpl\":\"{{ value_json.base }}\","
            "\"uniq_id\":\"linkey_%s_base\""
        "},",
        mac);

    // VCAP sensor (voltage) - instantaneous measurement
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"vcap\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Supercap Voltage\","
            "\"dev_cla\":\"voltage\","
            "\"unit_of_meas\":\"mV\","
            "\"stat_cla\":\"measurement\","
            "\"ic\":\"mdi:battery-heart-variant\","
            "\"val_tpl\":\"{{ value_json.vcap }}\","
            "\"uniq_id\":\"linkey_%s_vcap\""
        "},",
        mac);

    // PAPP sensor (apparent power) - instantaneous measurement
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"papp\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Apparent Power\","
            "\"dev_cla\":\"apparent_power\","
            "\"unit_of_meas\":\"VA\","
            "\"stat_cla\":\"measurement\","
            "\"ic\":\"mdi:flash\","
            "\"val_tpl\":\"{{ value_json.papp }}\","
            "\"uniq_id\":\"linkey_%s_papp\""
        "},",
        mac);

    // ADPS sensor (overcurrent warning) - instantaneous measurement
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"adps\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Overcurrent Warning\","
            "\"dev_cla\":\"current\","
            "\"unit_of_meas\":\"A\","
            "\"stat_cla\":\"measurement\","
            "\"ic\":\"mdi:alert\","
            "\"val_tpl\":\"{{ value_json.adps }}\","
            "\"uniq_id\":\"linkey_%s_adps\""
        "},",
        mac);

    // Uptime sensor (duration) - total increasing
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"uptime\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Uptime\","
            "\"dev_cla\":\"duration\","
            "\"unit_of_meas\":\"s\","
            "\"stat_cla\":\"total_increasing\","
            "\"ic\":\"mdi:timer-outline\","
            "\"val_tpl\":\"{{ value_json.uptime }}\","
            "\"uniq_id\":\"linkey_%s_uptime\""
        "}},",
        mac);

    // Shared state and availability topics
    snprintf(payload + offset, sizeof(payload) - offset,
        "\"stat_t\":\"%s\","
        "\"avty_t\":\"%s\""
        "}",
        MQTT_TOPIC_STATE, MQTT_TOPIC_STATUS);

    // Publish with retain flag so HA remembers the config
    esp_mqtt_client_publish(client, topic, payload, 0, 1, 1);
    DEBUG_LOG(TAG, "Published HA device discovery");
}

// Publish online status to availability topic
static void mqtt_publish_online(esp_mqtt_client_handle_t client)
{
    esp_mqtt_client_publish(client, MQTT_TOPIC_STATUS, "online", 0, 1, 1);
    DEBUG_LOG(TAG, "Published: online");
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    mqtt_state_t *state = (mqtt_state_t*)handler_args;
    if (!state) return;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            DEBUG_LOG(TAG, "MQTT connected");
            state->connected = true;
            // Publish online status and HA discovery configs
            mqtt_publish_online(state->client);
            mqtt_publish_ha_discovery(state->client);
            break;
        case MQTT_EVENT_DISCONNECTED:
            DEBUG_LOG(TAG, "MQTT disconnected");
            state->connected = false;
            break;
        case MQTT_EVENT_ERROR:
            DEBUG_LOGE(TAG, "MQTT error");
            state->connected = false;
            break;
        case MQTT_EVENT_DELETED:
            DEBUG_LOGW(TAG, "MQTT message expired - stopping WiFi immediately");
            state->connected = false;
            state->started = false;  // Mark MQTT as stopped (can't call esp_mqtt_client_stop from event handler)
            // Call wifi_stop callback if provided
            if (state->wifi_stop_cb) {
                state->wifi_stop_cb();
            }
            break;
        default:
            break;
    }
}

void mqtt_init(mqtt_state_t *state, wifi_stop_fn_t wifi_stop_cb)
{
    if (!state || state->initialized) {
        return;
    }

    // Store callback in state
    state->wifi_stop_cb = wifi_stop_cb;

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .session.keepalive = 5,  // Reduced from 30s for faster disconnect detection
        .session.disable_clean_session = 0,
        .session.last_will = {
            .topic = MQTT_TOPIC_STATUS,
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = 1
        },
        .network.timeout_ms = 1000,  // Reduced from 3000ms
        .network.refresh_connection_after_ms = 0,
        .buffer.size = 1536,
        .buffer.out_size = 1536,
    };

    if (strlen(MQTT_USERNAME) > 0) {
        mqtt_cfg.credentials.username = MQTT_USERNAME;
    }
    if (strlen(MQTT_PASSWORD) > 0) {
        mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
    }

    state->client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(state->client, ESP_EVENT_ANY_ID, mqtt_event_handler, state);

    state->initialized = true;
    DEBUG_LOG(TAG, "MQTT client initialized");
}

void mqtt_stop(mqtt_state_t *state)
{
    if (!state) return;

    if (state->started) {
        esp_mqtt_client_stop(state->client);
        state->started = false;
        state->connected = false;
    }
}

conn_result_t mqtt_connect(mqtt_state_t *state, voltage_check_fn_t voltage_check,
                           uint16_t voltage_threshold, wifi_check_fn_t wifi_check,
                           uint32_t timeout_ms, uint32_t poll_interval_ms)
{
    if (!state) return CONN_FAILED;

    // Check if already connected
    if (state->connected) {
        DEBUG_LOG(TAG, "MQTT already connected");
        return CONN_OK;
    }

    // Start or reconnect client
    if (!state->started) {
        esp_mqtt_client_start(state->client);
        state->started = true;
    } else {
        esp_mqtt_client_reconnect(state->client);
    }

    // Wait for MQTT connection with voltage and WiFi checks
    uint32_t wait_ms = 0;
    while (!state->connected && wait_ms < timeout_ms) {
        // Check voltage during MQTT connection
        if (voltage_check && voltage_check(voltage_threshold)) {
            return CONN_VOLTAGE_LOW;
        }

        // Check if WiFi is still connected (exit early if AP lost)
        if (wifi_check && !wifi_check()) {
            DEBUG_LOGW(TAG, "WiFi lost during MQTT connect");
            return CONN_FAILED;
        }

        vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        wait_ms += poll_interval_ms;
    }

    if (state->connected) {
        return CONN_OK;
    } else {
        DEBUG_LOGW(TAG, "MQTT connection timeout");
        return CONN_FAILED;
    }
}

bool mqtt_is_connected(mqtt_state_t *state)
{
    return state && state->connected;
}

bool mqtt_publish_linky_data(mqtt_state_t *state, linky_data_t *data)
{
    if (!state || !state->connected) {
        DEBUG_LOGW(TAG, "MQTT not connected, skipping publish");
        return false;
    }

    char payload[128];
    int offset = 0;
    bool first = true;

    // Build JSON payload with only valid fields
    offset += snprintf(payload + offset, sizeof(payload) - offset, "{");

    // Add IINST if valid
    if (data->valid_flags & LINKY_FLAG_IINST) {
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"iinst\":%u", data->iinst);
        first = false;
    }

    // Add BASE if valid
    if (data->valid_flags & LINKY_FLAG_BASE) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"base\":%lu", data->base);
        first = false;
    }

    // Add PAPP if valid
    if (data->valid_flags & LINKY_FLAG_PAPP) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"papp\":%lu", data->papp);
        first = false;
    }

    // Add ADPS if valid
    if (data->valid_flags & LINKY_FLAG_ADPS) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"adps\":%u", data->adps);
        first = false;
    }

    // VCAP is always included
    if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
    offset += snprintf(payload + offset, sizeof(payload) - offset,
                      "\"vcap\":%lu", data->voltage_cap);

    // Uptime is always included
    offset += snprintf(payload + offset, sizeof(payload) - offset,
                      ",\"uptime\":%lu", data->uptime_s);

    snprintf(payload + offset, sizeof(payload) - offset, "}");

    // Publish single JSON to state topic (QoS 1)
    int ret = esp_mqtt_client_publish(state->client, MQTT_TOPIC_STATE, payload, 0, 1, 0);
    if (ret < 0) {
        DEBUG_LOGW(TAG, "Failed to publish state");
        return false;
    }

    DEBUG_LOG(TAG, "Published: %s", payload);
    return true;
}
