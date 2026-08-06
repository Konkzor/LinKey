/**
 * @file provisioning_manager.h
 * @brief BLE WiFi provisioning wrapper with voltage-aware exit.
 */

#ifndef PROVISIONING_MANAGER_H
#define PROVISIONING_MANAGER_H

#include <stdint.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start BLE provisioning and wait for WiFi credentials.
 *
 * The call returns when provisioning succeeds, fails, or voltage drops below
 * the supplied threshold.
 */
conn_result_t provisioning_start_ble(uint16_t voltage_threshold,
                                     uint32_t poll_interval_ms);

#ifdef __cplusplus
}
#endif

#endif // PROVISIONING_MANAGER_H
