#include <string.h>
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hulp.h"
#include "qemu_test_data.h"
#include "provisioning_manager.h"
#include "ulp_linky.h"
#include "voltage_manager.h"
#include "wifi_manager.h"

static const char *TAG = "LINKEY_QEMU";
static int qemu_voltage_mv = 2700;
static TaskHandle_t *qemu_main_task_handle;
static TaskHandle_t qemu_ulp_task_handle;
static bool qemu_next_buffer_is_1;
static esp_eth_handle_t qemu_eth_handle;
static esp_eth_netif_glue_handle_t qemu_eth_glue;
static bool qemu_eth_has_ip;
static const uint8_t qemu_wifi_sta_mac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
#if defined(LINKEY_QEMU_MANUAL_PROVISION_TEST)
static bool qemu_boot_press_logged;
#endif

static const char *qemu_selected_frame(int *len)
{
#if defined(LINKEY_QEMU_TARIFF_OPTION_TEMPO)
    *len = TEST_TIC_FRAME_TEMPO_LEN;
    return TEST_TIC_FRAME_TEMPO;
#elif defined(LINKEY_QEMU_TARIFF_OPTION_EJP)
    *len = TEST_TIC_FRAME_EJP_LEN;
    return TEST_TIC_FRAME_EJP;
#elif defined(LINKEY_QEMU_TARIFF_OPTION_HPHC)
    *len = TEST_TIC_FRAME_HPHC_LEN;
    return TEST_TIC_FRAME_HPHC;
#else
    *len = TEST_TIC_FRAME_BASE_LEN;
    return TEST_TIC_FRAME_BASE;
#endif
}

static void qemu_write_frame_to_ulp_buffer(const char *frame, int len)
{
    ulp_var_t *buf = qemu_next_buffer_is_1 ? ulp_frame_buf_1 : ulp_frame_buf_0;
    const int capacity = 1 + (LINKY_MAX_FRAME_LEN / 2);

    if (len > LINKY_MAX_FRAME_LEN) {
        len = LINKY_MAX_FRAME_LEN;
    }

    memset(buf, 0, (size_t)capacity * sizeof(*buf));
    buf[0].val = (uint32_t)len;

    for (int i = 0; i < len; i++) {
        uint32_t shift = (uint32_t)((i % 2) * 8);
        buf[1 + (i / 2)].val |= ((uint32_t)(uint8_t)frame[i]) << shift;
    }

    ulp_active_buf.val = qemu_next_buffer_is_1 ? 1 : 0;
    qemu_next_buffer_is_1 = !qemu_next_buffer_is_1;
}

static void qemu_ulp_task(void *arg)
{
    (void)arg;

    while (1) {
        int frame_len = 0;
        const char *frame = qemu_selected_frame(&frame_len);

        qemu_write_frame_to_ulp_buffer(frame, frame_len);
        if (qemu_main_task_handle && *qemu_main_task_handle) {
            xTaskNotifyGive(*qemu_main_task_handle);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void qemu_eth_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    (void)event_base;
    (void)event_data;
    wifi_state_t *state = (wifi_state_t *)arg;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "QEMU Ethernet link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "QEMU Ethernet link down");
        qemu_eth_has_ip = false;
        if (state && state->event_group) {
            xEventGroupClearBits(state->event_group, WIFI_CONNECTED_BIT);
            xEventGroupSetBits(state->event_group, WIFI_FAIL_BIT);
        }
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "QEMU Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "QEMU Ethernet stopped");
        qemu_eth_has_ip = false;
        break;
    default:
        break;
    }
}

static void qemu_eth_got_ip_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    (void)event_base;
    (void)event_id;
    wifi_state_t *state = (wifi_state_t *)arg;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    qemu_eth_has_ip = true;
    ESP_LOGI(TAG, "QEMU Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    if (state && state->event_group) {
        xEventGroupClearBits(state->event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(state->event_group, WIFI_CONNECTED_BIT);
    }
}

static void qemu_check_esp(esp_err_t err)
{
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
}

esp_err_t __wrap_esp_pm_configure(const esp_pm_config_t *config)
{
    (void)config;
    ESP_LOGI(TAG, "Power management skipped for QEMU emulation");
    return ESP_OK;
}

int __real_gpio_get_level(gpio_num_t gpio_num);

int __wrap_gpio_get_level(gpio_num_t gpio_num)
{
    if (gpio_num == GPIO_NUM_0) {
#if defined(LINKEY_QEMU_MANUAL_PROVISION_TEST)
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool pressed = now_ms >= 5000 && now_ms < 12000;
        if (pressed && !qemu_boot_press_logged) {
            ESP_LOGI(TAG, "QEMU manual BOOT/GPIO0 press active");
            qemu_boot_press_logged = true;
        }
        return pressed ? 0 : 1;
#else
        return 1;
#endif
    }
    return __real_gpio_get_level(gpio_num);
}

esp_err_t __real_esp_read_mac(uint8_t *mac, esp_mac_type_t type);

esp_err_t __wrap_esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    if (!mac) {
        return ESP_ERR_INVALID_ARG;
    }
    if (type == ESP_MAC_WIFI_STA) {
        memcpy(mac, qemu_wifi_sta_mac, sizeof(qemu_wifi_sta_mac));
        return ESP_OK;
    }
    return __real_esp_read_mac(mac, type);
}

esp_err_t __wrap_hulp_ulp_isr_register(intr_handler_t handler, void *handler_arg)
{
    (void)handler;
    qemu_main_task_handle = (TaskHandle_t *)handler_arg;
    ESP_LOGI(TAG, "ULP ISR registration captured for QEMU emulation");
    return ESP_OK;
}

void __wrap_hulp_ulp_interrupt_en(void)
{
    if (!qemu_ulp_task_handle) {
        xTaskCreate(qemu_ulp_task, "qemu_ulp", 4096, NULL, 5, &qemu_ulp_task_handle);
    }
    ESP_LOGI(TAG, "ULP notifications emulated by qemu_ulp task");
}

void __wrap_init_ulp_linky(void)
{
    int frame_len = 0;
    const char *frame = qemu_selected_frame(&frame_len);

    qemu_write_frame_to_ulp_buffer(frame, frame_len);
    ESP_LOGI(TAG, "ULP Linky receiver emulated");
}

void __wrap_voltage_init(void)
{
    ESP_LOGI(TAG, "Voltage ADC emulated at %d mV", qemu_voltage_mv);
}

int __wrap_voltage_read_mv(void)
{
    return qemu_voltage_mv;
}

bool __wrap_voltage_is_low(uint16_t threshold)
{
    return qemu_voltage_mv < (int)threshold;
}

bool __wrap_voltage_is_low_dynamic(uint16_t floor_mv)
{
    return qemu_voltage_mv < (int)floor_mv;
}

void __wrap_voltage_reset_peak(void)
{
}

void __wrap_wifi_init(wifi_state_t *state)
{
    if (!state || state->initialized) {
        return;
    }

    state->event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    qemu_check_esp(esp_event_loop_create_default());

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(netif ? ESP_OK : ESP_FAIL);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 100;

    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);

    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &qemu_eth_handle));
    qemu_eth_glue = esp_eth_new_netif_glue(qemu_eth_handle);
    ESP_ERROR_CHECK(qemu_eth_glue ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(esp_netif_attach(netif, qemu_eth_glue));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               qemu_eth_event_handler, state));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               qemu_eth_got_ip_handler, state));
    ESP_ERROR_CHECK(esp_eth_start(qemu_eth_handle));

    state->initialized = true;
    state->started = true;
    ESP_LOGI(TAG, "QEMU Ethernet initialized for real MQTT networking");
}

void __wrap_wifi_start(wifi_state_t *state)
{
    if (!state) {
        return;
    }
    if (state->initialized && !state->started && qemu_eth_handle) {
        ESP_ERROR_CHECK(esp_eth_start(qemu_eth_handle));
        state->started = true;
    }
}

void __wrap_wifi_stop(wifi_state_t *state)
{
    if (!state) {
        return;
    }
    if (state->started && qemu_eth_handle) {
        esp_eth_stop(qemu_eth_handle);
        state->started = false;
        qemu_eth_has_ip = false;
    }
}

conn_result_t __wrap_wifi_connect(wifi_state_t *state,
                                  voltage_check_fn_t voltage_check,
                                  uint16_t voltage_threshold,
                                  uint32_t timeout_ms,
                                  uint32_t poll_interval_ms)
{
    if (!state || !state->event_group) {
        return CONN_FAILED;
    }

    uint32_t wait_ms = 0;
    while (wait_ms < timeout_ms) {
        if (voltage_check && voltage_check(voltage_threshold)) {
            return CONN_VOLTAGE_LOW;
        }

        EventBits_t bits = xEventGroupGetBits(state->event_group);
        if (bits & WIFI_CONNECTED_BIT) {
            return CONN_OK;
        }
        if (bits & WIFI_FAIL_BIT) {
            ESP_LOGW(TAG, "QEMU Ethernet connection failed");
            return CONN_FAILED;
        }

        vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        wait_ms += poll_interval_ms;
    }

    ESP_LOGW(TAG, "QEMU Ethernet connection timeout");
    return CONN_FAILED;
}

bool __wrap_wifi_has_config(void)
{
    return true;
}

bool __wrap_wifi_is_connected(void)
{
    return qemu_eth_has_ip;
}

conn_result_t provisioning_start_ble(uint16_t voltage_threshold,
                                     uint32_t poll_interval_ms)
{
    (void)voltage_threshold;
    (void)poll_interval_ms;
    ESP_LOGE(TAG, "BLE provisioning is not supported in QEMU emulation");
    return CONN_FAILED;
}
