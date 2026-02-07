#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// WiFi state structure (instance owned by caller)
typedef struct {
    bool initialized;           // One-time init done
    bool started;               // esp_wifi_start() called
    EventGroupHandle_t event_group;
} wifi_state_t;

// Event group bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Initialize WiFi subsystem (one-time)
void wifi_init(wifi_state_t *state);

// Stop WiFi to save power
// Note: Caller should stop MQTT before calling this
void wifi_stop(wifi_state_t *state);

// Start WiFi if it was stopped
void wifi_start(wifi_state_t *state);

// Connect to WiFi AP with voltage monitoring
// voltage_check: callback to check if voltage is low (can be NULL to skip)
// voltage_threshold: threshold to pass to voltage_check
// timeout_ms: connection timeout in milliseconds
// poll_interval_ms: polling interval for checks
conn_result_t wifi_connect(wifi_state_t *state, voltage_check_fn_t voltage_check,
                           uint16_t voltage_threshold, uint32_t timeout_ms,
                           uint32_t poll_interval_ms);

// Check if WiFi is connected to AP
bool wifi_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
