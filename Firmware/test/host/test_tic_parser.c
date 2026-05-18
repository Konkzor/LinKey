#include "unity.h"
#include "tic_parser.h"

#include <string.h>

// Compute the TIC checksum over `len` bytes — mirrors the production
// algorithm so test fixtures stay self-consistent. One hand-computed
// reference case below pins the algorithm itself.
static char compute_cs(const char *bytes, int len)
{
    int sum = 0;
    for (int i = 0; i < len; i++) sum += (unsigned char)bytes[i];
    return (char)((sum & 0x3F) + 0x20);
}

// Build "<label><SP><data><SP><CS>" into out, return length.
static int build_group(char *out, const char *label, const char *data)
{
    int label_len = (int)strlen(label);
    int data_len = (int)strlen(data);
    int n = 0;
    memcpy(out + n, label, label_len); n += label_len;
    out[n++] = ' ';
    memcpy(out + n, data, data_len); n += data_len;
    int controlled_len = n;
    out[n++] = ' ';
    out[n++] = compute_cs(out, controlled_len);
    return n;
}

// Wrap groups in a STX [LF group CR]... frame, matching the on-device
// input to tic_parse_frame: the ULP RX layer strips the trailing ETX so
// the byte buffer ends right after the last group's CR.
static int build_frame(char *out, const char **groups, int count)
{
    int n = 0;
    out[n++] = 0x02; // STX
    for (int g = 0; g < count; g++) {
        int gl = (int)strlen(groups[g]);
        out[n++] = 0x0A; // LF
        memcpy(out + n, groups[g], gl); n += gl;
        out[n++] = 0x0D; // CR
    }
    return n;
}

static linky_data_t data;

void setUp(void)    { memset(&data, 0, sizeof(data)); }
void tearDown(void) {}

// --- tic_validate_checksum -------------------------------------------------

// Hand-verified: sum("ADPS 030") = 475; 475 & 0x3F = 27; 0x20 + 27 = 0x3B = ';'.
// This case pins the algorithm independently of build_group.
static void test_cs_hand_computed_adps(void)
{
    TEST_ASSERT_TRUE(tic_validate_checksum("ADPS 030 ;", 10));
}

static void test_cs_valid_iinst(void)
{
    char g[32];
    int n = build_group(g, "IINST", "003");
    TEST_ASSERT_TRUE(tic_validate_checksum(g, n));
}

static void test_cs_valid_base(void)
{
    char g[32];
    int n = build_group(g, "BASE", "012345678");
    TEST_ASSERT_TRUE(tic_validate_checksum(g, n));
}

static void test_cs_off_by_one_rejected(void)
{
    char g[32];
    int n = build_group(g, "IINST", "003");
    g[n - 1]++;
    TEST_ASSERT_FALSE(tic_validate_checksum(g, n));
}

static void test_cs_mutated_payload_rejected(void)
{
    char g[32];
    int n = build_group(g, "BASE", "012345678");
    g[6]++;  // flip a digit in the value
    TEST_ASSERT_FALSE(tic_validate_checksum(g, n));
}

static void test_cs_too_short_rejected(void)
{
    TEST_ASSERT_FALSE(tic_validate_checksum("A B", 3));
}

static void test_cs_strips_trailing_cr(void)
{
    char g[32];
    int n = build_group(g, "IINST", "003");
    g[n++] = 0x0D;
    TEST_ASSERT_TRUE(tic_validate_checksum(g, n));
}

static void test_cs_strips_trailing_lf(void)
{
    char g[32];
    int n = build_group(g, "IINST", "003");
    g[n++] = 0x0A;
    TEST_ASSERT_TRUE(tic_validate_checksum(g, n));
}

// --- tic_parse_group -------------------------------------------------------

static void test_parse_base_valid(void)
{
    char g[32];
    int n = build_group(g, "BASE", "012345678");
    tic_parse_group(g, n, &data);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_BASE);
    TEST_ASSERT_EQUAL_UINT32(12345678u, data.base);
}

static void test_parse_base_max(void)
{
    char g[32];
    int n = build_group(g, "BASE", "999999999");
    tic_parse_group(g, n, &data);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_BASE);
    TEST_ASSERT_EQUAL_UINT32(999999999u, data.base);
}

static void test_parse_base_leading_zeros(void)
{
    char g[32];
    int n = build_group(g, "BASE", "000000042");
    tic_parse_group(g, n, &data);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_BASE);
    TEST_ASSERT_EQUAL_UINT32(42u, data.base);
}

static void test_parse_iinst(void)
{
    char g[32];
    int n = build_group(g, "IINST", "012");
    tic_parse_group(g, n, &data);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_IINST);
    TEST_ASSERT_EQUAL_UINT16(12, data.iinst);
}

static void test_parse_papp(void)
{
    char g[32];
    int n = build_group(g, "PAPP", "01234");
    tic_parse_group(g, n, &data);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_PAPP);
    TEST_ASSERT_EQUAL_UINT32(1234u, data.papp);
}

static void test_parse_adps(void)
{
    char g[32];
    int n = build_group(g, "ADPS", "030");
    tic_parse_group(g, n, &data);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_ADPS);
    TEST_ASSERT_EQUAL_UINT16(30, data.adps);
}

static void test_parse_unknown_label_ignored(void)
{
    char g[32];
    int n = build_group(g, "ZZZZ", "012345678");
    tic_parse_group(g, n, &data);
    TEST_ASSERT_EQUAL_UINT16(0, data.valid_flags);
}

static void test_parse_wrong_length_rejected(void)
{
    // IINST expects exactly 3 digits.
    char g[32];
    int n = build_group(g, "IINST", "0012");
    tic_parse_group(g, n, &data);
    TEST_ASSERT_EQUAL_UINT16(0, data.valid_flags);
}

static void test_parse_bad_checksum_rejected(void)
{
    char g[32];
    int n = build_group(g, "IINST", "003");
    g[n - 1]++;
    tic_parse_group(g, n, &data);
    TEST_ASSERT_EQUAL_UINT16(0, data.valid_flags);
}

static void test_parse_null_data_no_crash(void)
{
    char g[32];
    int n = build_group(g, "IINST", "003");
    tic_parse_group(g, n, NULL);
    // Pass: did not segfault.
}

static void test_parse_null_group_no_crash(void)
{
    tic_parse_group(NULL, 10, &data);
    TEST_ASSERT_EQUAL_UINT16(0, data.valid_flags);
}

static void test_parse_value_shorter_than_expected(void)
{
    // IINST expects exactly 3 digits — give it 2.
    char g[32];
    int n = build_group(g, "IINST", "03");
    tic_parse_group(g, n, &data);
    TEST_ASSERT_EQUAL_UINT16(0, data.valid_flags);
}

static void test_parse_non_digit_in_value_rejected(void)
{
    // Build a group with a valid checksum but a non-digit in the value.
    // parse_uint stops at the first non-digit, so the consumed-length
    // check rejects it without writing to data.
    char g[32];
    int n = build_group(g, "IINST", "0A3");
    tic_parse_group(g, n, &data);
    TEST_ASSERT_EQUAL_UINT16(0, data.valid_flags);
}

static void test_parse_extra_separator_tolerated(void)
{
    // The parser skips runs of SP after the label, so an extra space
    // between label and value should still parse.
    char g[32];
    int n = 0;
    memcpy(g, "IINST", 5);             n += 5;
    g[n++] = ' '; g[n++] = ' ';        // two separators instead of one
    memcpy(g + n, "003", 3);           n += 3;
    int controlled_len = n;
    g[n++] = ' ';
    g[n++] = compute_cs(g, controlled_len);
    tic_parse_group(g, n, &data);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_IINST);
    TEST_ASSERT_EQUAL_UINT16(3, data.iinst);
}

// --- tic_parse_frame -------------------------------------------------------

static void test_frame_base_complete_ordered(void)
{
    char g1[32], g2[32], g3[32], g4[32], g5[32], g6[32], g7[32];
    char g8[32], g9[32], g10[32], g11[32];
    int n1  = build_group(g1,  "ADCO",     "123456789012"); g1[n1]  = 0;
    int n2  = build_group(g2,  "OPTARIF",  "BASE");         g2[n2]  = 0;
    int n3  = build_group(g3,  "ISOUSC",   "30");           g3[n3]  = 0;
    int n4  = build_group(g4,  "BASE",     "012345678");    g4[n4]  = 0;
    int n5  = build_group(g5,  "PTEC",     "TH..");         g5[n5]  = 0;
    int n6  = build_group(g6,  "IINST",    "005");          g6[n6]  = 0;
    int n7  = build_group(g7,  "ADPS",     "030");          g7[n7]  = 0;
    int n8  = build_group(g8,  "IMAX",     "090");          g8[n8]  = 0;
    int n9  = build_group(g9,  "PAPP",     "00690");        g9[n9]  = 0;
    int n10 = build_group(g10, "HHPHC",    "A");            g10[n10] = 0;
    int n11 = build_group(g11, "MOTDETAT", "000000");       g11[n11] = 0;

    const char *groups[11] = { g1, g2, g3, g4, g5, g6, g7, g8, g9, g10, g11 };
    char frame[320];
    int fn = build_frame(frame, groups, 11);

    int parsed = tic_parse_frame(frame, fn, &data);
    TEST_ASSERT_EQUAL_INT(4, parsed);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_IINST);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_BASE);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_PAPP);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_ADPS);
    TEST_ASSERT_EQUAL_UINT16(5, data.iinst);
    TEST_ASSERT_EQUAL_UINT32(12345678u, data.base);
    TEST_ASSERT_EQUAL_UINT32(690u, data.papp);
    TEST_ASSERT_EQUAL_UINT16(30, data.adps);
}

static void test_frame_hphc_complete_ordered(void)
{
    char g1[32], g2[32], g3[32], g4[32], g5[32], g6[32], g7[32];
    char g8[32], g9[32], g10[32], g11[32], g12[32];
    int n1  = build_group(g1,  "ADCO",     "123456789012"); g1[n1]  = 0;
    int n2  = build_group(g2,  "OPTARIF",  "HC..");         g2[n2]  = 0;
    int n3  = build_group(g3,  "ISOUSC",   "30");           g3[n3]  = 0;
    int n4  = build_group(g4,  "HCHC",     "000123456");    g4[n4]  = 0;
    int n5  = build_group(g5,  "HCHP",     "000023456");    g5[n5]  = 0;
    int n6  = build_group(g6,  "PTEC",     "HP..");         g6[n6]  = 0;
    int n7  = build_group(g7,  "IINST",    "005");          g7[n7]  = 0;
    int n8  = build_group(g8,  "ADPS",     "030");          g8[n8]  = 0;
    int n9  = build_group(g9,  "IMAX",     "090");          g9[n9]  = 0;
    int n10 = build_group(g10, "PAPP",     "00690");        g10[n10] = 0;
    int n11 = build_group(g11, "HHPHC",    "A");            g11[n11] = 0;
    int n12 = build_group(g12, "MOTDETAT", "000000");       g12[n12] = 0;

    const char *groups[12] = { g1, g2, g3, g4, g5, g6, g7, g8, g9, g10, g11, g12 };
    char frame[384];
    int fn = build_frame(frame, groups, 12);

    int parsed = tic_parse_frame(frame, fn, &data);
    TEST_ASSERT_EQUAL_INT(5, parsed);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_IINST);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_HCHC);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_HCHP);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_PAPP);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_ADPS);
    TEST_ASSERT_EQUAL_UINT16(5, data.iinst);
    TEST_ASSERT_EQUAL_UINT32(123456u, data.hchc);
    TEST_ASSERT_EQUAL_UINT32(23456u, data.hchp);
    TEST_ASSERT_EQUAL_UINT32(690u, data.papp);
    TEST_ASSERT_EQUAL_UINT16(30, data.adps);
}

static void test_frame_ejp_complete_ordered(void)
{
    char g1[32], g2[32], g3[32], g4[32], g5[32], g6[32], g7[32];
    char g8[32], g9[32], g10[32], g11[32], g12[32], g13[32];
    int n1  = build_group(g1,  "ADCO",     "123456789012"); g1[n1]  = 0;
    int n2  = build_group(g2,  "OPTARIF",  "EJP ");         g2[n2]  = 0;
    int n3  = build_group(g3,  "ISOUSC",   "30");           g3[n3]  = 0;
    int n4  = build_group(g4,  "EJPHN",    "000123456");    g4[n4]  = 0;
    int n5  = build_group(g5,  "EJPHPM",   "000023456");    g5[n5]  = 0;
    int n6  = build_group(g6,  "PEJP",     "30");           g6[n6]  = 0;
    int n7  = build_group(g7,  "PTEC",     "HN..");         g7[n7]  = 0;
    int n8  = build_group(g8,  "IINST",    "005");          g8[n8]  = 0;
    int n9  = build_group(g9,  "ADPS",     "030");          g9[n9]  = 0;
    int n10 = build_group(g10, "IMAX",     "090");          g10[n10] = 0;
    int n11 = build_group(g11, "PAPP",     "00690");        g11[n11] = 0;
    int n12 = build_group(g12, "HHPHC",    "A");            g12[n12] = 0;
    int n13 = build_group(g13, "MOTDETAT", "000000");       g13[n13] = 0;

    const char *groups[13] = { g1, g2, g3, g4, g5, g6, g7, g8, g9, g10, g11, g12, g13 };
    char frame[400];
    int fn = build_frame(frame, groups, 13);

    int parsed = tic_parse_frame(frame, fn, &data);
    TEST_ASSERT_EQUAL_INT(5, parsed);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_IINST);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_EJPHN);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_EJPHPM);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_PAPP);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_ADPS);
    TEST_ASSERT_EQUAL_UINT16(5, data.iinst);
    TEST_ASSERT_EQUAL_UINT32(123456u, data.ejphn);
    TEST_ASSERT_EQUAL_UINT32(23456u, data.ejphpm);
    TEST_ASSERT_EQUAL_UINT32(690u, data.papp);
    TEST_ASSERT_EQUAL_UINT16(30, data.adps);
}

static void test_frame_tempo_complete_ordered(void)
{
    char g1[32], g2[32], g3[32], g4[32], g5[32], g6[32], g7[32];
    char g8[32], g9[32], g10[32], g11[32], g12[32], g13[32], g14[32];
    char g15[32], g16[32], g17[32];
    int n1  = build_group(g1,  "ADCO",     "123456789012"); g1[n1]  = 0;
    int n2  = build_group(g2,  "OPTARIF",  "BBR(");         g2[n2]  = 0;
    int n3  = build_group(g3,  "ISOUSC",   "30");           g3[n3]  = 0;
    int n4  = build_group(g4,  "BBRHCJB",  "000123456");    g4[n4]  = 0;
    int n5  = build_group(g5,  "BBRHPJB",  "000023456");    g5[n5]  = 0;
    int n6  = build_group(g6,  "BBRHCJW",  "000023457");    g6[n6]  = 0;
    int n7  = build_group(g7,  "BBRHPJW",  "000023458");    g7[n7]  = 0;
    int n8  = build_group(g8,  "BBRHCJR",  "000023459");    g8[n8]  = 0;
    int n9  = build_group(g9,  "BBRHPJR",  "000023460");    g9[n9]  = 0;
    int n10 = build_group(g10, "PTEC",     "HPJB");         g10[n10] = 0;
    int n11 = build_group(g11, "DEMAIN",   "----");         g11[n11] = 0;
    int n12 = build_group(g12, "IINST",    "005");          g12[n12] = 0;
    int n13 = build_group(g13, "ADPS",     "030");          g13[n13] = 0;
    int n14 = build_group(g14, "IMAX",     "090");          g14[n14] = 0;
    int n15 = build_group(g15, "PAPP",     "00690");        g15[n15] = 0;
    int n16 = build_group(g16, "HHPHC",    "A");            g16[n16] = 0;
    int n17 = build_group(g17, "MOTDETAT", "000000");       g17[n17] = 0;

    const char *groups[17] = {
        g1, g2, g3, g4, g5, g6, g7,
        g8, g9, g10, g11, g12, g13, g14,
        g15, g16, g17
    };
    char frame[512];
    int fn = build_frame(frame, groups, 17);

    int parsed = tic_parse_frame(frame, fn, &data);
    TEST_ASSERT_EQUAL_INT(9, parsed);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_IINST);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_BBRHCJB);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_BBRHPJB);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_BBRHCJW);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_BBRHPJW);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_BBRHCJR);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_BBRHPJR);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_PAPP);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_ADPS);
    TEST_ASSERT_EQUAL_UINT16(5, data.iinst);
    TEST_ASSERT_EQUAL_UINT32(123456u, data.bbrhcjb);
    TEST_ASSERT_EQUAL_UINT32(23456u, data.bbrhpjb);
    TEST_ASSERT_EQUAL_UINT32(23457u, data.bbrhcjw);
    TEST_ASSERT_EQUAL_UINT32(23458u, data.bbrhpjw);
    TEST_ASSERT_EQUAL_UINT32(23459u, data.bbrhcjr);
    TEST_ASSERT_EQUAL_UINT32(23460u, data.bbrhpjr);
    TEST_ASSERT_EQUAL_UINT32(690u, data.papp);
    TEST_ASSERT_EQUAL_UINT16(30, data.adps);
}

static void test_frame_bad_checksum_skipped(void)
{
    char g1[32], g2[32], g3[32];
    int n1 = build_group(g1, "IINST", "005"); g1[n1] = 0;
    int n2 = build_group(g2, "BASE",  "010000000");
    g2[n2 - 1]++;                              // corrupt BASE checksum
    g2[n2] = 0;
    int n3 = build_group(g3, "PAPP",  "01100"); g3[n3] = 0;
    const char *groups[3] = { g1, g2, g3 };
    char frame[128];
    int fn = build_frame(frame, groups, 3);

    int parsed = tic_parse_frame(frame, fn, &data);
    TEST_ASSERT_EQUAL_INT(2, parsed);
    TEST_ASSERT_TRUE(data.valid_flags  & LINKY_FLAG_IINST);
    TEST_ASSERT_FALSE(data.valid_flags & LINKY_FLAG_BASE);
    TEST_ASSERT_TRUE(data.valid_flags  & LINKY_FLAG_PAPP);
}

static void test_frame_unknown_label_ignored(void)
{
    char g1[32], g2[32];
    int n1 = build_group(g1, "ZZZZ",  "012345678"); g1[n1] = 0;
    int n2 = build_group(g2, "IINST", "007"); g2[n2] = 0;
    const char *groups[2] = { g1, g2 };
    char frame[64];
    int fn = build_frame(frame, groups, 2);

    int parsed = tic_parse_frame(frame, fn, &data);
    TEST_ASSERT_EQUAL_INT(1, parsed);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_IINST);
    TEST_ASSERT_EQUAL_UINT16(7, data.iinst);
}

static void test_frame_missing_stx_returns_zero(void)
{
    char g[32];
    int n = build_group(g, "IINST", "003");
    int parsed = tic_parse_frame(g, n, &data);
    TEST_ASSERT_EQUAL_INT(0, parsed);
    TEST_ASSERT_EQUAL_UINT16(0, data.valid_flags);
}

static void test_frame_empty_returns_zero(void)
{
    char buf[4] = { 0x02 };
    int parsed = tic_parse_frame(buf, 1, &data);
    TEST_ASSERT_EQUAL_INT(0, parsed);
}

static void test_frame_same_label_twice_last_wins(void)
{
    char g1[32], g2[32];
    int n1 = build_group(g1, "IINST", "003"); g1[n1] = 0;
    int n2 = build_group(g2, "IINST", "008"); g2[n2] = 0;
    const char *groups[2] = { g1, g2 };
    char frame[64];
    int fn = build_frame(frame, groups, 2);

    tic_parse_frame(frame, fn, &data);
    TEST_ASSERT_EQUAL_UINT16(8, data.iinst);
}

static void test_frame_no_lf_separators_returns_zero(void)
{
    // STX directly followed by group content without any LF separator.
    char frame[] = "\x02IINST 003 X\x03";
    int parsed = tic_parse_frame(frame, sizeof(frame) - 1, &data);
    TEST_ASSERT_EQUAL_INT(0, parsed);
}

static void test_frame_null_pointer_no_crash(void)
{
    int parsed = tic_parse_frame(NULL, 16, &data);
    TEST_ASSERT_EQUAL_INT(0, parsed);
    TEST_ASSERT_EQUAL_UINT16(0, data.valid_flags);
}

static void test_frame_only_stx_and_lf(void)
{
    // STX + LF with no payload bytes: i advances past the LF, the inner
    // scan finds nothing (i == frame_len), so no group is emitted.
    char frame[2] = { 0x02, 0x0A };
    int parsed = tic_parse_frame(frame, sizeof(frame), &data);
    TEST_ASSERT_EQUAL_INT(0, parsed);
    TEST_ASSERT_EQUAL_UINT16(0, data.valid_flags);
}

static void test_frame_garbage_between_stx_and_lf_skipped(void)
{
    // Bytes that are neither LF nor part of a group are walked over
    // without effect; the real group still parses.
    char g[32];
    int gn = build_group(g, "IINST", "004");

    char frame[64];
    int n = 0;
    frame[n++] = 0x02;                       // STX
    frame[n++] = 'X'; frame[n++] = 'Y';      // stray bytes before first LF
    frame[n++] = 0x0A;                       // LF
    memcpy(frame + n, g, gn); n += gn;
    frame[n++] = 0x0D;                       // CR

    int parsed = tic_parse_frame(frame, n, &data);
    TEST_ASSERT_EQUAL_INT(1, parsed);
    TEST_ASSERT_EQUAL_UINT16(4, data.iinst);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_cs_hand_computed_adps);
    RUN_TEST(test_cs_valid_iinst);
    RUN_TEST(test_cs_valid_base);
    RUN_TEST(test_cs_off_by_one_rejected);
    RUN_TEST(test_cs_mutated_payload_rejected);
    RUN_TEST(test_cs_too_short_rejected);
    RUN_TEST(test_cs_strips_trailing_cr);
    RUN_TEST(test_cs_strips_trailing_lf);

    RUN_TEST(test_parse_base_valid);
    RUN_TEST(test_parse_base_max);
    RUN_TEST(test_parse_base_leading_zeros);
    RUN_TEST(test_parse_iinst);
    RUN_TEST(test_parse_papp);
    RUN_TEST(test_parse_adps);
    RUN_TEST(test_parse_unknown_label_ignored);
    RUN_TEST(test_parse_wrong_length_rejected);
    RUN_TEST(test_parse_bad_checksum_rejected);
    RUN_TEST(test_parse_null_data_no_crash);
    RUN_TEST(test_parse_null_group_no_crash);
    RUN_TEST(test_parse_value_shorter_than_expected);
    RUN_TEST(test_parse_non_digit_in_value_rejected);
    RUN_TEST(test_parse_extra_separator_tolerated);

    RUN_TEST(test_frame_base_complete_ordered);
    RUN_TEST(test_frame_hphc_complete_ordered);
    RUN_TEST(test_frame_ejp_complete_ordered);
    RUN_TEST(test_frame_tempo_complete_ordered);
    RUN_TEST(test_frame_bad_checksum_skipped);
    RUN_TEST(test_frame_unknown_label_ignored);
    RUN_TEST(test_frame_missing_stx_returns_zero);
    RUN_TEST(test_frame_empty_returns_zero);
    RUN_TEST(test_frame_same_label_twice_last_wins);
    RUN_TEST(test_frame_no_lf_separators_returns_zero);
    RUN_TEST(test_frame_null_pointer_no_crash);
    RUN_TEST(test_frame_only_stx_and_lf);
    RUN_TEST(test_frame_garbage_between_stx_and_lf_skipped);

    return UNITY_END();
}
