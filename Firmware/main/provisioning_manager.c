#include <stdio.h>
#include <string.h>
#include "provisioning_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_wifi.h"
#ifdef CONFIG_LINKEY_DEBUG_LOGS
#include "qrcode.h"
#endif
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "wifi_provisioning/wifi_config.h"
#include "wifi_provisioning/wifi_scan.h"
#include "voltage_manager.h"
#include "debug.h"

static const char *TAG = "PROV_MGR";

#define PROV_DONE_BIT BIT0
#define PROV_FAIL_BIT BIT1
#define PROV_QR_VERSION "v1"
#define PROV_TRANSPORT_BLE "ble"
#define QRCODE_BASE_URL "https://espressif.github.io/esp-jumpstart/qrcode.html"

static EventGroupHandle_t prov_event_group;
static wifi_config_t deferred_wifi_config;
static bool deferred_wifi_config_set;
static bool deferred_wifi_config_applied;

static esp_err_t deferred_config_get_status(wifi_prov_config_get_data_t *resp_data,
                                            wifi_prov_ctx_t **ctx)
{
    (void)ctx;
    memset(resp_data, 0, sizeof(*resp_data));

    if (!deferred_wifi_config_applied) {
        resp_data->wifi_state = WIFI_PROV_STA_CONNECTING;
        resp_data->connecting_info.attempts_remaining = 1;
        return ESP_OK;
    }

    resp_data->wifi_state = WIFI_PROV_STA_CONNECTED;
    strlcpy(resp_data->conn_info.ip_addr, "0.0.0.0",
            sizeof(resp_data->conn_info.ip_addr));
    strlcpy(resp_data->conn_info.ssid, (const char *)deferred_wifi_config.sta.ssid,
            sizeof(resp_data->conn_info.ssid));
    memset(resp_data->conn_info.bssid, 0x02, sizeof(resp_data->conn_info.bssid));
    resp_data->conn_info.channel = deferred_wifi_config.sta.channel;
    resp_data->conn_info.auth_mode = WIFI_AUTH_WPA2_PSK;

    if (prov_event_group) {
        xEventGroupSetBits(prov_event_group, PROV_DONE_BIT);
    }
    DEBUG_LOG(TAG, "Reported deferred WiFi provisioning success to app");
    return ESP_OK;
}

static esp_err_t deferred_config_set(const wifi_prov_config_set_data_t *req_data,
                                     wifi_prov_ctx_t **ctx)
{
    (void)ctx;
    memset(&deferred_wifi_config, 0, sizeof(deferred_wifi_config));

    size_t ssid_len = strnlen(req_data->ssid, sizeof(deferred_wifi_config.sta.ssid));
    memcpy(deferred_wifi_config.sta.ssid, req_data->ssid, ssid_len);
    strlcpy((char *)deferred_wifi_config.sta.password, req_data->password,
            sizeof(deferred_wifi_config.sta.password));

    if (req_data->channel != 0) {
        deferred_wifi_config.sta.channel = req_data->channel;
    }
    if (memcmp(req_data->bssid, "\0\0\0\0\0\0", 6) != 0) {
        memcpy(deferred_wifi_config.sta.bssid, req_data->bssid,
               sizeof(deferred_wifi_config.sta.bssid));
        deferred_wifi_config.sta.bssid_set = 1;
    }

    deferred_wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    deferred_wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    deferred_wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    deferred_wifi_config.sta.listen_interval = 10;

    deferred_wifi_config_set = true;
    deferred_wifi_config_applied = false;
    DEBUG_LOG(TAG, "Stored deferred WiFi credentials for SSID %s", req_data->ssid);
    return ESP_OK;
}

static esp_err_t deferred_config_apply(wifi_prov_ctx_t **ctx)
{
    (void)ctx;
    if (!deferred_wifi_config_set) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &deferred_wifi_config);
    if (ret != ESP_OK) {
        DEBUG_LOGE(TAG, "Failed to store deferred WiFi credentials: %s",
                   esp_err_to_name(ret));
        return ret;
    }

    deferred_wifi_config_applied = true;
    DEBUG_LOG(TAG, "Deferred WiFi credentials saved; skipping immediate WiFi validation");
    return ESP_OK;
}

static wifi_prov_config_handlers_t deferred_config_handlers = {
    .get_status_handler = deferred_config_get_status,
    .set_config_handler = deferred_config_set,
    .apply_config_handler = deferred_config_apply,
    .ctx = NULL,
};

static esp_err_t low_power_scan_start(bool blocking, bool passive,
                                      uint8_t group_channels, uint32_t period_ms,
                                      wifi_prov_scan_ctx_t **ctx)
{
    (void)blocking;
    (void)passive;
    (void)group_channels;
    (void)period_ms;
    (void)ctx;
    DEBUG_LOG(TAG, "Ignoring WiFi scan request for low-power provisioning");
    return ESP_OK;
}

static esp_err_t low_power_scan_status(bool *scan_finished,
                                       uint16_t *result_count,
                                       wifi_prov_scan_ctx_t **ctx)
{
    (void)ctx;
    if (scan_finished) {
        *scan_finished = true;
    }
    if (result_count) {
        *result_count = 0;
    }
    return ESP_OK;
}

static esp_err_t low_power_scan_result(uint16_t result_index,
                                       wifi_prov_scan_result_t *result,
                                       wifi_prov_scan_ctx_t **ctx)
{
    (void)result_index;
    (void)result;
    (void)ctx;
    return ESP_ERR_NOT_FOUND;
}

static wifi_prov_scan_handlers_t low_power_scan_handlers = {
    .scan_start = low_power_scan_start,
    .scan_status = low_power_scan_status,
    .scan_result = low_power_scan_result,
    .ctx = NULL,
};

static void get_service_name(char *service_name, size_t service_name_size)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(service_name, service_name_size, "Linkey-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

static void print_provisioning_qr(const char *service_name)
{
    char payload[150] = {0};
    snprintf(payload, sizeof(payload),
             "{\"ver\":\"%s\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\"}",
             PROV_QR_VERSION, service_name, CONFIG_LINKEY_PROV_POP,
             PROV_TRANSPORT_BLE);

#ifdef CONFIG_LINKEY_DEBUG_LOGS
    ESP_LOGI(TAG, "Scan this QR code from the ESP provisioning app.");
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    esp_qrcode_generate(&cfg, payload);
#endif
    ESP_LOGI(TAG, "Provisioning URL:\n%s?data=%s", QRCODE_BASE_URL, payload);
}

static void provisioning_event_handler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                DEBUG_LOG(TAG, "BLE provisioning started");
                break;
            case WIFI_PROV_CRED_RECV:
            {
                const wifi_sta_config_t *wifi_sta_cfg = (const wifi_sta_config_t *)event_data;
                (void)wifi_sta_cfg;
                DEBUG_LOG(TAG, "Received WiFi credentials for SSID %s",
                          (const char *)wifi_sta_cfg->ssid);
                break;
            }
            case WIFI_PROV_CRED_FAIL:
                DEBUG_LOGW(TAG, "WiFi provisioning credentials failed");
                if (prov_event_group) {
                    xEventGroupSetBits(prov_event_group, PROV_FAIL_BIT);
                }
                break;
            case WIFI_PROV_CRED_SUCCESS:
                DEBUG_LOG(TAG, "WiFi provisioning credentials accepted");
                break;
            case WIFI_PROV_END:
                DEBUG_LOG(TAG, "BLE provisioning ended");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (prov_event_group) {
            xEventGroupSetBits(prov_event_group, PROV_DONE_BIT);
        }
    }
}

conn_result_t provisioning_start_ble(uint16_t voltage_threshold,
                                     uint32_t poll_interval_ms)
{
    char service_name[32];
    get_service_name(service_name, sizeof(service_name));

    prov_event_group = xEventGroupCreate();
    if (!prov_event_group) {
        return CONN_FAILED;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                               provisioning_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               provisioning_event_handler, NULL));

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };

    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    const wifi_prov_security1_params_t *sec_params = CONFIG_LINKEY_PROV_POP;
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1,
                                                     sec_params,
                                                     service_name,
                                                     NULL));

    deferred_wifi_config_set = false;
    deferred_wifi_config_applied = false;
    wifi_prov_mgr_endpoint_unregister("prov-config");
    ESP_ERROR_CHECK(wifi_prov_mgr_endpoint_register("prov-config",
                                                    wifi_prov_config_data_handler,
                                                    &deferred_config_handlers));
    DEBUG_LOG(TAG, "WiFi config endpoint replaced with deferred validation");

    wifi_prov_mgr_endpoint_unregister("prov-scan");
    ESP_ERROR_CHECK(wifi_prov_mgr_endpoint_register("prov-scan",
                                                    wifi_prov_scan_handler,
                                                    &low_power_scan_handlers));
    DEBUG_LOG(TAG, "WiFi scan endpoint replaced with low-power empty scan");

    DEBUG_LOG(TAG, "Provision with ESP app: name=%s pop=%s transport=ble security=1",
              service_name, CONFIG_LINKEY_PROV_POP);
    print_provisioning_qr(service_name);

    conn_result_t result = CONN_FAILED;
    while (1) {
        if (voltage_is_low_dynamic(voltage_threshold)) {
            DEBUG_LOGW(TAG, "Voltage too low during BLE provisioning");
            wifi_prov_mgr_stop_provisioning();
            result = CONN_VOLTAGE_LOW;
            break;
        }

        EventBits_t bits = xEventGroupWaitBits(prov_event_group,
                                               PROV_DONE_BIT | PROV_FAIL_BIT,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(poll_interval_ms));
        if (bits & PROV_DONE_BIT) {
            result = CONN_OK;
            break;
        }
        if (bits & PROV_FAIL_BIT) {
            result = CONN_FAILED;
            break;
        }
    }

    wifi_prov_mgr_deinit();
    esp_err_t stop_ret = esp_wifi_stop();
    if (stop_ret != ESP_OK && stop_ret != ESP_ERR_WIFI_NOT_STARTED) {
        DEBUG_LOGW(TAG, "Failed to stop WiFi after provisioning: %s",
                   esp_err_to_name(stop_ret));
    }
    if (result == CONN_OK) {
        wifi_prov_scheme_ble_event_cb_free_btdm(NULL, WIFI_PROV_DEINIT, NULL);
    }

    esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                 provisioning_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                 provisioning_event_handler);
    vEventGroupDelete(prov_event_group);
    prov_event_group = NULL;

    return result;
}
