#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"

#include "hulp.h"
#include "ulp_linky.h"
#include "debug.h"

static const char *TAG = "LINKY_MAIN";

// RGB LED pin definitions
#define RGB_LED_RED_PIN     GPIO_NUM_13
#define RGB_LED_GREEN_PIN   GPIO_NUM_15
#define RGB_LED_BLUE_PIN    GPIO_NUM_2

// Supercap voltage ADC configuration (GPIO 33 = ADC1_CHANNEL_5)
#define SUPERCAP_ADC_CHANNEL    ADC1_CHANNEL_5
#define SUPERCAP_ADC_ATTEN      ADC_ATTEN_DB_12  // Full scale ~3.3V
#define SUPERCAP_ADC_WIDTH      ADC_WIDTH_BIT_12

// WiFi and MQTT credentials from Kconfig
#define WIFI_SSID       CONFIG_LINKY_WIFI_SSID
#define WIFI_PASS       CONFIG_LINKY_WIFI_PASSWORD
#define MQTT_BROKER_URI CONFIG_LINKY_MQTT_BROKER_URI
#define MQTT_USERNAME   CONFIG_LINKY_MQTT_USERNAME
#define MQTT_PASSWORD   CONFIG_LINKY_MQTT_PASSWORD

// MQTT topics
#define MQTT_TOPIC_IINST CONFIG_LINKY_MQTT_TOPIC_PREFIX "/iinst"
#define MQTT_TOPIC_BASE  CONFIG_LINKY_MQTT_TOPIC_PREFIX "/base"
#define MQTT_TOPIC_VCAP  CONFIG_LINKY_MQTT_TOPIC_PREFIX "/vcap"

// Event group for WiFi connection
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// MQTT client handle
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

// Store WiFi connection info in RTC memory for fast reconnect
RTC_DATA_ATTR static uint8_t rtc_bssid[6] = {0};
RTC_DATA_ATTR static uint8_t rtc_channel = 0;
RTC_DATA_ATTR static bool rtc_bssid_valid = false;

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

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect(); // FIXME: not sure it is called if disconnection happens during light sleep
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        DEBUG_LOG(TAG, "WiFi connected - IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // Cache BSSID and channel for faster reconnection next time
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            memcpy(rtc_bssid, ap_info.bssid, 6);
            rtc_channel = ap_info.primary;
            rtc_bssid_valid = true;
            DEBUG_LOG(TAG, "Cached AP - Channel: %d", rtc_channel);
        }

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            DEBUG_LOG(TAG, "MQTT connected");
            mqtt_connected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            DEBUG_LOG(TAG, "MQTT disconnected");
            mqtt_connected = false;
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            mqtt_connected = false;
            break;
        default:
            break;
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

#ifdef CONFIG_LINKY_USE_STATIC_IP
    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr = esp_ip4addr_aton(CONFIG_LINKY_STATIC_IP);
    ip_info.gw.addr = esp_ip4addr_aton(CONFIG_LINKY_STATIC_GATEWAY);
    ip_info.netmask.addr = esp_ip4addr_aton(CONFIG_LINKY_STATIC_NETMASK);
    esp_netif_set_ip_info(netif, &ip_info);
    DEBUG_LOG(TAG, "Static IP: %s", CONFIG_LINKY_STATIC_IP);
#else
    (void)netif; // Suppress unused variable warning
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .bssid_set = 0,
            .listen_interval = 10,  // Allow missing 10 beacons
        },
    };

    if (rtc_bssid_valid) {
        memcpy(wifi_config.sta.bssid, rtc_bssid, 6);
        wifi_config.sta.bssid_set = 1;
        wifi_config.sta.channel = rtc_channel;
        DEBUG_LOG(TAG, "Using cached AP (Ch %d)", rtc_channel);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));
    DEBUG_LOG(TAG, "WiFi modem sleep enabled");

    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi connection timeout");
    }
}

static void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .session.keepalive = 30,
        .session.disable_clean_session = 0,
        .network.timeout_ms = 3000,
        .network.refresh_connection_after_ms = 0,
        .buffer.size = 512,
        .buffer.out_size = 512,
    };

    if (strlen(MQTT_USERNAME) > 0) {
        mqtt_cfg.credentials.username = MQTT_USERNAME;
    }
    if (strlen(MQTT_PASSWORD) > 0) {
        mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
    }

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    // Wait for MQTT connection
    int wait_ms = 0;
    while (!mqtt_connected && wait_ms < 1000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_ms += 50;
    }
}

// Publish Linky data to MQTT (QoS 0 for speed)
static void publish_linky_data(linky_data_t *data)
{
    if (!mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, skipping publish");
        return;
    }

    char payload[32];

    // Publish IINST
    if( data->valid_flags & 0x01 ){
        snprintf(payload, sizeof(payload), "%u", data->iinst);
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_IINST, payload, 0, 0, 0);
        DEBUG_LOG(TAG, "Published: IINST=%u", data->iinst);
    }
    else{
        DEBUG_LOG(TAG, "IINST data not valid, skipping publish");
    }

    // Publish BASE
    if( data->valid_flags & 0x02) {
        snprintf(payload, sizeof(payload), "%lu", data->base);
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_BASE, payload, 0, 0, 0);
        DEBUG_LOG(TAG, "Published: BASE=%lu", data->base);
    }
    else{
        DEBUG_LOG(TAG, "BASE not valid, skipping publish");
    }

    // Publish VCAP
    snprintf(payload, sizeof(payload), "%lu", data->voltage_cap);
    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_VCAP, payload, 0, 0, 0);
    DEBUG_LOG(TAG, "Published: VCAP=%lu", data->voltage_cap);
}

// Enter light sleep mode
static void enter_sleep(void)
{
    // Light sleep mode - use automatic sleep via vTaskDelay
    // The esp_pm_configure() with light_sleep_enable=true will
    // automatically enter light sleep during idle periods
    DEBUG_LOG(TAG, "Waiting for next ULP wakeup (auto light sleep enabled)");

    // Just delay - automatic light sleep will activate during idle
    // WiFi will wake for beacons automatically without conflicting
    vTaskDelay(pdMS_TO_TICKS(1000));  // 1 second delay, will auto-sleep

    DEBUG_LOG(TAG, "Delay expired, checking for data");
}

void app_main(void)
{
    linky_data_t linky_data;

    // Initialize NVS (used for Wifi)
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
        .max_freq_mhz = 160,      // Max CPU frequency
        .min_freq_mhz = 40,       // Min CPU frequency (APB will be 40MHz)
        .light_sleep_enable = true // Enable automatic light sleep
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    DEBUG_LOG(TAG, "Power management configured: auto light sleep enabled");

    // Wait for voltage to be above 2.5V
    linky_data.voltage_cap = 0;
    while(linky_data.voltage_cap <= 2500) {
        linky_data.voltage_cap = supercap_read_voltage_mv();
        DEBUG_LOG(TAG, "Supercap voltage: %lu mV", linky_data.voltage_cap);

        if(linky_data.voltage_cap < 2000 ) {
            gpio_set_level(RGB_LED_RED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(10));
            gpio_set_level(RGB_LED_RED_PIN, 0);
        }
        else {
            gpio_set_level(RGB_LED_RED_PIN, 1);
            gpio_set_level(RGB_LED_GREEN_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(10));
            gpio_set_level(RGB_LED_RED_PIN, 0);
            gpio_set_level(RGB_LED_GREEN_PIN, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    gpio_set_level(RGB_LED_GREEN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(RGB_LED_GREEN_PIN, 0);

    DEBUG_LOG(TAG, "Super cap charged enough - initializing WiFi/MQTT/ULP");

    // Connect to WiFi (with fast scan and power save)
    wifi_init_sta();

    // Initialize MQTT
    mqtt_init();

    // Initialize ULP
    init_ulp_linky();
    vTaskDelay(pdMS_TO_TICKS(100));

    DEBUG_LOG(TAG, "Initialization complete");

    // Main loop: waking up from light sleep
    while (1) {
        // Enter light sleep (WiFi stays connected)
        enter_sleep();

        // Notify current voltage value
        linky_data.voltage_cap = supercap_read_voltage_mv();
        if(linky_data.voltage_cap < 1000 ) {
            gpio_set_level(RGB_LED_RED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(10));
            gpio_set_level(RGB_LED_RED_PIN, 0);
        }
        else if(linky_data.voltage_cap < 2000 ) {
            gpio_set_level(RGB_LED_RED_PIN, 1);
            gpio_set_level(RGB_LED_GREEN_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(10));
            gpio_set_level(RGB_LED_RED_PIN, 0);
            gpio_set_level(RGB_LED_GREEN_PIN, 0);
        }
        else{
            gpio_set_level(RGB_LED_GREEN_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(10));
            gpio_set_level(RGB_LED_GREEN_PIN, 0);
        }

        // Get data from ULP
        get_linky_data(&linky_data);

        if (linky_data.valid_flags != 0) {
            DEBUG_LOG(TAG, "Linky data - IINST: %u A, BASE: %lu Wh",
                    linky_data.iinst, linky_data.base);

            // Monitor Wifi connection
            EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
            if (!(bits & WIFI_CONNECTED_BIT)) {
                DEBUG_LOG(TAG, "WiFi disconnected, reconnecting...");
                xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
                esp_wifi_connect();

                // Wait for reconnection
                bits = xEventGroupWaitBits(s_wifi_event_group,
                    WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                    pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));

                if (bits & WIFI_CONNECTED_BIT) {
                    DEBUG_LOG(TAG, "WiFi reconnected");
                } else {
                    ESP_LOGW(TAG, "WiFi reconnect failed");
                }
            }

            // Monitor MQTT connection
            if (!mqtt_connected) {
                DEBUG_LOG(TAG, "MQTT disconnected, reconnecting...");
                if (mqtt_client) {
                    esp_mqtt_client_stop(mqtt_client);
                    esp_mqtt_client_destroy(mqtt_client);
                    mqtt_client = NULL;
                }
                mqtt_init();
            }

            // Publish data
            publish_linky_data(&linky_data);
        }
    }
}
