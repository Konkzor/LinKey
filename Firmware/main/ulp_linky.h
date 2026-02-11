/**
 * @file ulp_linky.h
 * @brief ULP-based Linky TIC protocol decoder
 *
 * Implements 7E1 UART reception on the ULP coprocessor for low-power
 * monitoring of French Linky smart meter TIC (Teleinformation Client) data.
 */

#ifndef ULP_LINKY_H
#define ULP_LINKY_H

#include "hulp.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @name Linky TIC Configuration
 * @{
 */
#define LINKY_RX_GPIO       GPIO_NUM_14  /**< GPIO for TIC serial input */
#define LINKY_BAUD_RATE     1200         /**< TIC baud rate (1200 bps) */
#define LINKY_MAX_MSG_LEN   32           /**< Maximum message length */
/** @} */

/** @name TIC Message Labels
 * @{
 */
#define LABEL_IINST         "IINST"      /**< Instantaneous current label */
#define LABEL_BASE          "BASE"       /**< Base energy index label */
#define LABEL_PAPP          "PAPP"       /**< Apparent power label */
#define LABEL_ADPS          "ADPS"       /**< Overcurrent warning label */
/** @} */

/** @name Valid Flags for linky_data_t
 * @{
 */
#define LINKY_FLAG_IINST    0x01         /**< IINST value is valid */
#define LINKY_FLAG_BASE     0x02         /**< BASE value is valid */
#define LINKY_FLAG_PAPP     0x04         /**< PAPP value is valid */
#define LINKY_FLAG_ADPS     0x08         /**< ADPS value is valid */
/** @} */

/**
 * @brief Decoded Linky TIC data
 */
typedef struct {
    uint16_t iinst;        /**< Instantaneous current (A) */
    uint32_t base;         /**< Energy index (Wh) */
    uint32_t papp;         /**< Apparent power (VA) */
    uint16_t adps;         /**< Overcurrent warning current (A) */
    uint16_t valid_flags;  /**< Validity flags (LINKY_FLAG_*) */
    uint32_t voltage_cap;  /**< Supercap voltage (mV), filled by caller */
    uint32_t uptime_s;     /**< Device uptime (seconds), filled by caller */
} linky_data_t;

/** @name ULP RTC Buffers
 * @brief Ring buffers in RTC slow memory shared between ULP and main CPU
 * @{
 */
extern ulp_var_t ulp_rx_buffer[];
extern ulp_var_t ulp_rx_line1[];
extern ulp_var_t ulp_rx_line2[];
extern ulp_var_t ulp_rx_line3[];
extern ulp_var_t ulp_rx_line4[];
extern ulp_var_t ulp_rx_line5[];
extern ulp_var_t ulp_rx_line6[];
extern ulp_var_t ulp_rx_line7[];
extern ulp_var_t ulp_rx_line8[];
extern ulp_var_t ulp_rx_line9[];
extern ulp_var_t ulp_rx_line10[];
/** @} */

/**
 * @brief Initialize and start ULP Linky decoder
 *
 * Loads ULP program implementing 7E1 UART RX at 1200 baud.
 * ULP continuously receives TIC frames into 10 ring buffers.
 * Main CPU polls buffers periodically via get_linky_data().
 */
void init_ulp_linky(void);

/**
 * @brief Get decoded data from ULP buffers
 *
 * Reads all 10 line buffers, validates checksums, and parses
 * IINST/BASE values. Sets valid_flags for successfully decoded fields.
 *
 * @param[out] data Structure to fill with decoded values (zeroed first)
 */
void get_linky_data(linky_data_t *data);

#ifdef __cplusplus
}
#endif

#endif // ULP_LINKY_H
