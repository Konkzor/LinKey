/**
 * @file tic_types.h
 * @brief Linky TIC (Télé-Information Client) protocol types and constants.
 *
 * Pure C, no ESP-IDF includes — shared between ulp_linky.h (the HW RX
 * layer) and tic_parser.h (the host-testable parsing layer).
 */

#ifndef TIC_TYPES_H
#define TIC_TYPES_H

#include <stdint.h>

// Pick up CONFIG_LINKEY_TARIFF_* on ESP-IDF builds. On host builds (Unity
// tests) sdkconfig.h does not exist; with no tariff macro defined we fall
// back to the BASE tariff layout, which is what the test fixtures use.
#if defined(__has_include)
#  if __has_include("sdkconfig.h")
#    include "sdkconfig.h"
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @name TIC framing (mode historique)
 * @{ */
#define LINKY_MAX_FRAME_LEN       300    /**< Max frame set length (TEMPO ≈ 275 bytes) */
#define LINKY_TIC_FRAME_START     0x02   /**< STX — start of TIC frame */
#define LINKY_TIC_FRAME_END       0x03   /**< ETX — end of TIC frame */
#define LINKY_TIC_GROUP_START     0x0A   /**< LF — start of data group */
#define LINKY_TIC_GROUP_END       0x0D   /**< CR — end of data group */
#define LINKY_TIC_GROUP_SEP       0x20   /**< SP — label/data/checksum separator (mode historique) */
/** @} */

/** @name TIC labels
 * @{ */
#define LABEL_IINST         "IINST"
#define LABEL_PAPP          "PAPP"
#define LABEL_ADPS          "ADPS"

#if defined(CONFIG_LINKEY_TARIFF_HPHC)
#define LABEL_HCHC          "HCHC"
#define LABEL_HCHP          "HCHP"
#elif defined(CONFIG_LINKEY_TARIFF_EJP)
#define LABEL_EJPHN         "EJPHN"
#define LABEL_EJPHPM        "EJPHPM"
#elif defined(CONFIG_LINKEY_TARIFF_TEMPO)
#define LABEL_BBRHCJB       "BBRHCJB"
#define LABEL_BBRHPJB       "BBRHPJB"
#define LABEL_BBRHCJW       "BBRHCJW"
#define LABEL_BBRHPJW       "BBRHPJW"
#define LABEL_BBRHCJR       "BBRHCJR"
#define LABEL_BBRHPJR       "BBRHPJR"
#else // BASE (default)
#define LABEL_BASE          "BASE"
#endif
/** @} */

/** @name Validity flags for linky_data_t.valid_flags
 * @{ */
#define LINKY_FLAG_IINST    0x01
#define LINKY_FLAG_PAPP     0x04
#define LINKY_FLAG_ADPS     0x08

#if defined(CONFIG_LINKEY_TARIFF_HPHC)
#define LINKY_FLAG_HCHC     0x02
#define LINKY_FLAG_HCHP     0x10
#elif defined(CONFIG_LINKEY_TARIFF_EJP)
#define LINKY_FLAG_EJPHN    0x02
#define LINKY_FLAG_EJPHPM   0x10
#elif defined(CONFIG_LINKEY_TARIFF_TEMPO)
#define LINKY_FLAG_BBRHCJB  0x02
#define LINKY_FLAG_BBRHPJB  0x10
#define LINKY_FLAG_BBRHCJW  0x20
#define LINKY_FLAG_BBRHPJW  0x40
#define LINKY_FLAG_BBRHCJR  0x80
#define LINKY_FLAG_BBRHPJR  0x100
#else // BASE
#define LINKY_FLAG_BASE     0x02
#endif
/** @} */

/** Decoded Linky TIC data. */
typedef struct {
    uint16_t iinst;        /**< Instantaneous current (A) */
#if defined(CONFIG_LINKEY_TARIFF_HPHC)
    uint32_t hchc;         /**< Off-peak energy index (Wh) */
    uint32_t hchp;         /**< Peak energy index (Wh) */
#elif defined(CONFIG_LINKEY_TARIFF_EJP)
    uint32_t ejphn;        /**< Normal hours energy index (Wh) */
    uint32_t ejphpm;       /**< Mobile peak energy index (Wh) */
#elif defined(CONFIG_LINKEY_TARIFF_TEMPO)
    uint32_t bbrhcjb;
    uint32_t bbrhpjb;
    uint32_t bbrhcjw;
    uint32_t bbrhpjw;
    uint32_t bbrhcjr;
    uint32_t bbrhpjr;
#else // BASE
    uint32_t base;         /**< Energy index (Wh) */
#endif
    uint32_t papp;         /**< Apparent power (VA) */
    uint16_t adps;         /**< Overcurrent warning current (A) */
    uint16_t valid_flags;  /**< Bitmap of LINKY_FLAG_* indicating which fields were parsed */
    uint32_t voltage_cap;  /**< Supercap voltage (mV) — filled by caller */
    uint32_t uptime_s;     /**< Device uptime (seconds) — filled by caller */
} linky_data_t;

#ifdef __cplusplus
}
#endif

#endif // TIC_TYPES_H
