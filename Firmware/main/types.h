/**
 * @file types.h
 * @brief Shared types for WiFi and MQTT managers
 */

#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Connection result codes
 */
typedef enum {
    CONN_OK,           /**< Connection successful */
    CONN_FAILED,       /**< Connection failed (timeout or error) */
    CONN_VOLTAGE_LOW   /**< Connection aborted due to low voltage */
} conn_result_t;

/**
 * @brief Voltage check callback type
 * @param threshold Voltage threshold in millivolts
 * @return true if voltage is below threshold (low condition)
 */
typedef bool (*voltage_check_fn_t)(uint16_t threshold);

#ifdef __cplusplus
}
#endif

#endif // TYPES_H
