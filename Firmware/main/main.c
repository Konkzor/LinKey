#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_timer.h"
#include "types.h"
#include "ulp_linky.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "debug.h"

static const char *TAG = "LINKY_MAIN";

// RGB LED pin definitions
#define RGB_LED_RED_PIN     GPIO_NUM_13
#define RGB_LED_GREEN_PIN   GPIO_NUM_15
#define RGB_LED_BLUE_PIN    GPIO_NUM_2

// RGB Color macros - define colors as (R, G, B) where each is 0 or 1
#define RGB_COLOR(r, g, b)  ((r) | ((g) << 1) | ((b) << 2))
#define RGB_OFF             RGB_COLOR(0, 0, 0)
#define RGB_RED             RGB_COLOR(1, 0, 0)
#define RGB_GREEN           RGB_COLOR(0, 1, 0)
#define RGB_BLUE            RGB_COLOR(0, 0, 1)
#define RGB_YELLOW          RGB_COLOR(1, 1, 0)
#define RGB_CYAN            RGB_COLOR(0, 1, 1)
#define RGB_MAGENTA         RGB_COLOR(1, 0, 1)
#define RGB_WHITE           RGB_COLOR(1, 1, 1)

// State LED color configuration (easily customizable)
#define LED_COLOR_INIT          RGB_CYAN
#define LED_COLOR_WAIT_VOLTAGE  RGB_RED
#define LED_COLOR_WIFI_CONNECT     RGB_BLUE
#define LED_COLOR_MQTT_CONNECT     RGB_MAGENTA
#define LED_COLOR_WAIT_ULP_DATA      RGB_YELLOW
#define LED_COLOR_PUBLISH_DATA       RGB_GREEN

// Supercap voltage ADC configuration (GPIO 33 = ADC1_CHANNEL_5)
#define SUPERCAP_ADC_CHANNEL    ADC1_CHANNEL_5
#define SUPERCAP_ADC_ATTEN      ADC_ATTEN_DB_12  // Full scale ~3.3V
#define SUPERCAP_ADC_WIDTH      ADC_WIDTH_BIT_12

// Voltage thresholds (millivolts)
#define VOLTAGE_WIFI_START_MV   2500    // Minimum voltage to start WiFi

// Per-state fallback thresholds (voltage to trigger fallback to WAIT_VOLTAGE)
#define VOLTAGE_FALLBACK_MIN_MV         1500    // Minimum fallback threshold
#define VOLTAGE_FALLBACK_DROP_MV        200     // Max allowed drop from peak

// Timeouts (milliseconds)
#define WIFI_CONNECT_TIMEOUT_MS 6000    // WiFi connection timeout
#define MQTT_CONNECT_TIMEOUT_MS 1000    // MQTT connection timeout
#define ULP_DATA_TIMEOUT_MS     500     // Timeout waiting for ULP data
#define POLL_INTERVAL_MS        100     // Polling interval in connect/wait loops

// FSM States
typedef enum {
    STATE_INIT,
    STATE_WAIT_VOLTAGE,
    STATE_WIFI_CONNECT,
    STATE_MQTT_CONNECT,
    STATE_WAIT_ULP_DATA,
    STATE_PUBLISH_DATA
} app_state_t;

static app_state_t current_state = STATE_INIT;

// State LED colors lookup table
static const uint8_t state_colors[] = {
    LED_COLOR_INIT,
    LED_COLOR_WAIT_VOLTAGE,
    LED_COLOR_WIFI_CONNECT,
    LED_COLOR_MQTT_CONNECT,
    LED_COLOR_WAIT_ULP_DATA,
    LED_COLOR_PUBLISH_DATA
};

// WiFi state (owned by main, passed to wifi_manager)
static wifi_state_t wifi_state = {0};

// MQTT state (owned by main, passed to mqtt_manager)
static mqtt_state_t mqtt_state = {0};

// ULP initialization flag (one-time init)
static bool ulp_initialized = false;

// ADC calibration characteristics for supercap voltage reading
static esp_adc_cal_characteristics_t *adc_chars = NULL;

static void supercap_adc_init(void)
{
    // Configure ADC width (shared for all ADC1 channels - HULP may have set this already)
    adc1_config_width(SUPERCAP_ADC_WIDTH);

    // Configure channel attenuation
    adc1_config_channel_atten(SUPERCAP_ADC_CHANNEL, SUPERCAP_ADC_ATTEN);

    // Initialize calibration
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t cal_type = esp_adc_cal_characterize(
        ADC_UNIT_1, SUPERCAP_ADC_ATTEN, SUPERCAP_ADC_WIDTH, 1100, adc_chars);

    if (cal_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        DEBUG_LOG(TAG, "ADC calibration: Two Point");
    } else if (cal_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        DEBUG_LOG(TAG, "ADC calibration: eFuse Vref");
    } else {
        DEBUG_LOG(TAG, "ADC calibration: Default");
    }

    DEBUG_LOG(TAG, "Supercap ADC initialized (GPIO 33, ADC1_CH5)");
}

// Read supercap voltage in millivolts
int supercap_read_voltage_mv(void)
{
    int raw_value = adc1_get_raw(SUPERCAP_ADC_CHANNEL);
    uint32_t voltage_mv = esp_adc_cal_raw_to_voltage(raw_value, adc_chars);
    return (int)voltage_mv;
}

// Check if voltage is below threshold (returns true if low voltage condition)
static bool voltage_is_low(uint16_t threshold)
{
    int voltage_mv = supercap_read_voltage_mv();
    if (voltage_mv < threshold) {
        DEBUG_LOGW(TAG, "Voltage dropped to %d mV (threshold %d)", voltage_mv, threshold);
        return true;
    }
    return false;
}

// Dynamic voltage peak tracker for drain detection
static int dynamic_voltage_peak_mv = 0;

static bool voltage_is_low_dynamic(uint16_t floor_mv)
{
    int voltage_mv = supercap_read_voltage_mv();
    if (voltage_mv > dynamic_voltage_peak_mv) {
        dynamic_voltage_peak_mv = voltage_mv;
    }
    int drop_threshold = dynamic_voltage_peak_mv - VOLTAGE_FALLBACK_DROP_MV;
    uint16_t threshold = (drop_threshold > floor_mv) ? drop_threshold : floor_mv;
    if (voltage_mv < threshold) {
        DEBUG_LOGW(TAG, "Voltage %d mV below dynamic threshold %d mV (peak %d, floor %d)",
                   voltage_mv, threshold, dynamic_voltage_peak_mv, floor_mv);
        return true;
    }
    return false;
}

static void rgb_led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RGB_LED_RED_PIN) | (1ULL << RGB_LED_GREEN_PIN) | (1ULL << RGB_LED_BLUE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // Turn off all LEDs initially (assuming active-high LEDs)
    gpio_set_level(RGB_LED_RED_PIN, 0);
    gpio_set_level(RGB_LED_GREEN_PIN, 0);
    gpio_set_level(RGB_LED_BLUE_PIN, 0);

    DEBUG_LOG(TAG, "RGB LEDs initialized (R:%d, G:%d, B:%d)", RGB_LED_RED_PIN, RGB_LED_GREEN_PIN, RGB_LED_BLUE_PIN);
}

// Set LED to a color (from RGB_COLOR macro)
static void rgb_led_set(uint8_t color)
{
    gpio_set_level(RGB_LED_RED_PIN,   (color >> 0) & 1);
    gpio_set_level(RGB_LED_GREEN_PIN, (color >> 1) & 1);
    gpio_set_level(RGB_LED_BLUE_PIN,  (color >> 2) & 1);
}

// Blink LED with color for specified duration, then turn off
static void rgb_led_blink(uint8_t color, int duration_ms)
{
    rgb_led_set(color);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    rgb_led_set(RGB_OFF);
}

// Stop WiFi and MQTT (WiFi first to cut radio power immediately)
static void stop_all_connections(void)
{
    wifi_stop(&wifi_state);
    mqtt_stop(&mqtt_state);
}

// Emergency WiFi stop callback for MQTT event handler (called on message expiry)
static void emergency_wifi_stop(void)
{
    if (wifi_state.started) {
        esp_wifi_stop();
        wifi_state.started = false;
        xEventGroupClearBits(wifi_state.event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
}

// Wait for ULP data to be received (with voltage checks)
static conn_result_t ulp_wait_data(void)
{
    uint32_t wait_ms = 0;
    linky_data_t data;
    while (wait_ms < ULP_DATA_TIMEOUT_MS) {
        // Check voltage
        if (voltage_is_low_dynamic(VOLTAGE_FALLBACK_MIN_MV)) {
            return CONN_VOLTAGE_LOW;
        }
            // Check if WiFi is still connected
        if (!wifi_is_connected() || !mqtt_is_connected(&mqtt_state)) {
            return CONN_WIFI_LOST;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        wait_ms += POLL_INTERVAL_MS;

        get_linky_data(&data);
        if (data.valid_flags != 0) {
            DEBUG_LOG(TAG, "ULP data received - IINST: %u, BASE: %lu", data.iinst, data.base);
            return CONN_OK;
        }
    }

    DEBUG_LOGW(TAG, "ULP data reception timeout");
    return CONN_FAILED;
}

// Forward declarations for state handlers
static app_state_t handle_state_init(void);
static app_state_t handle_state_wait_voltage(void);
static app_state_t handle_state_wifi_connect(void);
static app_state_t handle_state_mqtt_connect(void);
static app_state_t handle_state_wait_ulp_data(void);
static app_state_t handle_state_publish_data(void);

// STATE_INIT: Initialize LED, ADC, pm_config
static app_state_t handle_state_init(void)
{
    // Initialize NVS (used for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize RGB LEDs
    rgb_led_init();

    // Initialize supercap voltage ADC
    supercap_adc_init();

    // Configure automatic power management for light sleep
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    DEBUG_LOG(TAG, "Power management configured: auto light sleep enabled");

    return STATE_WAIT_VOLTAGE;
}

// STATE_WAIT_VOLTAGE: Wait for voltage >= 2.5V
static app_state_t handle_state_wait_voltage(void)
{
    int voltage_mv = supercap_read_voltage_mv();
    DEBUG_LOG(TAG, "Supercap voltage: %d mV", voltage_mv);

    if (voltage_mv >= VOLTAGE_WIFI_START_MV) {
        DEBUG_LOG(TAG, "Voltage sufficient - proceeding to WiFi init");
        return STATE_WIFI_CONNECT;
    }

    return STATE_WAIT_VOLTAGE;
}

// STATE_WIFI_CONNECT: Initialize WiFi (once) and connect
static app_state_t handle_state_wifi_connect(void)
{
    // One-time WiFi initialization
    if (!wifi_state.initialized) {
        DEBUG_LOG(TAG, "Initializing WiFi...");
        wifi_init(&wifi_state);
    }

    // Restart WiFi if it was stopped
    if (!wifi_state.started) {
        DEBUG_LOG(TAG, "Restarting WiFi...");
        wifi_start(&wifi_state);
    }

    DEBUG_LOG(TAG, "Connecting to WiFi...");

    conn_result_t result = wifi_connect(&wifi_state, voltage_is_low,
                                        VOLTAGE_FALLBACK_MIN_MV,
                                        WIFI_CONNECT_TIMEOUT_MS, POLL_INTERVAL_MS);
    switch (result) {
        case CONN_OK:
            DEBUG_LOG(TAG, "WiFi connected - proceeding to MQTT init");
            return STATE_MQTT_CONNECT;

        case CONN_FAILED:
        case CONN_WIFI_LOST:
            DEBUG_LOG(TAG, "WiFi connection failed - retrying...");
            return STATE_WIFI_CONNECT;

        case CONN_VOLTAGE_LOW:
            DEBUG_LOGW(TAG, "Voltage too low - stopping WiFi and returning to WAIT_VOLTAGE");
            stop_all_connections();
            return STATE_WAIT_VOLTAGE;
    }

    return STATE_WIFI_CONNECT;  // Should not reach here
}

// STATE_MQTT_CONNECT: Initialize MQTT (once) and connect
static app_state_t handle_state_mqtt_connect(void)
{
    // One-time MQTT initialization
    if (!mqtt_state.initialized) {
        DEBUG_LOG(TAG, "Initializing MQTT...");
        mqtt_init(&mqtt_state, emergency_wifi_stop);
    }

    DEBUG_LOG(TAG, "Connecting to MQTT...");
    // Connect MQTT with voltage checks (dynamic threshold) and WiFi checks (exit early if WiFi lost)
    conn_result_t result = mqtt_connect(&mqtt_state, voltage_is_low_dynamic,
                                        VOLTAGE_FALLBACK_MIN_MV, wifi_is_connected,
                                        MQTT_CONNECT_TIMEOUT_MS, POLL_INTERVAL_MS);
    switch (result) {
        case CONN_OK:
            DEBUG_LOG(TAG, "MQTT connected - proceeding to ULP_WAIT_DATA");
            return STATE_WAIT_ULP_DATA;

        case CONN_FAILED:
            DEBUG_LOG(TAG, "MQTT not available - retrying...");
            return STATE_MQTT_CONNECT;

        case CONN_VOLTAGE_LOW:
            DEBUG_LOGW(TAG, "Voltage too low - stopping WiFi and returning to WAIT_VOLTAGE");
            stop_all_connections();
            return STATE_WAIT_VOLTAGE;
        
        case CONN_WIFI_LOST:
            DEBUG_LOGW(TAG, "WiFi lost during MQTT connect - returning to WIFI_CONNECT");
            stop_all_connections();
            return STATE_WIFI_CONNECT;
    }

    return STATE_MQTT_CONNECT;  // Should not reach here
}

// STATE_WAIT_ULP_DATA: Initialize ULP (once) and wait for data
static app_state_t handle_state_wait_ulp_data(void)
{
    // One-time ULP initialization
    if (!ulp_initialized) {
        DEBUG_LOG(TAG, "Initializing ULP...");
        init_ulp_linky();
        ulp_initialized = true;
    }

    DEBUG_LOG(TAG, "Waiting for ULP data...");

    conn_result_t result = ulp_wait_data();
    switch (result) {
        case CONN_OK:
            DEBUG_LOG(TAG, "ULP data confirmed - entering PUBLISH_DATA state");
            return STATE_PUBLISH_DATA;

        case CONN_FAILED:
            DEBUG_LOG(TAG, "ULP data not received - waiting...");
            return STATE_WAIT_ULP_DATA;

        case CONN_VOLTAGE_LOW:
            DEBUG_LOGW(TAG, "Voltage too low - stopping WiFi and returning to WAIT_VOLTAGE");
            stop_all_connections();
            return STATE_WAIT_VOLTAGE;
        
        case CONN_WIFI_LOST:
            DEBUG_LOGW(TAG, "WiFi or MQTT lost during ULP wait - returning to WIFI_CONNECT");
            stop_all_connections();
            return STATE_WIFI_CONNECT;
    }

    return STATE_WAIT_ULP_DATA;  // Should not reach here
}

// STATE_PUBLISH_DATA: Normal operation - get data and publish
static app_state_t handle_state_publish_data(void)
{
    linky_data_t linky_data;

    // Check voltage (global check already done in main loop, but double-check here)
    if (voltage_is_low_dynamic(VOLTAGE_FALLBACK_MIN_MV)) {
        stop_all_connections();
        return STATE_WAIT_VOLTAGE;
    }

    // Check WiFi connection
    if (!wifi_is_connected()) {
        DEBUG_LOGW(TAG, "WiFi disconnected - returning to WIFI_CONNECT");
        stop_all_connections();
        return STATE_WIFI_CONNECT;
    }

    // Check MQTT connection
    if (!mqtt_is_connected(&mqtt_state)) {
        DEBUG_LOGW(TAG, "MQTT disconnected - returning to MQTT_CONNECT");
        return STATE_MQTT_CONNECT;
    }

    // Get data from ULP
    get_linky_data(&linky_data);
    linky_data.voltage_cap = supercap_read_voltage_mv();
    linky_data.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    // Check if data received
    if (linky_data.valid_flags == 0) {
        DEBUG_LOGW(TAG, "No ULP data received - returning to WAIT_ULP_DATA");
        return STATE_WAIT_ULP_DATA;
    }

    DEBUG_LOG(TAG, "Linky data - IINST: %u A, BASE: %lu Wh",
            linky_data.iinst, linky_data.base);

    // Publish data
    if (!mqtt_publish_linky_data(&mqtt_state, &linky_data)) {
        DEBUG_LOGW(TAG, "Publish failed - returning to MQTT_CONNECT");
        return STATE_MQTT_CONNECT;
    }

    return STATE_PUBLISH_DATA;
}

void app_main(void)
{
    DEBUG_LOG(TAG, "Starting FSM...");
    app_state_t previous_state = STATE_INIT;

    while (1) {
        // Reset dynamic peak tracker on state transitions into dynamic states
        if (current_state != previous_state) {
            if (current_state == STATE_MQTT_CONNECT ||
                current_state == STATE_WAIT_ULP_DATA ||
                current_state == STATE_PUBLISH_DATA) {
                dynamic_voltage_peak_mv = 0;
            }
            previous_state = current_state;
        }

        // Voltage check only possible after INIT (ADC must be initialized first)
        if (current_state != STATE_INIT && current_state != STATE_WAIT_VOLTAGE) {
            bool low = false;
            if (current_state == STATE_WIFI_CONNECT) {
                low = voltage_is_low(VOLTAGE_FALLBACK_MIN_MV);
            } else {
                low = voltage_is_low_dynamic(VOLTAGE_FALLBACK_MIN_MV);
            }
            if (low) {
                stop_all_connections();
                current_state = STATE_WAIT_VOLTAGE;
            }
        }

        // Blink LED for current state (10ms)
        rgb_led_blink(state_colors[current_state], 10);

        // Handle current state
        switch (current_state) {
            case STATE_INIT:
                current_state = handle_state_init();
                break;
            case STATE_WAIT_VOLTAGE:
                current_state = handle_state_wait_voltage();
                break;
            case STATE_WIFI_CONNECT:
                current_state = handle_state_wifi_connect();
                break;
            case STATE_MQTT_CONNECT:
                current_state = handle_state_mqtt_connect();
                break;
            case STATE_WAIT_ULP_DATA:
                current_state = handle_state_wait_ulp_data();
                break;
            case STATE_PUBLISH_DATA:
                current_state = handle_state_publish_data();
                break;
        }

        // Sleep for ~1s (auto light sleep will activate) - skip in INIT state
        if (current_state != STATE_INIT) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
