/**
 * @file fsm.h
 * @brief Pure (HW-free) state-transition logic for the application FSM.
 *
 * The functions here contain no IDF, FreeRTOS or hardware dependencies, so
 * they can be unit-tested on a host machine. main.c holds the handlers
 * that perform the side effects (init calls, connect calls, stop_all_*,
 * logging, LED blink) and delegates each post-call decision to one of the
 * functions below.
 */

#ifndef FSM_H
#define FSM_H

#include <stdbool.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Application FSM states.
 */
typedef enum {
    STATE_INIT,
    STATE_WAIT_VOLTAGE,
    STATE_BLE_PROVISION,
    STATE_WIFI_CONNECT,
    STATE_MQTT_CONNECT,
    STATE_WAIT_ULP_DATA,
    STATE_PUBLISH_DATA
} app_state_t;

/** @brief Next state after a wifi_connect() call. */
app_state_t fsm_after_wifi_connect(conn_result_t result);

/** @brief Next state after an mqtt_connect() call. */
app_state_t fsm_after_mqtt_connect(conn_result_t result);

/** @brief Next state after an ulp_wait_data() call. */
app_state_t fsm_after_wait_ulp_data(conn_result_t result);

/**
 * @brief Next state for STATE_PUBLISH_DATA based on the per-iteration
 *        guard checks. Inputs are evaluated in precedence order:
 *        voltage_low > !wifi_connected > !mqtt_connected > !data_valid >
 *        !publish_ok; callers must short-circuit accordingly so the
 *        boolean values reflect that ordering.
 */
app_state_t fsm_after_publish_data(bool voltage_low,
                                   bool wifi_connected,
                                   bool mqtt_connected,
                                   bool data_valid,
                                   bool publish_ok);

/**
 * @brief Voltage watchdog: if @p voltage_low is true and the current state
 *        runs after init, return STATE_WAIT_VOLTAGE; otherwise return
 *        @p current unchanged. STATE_INIT and STATE_WAIT_VOLTAGE pass
 *        through (their own logic owns the voltage check).
 */
app_state_t fsm_voltage_watchdog_next(app_state_t current, bool voltage_low);

/**
 * @brief Whether entering @p state should reset the dynamic-voltage peak.
 *        True for the states that read the dynamic threshold (MQTT_CONNECT,
 *        WAIT_ULP_DATA, PUBLISH_DATA); false elsewhere.
 */
bool fsm_should_reset_peak_on_enter(app_state_t state);

#ifdef __cplusplus
}
#endif

#endif // FSM_H
