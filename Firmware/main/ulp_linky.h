#ifndef ULP_LINKY_H
#define ULP_LINKY_H

#include "hulp.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Linky TIC configuration
#define LINKY_RX_GPIO       GPIO_NUM_14
#define LINKY_BAUD_RATE     1200
#define LINKY_MAX_MSG_LEN   32 

// Message labels we're interested in
#define LABEL_IINST         "IINST"
#define LABEL_BASE          "BASE"

// Structure to hold decoded Linky data
typedef struct {
    uint16_t iinst;         // Instantaneous current (A)
    uint32_t base;          // Energy index (Wh)
    uint16_t valid_flags;   // Bit flags: bit0=iinst, bit1=base
    uint32_t voltage_cap;   // Super cap voltage (mV)
} linky_data_t;

// RTC data shared between ULP and main CPU
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

// Function to initialize and start ULP
void init_ulp_linky(void);

// Function to get data from ULP
void get_linky_data(linky_data_t *data);

#ifdef __cplusplus
}
#endif

#endif // ULP_LINKY_H
