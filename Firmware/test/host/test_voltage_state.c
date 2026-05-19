#include "unity.h"
#include "voltage_state.h"

static voltage_state_t state;

void setUp(void)
{
    state = (voltage_state_t){0};
}

void tearDown(void) {}

// --- voltage_state_is_low ---------------------------------------------------

static void test_is_low_above_threshold(void)
{
    TEST_ASSERT_FALSE(voltage_state_is_low(2600, 2500));
}

static void test_is_low_at_threshold(void)
{
    // Threshold check is strict less-than: equal is NOT low.
    TEST_ASSERT_FALSE(voltage_state_is_low(2500, 2500));
}

static void test_is_low_below_threshold(void)
{
    TEST_ASSERT_TRUE(voltage_state_is_low(2400, 2500));
}

static void test_is_low_threshold_zero(void)
{
    TEST_ASSERT_FALSE(voltage_state_is_low(0, 0));
    TEST_ASSERT_FALSE(voltage_state_is_low(1, 0));
}

static void test_is_low_negative_voltage(void)
{
    // A transient ADC math glitch could yield a negative reading; it must
    // be treated as below any sane threshold so the watchdog reacts.
    TEST_ASSERT_TRUE(voltage_state_is_low(-1, 2500));
    TEST_ASSERT_TRUE(voltage_state_is_low(-1, 1));
}

// --- voltage_state_is_low_dynamic ------------------------------------------

static void test_dynamic_first_call_clamps_to_floor(void)
{
    // Peak starts at 0; first call should compare against floor, not (peak - drop).
    TEST_ASSERT_FALSE(voltage_state_is_low_dynamic(&state, 1600, 1500, 200));
    TEST_ASSERT_EQUAL_INT(1600, state.peak_mv);
}

static void test_dynamic_peak_rises_with_voltage(void)
{
    voltage_state_is_low_dynamic(&state, 2000, 1500, 200);
    voltage_state_is_low_dynamic(&state, 2500, 1500, 200);
    voltage_state_is_low_dynamic(&state, 2300, 1500, 200);  // does not lower peak
    TEST_ASSERT_EQUAL_INT(2500, state.peak_mv);
}

static void test_dynamic_drop_within_tolerance_not_low(void)
{
    voltage_state_is_low_dynamic(&state, 2500, 1500, 200);  // peak = 2500
    // 2500 - 200 = 2300. Voltage 2350 stays above the dynamic threshold.
    TEST_ASSERT_FALSE(voltage_state_is_low_dynamic(&state, 2350, 1500, 200));
}

static void test_dynamic_drop_exceeds_tolerance_is_low(void)
{
    voltage_state_is_low_dynamic(&state, 2500, 1500, 200);  // peak = 2500
    // 2500 - 200 = 2300; 2299 is below.
    TEST_ASSERT_TRUE(voltage_state_is_low_dynamic(&state, 2299, 1500, 200));
}

static void test_dynamic_floor_dominates_when_peak_is_high(void)
{
    voltage_state_is_low_dynamic(&state, 3000, 2800, 200);  // peak = 3000
    // peak - drop = 2800 = floor. Strict less-than: 2800 itself is NOT low.
    TEST_ASSERT_FALSE(voltage_state_is_low_dynamic(&state, 2800, 2800, 200));
    TEST_ASSERT_TRUE(voltage_state_is_low_dynamic(&state, 2799, 2800, 200));
}

static void test_dynamic_floor_dominates_when_peak_is_low(void)
{
    // peak = 1500, drop = 200 -> peak-drop = 1300, floor = 1500 wins.
    voltage_state_is_low_dynamic(&state, 1500, 1500, 200);
    TEST_ASSERT_TRUE(voltage_state_is_low_dynamic(&state, 1499, 1500, 200));
    TEST_ASSERT_FALSE(voltage_state_is_low_dynamic(&state, 1500, 1500, 200));
}

static void test_dynamic_voltage_above_peak_updates_peak_then_compares(void)
{
    voltage_state_is_low_dynamic(&state, 2500, 1500, 200);   // peak = 2500
    // New voltage 2700 > peak: peak rises to 2700 before threshold check.
    TEST_ASSERT_FALSE(voltage_state_is_low_dynamic(&state, 2700, 1500, 200));
    TEST_ASSERT_EQUAL_INT(2700, state.peak_mv);
}

static void test_dynamic_voltage_exactly_at_drop_threshold(void)
{
    // peak = 2500, drop = 200 -> drop_threshold = 2300. With floor < 2300
    // the dynamic threshold is 2300 and the check is strict less-than:
    // voltage == 2300 is NOT low. Pins the boundary against future drift.
    voltage_state_is_low_dynamic(&state, 2500, 1500, 200);
    TEST_ASSERT_FALSE(voltage_state_is_low_dynamic(&state, 2300, 1500, 200));
    TEST_ASSERT_TRUE(voltage_state_is_low_dynamic(&state, 2299, 1500, 200));
}

static void test_dynamic_zero_voltage_is_low(void)
{
    // A genuine 0 mV reading must trip the watchdog regardless of peak.
    voltage_state_is_low_dynamic(&state, 2500, 1500, 200);
    TEST_ASSERT_TRUE(voltage_state_is_low_dynamic(&state, 0, 1500, 200));
}

static void test_dynamic_negative_voltage_is_low(void)
{
    voltage_state_is_low_dynamic(&state, 2500, 1500, 200);
    TEST_ASSERT_TRUE(voltage_state_is_low_dynamic(&state, -1, 1500, 200));
}

static void test_dynamic_floor_zero_uses_drop_threshold(void)
{
    // With floor = 0, the drop-from-peak threshold dominates from the
    // moment peak rises above drop_mv. Documents the no-floor mode.
    voltage_state_is_low_dynamic(&state, 1000, 0, 200);
    TEST_ASSERT_FALSE(voltage_state_is_low_dynamic(&state, 800, 0, 200));  // == threshold
    TEST_ASSERT_TRUE(voltage_state_is_low_dynamic(&state, 799, 0, 200));
}

// --- voltage_state_reset_peak ----------------------------------------------

static void test_reset_peak_returns_to_zero(void)
{
    voltage_state_is_low_dynamic(&state, 2500, 1500, 200);
    TEST_ASSERT_EQUAL_INT(2500, state.peak_mv);
    voltage_state_reset_peak(&state);
    TEST_ASSERT_EQUAL_INT(0, state.peak_mv);
}

static void test_reset_peak_relearns_from_next_sample(void)
{
    voltage_state_is_low_dynamic(&state, 2500, 1500, 200);
    voltage_state_reset_peak(&state);
    voltage_state_is_low_dynamic(&state, 1800, 1500, 200);
    TEST_ASSERT_EQUAL_INT(1800, state.peak_mv);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_is_low_above_threshold);
    RUN_TEST(test_is_low_at_threshold);
    RUN_TEST(test_is_low_below_threshold);
    RUN_TEST(test_is_low_threshold_zero);
    RUN_TEST(test_is_low_negative_voltage);
    RUN_TEST(test_dynamic_first_call_clamps_to_floor);
    RUN_TEST(test_dynamic_peak_rises_with_voltage);
    RUN_TEST(test_dynamic_drop_within_tolerance_not_low);
    RUN_TEST(test_dynamic_drop_exceeds_tolerance_is_low);
    RUN_TEST(test_dynamic_floor_dominates_when_peak_is_high);
    RUN_TEST(test_dynamic_floor_dominates_when_peak_is_low);
    RUN_TEST(test_dynamic_voltage_above_peak_updates_peak_then_compares);
    RUN_TEST(test_dynamic_voltage_exactly_at_drop_threshold);
    RUN_TEST(test_dynamic_zero_voltage_is_low);
    RUN_TEST(test_dynamic_negative_voltage_is_low);
    RUN_TEST(test_dynamic_floor_zero_uses_drop_threshold);
    RUN_TEST(test_reset_peak_returns_to_zero);
    RUN_TEST(test_reset_peak_relearns_from_next_sample);
    return UNITY_END();
}
