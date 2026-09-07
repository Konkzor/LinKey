#include <string.h>
#include <stdio.h>
#include "mqtt_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "debug.h"
#include "factory_config.h"
#include "types.h"

#if CONFIG_LINKEY_MQTT_AUTODISCOVERY
#include "mdns.h"
#endif

static const char *TAG = "MQTT_MGR";

// MQTT credentials from Kconfig/factory data
#define MQTT_BROKER_URI CONFIG_LINKEY_MQTT_BROKER_URI
#define MQTT_USERNAME   CONFIG_LINKEY_MQTT_USERNAME

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
static char default_mqtt_username[14] = {0};  // "linkey_" + 6 hex chars + null
static char device_display_name[32] = {0};
static char mqtt_topic_state[20];       // "linkey/" + 6 hex chars + "/state" + null
static char mqtt_topic_status[21];      // "linkey/" + 6 hex chars + "/status" + null
static char mqtt_topic_debug_req[28];   // "linkey/" + 6 hex chars + "/debug/request" + null
static char mqtt_topic_debug_frame[31]; // "linkey/" + 6 hex chars + "/debug/tic_frame" + null

#if CONFIG_LINKEY_MQTT_AUTODISCOVERY
#define MQTT_DISCOVERY_TIMEOUT_MS 3000
#define MQTT_DISCOVERY_MAX_RESULTS 2

static bool mdns_started;
static char discovered_mqtt_uri[64];

static bool mqtt_uri_from_result(const mdns_result_t *result, uint16_t broker_port,
                                 bool use_result_port, char *uri, size_t uri_len)
{
    for (const mdns_result_t *r = result; r; r = r->next) {
        uint16_t port = use_result_port && r->port ? r->port : broker_port;
        for (const mdns_ip_addr_t *addr = r->addr; addr; addr = addr->next) {
            if (addr->addr.type == ESP_IPADDR_TYPE_V4) {
                int written = snprintf(uri, uri_len, "mqtt://" IPSTR ":%u",
                                       IP2STR(&addr->addr.u_addr.ip4), port);
                return written > 0 && (size_t)written < uri_len;
            }
        }

        if (r->hostname && r->hostname[0]) {
            int written = snprintf(uri, uri_len, "mqtt://%s.local:%u",
                                   r->hostname, port);
            return written > 0 && (size_t)written < uri_len;
        }
    }

    return false;
}

static bool mqtt_discover_service(const char *service, uint16_t broker_port,
                                  bool use_result_port, char *uri, size_t uri_len)
{
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(service, "_tcp", MQTT_DISCOVERY_TIMEOUT_MS,
                                   MQTT_DISCOVERY_MAX_RESULTS, &results);
    bool found = false;

    if (err == ESP_OK && results) {
        found = mqtt_uri_from_result(results, broker_port, use_result_port,
                                     uri, uri_len);
    }

    if (results) {
        mdns_query_results_free(results);
    }

    return found;
}

static const char *mqtt_broker_uri(void)
{
    if (discovered_mqtt_uri[0]) {
        return discovered_mqtt_uri;
    }

    if (!mdns_started) {
        esp_err_t err = mdns_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "mDNS init failed (%s), using MQTT fallback URI",
                     esp_err_to_name(err));
            return MQTT_BROKER_URI;
        }
        mdns_started = true;
    }

    if (mqtt_discover_service("_home-assistant", 1883, false, discovered_mqtt_uri,
                              sizeof(discovered_mqtt_uri))) {
        ESP_LOGI(TAG, "Home Assistant discovered via mDNS, using MQTT URI: %s",
                 discovered_mqtt_uri);
        return discovered_mqtt_uri;
    }

    ESP_LOGI(TAG, "No MQTT broker discovered with mDNS, using fallback URI: %s",
             MQTT_BROKER_URI);
    return MQTT_BROKER_URI;
}
#else
static const char *mqtt_broker_uri(void)
{
    return MQTT_BROKER_URI;
}
#endif

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

static const char *mqtt_username(void)
{
    if (strlen(MQTT_USERNAME) > 0) {
        return MQTT_USERNAME;
    }

    if (default_mqtt_username[0] == 0) {
        const char *mac = get_device_mac_str();
        snprintf(default_mqtt_username, sizeof(default_mqtt_username),
                 "linkey_%s", mac + 6);
    }

    return default_mqtt_username;
}

static const char *ha_device_name(void)
{
    if (strlen(DEVICE_NAME) > 0) {
        return DEVICE_NAME;
    }

    if (device_display_name[0] == 0) {
        const char *mac = get_device_mac_str();
        snprintf(device_display_name, sizeof(device_display_name),
                 "Linkey_%s", mac + 6);
    }

    return device_display_name;
}

static void mqtt_topics_init(void)
{
    if (mqtt_topic_state[0] != 0) {
        return;
    }

    const char *mac = get_device_mac_str();
    const char *suffix = mac + 6;
    snprintf(mqtt_topic_state, sizeof(mqtt_topic_state), "linkey/%s/state", suffix);
    snprintf(mqtt_topic_status, sizeof(mqtt_topic_status), "linkey/%s/status", suffix);
    snprintf(mqtt_topic_debug_req, sizeof(mqtt_topic_debug_req),
             "linkey/%s/debug/request", suffix);
    snprintf(mqtt_topic_debug_frame, sizeof(mqtt_topic_debug_frame),
             "linkey/%s/debug/tic_frame", suffix);
}

// Publish Home Assistant device discovery config (single payload for all sensors)
// Topic: homeassistant/device/linkey_<mac>/config
static void mqtt_publish_ha_discovery(esp_mqtt_client_handle_t client)
{
    const char *mac = get_device_mac_str();
    mqtt_topics_init();
    char topic[128];
    char payload[4096];
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
        mac, ha_device_name(), DEVICE_MANUFACTURER, DEVICE_MODEL,
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
            "\"sug_dsp_prc\":0,"
            "\"ic\":\"mdi:current-ac\","
            "\"val_tpl\":\"{{ value_json.iinst }}\","
            "\"uniq_id\":\"linkey_%s_iinst\""
        "},",
        mac);

    // Energy index sensors (tariff-dependent)
#if defined(CONFIG_LINKEY_TARIFF_HPHC)
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"hchc\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Off-Peak Energy\","
            "\"dev_cla\":\"energy\","
            "\"unit_of_meas\":\"kWh\","
            "\"stat_cla\":\"total_increasing\","
            "\"sug_dsp_prc\":3,"
            "\"ic\":\"mdi:counter\","
            "\"val_tpl\":\"{{ value_json.hchc / 1000 }}\","
            "\"uniq_id\":\"linkey_%s_hchc\""
        "},",
        mac);
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"hchp\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Peak Energy\","
            "\"dev_cla\":\"energy\","
            "\"unit_of_meas\":\"kWh\","
            "\"stat_cla\":\"total_increasing\","
            "\"sug_dsp_prc\":3,"
            "\"ic\":\"mdi:counter\","
            "\"val_tpl\":\"{{ value_json.hchp / 1000 }}\","
            "\"uniq_id\":\"linkey_%s_hchp\""
        "},",
        mac);
#elif defined(CONFIG_LINKEY_TARIFF_EJP)
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"ejphn\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Normal Hours Energy\","
            "\"dev_cla\":\"energy\","
            "\"unit_of_meas\":\"kWh\","
            "\"stat_cla\":\"total_increasing\","
            "\"sug_dsp_prc\":3,"
            "\"ic\":\"mdi:counter\","
            "\"val_tpl\":\"{{ value_json.ejphn / 1000 }}\","
            "\"uniq_id\":\"linkey_%s_ejphn\""
        "},",
        mac);
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"ejphpm\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Mobile Peak Energy\","
            "\"dev_cla\":\"energy\","
            "\"unit_of_meas\":\"kWh\","
            "\"stat_cla\":\"total_increasing\","
            "\"sug_dsp_prc\":3,"
            "\"ic\":\"mdi:counter\","
            "\"val_tpl\":\"{{ value_json.ejphpm / 1000 }}\","
            "\"uniq_id\":\"linkey_%s_ejphpm\""
        "},",
        mac);
#elif defined(CONFIG_LINKEY_TARIFF_TEMPO)
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"bbrhcjb\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Blue Off-Peak Energy\","
            "\"dev_cla\":\"energy\","
            "\"unit_of_meas\":\"kWh\","
            "\"stat_cla\":\"total_increasing\","
            "\"sug_dsp_prc\":3,"
            "\"ic\":\"mdi:counter\","
            "\"val_tpl\":\"{{ value_json.bbrhcjb / 1000 }}\","
            "\"uniq_id\":\"linkey_%s_bbrhcjb\""
        "},",
        mac);
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"bbrhpjb\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Blue Peak Energy\","
            "\"dev_cla\":\"energy\","
            "\"unit_of_meas\":\"kWh\","
            "\"stat_cla\":\"total_increasing\","
            "\"sug_dsp_prc\":3,"
            "\"ic\":\"mdi:counter\","
            "\"val_tpl\":\"{{ value_json.bbrhpjb / 1000 }}\","
            "\"uniq_id\":\"linkey_%s_bbrhpjb\""
        "},",
        mac);
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"bbrhcjw\":{"
            "\"p\":\"sensor\","
            "\"name\":\"White Off-Peak Energy\","
            "\"dev_cla\":\"energy\","
            "\"unit_of_meas\":\"kWh\","
            "\"stat_cla\":\"total_increasing\","
            "\"sug_dsp_prc\":3,"
            "\"ic\":\"mdi:counter\","
            "\"val_tpl\":\"{{ value_json.bbrhcjw / 1000 }}\","
            "\"uniq_id\":\"linkey_%s_bbrhcjw\""
        "},",
        mac);
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"bbrhpjw\":{"
            "\"p\":\"sensor\","
            "\"name\":\"White Peak Energy\","
            "\"dev_cla\":\"energy\","
            "\"unit_of_meas\":\"kWh\","
            "\"stat_cla\":\"total_increasing\","
            "\"sug_dsp_prc\":3,"
            "\"ic\":\"mdi:counter\","
            "\"val_tpl\":\"{{ value_json.bbrhpjw / 1000 }}\","
            "\"uniq_id\":\"linkey_%s_bbrhpjw\""
        "},",
        mac);
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"bbrhcjr\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Red Off-Peak Energy\","
            "\"dev_cla\":\"energy\","
            "\"unit_of_meas\":\"kWh\","
            "\"stat_cla\":\"total_increasing\","
            "\"sug_dsp_prc\":3,"
            "\"ic\":\"mdi:counter\","
            "\"val_tpl\":\"{{ value_json.bbrhcjr / 1000 }}\","
            "\"uniq_id\":\"linkey_%s_bbrhcjr\""
        "},",
        mac);
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"bbrhpjr\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Red Peak Energy\","
            "\"dev_cla\":\"energy\","
            "\"unit_of_meas\":\"kWh\","
            "\"stat_cla\":\"total_increasing\","
            "\"sug_dsp_prc\":3,"
            "\"ic\":\"mdi:counter\","
            "\"val_tpl\":\"{{ value_json.bbrhpjr / 1000 }}\","
            "\"uniq_id\":\"linkey_%s_bbrhpjr\""
        "},",
        mac);
#else // BASE
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"base\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Energy Index\","
            "\"dev_cla\":\"energy\","
            "\"unit_of_meas\":\"kWh\","
            "\"stat_cla\":\"total_increasing\","
            "\"sug_dsp_prc\":3,"
            "\"ic\":\"mdi:counter\","
            "\"val_tpl\":\"{{ value_json.base / 1000 }}\","
            "\"uniq_id\":\"linkey_%s_base\""
        "},",
        mac);
#endif

    // VCAP sensor (voltage) - instantaneous measurement
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"vcap\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Supercap Voltage\","
            "\"dev_cla\":\"voltage\","
            "\"unit_of_meas\":\"mV\","
            "\"stat_cla\":\"measurement\","
            "\"sug_dsp_prc\":0,"
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
            "\"sug_dsp_prc\":0,"
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
            "\"sug_dsp_prc\":0,"
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
            "\"sug_dsp_prc\":0,"
            "\"ic\":\"mdi:timer-outline\","
            "\"val_tpl\":\"{{ value_json.uptime }}\","
            "\"uniq_id\":\"linkey_%s_uptime\""
        "}},",
        mac);

    // Shared state and availability topics
    offset += snprintf(payload + offset, sizeof(payload) - offset,
        "\"stat_t\":\"%s\","
        "\"avty_t\":\"%s\""
        "}",
        mqtt_topic_state, mqtt_topic_status);

    DEBUG_LOG(TAG, "HA discovery payload size: %d bytes", offset);

    // Publish with retain flag so HA remembers the config
    esp_mqtt_client_publish(client, topic, payload, 0, 1, 1);
    DEBUG_LOG(TAG, "Published HA device discovery");
}

// Subscribe to debug request topic
static void mqtt_subscribe_debug_topic(esp_mqtt_client_handle_t client)
{
    esp_mqtt_client_subscribe(client, mqtt_topic_debug_req, 1);
    DEBUG_LOG(TAG, "Subscribed to debug request topic");
}

// Publish online status to availability topic
static void mqtt_publish_online(esp_mqtt_client_handle_t client)
{
    esp_mqtt_client_publish(client, mqtt_topic_status, "online", 0, 1, 1);
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
            // Subscribe to debug request topic
            mqtt_subscribe_debug_topic(state->client);
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
            DEBUG_LOGW(TAG, "MQTT message expired");
            break;
        case MQTT_EVENT_DATA:
        {
            esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
            if (event->topic_len > 0 && event->data_len > 0) {
                // Check if this is a debug request message
                if (event->topic_len == strlen(mqtt_topic_debug_req)
                    && strncmp(event->topic, mqtt_topic_debug_req, event->topic_len) == 0) {
                    // Check for magic word "GET_TIC_FRAME"
                    if (event->data_len >= 12 && strncmp(event->data, "GET_TIC_FRAME", 13) == 0) {
                        DEBUG_LOG(TAG, "Debug request received: GET_TIC_FRAME");
                        state->debug_frame_requested = true;
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

void mqtt_init(mqtt_state_t *state)
{
    if (!state || state->initialized) {
        return;
    }

    const char *broker_uri = mqtt_broker_uri();
    mqtt_topics_init();
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .session.keepalive = 5,  // Reduced from 30s for faster disconnect detection
        .session.disable_clean_session = 0,
        .session.last_will = {
            .topic = mqtt_topic_status,
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = 1
        },
        .network.timeout_ms = 1500,  // Must exceed WiFi modem sleep interval (~1s with listen_interval=10)
        .network.refresh_connection_after_ms = 0,
        .buffer.size = 4096,
        .buffer.out_size = 4096,
    };

    mqtt_cfg.credentials.username = mqtt_username();
    const char *mqtt_password = factory_config_get_mqtt_password();
    if (strlen(mqtt_password) > 0) {
        mqtt_cfg.credentials.authentication.password = mqtt_password;
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
            return CONN_WIFI_LOST;
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

    char payload[256];
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

    // Add energy index fields only on change (tariff-dependent)
#if defined(CONFIG_LINKEY_TARIFF_HPHC)
    static uint32_t last_hchc = 0, last_hchp = 0;
    if ((data->valid_flags & LINKY_FLAG_HCHC) && data->hchc != last_hchc) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"hchc\":%lu", data->hchc);
        last_hchc = data->hchc;
        first = false;
    }
    if ((data->valid_flags & LINKY_FLAG_HCHP) && data->hchp != last_hchp) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"hchp\":%lu", data->hchp);
        last_hchp = data->hchp;
        first = false;
    }
#elif defined(CONFIG_LINKEY_TARIFF_EJP)
    static uint32_t last_ejphn = 0, last_ejphpm = 0;
    if ((data->valid_flags & LINKY_FLAG_EJPHN) && data->ejphn != last_ejphn) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"ejphn\":%lu", data->ejphn);
        last_ejphn = data->ejphn;
        first = false;
    }
    if ((data->valid_flags & LINKY_FLAG_EJPHPM) && data->ejphpm != last_ejphpm) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"ejphpm\":%lu", data->ejphpm);
        last_ejphpm = data->ejphpm;
        first = false;
    }
#elif defined(CONFIG_LINKEY_TARIFF_TEMPO)
    static uint32_t last_bbrhcjb = 0, last_bbrhpjb = 0;
    static uint32_t last_bbrhcjw = 0, last_bbrhpjw = 0;
    static uint32_t last_bbrhcjr = 0, last_bbrhpjr = 0;
    if ((data->valid_flags & LINKY_FLAG_BBRHCJB) && data->bbrhcjb != last_bbrhcjb) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"bbrhcjb\":%lu", data->bbrhcjb);
        last_bbrhcjb = data->bbrhcjb;
        first = false;
    }
    if ((data->valid_flags & LINKY_FLAG_BBRHPJB) && data->bbrhpjb != last_bbrhpjb) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"bbrhpjb\":%lu", data->bbrhpjb);
        last_bbrhpjb = data->bbrhpjb;
        first = false;
    }
    if ((data->valid_flags & LINKY_FLAG_BBRHCJW) && data->bbrhcjw != last_bbrhcjw) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"bbrhcjw\":%lu", data->bbrhcjw);
        last_bbrhcjw = data->bbrhcjw;
        first = false;
    }
    if ((data->valid_flags & LINKY_FLAG_BBRHPJW) && data->bbrhpjw != last_bbrhpjw) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"bbrhpjw\":%lu", data->bbrhpjw);
        last_bbrhpjw = data->bbrhpjw;
        first = false;
    }
    if ((data->valid_flags & LINKY_FLAG_BBRHCJR) && data->bbrhcjr != last_bbrhcjr) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"bbrhcjr\":%lu", data->bbrhcjr);
        last_bbrhcjr = data->bbrhcjr;
        first = false;
    }
    if ((data->valid_flags & LINKY_FLAG_BBRHPJR) && data->bbrhpjr != last_bbrhpjr) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"bbrhpjr\":%lu", data->bbrhpjr);
        last_bbrhpjr = data->bbrhpjr;
        first = false;
    }
#else // BASE
    static uint32_t last_base = 0;
    if ((data->valid_flags & LINKY_FLAG_BASE) && data->base != last_base) {
        if (!first) offset += snprintf(payload + offset, sizeof(payload) - offset, ",");
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                          "\"base\":%lu", data->base);
        last_base = data->base;
        first = false;
    }
#endif

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
    int ret = esp_mqtt_client_publish(state->client, mqtt_topic_state, payload, 0, 1, 0);
    if (ret < 0) {
        DEBUG_LOGW(TAG, "Failed to publish state");
        return false;
    }

    DEBUG_LOG(TAG, "Published: %s", payload);
    return true;
}

bool mqtt_publish_tic_frame_debug(mqtt_state_t *state, const char *frame, int frame_len)
{
    if (!state || !state->connected || !frame || frame_len <= 0) {
        return false;
    }

    int ret = esp_mqtt_client_publish(state->client, mqtt_topic_debug_frame, frame, frame_len, 1, 0);
    if (ret < 0) {
        DEBUG_LOGW(TAG, "Failed to publish debug frame");
        return false;
    }

    DEBUG_LOG(TAG, "Published debug TIC frame (%d bytes)", frame_len);
    return true;
}
