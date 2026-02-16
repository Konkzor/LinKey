#include "voltage_manager.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "debug.h"

static const char *TAG = "VOLTAGE_MGR";

// Supercap voltage ADC configuration (GPIO 33 = ADC1_CHANNEL_5)
#define SUPERCAP_ADC_CHANNEL    ADC1_CHANNEL_5
#define SUPERCAP_ADC_ATTEN      ADC_ATTEN_DB_12  // Full scale ~3.3V
#define SUPERCAP_ADC_WIDTH      ADC_WIDTH_BIT_12

// ADC calibration characteristics
static esp_adc_cal_characteristics_t *adc_chars = NULL;

// Dynamic voltage peak tracker for drain detection
static int dynamic_voltage_peak_mv = 0;

void voltage_init(void)
{
    adc1_config_width(SUPERCAP_ADC_WIDTH);
    adc1_config_channel_atten(SUPERCAP_ADC_CHANNEL, SUPERCAP_ADC_ATTEN);

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

int voltage_read_mv(void)
{
    int raw_value = adc1_get_raw(SUPERCAP_ADC_CHANNEL);
    uint32_t voltage_mv = esp_adc_cal_raw_to_voltage(raw_value, adc_chars);
    return (int)voltage_mv;
}

bool voltage_is_low(uint16_t threshold)
{
    int voltage_mv = voltage_read_mv();
    if (voltage_mv < threshold) {
        DEBUG_LOGW(TAG, "Voltage dropped to %d mV (threshold %d)", voltage_mv, threshold);
        return true;
    }
    return false;
}

bool voltage_is_low_dynamic(uint16_t floor_mv)
{
    int voltage_mv = voltage_read_mv();
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

void voltage_reset_peak(void)
{
    dynamic_voltage_peak_mv = 0;
}
