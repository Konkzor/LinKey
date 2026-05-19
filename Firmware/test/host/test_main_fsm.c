#include "unity.h"
#include "fsm.h"

void setUp(void)    {}
void tearDown(void) {}

// --- fsm_after_wifi_connect -----------------------------------------------

static void test_wifi_ok_to_mqtt(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_MQTT_CONNECT, fsm_after_wifi_connect(CONN_OK));
}

static void test_wifi_failed_retries(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_WIFI_CONNECT, fsm_after_wifi_connect(CONN_FAILED));
}

static void test_wifi_lost_retries(void)
{
    // CONN_WIFI_LOST during the wifi_connect call is treated as a failure
    // to connect (radio dropped before association completed).
    TEST_ASSERT_EQUAL_INT(STATE_WIFI_CONNECT, fsm_after_wifi_connect(CONN_WIFI_LOST));
}

static void test_wifi_voltage_low_to_wait_voltage(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_VOLTAGE, fsm_after_wifi_connect(CONN_VOLTAGE_LOW));
}

// --- fsm_after_mqtt_connect -----------------------------------------------

static void test_mqtt_ok_to_wait_ulp(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_ULP_DATA, fsm_after_mqtt_connect(CONN_OK));
}

static void test_mqtt_failed_retries(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_MQTT_CONNECT, fsm_after_mqtt_connect(CONN_FAILED));
}

static void test_mqtt_voltage_low_to_wait_voltage(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_VOLTAGE, fsm_after_mqtt_connect(CONN_VOLTAGE_LOW));
}

static void test_mqtt_wifi_lost_to_wifi_connect(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_WIFI_CONNECT, fsm_after_mqtt_connect(CONN_WIFI_LOST));
}

// --- fsm_after_wait_ulp_data ---------------------------------------------

static void test_ulp_ok_to_publish(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_PUBLISH_DATA, fsm_after_wait_ulp_data(CONN_OK));
}

static void test_ulp_failed_retries(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_ULP_DATA, fsm_after_wait_ulp_data(CONN_FAILED));
}

static void test_ulp_voltage_low_to_wait_voltage(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_VOLTAGE, fsm_after_wait_ulp_data(CONN_VOLTAGE_LOW));
}

static void test_ulp_wifi_lost_to_wifi_connect(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_WIFI_CONNECT, fsm_after_wait_ulp_data(CONN_WIFI_LOST));
}

// --- fsm_after_publish_data ----------------------------------------------
// Precedence chain: voltage_low > !wifi > !mqtt > !data_valid > !publish_ok.
// Callers must pass short-circuited booleans so downstream inputs reflect
// that ordering (e.g. wifi_connected==false when voltage_low==true).

static void test_publish_voltage_low_dominates(void)
{
    // Even with everything else "OK", voltage_low must drop to WAIT_VOLTAGE.
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_VOLTAGE,
        fsm_after_publish_data(true, false, false, false, false));
}

static void test_publish_wifi_lost_to_wifi_connect(void)
{
    // voltage ok, wifi disconnected.
    TEST_ASSERT_EQUAL_INT(STATE_WIFI_CONNECT,
        fsm_after_publish_data(false, false, false, false, false));
}

static void test_publish_mqtt_lost_to_mqtt_connect(void)
{
    // voltage + wifi ok, mqtt disconnected.
    TEST_ASSERT_EQUAL_INT(STATE_MQTT_CONNECT,
        fsm_after_publish_data(false, true, false, false, false));
}

static void test_publish_no_data_to_wait_ulp(void)
{
    // Connections ok, but linky_data.valid_flags == 0.
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_ULP_DATA,
        fsm_after_publish_data(false, true, true, false, false));
}

static void test_publish_publish_failed_to_mqtt_connect(void)
{
    // Connections ok, data valid, but mqtt_publish_linky_data returned false.
    TEST_ASSERT_EQUAL_INT(STATE_MQTT_CONNECT,
        fsm_after_publish_data(false, true, true, true, false));
}

static void test_publish_happy_path_stays(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_PUBLISH_DATA,
        fsm_after_publish_data(false, true, true, true, true));
}

// --- fsm_voltage_watchdog_next -------------------------------------------

static void test_watchdog_init_passes_through_low(void)
{
    // STATE_INIT owns its own voltage handling (ADC not initialized yet);
    // the watchdog must not pre-empt it.
    TEST_ASSERT_EQUAL_INT(STATE_INIT, fsm_voltage_watchdog_next(STATE_INIT, true));
}

static void test_watchdog_wait_voltage_passes_through_low(void)
{
    // STATE_WAIT_VOLTAGE already checks voltage; double-trigger is harmless
    // but unnecessary — keep the contract that the watchdog skips it.
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_VOLTAGE,
        fsm_voltage_watchdog_next(STATE_WAIT_VOLTAGE, true));
}

static void test_watchdog_drops_wifi_connect_to_wait_voltage(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_VOLTAGE,
        fsm_voltage_watchdog_next(STATE_WIFI_CONNECT, true));
}

static void test_watchdog_drops_mqtt_connect_to_wait_voltage(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_VOLTAGE,
        fsm_voltage_watchdog_next(STATE_MQTT_CONNECT, true));
}

static void test_watchdog_drops_wait_ulp_to_wait_voltage(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_VOLTAGE,
        fsm_voltage_watchdog_next(STATE_WAIT_ULP_DATA, true));
}

static void test_watchdog_drops_publish_to_wait_voltage(void)
{
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_VOLTAGE,
        fsm_voltage_watchdog_next(STATE_PUBLISH_DATA, true));
}

static void test_watchdog_voltage_ok_keeps_state(void)
{
    // For every state, voltage_low=false must be a no-op.
    TEST_ASSERT_EQUAL_INT(STATE_INIT,         fsm_voltage_watchdog_next(STATE_INIT, false));
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_VOLTAGE, fsm_voltage_watchdog_next(STATE_WAIT_VOLTAGE, false));
    TEST_ASSERT_EQUAL_INT(STATE_WIFI_CONNECT, fsm_voltage_watchdog_next(STATE_WIFI_CONNECT, false));
    TEST_ASSERT_EQUAL_INT(STATE_MQTT_CONNECT, fsm_voltage_watchdog_next(STATE_MQTT_CONNECT, false));
    TEST_ASSERT_EQUAL_INT(STATE_WAIT_ULP_DATA, fsm_voltage_watchdog_next(STATE_WAIT_ULP_DATA, false));
    TEST_ASSERT_EQUAL_INT(STATE_PUBLISH_DATA, fsm_voltage_watchdog_next(STATE_PUBLISH_DATA, false));
}

// --- fsm_should_reset_peak_on_enter --------------------------------------

static void test_reset_peak_true_for_dynamic_states(void)
{
    TEST_ASSERT_TRUE(fsm_should_reset_peak_on_enter(STATE_MQTT_CONNECT));
    TEST_ASSERT_TRUE(fsm_should_reset_peak_on_enter(STATE_WAIT_ULP_DATA));
    TEST_ASSERT_TRUE(fsm_should_reset_peak_on_enter(STATE_PUBLISH_DATA));
}

static void test_reset_peak_false_for_other_states(void)
{
    TEST_ASSERT_FALSE(fsm_should_reset_peak_on_enter(STATE_INIT));
    TEST_ASSERT_FALSE(fsm_should_reset_peak_on_enter(STATE_WAIT_VOLTAGE));
    TEST_ASSERT_FALSE(fsm_should_reset_peak_on_enter(STATE_WIFI_CONNECT));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_wifi_ok_to_mqtt);
    RUN_TEST(test_wifi_failed_retries);
    RUN_TEST(test_wifi_lost_retries);
    RUN_TEST(test_wifi_voltage_low_to_wait_voltage);

    RUN_TEST(test_mqtt_ok_to_wait_ulp);
    RUN_TEST(test_mqtt_failed_retries);
    RUN_TEST(test_mqtt_voltage_low_to_wait_voltage);
    RUN_TEST(test_mqtt_wifi_lost_to_wifi_connect);

    RUN_TEST(test_ulp_ok_to_publish);
    RUN_TEST(test_ulp_failed_retries);
    RUN_TEST(test_ulp_voltage_low_to_wait_voltage);
    RUN_TEST(test_ulp_wifi_lost_to_wifi_connect);

    RUN_TEST(test_publish_voltage_low_dominates);
    RUN_TEST(test_publish_wifi_lost_to_wifi_connect);
    RUN_TEST(test_publish_mqtt_lost_to_mqtt_connect);
    RUN_TEST(test_publish_no_data_to_wait_ulp);
    RUN_TEST(test_publish_publish_failed_to_mqtt_connect);
    RUN_TEST(test_publish_happy_path_stays);

    RUN_TEST(test_watchdog_init_passes_through_low);
    RUN_TEST(test_watchdog_wait_voltage_passes_through_low);
    RUN_TEST(test_watchdog_drops_wifi_connect_to_wait_voltage);
    RUN_TEST(test_watchdog_drops_mqtt_connect_to_wait_voltage);
    RUN_TEST(test_watchdog_drops_wait_ulp_to_wait_voltage);
    RUN_TEST(test_watchdog_drops_publish_to_wait_voltage);
    RUN_TEST(test_watchdog_voltage_ok_keeps_state);

    RUN_TEST(test_reset_peak_true_for_dynamic_states);
    RUN_TEST(test_reset_peak_false_for_other_states);

    return UNITY_END();
}
