#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "driver/rtc_io.h"
#include "soc/rtc.h"

extern "C" {
#include "hulp.h"
#include "hulp_uart.h"
}

#include "ulp_linky.h"
#include "debug.h"

static const char *TAG = "ULP_LINKY";

// Custom UART RX macro for 7E1 (7 data bits, Even parity, 1 stop bit)
// Based on HULP's M_INCLUDE_UART_RX - MINIMAL changes from original
#define M_INCLUDE_UART_RX_7E1(label_entry, baud_rate, rx_gpio, reg_string_ptr, reg_scr, reg_return, termination_char) \
    M_LABEL(label_entry), \
        M_MOVL(R0,label_entry),                         /*Need all the registers we can get for this one, so the return address */  \
        I_ST(reg_return,R0,40),                         /*  is saved temporarily (offset increased for 7E1 extra instructions)*/                                                  \
        I_MOVI(reg_scr,0),                              /*reg_scr is used to count received bytes*/                                 \
        I_LD(R0,reg_string_ptr,0),                      /*Begin loop for each byte: Load the metadata*/                             \
        I_RSHI(R0,R0,8),                                /*  Isolate the buffer size from it*/                                       \
        I_SUBR(R0,R0,reg_scr),                          /*  Then compare buffer size with current length to check if full*/         \
        I_BL(23, 1),                                    /* CHANGED: 23 instead of 21 (added 2 instructions: delay + read parity) */ \
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
        I_JUMPS(-6, 7, JUMPS_LT),                       /* CHANGED: 7 instead of 8 - read 7 data bits */                           \
        I_RSHI(reg_return,reg_return,1),                /* NEW: Shift once more to align 7 bits to [15:8] like 8-bit */           \
        I_DELAY((uint16_t)(hulp_get_fast_clk_freq() / (baud_rate) - 34)),  /* NEW: Skip parity bit timing */                     \
        I_RSHI(R0,reg_scr,1),                           /*Store the byte. ulpstring =one word metadata, then 2 chars in every */    \
        I_ADDR(R0,reg_string_ptr,R0),                   /*  word thereafter, so offset = 1+length/2 (ie. length >> 1, */            \
        I_ST(reg_return,R0,1),                          /*  add that to string ptr, then I_ST with 1 offset) */                     \
        I_GPIO_READ(rx_gpio),                           /*Wait here until pin goes high to sync with stop bit*/                     \
        I_BL(-1,1),                                                                                                                 \
        I_SUBI(R0,reg_return,(termination_char)<<8),    /* 7-bit data now aligned to [15:8] like original */                       \
        I_BL(3,1<<8),                                   /* Check if upper byte is zero */                 \
        I_ADDI(reg_scr,reg_scr,1),                      /*  else increment length and loop back to beginning of new byte */         \
        I_BGE(-25,0),                                   /* CHANGED: -25 instead of -23 (added 2 instructions: delay + read parity) */                 \
        I_LD(reg_return,reg_string_ptr,0),              /*Load the metadata (termination char / buffer full branches here)*/        \
        I_ANDI(reg_return,reg_return,0xFF<<8),          /*Update metadata with received length*/                                    \
        I_ORR(reg_return,reg_return,reg_scr),                                                                                       \
        I_ST(reg_return,reg_string_ptr,0),              /*  This I_ST also sets updated flag on metadata var */                     \
        M_MOVL(reg_return,label_entry),                 /*Now need to load the return address saved at the beginning, and return */ \
        I_LD(reg_return,reg_return,40),                                                                                             \
        I_BXR(reg_return),                                                                                                          \
        I_HALT()

// Wrapper with default registers like the original
#define M_INCLUDE_UART_RX_7E1_SIMPLE(label_entry, baud_rate, rx_gpio, termination_char) \
    M_INCLUDE_UART_RX_7E1(label_entry, baud_rate, rx_gpio, R1, R2, R3, termination_char)

// RTC_DATA_ATTR variables are stored in RTC slow memory and persist across deep sleep
RTC_DATA_ATTR ulp_var_t ulp_rx_buffer HULP_UART_STRING_BUFFER(LINKY_MAX_MSG_LEN);
RTC_DATA_ATTR ulp_var_t ulp_rx_line1 HULP_UART_STRING_BUFFER(LINKY_MAX_MSG_LEN);
RTC_DATA_ATTR ulp_var_t ulp_rx_line2 HULP_UART_STRING_BUFFER(LINKY_MAX_MSG_LEN);
RTC_DATA_ATTR ulp_var_t ulp_rx_line3 HULP_UART_STRING_BUFFER(LINKY_MAX_MSG_LEN);
RTC_DATA_ATTR ulp_var_t ulp_rx_line4 HULP_UART_STRING_BUFFER(LINKY_MAX_MSG_LEN);
RTC_DATA_ATTR ulp_var_t ulp_rx_line5 HULP_UART_STRING_BUFFER(LINKY_MAX_MSG_LEN);
RTC_DATA_ATTR ulp_var_t ulp_rx_line6 HULP_UART_STRING_BUFFER(LINKY_MAX_MSG_LEN);
RTC_DATA_ATTR ulp_var_t ulp_rx_line7 HULP_UART_STRING_BUFFER(LINKY_MAX_MSG_LEN);
RTC_DATA_ATTR ulp_var_t ulp_rx_line8 HULP_UART_STRING_BUFFER(LINKY_MAX_MSG_LEN);
RTC_DATA_ATTR ulp_var_t ulp_rx_line9 HULP_UART_STRING_BUFFER(LINKY_MAX_MSG_LEN);
RTC_DATA_ATTR ulp_var_t ulp_rx_line10 HULP_UART_STRING_BUFFER(LINKY_MAX_MSG_LEN);

// Helper to compare strings in ULP context
// Returns 1 if str1 starts with str2
static int str_starts_with(const char *str1, const char *str2) {
    while (*str2) {
        if (*str1 != *str2) return 0;
        str1++;
        str2++;
    }
    return 1;
}

// Parse integer from string
static uint32_t parse_uint(const char *str, int *len) {
    uint32_t val = 0;
    int count = 0;
    while (*str && isdigit((unsigned char)*str)) {
        val = val * 10 + (*str - '0');
        str++;
        count++;
    }
    if (len) *len = count;
    return val;
}

// Validate Linky checksum
// Format: "LABEL DATA CS\r\n"
// Checksum = (sum_of_bytes & 0x3F) + 0x20
static int validate_checksum(const char *msg, int msg_len) {
    if (msg_len < 5) return 0; // Too short

    // Find checksum (last char before \r or \n)
    int cs_pos = msg_len - 1;
    while (cs_pos > 0 && (msg[cs_pos] == '\r' || msg[cs_pos] == '\n')) {
        cs_pos--;
    }

    if (cs_pos < 2) return 0;

    char expected_cs = msg[cs_pos];

    // Calculate checksum (sum all bytes except checksum and line endings)
    int sum = 0;
    for (int i = 0; i < cs_pos - 1; i++) { // -1 to skip the space before checksum
        sum += (unsigned char)msg[i];
    }

    char calculated_cs = (sum & 0x3F) + 0x20;

    return (calculated_cs == expected_cs);
}

// Process received message
static void process_message(const char *msg, int len, linky_data_t *data) {
    DEBUG_LOG(TAG, "RX (%d): %s", len, msg);

    // Validate checksum first
    if (!validate_checksum(msg, len)) {
        ESP_LOGW(TAG, "Invalid checksum");
        return;
    }

    // Parse message: "LABEL DATA CS"
    // Skip leading whitespace
    while (*msg && isspace((unsigned char)*msg)) msg++;

    // Check label and extract data
    int len_parsed = 0;
    if (str_starts_with(msg, LABEL_IINST)) {
        msg += strlen(LABEL_IINST);
        while (*msg && isspace((unsigned char)*msg)) msg++;
        uint16_t iinst_temp = (uint16_t)parse_uint(msg, &len_parsed);
        if(len_parsed == 3){
            DEBUG_LOG(TAG, "IINST: %d A", data->iinst);
            data->iinst = iinst_temp;
            data->valid_flags |= 0x01;
        }
    }
    else if (str_starts_with(msg, LABEL_BASE)) {
        msg += strlen(LABEL_BASE);
        while (*msg && isspace((unsigned char)*msg)) msg++;
        uint32_t base_temp = parse_uint(msg, &len_parsed);
        if(len_parsed == 9){  
            DEBUG_LOG(TAG, "BASE: %lu Wh", data->base );
            data->base = base_temp;
            data->valid_flags |= 0x02;
        }
    }
}

void init_ulp_linky(void)
{
    DEBUG_LOG(TAG, "Initializing ULP for Linky monitoring");

    // Labels for ULP program - follow HULP example pattern
    enum {
        LBL_RX_INIT,
        LBL_RX_LINE1,
        LBL_RX_LINE2,
        LBL_RX_LINE3,
        LBL_RX_LINE4,
        LBL_RX_LINE5,
        LBL_RX_LINE6,
        LBL_RX_LINE7,
        LBL_RX_LINE8,
        LBL_RX_LINE9,
        LBL_RX_LINE10,
        LBL_SUBROUTINE_RX_ENTRY,
    };

    const ulp_insn_t program[] = {
        // Receive UART data into buffer using 7E1 format
        I_MOVO(R1, ulp_rx_buffer),
        M_RETURN(LBL_RX_INIT, R3, LBL_SUBROUTINE_RX_ENTRY),
        // Receive UART data into buffer using 7E1 format
        I_MOVO(R1, ulp_rx_line1),
        M_RETURN(LBL_RX_LINE1, R3, LBL_SUBROUTINE_RX_ENTRY),
        I_MOVO(R1, ulp_rx_line2),
        M_RETURN(LBL_RX_LINE2, R3, LBL_SUBROUTINE_RX_ENTRY),
        I_MOVO(R1, ulp_rx_line3),
        M_RETURN(LBL_RX_LINE3, R3, LBL_SUBROUTINE_RX_ENTRY),
        I_MOVO(R1, ulp_rx_line4),
        M_RETURN(LBL_RX_LINE4, R3, LBL_SUBROUTINE_RX_ENTRY),
        I_MOVO(R1, ulp_rx_line5),
        M_RETURN(LBL_RX_LINE5, R3, LBL_SUBROUTINE_RX_ENTRY),
        I_MOVO(R1, ulp_rx_line6),
        M_RETURN(LBL_RX_LINE6, R3, LBL_SUBROUTINE_RX_ENTRY),
        I_MOVO(R1, ulp_rx_line7),
        M_RETURN(LBL_RX_LINE7, R3, LBL_SUBROUTINE_RX_ENTRY),
        I_MOVO(R1, ulp_rx_line8),
        M_RETURN(LBL_RX_LINE8, R3, LBL_SUBROUTINE_RX_ENTRY),
        I_MOVO(R1, ulp_rx_line9),
        M_RETURN(LBL_RX_LINE9, R3, LBL_SUBROUTINE_RX_ENTRY),
        I_MOVO(R1, ulp_rx_line10),
        M_RETURN(LBL_RX_LINE10, R3, LBL_SUBROUTINE_RX_ENTRY),
        //Sleep
        I_WAKE(),
        I_HALT(),

        // UART RX subroutine - 7E1 format for Linky - LF as termination character
        M_INCLUDE_UART_RX_7E1_SIMPLE(LBL_SUBROUTINE_RX_ENTRY, LINKY_BAUD_RATE, LINKY_RX_GPIO, '\n'),
    };

    // Configure GPIO for RX (input with pullup)
    ESP_ERROR_CHECK(hulp_configure_pin(LINKY_RX_GPIO, RTC_GPIO_MODE_INPUT_ONLY, GPIO_PULLUP_ONLY, 0));

    // Load and run ULP program peridically
    ESP_ERROR_CHECK(hulp_ulp_load(program, sizeof(program), 10ULL * 1000 * 1000, 0));
    ESP_ERROR_CHECK(hulp_ulp_run(0));

    DEBUG_LOG(TAG, "ULP started, monitoring GPIO %d at %d baud", LINKY_RX_GPIO, LINKY_BAUD_RATE);
}

void get_linky_data(linky_data_t *data)
{
    if (!data) return;
    memset(data, 0, sizeof(linky_data_t));

    // Array of line buffers
    ulp_var_t* ulp_rx_lines[10] = {
        ulp_rx_line1,
        ulp_rx_line2,
        ulp_rx_line3,
        ulp_rx_line4,
        ulp_rx_line5,
        ulp_rx_line6,
        ulp_rx_line7,
        ulp_rx_line8,
        ulp_rx_line9,
        ulp_rx_line10
    };

    // Get received message for processing
    for(int i = 0 ; i < 10; i++) {

        char msg_buffer[LINKY_MAX_MSG_LEN + 1];
        int len = hulp_uart_string_get(ulp_rx_lines[i], msg_buffer, sizeof(msg_buffer), true);

        DEBUG_LOG(TAG, "Buffer length: %d", len);

        if (len > 0) {
#ifdef CONFIG_LINKY_DEBUG_LOGS
            // Dump raw hex
            ESP_LOG_BUFFER_HEX(TAG, msg_buffer, len);

            // Dump as ASCII (with non-printable shown as '.')
            for (int i = 0; i < len; i++) {
                if (i % 32 == 0) {
                    if (i > 0) printf("\n");
                    printf("%04d: ", i);
                }
                char c = msg_buffer[i];
                printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
            printf("\n");
#endif
            process_message(msg_buffer, len, data);
        }
    }
}
