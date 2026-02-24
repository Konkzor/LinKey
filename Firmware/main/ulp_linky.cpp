#include <string.h>
#include <ctype.h>
#include "esp_log.h"

extern "C" {
#include "hulp.h"
#include "hulp_uart.h"
}

#include "ulp_linky.h"
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
// Format: "LABEL\tDATA\tCS\r" (data group extracted from TIC frame)
// Checksum = (sum_of_bytes & 0x3F) + 0x20
static int validate_checksum(const char *msg, int msg_len) {
    if (msg_len < 5) return 0; // Too short

    // Find checksum (last char before group end/start delimiters)
    int cs_pos = msg_len - 1;
    while (cs_pos > 0 && (msg[cs_pos] == LINKY_TIC_GROUP_END || msg[cs_pos] == LINKY_TIC_GROUP_START)) {
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
        DEBUG_LOGW(TAG, "Invalid checksum");
        return;
    }

    // Parse message: "LABEL DATA CS"
    // Skip leading whitespace
    while (*msg == LINKY_TIC_GROUP_SEP) msg++;

    // Check label and extract data
    int len_parsed = 0;
    if (str_starts_with(msg, LABEL_IINST)) {
        msg += strlen(LABEL_IINST);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint16_t iinst_temp = (uint16_t)parse_uint(msg, &len_parsed);
        if(len_parsed == 3){
            DEBUG_LOG(TAG, "IINST: %d A", iinst_temp);
            data->iinst = iinst_temp;
            data->valid_flags |= LINKY_FLAG_IINST;
        }
    }
#if defined(CONFIG_LINKEY_TARIFF_HPHC)
    else if (str_starts_with(msg, LABEL_HCHC)) {
        msg += strlen(LABEL_HCHC);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint32_t val = parse_uint(msg, &len_parsed);
        if(len_parsed == 9){
            DEBUG_LOG(TAG, "HCHC: %lu Wh", val);
            data->hchc = val;
            data->valid_flags |= LINKY_FLAG_HCHC;
        }
    }
    else if (str_starts_with(msg, LABEL_HCHP)) {
        msg += strlen(LABEL_HCHP);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint32_t val = parse_uint(msg, &len_parsed);
        if(len_parsed == 9){
            DEBUG_LOG(TAG, "HCHP: %lu Wh", val);
            data->hchp = val;
            data->valid_flags |= LINKY_FLAG_HCHP;
        }
    }
#elif defined(CONFIG_LINKEY_TARIFF_EJP)
    else if (str_starts_with(msg, LABEL_EJPHN)) {
        msg += strlen(LABEL_EJPHN);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint32_t val = parse_uint(msg, &len_parsed);
        if(len_parsed == 9){
            DEBUG_LOG(TAG, "EJPHN: %lu Wh", val);
            data->ejphn = val;
            data->valid_flags |= LINKY_FLAG_EJPHN;
        }
    }
    else if (str_starts_with(msg, LABEL_EJPHPM)) {
        msg += strlen(LABEL_EJPHPM);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint32_t val = parse_uint(msg, &len_parsed);
        if(len_parsed == 9){
            DEBUG_LOG(TAG, "EJPHPM: %lu Wh", val);
            data->ejphpm = val;
            data->valid_flags |= LINKY_FLAG_EJPHPM;
        }
    }
#elif defined(CONFIG_LINKEY_TARIFF_TEMPO)
    else if (str_starts_with(msg, LABEL_BBRHCJB)) {
        msg += strlen(LABEL_BBRHCJB);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint32_t val = parse_uint(msg, &len_parsed);
        if(len_parsed == 9){
            DEBUG_LOG(TAG, "BBRHCJB: %lu Wh", val);
            data->bbrhcjb = val;
            data->valid_flags |= LINKY_FLAG_BBRHCJB;
        }
    }
    else if (str_starts_with(msg, LABEL_BBRHPJB)) {
        msg += strlen(LABEL_BBRHPJB);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint32_t val = parse_uint(msg, &len_parsed);
        if(len_parsed == 9){
            DEBUG_LOG(TAG, "BBRHPJB: %lu Wh", val);
            data->bbrhpjb = val;
            data->valid_flags |= LINKY_FLAG_BBRHPJB;
        }
    }
    else if (str_starts_with(msg, LABEL_BBRHCJW)) {
        msg += strlen(LABEL_BBRHCJW);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint32_t val = parse_uint(msg, &len_parsed);
        if(len_parsed == 9){
            DEBUG_LOG(TAG, "BBRHCJW: %lu Wh", val);
            data->bbrhcjw = val;
            data->valid_flags |= LINKY_FLAG_BBRHCJW;
        }
    }
    else if (str_starts_with(msg, LABEL_BBRHPJW)) {
        msg += strlen(LABEL_BBRHPJW);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint32_t val = parse_uint(msg, &len_parsed);
        if(len_parsed == 9){
            DEBUG_LOG(TAG, "BBRHPJW: %lu Wh", val);
            data->bbrhpjw = val;
            data->valid_flags |= LINKY_FLAG_BBRHPJW;
        }
    }
    else if (str_starts_with(msg, LABEL_BBRHCJR)) {
        msg += strlen(LABEL_BBRHCJR);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint32_t val = parse_uint(msg, &len_parsed);
        if(len_parsed == 9){
            DEBUG_LOG(TAG, "BBRHCJR: %lu Wh", val);
            data->bbrhcjr = val;
            data->valid_flags |= LINKY_FLAG_BBRHCJR;
        }
    }
    else if (str_starts_with(msg, LABEL_BBRHPJR)) {
        msg += strlen(LABEL_BBRHPJR);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint32_t val = parse_uint(msg, &len_parsed);
        if(len_parsed == 9){
            DEBUG_LOG(TAG, "BBRHPJR: %lu Wh", val);
            data->bbrhpjr = val;
            data->valid_flags |= LINKY_FLAG_BBRHPJR;
        }
    }
#else // BASE
    else if (str_starts_with(msg, LABEL_BASE)) {
        msg += strlen(LABEL_BASE);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint32_t base_temp = parse_uint(msg, &len_parsed);
        if(len_parsed == 9){
            DEBUG_LOG(TAG, "BASE: %lu Wh", base_temp);
            data->base = base_temp;
            data->valid_flags |= LINKY_FLAG_BASE;
        }
    }
#endif
    else if (str_starts_with(msg, LABEL_PAPP)) {
        msg += strlen(LABEL_PAPP);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint32_t papp_temp = parse_uint(msg, &len_parsed);
        if(len_parsed == 5){
            DEBUG_LOG(TAG, "PAPP: %lu VA", papp_temp);
            data->papp = papp_temp;
            data->valid_flags |= LINKY_FLAG_PAPP;
        }
    }
    else if (str_starts_with(msg, LABEL_ADPS)) {
        msg += strlen(LABEL_ADPS);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;
        uint16_t adps_temp = (uint16_t)parse_uint(msg, &len_parsed);
        if(len_parsed == 3){
            DEBUG_LOGW(TAG, "ADPS: %d A (overcurrent!)", adps_temp);
            data->adps = adps_temp;
            data->valid_flags |= LINKY_FLAG_ADPS;
        }
    }
}

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
        return;  // Partial or empty frame — skip
    }

#ifdef CONFIG_LINKEY_DEBUG_LOGS
    // Dump raw hex
    ESP_LOG_BUFFER_HEX(TAG, frame, len);
#endif

    // Extract and process each data group from the frame
    // Frame format: STX [LF LABEL HT DATA HT CS CR] [LF ...] ...
    int i = 1;  // Skip STX (0x02)
    while (i < len) {
        if (frame[i] == LINKY_TIC_GROUP_START) {
            i++;  // Skip LF (start of data group)
            int start = i;
            // Find next LF or end of frame
            while (i < len && frame[i] != LINKY_TIC_GROUP_START) i++;
            if (i > start) {
                process_message(&frame[start], i - start, data);
            }
        } else {
            i++;
        }
    }
}
