/**
 * @file ulp_linky.h
 * @brief ULP-based Linky TIC protocol decoder (hardware layer)
 *
 * Implements 7E1 UART reception on the ULP coprocessor for low-power
 * monitoring of French Linky smart meter TIC (Téléinformation Client)
 * frames. The pure parsing logic lives in tic_parser.h.
 */

#ifndef ULP_LINKY_H
#define ULP_LINKY_H

#include "tic_types.h"
#include "hulp.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @name Hardware configuration
 * @{ */
#define LINKY_RX_GPIO   GPIO_NUM_14   /**< GPIO for TIC serial input */
#define LINKY_BAUD_RATE 1200          /**< TIC baud rate (1200 bps) */
/** @} */

/** @name ULP RTC frame buffers (ping-pong, double-buffered)
 *
 * ULP alternates between buf_0 and buf_1, receiving full TIC frames
 * (terminated by ETX 0x03). `ulp_active_buf` indicates which buffer
 * the CPU should read (the one just completed).
 * @{ */
extern ulp_var_t ulp_frame_buf_0[];
extern ulp_var_t ulp_frame_buf_1[];
extern ulp_var_t ulp_active_buf;
/** @} */

/**
 * @brief Initialize and start the ULP Linky decoder.
 *
 * Loads the ULP program implementing 7E1 UART RX at 1200 baud and runs
 * it continuously, receiving TIC frames into double-buffered RTC memory.
 * The main CPU consumes frames via get_linky_data().
 */
void init_ulp_linky(void);

/**
 * @brief Read and parse the active ULP frame buffer.
 *
 * Reads the buffer indicated by `ulp_active_buf`, unpacks the RTC
 * format into a plain char array, and hands it to tic_parse_frame()
 * for checksum validation and field extraction.
 *
 * @param[out] data Structure to fill with decoded values (zeroed first).
 */
void get_linky_data(linky_data_t *data);

#ifdef __cplusplus
}
#endif

#endif // ULP_LINKY_H
