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
#include "esp_timer.h"
#include "types.h"
#include "ulp_linky.h"
#include "voltage_manager.h"
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

// Timeouts (milliseconds)
#define WIFI_CONNECT_TIMEOUT_MS 6000    // WiFi connection timeout
#define MQTT_CONNECT_TIMEOUT_MS 1000    // MQTT connection timeout
// Timeout waiting for ULP data: frame-level reception needs at least 2 frame periods
// (partial frame + first complete frame) before valid data is available
#if defined(CONFIG_LINKEY_TARIFF_TEMPO)
#define ULP_DATA_TIMEOUT_MS     5000
#elif defined(CONFIG_LINKEY_TARIFF_EJP)
#define ULP_DATA_TIMEOUT_MS     4000
#elif defined(CONFIG_LINKEY_TARIFF_HPHC)
#define ULP_DATA_TIMEOUT_MS     3500
#else
#define ULP_DATA_TIMEOUT_MS     3000
#endif
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

// Task handle for ULP wake notification
static TaskHandle_t main_task_handle;

// ULP wake ISR: notify main task when a new frame is received
static void IRAM_ATTR ulp_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(*(TaskHandle_t *)arg, &woken);
    portYIELD_FROM_ISR(woken);
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

// Wait for ULP data to be received (with voltage checks)
static conn_result_t ulp_wait_data(void)
{
    linky_data_t data;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ULP_DATA_TIMEOUT_MS);

    while (xTaskGetTickCount() < deadline) {
        // Block until ULP notification or periodic check interval
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_INTERVAL_MS));

        if (voltage_is_low_dynamic(VOLTAGE_FALLBACK_MIN_MV)) {
            return CONN_VOLTAGE_LOW;
        }
        if (!wifi_is_connected() || !mqtt_is_connected(&mqtt_state)) {
            return CONN_WIFI_LOST;
        }

        get_linky_data(&data);
        if (data.valid_flags != 0) {
            DEBUG_LOG(TAG, "ULP data received (flags: 0x%04x)", data.valid_flags);
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

    // Initialize voltage monitoring
    voltage_init();

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
    int voltage_mv = voltage_read_mv();
    DEBUG_LOG(TAG, "Supercap voltage: %d mV", voltage_mv);

    if (voltage_mv >= VOLTAGE_START_MV) {
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
        mqtt_init(&mqtt_state);
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
    // One-time ULP initialization + ISR registration
    if (!ulp_initialized) {
        DEBUG_LOG(TAG, "Initializing ULP...");
        main_task_handle = xTaskGetCurrentTaskHandle();
        hulp_ulp_isr_register(&ulp_isr, &main_task_handle);
        hulp_ulp_interrupt_en();
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
    linky_data.voltage_cap = voltage_read_mv();
    linky_data.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    // Check if data received
    if (linky_data.valid_flags == 0) {
        DEBUG_LOGW(TAG, "No ULP data received - returning to WAIT_ULP_DATA");
        return STATE_WAIT_ULP_DATA;
    }

    DEBUG_LOG(TAG, "Linky data received (flags: 0x%04x, IINST: %u A)",
            linky_data.valid_flags, linky_data.iinst);

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
                voltage_reset_peak();
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

        // Wait for next event based on current state
        switch (current_state) {
            case STATE_PUBLISH_DATA:
                // Block until ULP signals a new frame is ready
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ULP_DATA_TIMEOUT_MS));
                break;
            default:
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
        }
    }
}
