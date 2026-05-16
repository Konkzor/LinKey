/**
 * @file voltage_state.h
 * @brief Pure (HW-free) voltage threshold logic for the supercap monitor.
 *
 * Functions here take the measured voltage as an argument and contain no
 * ADC, ESP-IDF or RTOS dependencies, which makes them unit-testable on a
 * host machine. voltage_manager.c is the thin shim that reads the ADC and
 * delegates to these functions.
 *
 * Naming follows the existing wifi_state_t / mqtt_state_t convention:
 * voltage_state_t is caller-owned and passed by pointer.
 */

#ifndef VOLTAGE_STATE_H
#define VOLTAGE_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int peak_mv;
} voltage_state_t;

/**
 * @brief Voltage strictly below a fixed threshold?
 */
bool voltage_state_is_low(int voltage_mv, uint16_t threshold_mv);

/**
 * @brief Voltage dropped more than @p drop_mv from the running peak?
 *
 * Updates @p state->peak_mv if @p voltage_mv exceeds it. The effective
 * threshold is `max(peak - drop_mv, floor_mv)`, so the check never falls
 * below @p floor_mv even on the first call when peak is zero.
 *
 * @return true when @p voltage_mv is below the dynamic threshold.
 */
bool voltage_state_is_low_dynamic(voltage_state_t *state,
                                  int voltage_mv,
                                  uint16_t floor_mv,
                                  uint16_t drop_mv);

/** @brief Reset the running peak so it is re-learned. */
void voltage_state_reset_peak(voltage_state_t *state);

#ifdef __cplusplus
}
#endif

#endif // VOLTAGE_STATE_H
