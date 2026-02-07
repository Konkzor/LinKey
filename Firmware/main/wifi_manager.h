/**
 * @file wifi_manager.h
 * @brief WiFi connection manager with power-aware features
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Event bit indicating WiFi is connected */
#define WIFI_CONNECTED_BIT BIT0
/** @brief Event bit indicating WiFi connection failed */
#define WIFI_FAIL_BIT      BIT1

/**
 * @brief WiFi state structure
 * @note Instance must be owned by caller and passed to all functions
 */
typedef struct {
    bool initialized;              /**< One-time initialization done */
    bool started;                  /**< esp_wifi_start() has been called */
    EventGroupHandle_t event_group; /**< FreeRTOS event group for connection status */
} wifi_state_t;

/**
 * @brief Initialize WiFi subsystem (one-time)
 *
 * Initializes netif, event loop, WiFi driver and registers event handlers.
 * Uses credentials from Kconfig. Enables modem sleep for power saving.
 *
 * @param[in,out] state WiFi state structure (caller-owned)
 */
void wifi_init(wifi_state_t *state);

/**
 * @brief Stop WiFi to save power
 *
 * @note Caller should stop MQTT before calling this function
 *
 * @param[in,out] state WiFi state structure
 */
void wifi_stop(wifi_state_t *state);

/**
 * @brief Start WiFi if it was previously stopped
 *
 * @param[in,out] state WiFi state structure
 */
void wifi_start(wifi_state_t *state);

/**
 * @brief Connect to WiFi AP with voltage monitoring
 *
 * Attempts connection with periodic voltage checks. Uses cached BSSID/channel
 * from RTC memory for faster reconnection after deep sleep.
 *
 * @param[in,out] state            WiFi state structure
 * @param[in]     voltage_check    Callback to check low voltage (NULL to skip)
 * @param[in]     voltage_threshold Threshold passed to voltage_check (mV)
 * @param[in]     timeout_ms       Connection timeout in milliseconds
 * @param[in]     poll_interval_ms Polling interval for status checks
 *
 * @return CONN_OK on success, CONN_FAILED on timeout, CONN_VOLTAGE_LOW if voltage dropped
 */
conn_result_t wifi_connect(wifi_state_t *state, voltage_check_fn_t voltage_check,
                           uint16_t voltage_threshold, uint32_t timeout_ms,
                           uint32_t poll_interval_ms);

/**
 * @brief Check if WiFi is currently connected to AP
 *
 * @return true if connected, false otherwise
 */
bool wifi_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
