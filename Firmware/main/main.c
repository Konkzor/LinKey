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

#include "hulp.h"
#include "ulp_linky.h"
#include "debug.h"

static const char *TAG = "LINKY_MAIN";

// Sleep mode configuration
#ifdef CONFIG_LINKY_SLEEP_MODE_LIGHT
    #define USE_LIGHT_SLEEP 1
#else
    #define USE_LIGHT_SLEEP 0
#endif

// WiFi and MQTT credentials from Kconfig
#define WIFI_SSID       CONFIG_LINKY_WIFI_SSID
#define WIFI_PASS       CONFIG_LINKY_WIFI_PASSWORD
#define MQTT_BROKER_URI CONFIG_LINKY_MQTT_BROKER_URI
#define MQTT_USERNAME   CONFIG_LINKY_MQTT_USERNAME
#define MQTT_PASSWORD   CONFIG_LINKY_MQTT_PASSWORD

// MQTT topics
#define MQTT_TOPIC_IINST CONFIG_LINKY_MQTT_TOPIC_PREFIX "/iinst"
#define MQTT_TOPIC_BASE  CONFIG_LINKY_MQTT_TOPIC_PREFIX "/base"

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

#if USE_LIGHT_SLEEP
// Light sleep: Track initialization state
RTC_DATA_ATTR static bool wifi_mqtt_initialized = false;
#endif

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Nothing to do.
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
    esp_mqtt_event_handle_t event = event_data;

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

#if USE_LIGHT_SLEEP
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    DEBUG_LOG(TAG, "WiFi modem sleep enabled");
#endif

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
}

// Enter sleep mode (deep or light based on configuration)
static void enter_sleep(void)
{
#if USE_LIGHT_SLEEP
    // Light sleep mode - WiFi/MQTT stay connected
    DEBUG_LOG(TAG, "Entering light sleep - WiFi/MQTT stay connected");

    // Clear all pending wakeup conditions and enable ULP wakeup only
    ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
    ESP_ERROR_CHECK(esp_sleep_enable_ulp_wakeup());

    // Enter light sleep (WiFi modem sleep handles power saving)
    esp_light_sleep_start();

    DEBUG_LOG(TAG, "Woke from light sleep");
#else
    // Deep sleep mode - disconnect everything
    DEBUG_LOG(TAG, "Entering deep sleep - ULP will wake us up");

    // Disconnect WiFi and MQTT to save power
    if (mqtt_client) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }
    esp_wifi_stop();

    // Enable ULP wakeup
    ESP_ERROR_CHECK(esp_sleep_enable_ulp_wakeup());

    // Enter deep sleep
    esp_deep_sleep_start();
#endif
}

void app_main(void)
{
    linky_data_t linky_data;

    // Print wake-up reason
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    switch (wakeup_reason) {
        case ESP_SLEEP_WAKEUP_ULP:
            DEBUG_LOG(TAG, "Wake-up from ULP");
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            DEBUG_LOG(TAG, "Wake-up from timer");
            break;
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        default:
            DEBUG_LOG(TAG, "Initial boot");
            break;
    }

    // Initialize NVS (used for Wifi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#if USE_LIGHT_SLEEP
    // Light sleep mode: Initialize once and loop forever
    DEBUG_LOG(TAG, "First boot - initializing WiFi/MQTT/ULP");

    // Configure automatic power management for light sleep
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,      // Max CPU frequency
        .min_freq_mhz = 40,       // Min CPU frequency (APB will be 40MHz)
        .light_sleep_enable = true // Enable automatic light sleep
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    DEBUG_LOG(TAG, "Power management configured: auto light sleep enabled");

    // Connect to WiFi (with fast scan and power save)
    wifi_init_sta();

    // Initialize MQTT once
    mqtt_init();

    // Initialize ULP
    init_ulp_linky();
    vTaskDelay(pdMS_TO_TICKS(1000));

    DEBUG_LOG(TAG, "Initialization complete");

    // Light sleep: Loop forever, waking up from ULP
    while (1) {
        // Get data from ULP
        get_linky_data(&linky_data);

        if (linky_data.valid_flags != 0) {
            DEBUG_LOG(TAG, "Linky data - IINST: %u A, BASE: %lu Wh",
                    linky_data.iinst, linky_data.base);

            // Publish data (WiFi/MQTT already connected)
            publish_linky_data(&linky_data);
        }

        // Enter light sleep (WiFi stays connected)
        enter_sleep();
    }

#else
    // Deep sleep mode: Original behavior

    // Initialize ULP (only on first boot)
    if (wakeup_reason != ESP_SLEEP_WAKEUP_ULP && wakeup_reason != ESP_SLEEP_WAKEUP_TIMER) {
        init_ulp_linky();
        DEBUG_LOG(TAG, "ULP initialized, entering deep sleep for ULP to collect data");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Give ULP time to start
        enter_sleep();
        return; // Never reached
    }

    // Woke up from ULP - get data
    get_linky_data(&linky_data);

    if (linky_data.valid_flags != 0) {
        DEBUG_LOG(TAG, "Linky data - IINST: %u A, BASE: %lu Wh",
                linky_data.iinst, linky_data.base);

        // Connect to WiFi
        wifi_init_sta();

        // Connect to MQTT
        mqtt_init();

        // Publish data
        publish_linky_data(&linky_data);
    }

    // Go back to deep sleep
    enter_sleep();
    // Never reached
#endif
}
