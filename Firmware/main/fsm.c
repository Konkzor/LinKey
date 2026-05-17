#include "fsm.h"

app_state_t fsm_after_wifi_connect(conn_result_t result)
{
    switch (result) {
        case CONN_OK:
            return STATE_MQTT_CONNECT;
        case CONN_FAILED:
        case CONN_WIFI_LOST:
            return STATE_WIFI_CONNECT;
        case CONN_VOLTAGE_LOW:
            return STATE_WAIT_VOLTAGE;
    }
    return STATE_WIFI_CONNECT;
}

app_state_t fsm_after_mqtt_connect(conn_result_t result)
{
    switch (result) {
        case CONN_OK:
            return STATE_WAIT_ULP_DATA;
        case CONN_FAILED:
            return STATE_MQTT_CONNECT;
        case CONN_VOLTAGE_LOW:
            return STATE_WAIT_VOLTAGE;
        case CONN_WIFI_LOST:
            return STATE_WIFI_CONNECT;
    }
    return STATE_MQTT_CONNECT;
}

app_state_t fsm_after_wait_ulp_data(conn_result_t result)
{
    switch (result) {
        case CONN_OK:
            return STATE_PUBLISH_DATA;
        case CONN_FAILED:
            return STATE_WAIT_ULP_DATA;
        case CONN_VOLTAGE_LOW:
            return STATE_WAIT_VOLTAGE;
        case CONN_WIFI_LOST:
            return STATE_WIFI_CONNECT;
    }
    return STATE_WAIT_ULP_DATA;
}

app_state_t fsm_after_publish_data(bool voltage_low,
                                   bool wifi_connected,
                                   bool mqtt_connected,
                                   bool data_valid,
                                   bool publish_ok)
{
    if (voltage_low)        return STATE_WAIT_VOLTAGE;
    if (!wifi_connected)    return STATE_WIFI_CONNECT;
    if (!mqtt_connected)    return STATE_MQTT_CONNECT;
    if (!data_valid)        return STATE_WAIT_ULP_DATA;
    if (!publish_ok)        return STATE_MQTT_CONNECT;
    return STATE_PUBLISH_DATA;
}

app_state_t fsm_voltage_watchdog_next(app_state_t current, bool voltage_low)
{
    if (current == STATE_INIT || current == STATE_WAIT_VOLTAGE) return current;
    return voltage_low ? STATE_WAIT_VOLTAGE : current;
}

bool fsm_should_reset_peak_on_enter(app_state_t state)
{
    return state == STATE_MQTT_CONNECT
        || state == STATE_WAIT_ULP_DATA
        || state == STATE_PUBLISH_DATA;
}
