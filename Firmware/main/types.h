#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Connection result (used by wifi and mqtt managers)
typedef enum {
    CONN_OK,
    CONN_FAILED,
    CONN_VOLTAGE_LOW
} conn_result_t;

// Voltage check callback type (returns true if voltage is low)
typedef bool (*voltage_check_fn_t)(uint16_t threshold);

#ifdef __cplusplus
}
#endif

#endif // TYPES_H
