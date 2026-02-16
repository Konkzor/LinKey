/**
 * @file voltage_manager.h
 * @brief Supercap voltage monitoring with dynamic drain detection
 */

#ifndef VOLTAGE_MANAGER_H
#define VOLTAGE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Voltage thresholds (millivolts)
#define VOLTAGE_START_MV            2500    // Minimum voltage to start active states
#define VOLTAGE_FALLBACK_MIN_MV     1500    // Minimum fallback threshold
#define VOLTAGE_FALLBACK_DROP_MV    200     // Max allowed drop from peak

/**
 * @brief Initialize supercap voltage ADC (GPIO 33, ADC1_CH5)
 */
void voltage_init(void);

/**
 * @brief Read supercap voltage in millivolts
 * @return Voltage in mV
 */
int voltage_read_mv(void);

/**
 * @brief Check if voltage is below a fixed threshold
 * @param threshold Voltage threshold in millivolts
 * @return true if voltage is below threshold
 */
bool voltage_is_low(uint16_t threshold);

/**
 * @brief Check if voltage dropped significantly from peak (drain detection)
 *
 * Tracks the highest voltage seen since last reset. Triggers if voltage
 * drops more than VOLTAGE_FALLBACK_DROP_MV from peak, with floor_mv
 * as the minimum threshold.
 *
 * @param floor_mv Minimum threshold in millivolts
 * @return true if voltage dropped below dynamic threshold
 */
bool voltage_is_low_dynamic(uint16_t floor_mv);

/**
 * @brief Reset the dynamic peak tracker
 *
 * Call on state transitions into states that use dynamic voltage checking,
 * so the peak is re-learned for the new state's current draw profile.
 */
void voltage_reset_peak(void);

#ifdef __cplusplus
}
#endif

#endif // VOLTAGE_MANAGER_H
