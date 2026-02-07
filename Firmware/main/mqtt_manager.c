#include <string.h>
#include "mqtt_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "debug.h"

static const char *TAG = "MQTT_MGR";

// MQTT credentials from Kconfig
#define MQTT_BROKER_URI CONFIG_LINKY_MQTT_BROKER_URI
#define MQTT_USERNAME   CONFIG_LINKY_MQTT_USERNAME
#define MQTT_PASSWORD   CONFIG_LINKY_MQTT_PASSWORD

// MQTT topics
#define MQTT_TOPIC_IINST CONFIG_LINKY_MQTT_TOPIC_PREFIX "/iinst"
#define MQTT_TOPIC_BASE  CONFIG_LINKY_MQTT_TOPIC_PREFIX "/base"
#define MQTT_TOPIC_VCAP  CONFIG_LINKY_MQTT_TOPIC_PREFIX "/vcap"

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    mqtt_state_t *state = (mqtt_state_t*)handler_args;
    if (!state) return;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            DEBUG_LOG(TAG, "MQTT connected");
            state->connected = true;
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
        .network.timeout_ms = 1000,  // Reduced from 3000ms
        .network.refresh_connection_after_ms = 0,
        .buffer.size = 512,
        .buffer.out_size = 512,
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

    char payload[32];
    int ret;

    // Publish IINST (QoS 1 for ACK-based connection monitoring)
    if (data->valid_flags & LINKY_FLAG_IINST) {
        snprintf(payload, sizeof(payload), "%u", data->iinst);
        ret = esp_mqtt_client_publish(state->client, MQTT_TOPIC_IINST, payload, 0, 1, 0);
        if (ret < 0) {
            DEBUG_LOGW(TAG, "Failed to publish IINST");
            return false;
        }
        DEBUG_LOG(TAG, "Published: IINST=%u", data->iinst);
    }

    // Publish BASE (QoS 1)
    if (data->valid_flags & LINKY_FLAG_BASE) {
        snprintf(payload, sizeof(payload), "%lu", data->base);
        ret = esp_mqtt_client_publish(state->client, MQTT_TOPIC_BASE, payload, 0, 1, 0);
        if (ret < 0) {
            DEBUG_LOGW(TAG, "Failed to publish BASE");
            return false;
        }
        DEBUG_LOG(TAG, "Published: BASE=%lu", data->base);
    }

    // Publish VCAP (QoS 1)
    snprintf(payload, sizeof(payload), "%lu", data->voltage_cap);
    ret = esp_mqtt_client_publish(state->client, MQTT_TOPIC_VCAP, payload, 0, 1, 0);
    if (ret < 0) {
        DEBUG_LOGW(TAG, "Failed to publish VCAP");
        return false;
    }
    DEBUG_LOG(TAG, "Published: VCAP=%lu", data->voltage_cap);

    return true;
}
