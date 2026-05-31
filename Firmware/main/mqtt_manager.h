/**
 * @file mqtt_manager.h
 * @brief MQTT client manager with power-aware features
 */

#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include "mqtt_client.h"
#include "ulp_linky.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi connection check callback type
 * @return true if WiFi is connected
 */
typedef bool (*wifi_check_fn_t)(void);

/**
 * @brief MQTT state structure
 * @note Instance must be owned by caller and passed to all functions
 */
typedef struct {
    bool initialized;              /**< One-time initialization done */
    bool started;                  /**< esp_mqtt_client_start() has been called */
    bool connected;                /**< Currently connected to broker */
    esp_mqtt_client_handle_t client; /**< ESP-MQTT client handle */
    bool debug_frame_requested;    /**< Debug: request to publish raw TIC frame */
} mqtt_state_t;

/**
 * @brief Initialize MQTT client (one-time)
 *
 * Creates MQTT client with credentials from Kconfig and registers event handlers.
 *
 * @param[in,out] state MQTT state structure (caller-owned)
 */
void mqtt_init(mqtt_state_t *state);

/**
 * @brief Stop MQTT client
 *
 * @param[in,out] state MQTT state structure
 */
void mqtt_stop(mqtt_state_t *state);

/**
 * @brief Connect to MQTT broker with voltage and WiFi monitoring
 *
 * Attempts connection with periodic voltage and WiFi status checks.
 * Uses QoS 1 for reliable message delivery and disconnect detection.
 *
 * @param[in,out] state            MQTT state structure
 * @param[in]     voltage_check    Callback to check low voltage (NULL to skip)
 * @param[in]     voltage_threshold Threshold passed to voltage_check (mV)
 * @param[in]     wifi_check       Callback to check WiFi status (NULL to skip)
 * @param[in]     timeout_ms       Connection timeout in milliseconds
 * @param[in]     poll_interval_ms Polling interval for status checks
 *
 * @return CONN_OK on success, CONN_FAILED on timeout/WiFi lost, CONN_VOLTAGE_LOW if voltage dropped
 */
conn_result_t mqtt_connect(mqtt_state_t *state, voltage_check_fn_t voltage_check,
                           uint16_t voltage_threshold, wifi_check_fn_t wifi_check,
                           uint32_t timeout_ms, uint32_t poll_interval_ms);

/**
 * @brief Check if MQTT is currently connected to broker
 *
 * @param[in] state MQTT state structure
 * @return true if connected, false otherwise
 */
bool mqtt_is_connected(mqtt_state_t *state);

/**
 * @brief Publish Linky data to MQTT broker
 *
 * Publishes IINST, BASE (if valid) and VCAP values to configured topics.
 * Uses QoS 1 for ACK-based connection monitoring.
 *
 * @param[in] state MQTT state structure
 * @param[in] data  Linky data to publish
 *
 * @return true if all publishes succeeded, false otherwise
 */
bool mqtt_publish_linky_data(mqtt_state_t *state, linky_data_t *data);

/**
 * @brief Publish raw TIC frame to debug topic (linkey/debug/tic_frame)
 *
 * @param[in] state      MQTT state structure
 * @param[in] frame      Raw TIC frame buffer (including STX/ETX)
 * @param[in] frame_len  Length of frame
 *
 * @return true if publish succeeded, false otherwise
 */
bool mqtt_publish_tic_frame_debug(mqtt_state_t *state, const char *frame, int frame_len);

#ifdef __cplusplus
}
#endif

#endif // MQTT_MANAGER_H
