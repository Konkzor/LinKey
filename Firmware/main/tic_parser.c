#include "tic_parser.h"
#include "debug.h"

#include <stddef.h>
#include <string.h>
#include <ctype.h>

static const char *TAG = "TIC_PARSER";

// Returns 1 if str1 starts with str2.
static int str_starts_with(const char *str1, const char *str2)
{
    while (*str2) {
        if (*str1 != *str2) return 0;
        str1++;
        str2++;
    }
    return 1;
}

// Decimal ASCII -> uint32. *len receives the number of digits consumed.
static uint32_t parse_uint(const char *str, int *len)
{
    uint32_t val = 0;
    int count = 0;
    while (*str && isdigit((unsigned char)*str)) {
        val = val * 10 + (uint32_t)(*str - '0');
        str++;
        count++;
    }
    if (len) *len = count;
    return val;
}

// Label descriptor for table-driven parsing.
typedef struct {
    const char *label;
    uint8_t     data_len;    // Expected number of digits in the value
    const char *unit;        // Informational, used in debug logs
    uint16_t    flag;        // LINKY_FLAG_* bit to set on successful parse
    uint16_t    offset;      // offsetof() into linky_data_t
    uint8_t     field_size;  // sizeof field: 2 (uint16_t) or 4 (uint32_t)
} linky_label_t;

#define FIELD(member) (uint16_t)offsetof(linky_data_t, member), (uint8_t)sizeof(((linky_data_t *)0)->member)

static const linky_label_t linky_labels[] = {
    { LABEL_IINST, 3, "A",  LINKY_FLAG_IINST, FIELD(iinst) },
    { LABEL_BASE, 9, "Wh", LINKY_FLAG_BASE, FIELD(base) },
    { LABEL_HCHC,  9, "Wh", LINKY_FLAG_HCHC,  FIELD(hchc) },
    { LABEL_HCHP,  9, "Wh", LINKY_FLAG_HCHP,  FIELD(hchp) },
    { LABEL_EJPHN,  9, "Wh", LINKY_FLAG_EJPHN,  FIELD(ejphn) },
    { LABEL_EJPHPM, 9, "Wh", LINKY_FLAG_EJPHPM, FIELD(ejphpm) },
    { LABEL_BBRHCJB, 9, "Wh", LINKY_FLAG_BBRHCJB, FIELD(bbrhcjb) },
    { LABEL_BBRHPJB, 9, "Wh", LINKY_FLAG_BBRHPJB, FIELD(bbrhpjb) },
    { LABEL_BBRHCJW, 9, "Wh", LINKY_FLAG_BBRHCJW, FIELD(bbrhcjw) },
    { LABEL_BBRHPJW, 9, "Wh", LINKY_FLAG_BBRHPJW, FIELD(bbrhpjw) },
    { LABEL_BBRHCJR, 9, "Wh", LINKY_FLAG_BBRHCJR, FIELD(bbrhcjr) },
    { LABEL_BBRHPJR, 9, "Wh", LINKY_FLAG_BBRHPJR, FIELD(bbrhpjr) },
    { LABEL_PAPP, 5, "VA", LINKY_FLAG_PAPP, FIELD(papp) },
    { LABEL_ADPS, 3, "A",  LINKY_FLAG_ADPS, FIELD(adps) },
};

#define LINKY_LABELS_COUNT (sizeof(linky_labels) / sizeof(linky_labels[0]))

bool tic_validate_checksum(const char *group, int group_len)
{
    if (!group || group_len < 5) return false;

    // Trim trailing CR / LF — checksum is the last byte before those.
    int cs_pos = group_len - 1;
    while (cs_pos > 0 && (group[cs_pos] == LINKY_TIC_GROUP_END
                       || group[cs_pos] == LINKY_TIC_GROUP_START)) {
        cs_pos--;
    }
    if (cs_pos < 2) return false;

    char expected_cs = group[cs_pos];

    // Sum the controlled zone: everything up to (but excluding) the
    // separator that precedes the checksum.
    int sum = 0;
    for (int i = 0; i < cs_pos - 1; i++) {
        sum += (unsigned char)group[i];
    }

    char calculated_cs = (char)((sum & 0x3F) + 0x20);
    return calculated_cs == expected_cs;
}

void tic_parse_group(const char *group, int group_len, linky_data_t *data)
{
    if (!group || !data) return;

    DEBUG_LOG(TAG, "RX (%d): %s", group_len, group);

    if (!tic_validate_checksum(group, group_len)) {
        DEBUG_LOGW(TAG, "Invalid checksum");
        return;
    }

    const char *msg = group;
    while (*msg == LINKY_TIC_GROUP_SEP) msg++;

    for (int i = 0; i < (int)LINKY_LABELS_COUNT; i++) {
        const linky_label_t *lbl = &linky_labels[i];
        if (!str_starts_with(msg, lbl->label)) continue;

        msg += strlen(lbl->label);
        while (*msg == LINKY_TIC_GROUP_SEP) msg++;

        int len_parsed = 0;
        uint32_t val = parse_uint(msg, &len_parsed);
        if (len_parsed != lbl->data_len) return;

        DEBUG_LOG(TAG, "%s: %lu %s", lbl->label, (unsigned long)val, lbl->unit);

        if (lbl->field_size == 2) {
            *(uint16_t *)((char *)data + lbl->offset) = (uint16_t)val;
        } else {
            *(uint32_t *)((char *)data + lbl->offset) = val;
        }
        data->valid_flags |= lbl->flag;
        return;
    }
}

int tic_parse_frame(const char *frame, int frame_len, linky_data_t *data)
{
    if (!frame || !data) return 0;
    if (frame_len < 2 || frame[0] != LINKY_TIC_FRAME_START) return 0;

    int parsed = 0;
    int i = 1;  // skip STX

    while (i < frame_len) {
        if (frame[i] == LINKY_TIC_GROUP_START) {
            i++;  // skip LF
            int start = i;
            while (i < frame_len && frame[i] != LINKY_TIC_GROUP_START) i++;
            if (i > start) {
                uint16_t flags_before = data->valid_flags;
                tic_parse_group(&frame[start], i - start, data);
                if (data->valid_flags != flags_before) parsed++;
            }
        } else {
            i++;
        }
    }
    return parsed;
}
