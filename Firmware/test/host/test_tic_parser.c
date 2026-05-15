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

// --- tic_parse_frame -------------------------------------------------------

static void test_frame_three_groups(void)
{
    char g1[32], g2[32], g3[32];
    int n1 = build_group(g1, "IINST", "005"); g1[n1] = 0;
    int n2 = build_group(g2, "BASE",  "010000000"); g2[n2] = 0;
    int n3 = build_group(g3, "PAPP",  "01100"); g3[n3] = 0;
    const char *groups[3] = { g1, g2, g3 };
    char frame[128];
    int fn = build_frame(frame, groups, 3);

    int parsed = tic_parse_frame(frame, fn, &data);
    TEST_ASSERT_EQUAL_INT(3, parsed);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_IINST);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_BASE);
    TEST_ASSERT_TRUE(data.valid_flags & LINKY_FLAG_PAPP);
    TEST_ASSERT_EQUAL_UINT16(5, data.iinst);
    TEST_ASSERT_EQUAL_UINT32(10000000u, data.base);
    TEST_ASSERT_EQUAL_UINT32(1100u, data.papp);
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

    RUN_TEST(test_frame_three_groups);
    RUN_TEST(test_frame_bad_checksum_skipped);
    RUN_TEST(test_frame_unknown_label_ignored);
    RUN_TEST(test_frame_missing_stx_returns_zero);
    RUN_TEST(test_frame_empty_returns_zero);
    RUN_TEST(test_frame_same_label_twice_last_wins);
    RUN_TEST(test_frame_no_lf_separators_returns_zero);

    return UNITY_END();
}
