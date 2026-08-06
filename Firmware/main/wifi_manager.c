#include <string.h>
#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "debug.h"

static const char *TAG = "WIFI_MGR";

// Store WiFi connection info in RTC memory for fast reconnect
RTC_DATA_ATTR static uint8_t rtc_bssid[6] = {0};
RTC_DATA_ATTR static uint8_t rtc_channel = 0;
RTC_DATA_ATTR static bool rtc_bssid_valid = false;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    wifi_state_t *state = (wifi_state_t*)arg;
    if (!state) return;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        DEBUG_LOG(TAG, "WiFi STA started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        (void)event;
        DEBUG_LOG(TAG, "WiFi disconnected, reason: %d", event->reason);
        xEventGroupClearBits(state->event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(state->event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        (void)event;
        DEBUG_LOG(TAG, "WiFi connected - IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // Cache BSSID and channel for faster reconnection next time
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            memcpy(rtc_bssid, ap_info.bssid, 6);
            rtc_channel = ap_info.primary;
            rtc_bssid_valid = true;
            DEBUG_LOG(TAG, "Cached AP - Channel: %d", rtc_channel);
        }

        xEventGroupSetBits(state->event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init(wifi_state_t *state)
{
    if (!state || state->initialized) {
        return;
    }

    state->event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

#ifdef CONFIG_LINKEY_USE_STATIC_IP
    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr = esp_ip4addr_aton(CONFIG_LINKEY_STATIC_IP);
    ip_info.gw.addr = esp_ip4addr_aton(CONFIG_LINKEY_STATIC_GATEWAY);
    ip_info.netmask.addr = esp_ip4addr_aton(CONFIG_LINKEY_STATIC_NETMASK);
    esp_netif_set_ip_info(netif, &ip_info);
    DEBUG_LOG(TAG, "Static IP: %s", CONFIG_LINKEY_STATIC_IP);
#else
    (void)netif;
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 1;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        state,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        state,
                                                        &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_config = {0};
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_config));

    if (wifi_config.sta.ssid[0] != '\0') {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config.sta.scan_method = WIFI_FAST_SCAN;
        wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        wifi_config.sta.listen_interval = 10;

        if (rtc_bssid_valid) {
            memcpy(wifi_config.sta.bssid, rtc_bssid, 6);
            wifi_config.sta.bssid_set = 1;
            wifi_config.sta.channel = rtc_channel;
            DEBUG_LOG(TAG, "Using cached AP (Ch %d)", rtc_channel);
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    }

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));
    DEBUG_LOG(TAG, "WiFi modem sleep enabled");

    state->initialized = true;
    DEBUG_LOG(TAG, "WiFi initialized");
}

void wifi_stop(wifi_state_t *state)
{
    if (!state) return;

    if (state->initialized && state->started) {
        DEBUG_LOG(TAG, "Stopping WiFi to save power...");
        esp_wifi_stop();
        state->started = false;
        xEventGroupClearBits(state->event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
}

void wifi_start(wifi_state_t *state)
{
    if (!state) return;

    if (state->initialized && !state->started) {
        DEBUG_LOG(TAG, "Starting WiFi...");
        ESP_ERROR_CHECK(esp_wifi_start());
        // Set shorter beacon timeout after WiFi is started (minimum 3s for STA).
        ESP_ERROR_CHECK(esp_wifi_set_inactive_time(WIFI_IF_STA, 3));
        state->started = true;
    }
}

bool wifi_has_config(void)
{
    wifi_config_t wifi_config = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) != ESP_OK) {
        return false;
    }
    return wifi_config.sta.ssid[0] != '\0';
}

void wifi_mark_started(wifi_state_t *state)
{
    if (!state) return;
    ESP_ERROR_CHECK(esp_wifi_set_inactive_time(WIFI_IF_STA, 3));
    state->started = true;
}

conn_result_t wifi_connect(wifi_state_t *state, voltage_check_fn_t voltage_check,
                           uint16_t voltage_threshold, uint32_t timeout_ms,
                           uint32_t poll_interval_ms)
{
    if (!state) return CONN_FAILED;

    // Check if already connected
    EventBits_t bits = xEventGroupGetBits(state->event_group);
    if (bits & WIFI_CONNECTED_BIT) {
        DEBUG_LOG(TAG, "WiFi already connected");
        return CONN_OK;
    }

    // Disconnect first to cancel any ongoing connection attempt
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));

    // Clear bits and trigger connection
    xEventGroupClearBits(state->event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_connect();

    // Poll for connection with voltage checks
    uint32_t wait_ms = 0;
    while (wait_ms < timeout_ms) {
        // Check voltage if callback provided
        if (voltage_check && voltage_check(voltage_threshold)) {
            return CONN_VOLTAGE_LOW;
        }

        // Check connection status
        bits = xEventGroupGetBits(state->event_group);
        if (bits & WIFI_CONNECTED_BIT) {
            return CONN_OK;
        }
        if (bits & WIFI_FAIL_BIT) {
            DEBUG_LOGW(TAG, "WiFi connection failed");
            return CONN_FAILED;
        }

        vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        wait_ms += poll_interval_ms;
    }

    DEBUG_LOGW(TAG, "WiFi connection timeout");
    return CONN_FAILED;
}

bool wifi_is_connected(void)
{
    wifi_ap_record_t ap_info;
    return (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
}
