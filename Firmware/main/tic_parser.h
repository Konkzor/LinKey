/**
 * @file tic_parser.h
 * @brief Pure (HW-free) parser for Linky TIC mode historique frames.
 *
 * No ESP-IDF, RTC or HW dependency — host-testable. The HW RX layer
 * (ulp_linky.cpp) reads a frame out of the RTC buffer and hands it to
 * tic_parse_frame as a plain char buffer.
 */

#ifndef TIC_PARSER_H
#define TIC_PARSER_H

#include <stdbool.h>
#include "tic_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Validate the TIC checksum of a single information group.
 *
 * @param group     Pointer to the group bytes (between two LF delimiters in
 *                  the frame). May include trailing CR/ETX/LF — those are
 *                  stripped before locating the checksum byte.
 * @param group_len Length of @p group in bytes.
 * @return true if the trailing checksum byte equals `(sum & 0x3F) + 0x20`
 *         over the controlled zone.
 */
bool tic_validate_checksum(const char *group, int group_len);

/**
 * @brief Parse a single TIC information group into @p data.
 *
 * Validates the checksum, matches the label against the (tariff-dependent)
 * label table, parses the value, and updates the matching field plus the
 * corresponding `LINKY_FLAG_*` bit in `data->valid_flags`. On any error
 * (bad checksum, unknown label, malformed value), @p data is left
 * untouched.
 */
void tic_parse_group(const char *group, int group_len, linky_data_t *data);

/**
 * @brief Parse a complete TIC frame (between STX and ETX) into @p data.
 *
 * Walks the frame, splits it into groups on `LF` boundaries, and calls
 * tic_parse_group on each. Caller is expected to zero @p data first if
 * a fresh result is wanted; this function only sets bits in valid_flags,
 * it never clears them.
 *
 * @return Number of groups successfully parsed (i.e. that updated a
 *         field in @p data). Returns 0 if the frame is empty, too short,
 *         or does not start with STX.
 */
int tic_parse_frame(const char *frame, int frame_len, linky_data_t *data);

#ifdef __cplusplus
}
#endif

#endif // TIC_PARSER_H
