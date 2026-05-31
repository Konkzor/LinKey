/**
 * @file qemu_test_data.h
 * @brief Sample valid TIC frames for testing
 *
 * These are real-world examples of valid Linky TIC frames in historique mode
 * with correct checksums. Use these to test the parser and main flow.
 */

#ifndef QEMU_TEST_DATA_H
#define QEMU_TEST_DATA_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Valid TIC frame for BASE tariff option
 *
 * Mirrors test_frame_base_complete_ordered() from the host parser tests.
 */
#define TEST_TIC_FRAME_BASE \
    "\x02"  /* STX */ \
    "\nADCO 123456789012 G\r" \
    "\nOPTARIF BASE 0\r" \
    "\nISOUSC 30 9\r" \
    "\nBASE 012345678 /\r" \
    "\nPTEC TH.. $\r" \
    "\nIINST 005 \\" "\r" \
    "\nADPS 030 ;\r" \
    "\nIMAX 090 H\r" \
    "\nPAPP 00690 0\r" \
    "\nHHPHC A ,\r" \
    "\nMOTDETAT 000000 B\r"

#define TEST_TIC_FRAME_BASE_LEN (sizeof(TEST_TIC_FRAME_BASE) - 1)

/**
 * @brief Valid TIC frame for HPHC tariff option
 *
 * Mirrors test_frame_hphc_complete_ordered() from the host parser tests.
 */
#define TEST_TIC_FRAME_HPHC \
    "\x02"  /* STX */ \
    "\nADCO 123456789012 G\r" \
    "\nOPTARIF HC.. <\r" \
    "\nISOUSC 30 9\r" \
    "\nHCHC 000123456 [\r" \
    "\nHCHP 000023456 '\r" \
    "\nPTEC HP..  \r" \
    "\nIINST 005 \\" "\r" \
    "\nADPS 030 ;\r" \
    "\nIMAX 090 H\r" \
    "\nPAPP 00690 0\r" \
    "\nHHPHC A ,\r" \
    "\nMOTDETAT 000000 B\r"

#define TEST_TIC_FRAME_HPHC_LEN (sizeof(TEST_TIC_FRAME_HPHC) - 1)

/**
 * @brief Valid TIC frame for EJP tariff option
 *
 * Mirrors test_frame_ejp_complete_ordered() from the host parser tests.
 */
#define TEST_TIC_FRAME_EJP \
    "\x02"  /* STX */ \
    "\nADCO 123456789012 G\r" \
    "\nOPTARIF EJP  T\r" \
    "\nISOUSC 30 9\r" \
    "\nEJPHN 000123456 :\r" \
    "\nEJPHPM 000023456 H\r" \
    "\nPEJP 30 R\r" \
    "\nPTEC HN.. ^\r" \
    "\nIINST 005 \\" "\r" \
    "\nADPS 030 ;\r" \
    "\nIMAX 090 H\r" \
    "\nPAPP 00690 0\r" \
    "\nHHPHC A ,\r" \
    "\nMOTDETAT 000000 B\r"

#define TEST_TIC_FRAME_EJP_LEN (sizeof(TEST_TIC_FRAME_EJP) - 1)

/**
 * @brief Valid TIC frame for TEMPO tariff option
 *
 * Mirrors test_frame_tempo_complete_ordered() from the host parser tests.
 */
#define TEST_TIC_FRAME_TEMPO \
    "\x02"  /* STX */ \
    "\nADCO 123456789012 G\r" \
    "\nOPTARIF BBR( S\r" \
    "\nISOUSC 30 9\r" \
    "\nBBRHCJB 000123456 2\r" \
    "\nBBRHPJB 000023456 >\r" \
    "\nBBRHCJW 000023457 G\r" \
    "\nBBRHPJW 000023458 U\r" \
    "\nBBRHCJR 000023459 D\r" \
    "\nBBRHPJR 000023460 I\r" \
    "\nPTEC HPJB P\r" \
    "\nDEMAIN ---- \"\r" \
    "\nIINST 005 \\" "\r" \
    "\nADPS 030 ;\r" \
    "\nIMAX 090 H\r" \
    "\nPAPP 00690 0\r" \
    "\nHHPHC A ,\r" \
    "\nMOTDETAT 000000 B\r"

#define TEST_TIC_FRAME_TEMPO_LEN (sizeof(TEST_TIC_FRAME_TEMPO) - 1)

/**
 * @brief High current test case (good for power consumption testing)
 */
#define TEST_TIC_FRAME_HIGH_CURRENT \
    "\x02"  /* STX */ \
    "\nADCO 270612345678 J\r" \
    "\nISUB 00 S\r" \
    "\nBASE 012345678 /\r" \
    "\nIINST 075 #\r" \
    "\nPAPP 17250 0\r" \
    "\nHHPHC A ,\r" \
    "\nMOTDETAT 000000 B\r"

#define TEST_TIC_FRAME_HIGH_CURRENT_LEN (sizeof(TEST_TIC_FRAME_HIGH_CURRENT) - 1)

/**
 * @brief Low current test case (good for sleep/idle testing)
 */
#define TEST_TIC_FRAME_LOW_CURRENT \
    "\x02"  /* STX */ \
    "\nADCO 270612345678 J\r" \
    "\nISUB 00 S\r" \
    "\nBASE 012345678 /\r" \
    "\nIINST 002 Y\r" \
    "\nPAPP 00460 +\r" \
    "\nHHPHC A ,\r" \
    "\nMOTDETAT 000000 B\r"

#define TEST_TIC_FRAME_LOW_CURRENT_LEN (sizeof(TEST_TIC_FRAME_LOW_CURRENT) - 1)

#ifdef __cplusplus
}
#endif

#endif // QEMU_TEST_DATA_H
