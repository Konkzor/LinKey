#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include "mqtt_client.h"
#include "ulp_linky.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// WiFi stop callback type (for emergency stop from event handler)
typedef void (*wifi_stop_fn_t)(void);

// WiFi check callback type (returns true if WiFi is connected)
typedef bool (*wifi_check_fn_t)(void);

// MQTT state structure (instance owned by caller)
typedef struct {
    bool initialized;           // One-time init done
    bool started;               // esp_mqtt_client_start() called
    bool connected;             // Connected to broker
    esp_mqtt_client_handle_t client;
    wifi_stop_fn_t wifi_stop_cb;  // Callback for emergency WiFi stop
} mqtt_state_t;

// WiFi check callback type (returns true if WiFi is connected)
typedef bool (*wifi_check_fn_t)(void);

// WiFi stop callback type (for emergency stop from event handler)
typedef void (*wifi_stop_fn_t)(void);

// Initialize MQTT client (one-time)
// wifi_stop_cb: callback to stop WiFi immediately on MQTT message expiry (can be NULL)
void mqtt_init(mqtt_state_t *state, wifi_stop_fn_t wifi_stop_cb);

// Stop MQTT client
void mqtt_stop(mqtt_state_t *state);

// Connect to MQTT broker with voltage and WiFi monitoring
// voltage_check: callback to check if voltage is low (can be NULL to skip)
// voltage_threshold: threshold to pass to voltage_check
// wifi_check: callback to check if WiFi is connected (can be NULL to skip)
// timeout_ms: connection timeout in milliseconds
// poll_interval_ms: polling interval for checks
conn_result_t mqtt_connect(mqtt_state_t *state, voltage_check_fn_t voltage_check,
                           uint16_t voltage_threshold, wifi_check_fn_t wifi_check,
                           uint32_t timeout_ms, uint32_t poll_interval_ms);

// Check if MQTT is connected
bool mqtt_is_connected(mqtt_state_t *state);

// Publish Linky data to MQTT
// Returns true if all publishes succeeded
bool mqtt_publish_linky_data(mqtt_state_t *state, linky_data_t *data);

#ifdef __cplusplus
}
#endif

#endif // MQTT_MANAGER_H
