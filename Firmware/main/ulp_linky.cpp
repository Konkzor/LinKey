#include <string.h>
#include <stddef.h>
#include "esp_log.h"

extern "C" {
#include "hulp.h"
#include "hulp_uart.h"
}

#include "ulp_linky.h"
#include "tic_parser.h"
#include "debug.h"

static const char *TAG = "ULP_LINKY";

// Custom UART RX macro for 7E1 (7 data bits, Even parity, 1 stop bit)
// Based on HULP's M_INCLUDE_UART_RX - modified for 7E1 and 16-bit length.
// Uses hardcoded buffer_capacity instead of 8-bit metadata field, allowing >255 byte buffers.
// Metadata word stores only the received length (full 16 bits).
#define M_INCLUDE_UART_RX_7E1(label_entry, baud_rate, rx_gpio, reg_string_ptr, reg_scr, reg_return, termination_char, buffer_capacity) \
    M_LABEL(label_entry), \
        M_MOVL(R0,label_entry),                         /*Need all the registers we can get for this one, so the return address */  \
        I_ST(reg_return,R0,36),                         /*  is saved temporarily (offset 36 = subroutine size) */                   \
        I_MOVI(reg_scr,0),                              /*reg_scr is used to count received bytes*/                                 \
        I_MOVI(R0,(buffer_capacity)),                   /*Begin loop for each byte: load capacity as immediate (16-bit) */          \
        I_SUBR(R0,R0,reg_scr),                          /*  Compare buffer capacity with current length to check if full*/          \
        I_BL(23, 1),                                    /*  If full, branch to end (store length + return) */                       \
        I_GPIO_READ(rx_gpio),                           /*Wait here until pin goes low (start bit)*/                                \
        I_BGE(-1,1),                                                                                                                \
        I_STAGE_RST(),                                                                                                              \
        I_DELAY((uint16_t)(hulp_get_fast_clk_freq() / 2 / (baud_rate) + 34 - 36)),                                                   \
        I_DELAY((uint16_t)(hulp_get_fast_clk_freq() / (baud_rate) - 34)),                                                        \
        I_GPIO_READ(rx_gpio),                           /*Read the new bit, make room for it in another reg, and OR it in*/         \
        I_RSHI(reg_return,reg_return,1),                                                                                            \
        I_LSHI(R0,R0,15),                                                                                                           \
        I_ORR(reg_return,reg_return,R0),                                                                                            \
        I_STAGE_INC(1),                                                                                                             \
        I_JUMPS(-6, 7, JUMPS_LT),                       /* 7 instead of 8 - read 7 data bits */                                    \
        I_RSHI(reg_return,reg_return,1),                /* Shift once more to align 7 bits to [15:8] like 8-bit */                  \
        I_DELAY((uint16_t)(hulp_get_fast_clk_freq() / (baud_rate) - 34)),  /* Skip parity bit timing */                            \
        I_RSHI(R0,reg_scr,1),                           /*Store the byte. ulpstring =one word metadata, then 2 chars in every */    \
        I_ADDR(R0,reg_string_ptr,R0),                   /*  word thereafter, so offset = 1+length/2 (ie. length >> 1, */            \
        I_ST(reg_return,R0,1),                          /*  add that to string ptr, then I_ST with 1 offset) */                     \
        I_GPIO_READ(rx_gpio),                           /*Wait here until pin goes high to sync with stop bit*/                     \
        I_BL(-1,1),                                                                                                                 \
        I_SUBI(R0,reg_return,(termination_char)<<8),    /* 7-bit data now aligned to [15:8] like original */                        \
        I_BL(3,1<<8),                                   /* If termination char found, branch to end */                              \
        I_ADDI(reg_scr,reg_scr,1),                      /*  else increment length and loop back to capacity check */                \
        I_BGE(-24,0),                                   /* Back to I_MOVI capacity (-24 instead of -25: 1 fewer instruction) */     \
        I_ST(reg_scr,reg_string_ptr,0),                 /*Store received length as full 16-bit value in metadata word */            \
        M_MOVL(reg_return,label_entry),                 /*Now need to load the return address saved at the beginning, and return */ \
        I_LD(reg_return,reg_return,36),                                                                                             \
        I_BXR(reg_return),                                                                                                          \
        I_HALT()

// Wrapper with default registers like the original
#define M_INCLUDE_UART_RX_7E1_SIMPLE(label_entry, baud_rate, rx_gpio, termination_char, buffer_capacity) \
    M_INCLUDE_UART_RX_7E1(label_entry, baud_rate, rx_gpio, R1, R2, R3, termination_char, buffer_capacity)

// RTC_DATA_ATTR variables are stored in RTC slow memory and persist across deep sleep
// Double-buffered frame reception: ULP alternates between buf_0 and buf_1
// Raw arrays instead of HULP_UART_STRING_BUFFER (which is limited to 255 bytes
// by its 8-bit metadata fields). Layout: [0]=16-bit length, [1..N]=2 chars per word.
RTC_DATA_ATTR ulp_var_t ulp_frame_buf_0[1 + (LINKY_MAX_FRAME_LEN / 2)];
RTC_DATA_ATTR ulp_var_t ulp_frame_buf_1[1 + (LINKY_MAX_FRAME_LEN / 2)];
RTC_DATA_ATTR ulp_var_t ulp_active_buf;  // 0 or 1: which buffer CPU should read

void init_ulp_linky(void)
{
    DEBUG_LOG(TAG, "Initializing ULP for Linky monitoring");

    // Labels for ULP program
    enum {
        LBL_LOOP,
        LBL_RX_0,
        LBL_RX_1,
        LBL_SUBROUTINE_RX_ENTRY,
    };

    const ulp_insn_t program[] = {
        // Main loop: ping-pong between two frame buffers
        M_LABEL(LBL_LOOP),

        // Receive frame into buffer 0
        I_MOVO(R1, ulp_frame_buf_0),
        M_RETURN(LBL_RX_0, R3, LBL_SUBROUTINE_RX_ENTRY),
        // Signal CPU: read buffer 0
        I_MOVO(R0, ulp_active_buf),
        I_MOVI(R1, 0),
        I_ST(R1, R0, 0),
        I_WAKE(),

        // Receive frame into buffer 1
        I_MOVO(R1, ulp_frame_buf_1),
        M_RETURN(LBL_RX_1, R3, LBL_SUBROUTINE_RX_ENTRY),
        // Signal CPU: read buffer 1
        I_MOVO(R0, ulp_active_buf),
        I_MOVI(R1, 1),
        I_ST(R1, R0, 0),
        I_WAKE(),

        M_BX(LBL_LOOP),

        // UART RX subroutine - 7E1 format for Linky - ETX (0x03) as termination
        M_INCLUDE_UART_RX_7E1_SIMPLE(LBL_SUBROUTINE_RX_ENTRY, LINKY_BAUD_RATE, LINKY_RX_GPIO, LINKY_TIC_FRAME_END, LINKY_MAX_FRAME_LEN),
    };

    // Configure GPIO for RX (input with pullup)
    ESP_ERROR_CHECK(hulp_configure_pin(LINKY_RX_GPIO, RTC_GPIO_MODE_INPUT_ONLY, GPIO_PULLUP_ONLY, 0));

    // Load and run ULP program continuously (no periodic timer)
    ESP_ERROR_CHECK(hulp_ulp_load(program, sizeof(program), 0, 0));
    ESP_ERROR_CHECK(hulp_ulp_run(0));

    DEBUG_LOG(TAG, "ULP started, monitoring GPIO %d at %d baud", LINKY_RX_GPIO, LINKY_BAUD_RATE);
}

// Extract frame string from ULP buffer with 16-bit length in metadata word.
// Same char packing as HULP (2 chars per 16-bit word), but supports >255 bytes.
static int frame_string_get(ulp_var_t *buf, char *frame, int frame_size)
{
    int len = buf[0].val & 0xFFFF;
    if (len <= 0 || len >= frame_size) return -1;
    for (int i = 0; i < len; i++) {
        frame[i] = (char)((buf[1 + i / 2].val >> ((i % 2) * 8)) & 0xFF);
    }
    frame[len] = '\0';
    return len;
}

void get_linky_data(linky_data_t *data)
{
    if (!data) return;
    memset(data, 0, sizeof(linky_data_t));

    // Read the buffer the ULP says is ready (ping-pong)
    ulp_var_t *buf = (ulp_active_buf.val & 1) ? ulp_frame_buf_1 : ulp_frame_buf_0;

    char frame[LINKY_MAX_FRAME_LEN + 1];
    int len = frame_string_get(buf, frame, sizeof(frame));

    DEBUG_LOG(TAG, "Frame buffer length: %d", len);

    if (len < 2 || frame[0] != LINKY_TIC_FRAME_START) {
        DEBUG_LOGW(TAG, "No valid frame (len=%d, first=0x%02x)", len, len > 0 ? (unsigned char)frame[0] : 0);
        return;
    }

#ifdef CONFIG_LINKEY_DEBUG_LOGS
    ESP_LOG_BUFFER_HEX(TAG, frame, len);
#endif

    tic_parse_frame(frame, len, data);
}
