#include "vibe_cardputer_battery.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

#define CARDPUTER_BATTERY_ADC_CHANNEL ADC_CHANNEL_9

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_calibration;

static int voltage_to_percent(int voltage_mv)
{
    static const struct {
        int voltage_mv;
        int percent;
    } curve[] = {
        {3300, 0}, {3500, 10}, {3600, 20}, {3680, 30}, {3740, 40},
        {3790, 50}, {3840, 60}, {3890, 70}, {3950, 80}, {4050, 90},
        {4200, 100},
    };
    if (voltage_mv <= curve[0].voltage_mv) {
        return curve[0].percent;
    }
    const size_t last = sizeof(curve) / sizeof(curve[0]) - 1;
    if (voltage_mv >= curve[last].voltage_mv) {
        return curve[last].percent;
    }
    for (size_t index = 1; index <= last; ++index) {
        if (voltage_mv <= curve[index].voltage_mv) {
            const int mv_span =
                curve[index].voltage_mv - curve[index - 1].voltage_mv;
            const int pct_span =
                curve[index].percent - curve[index - 1].percent;
            return curve[index - 1].percent +
                   ((voltage_mv - curve[index - 1].voltage_mv) * pct_span +
                    mv_span / 2) /
                       mv_span;
        }
    }
    return curve[last].percent;
}

esp_err_t vibe_cardputer_battery_init(void)
{
    if (s_adc) {
        return ESP_OK;
    }
    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_config, &s_adc),
                        "card_battery", "battery adc");
    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(
        adc_oneshot_config_channel(s_adc, CARDPUTER_BATTERY_ADC_CHANNEL,
                                   &channel_config),
        "card_battery", "battery channel");

    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = ADC_UNIT_1,
        .chan = CARDPUTER_BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t calibration_err = adc_cali_create_scheme_curve_fitting(
        &calibration_config, &s_calibration);
    if (calibration_err != ESP_OK) {
        s_calibration = NULL;
        ESP_LOGW("card_battery", "battery calibration unavailable: %s",
                 esp_err_to_name(calibration_err));
    }
    return ESP_OK;
}

esp_err_t vibe_cardputer_battery_level(int *level_percent)
{
    ESP_RETURN_ON_FALSE(level_percent && s_adc, ESP_ERR_INVALID_STATE,
                        "card_battery", "battery not ready");
    int total = 0;
    for (int index = 0; index < 8; ++index) {
        int raw = 0;
        ESP_RETURN_ON_ERROR(
            adc_oneshot_read(s_adc, CARDPUTER_BATTERY_ADC_CHANNEL, &raw),
            "card_battery", "battery read");
        total += raw;
    }
    const int raw = total / 8;
    int pin_mv = 0;
    if (s_calibration) {
        ESP_RETURN_ON_ERROR(
            adc_cali_raw_to_voltage(s_calibration, raw, &pin_mv),
            "card_battery", "battery calibrate");
    } else {
        pin_mv = (raw * 3300 + 2047) / 4095;
    }
    const int battery_mv = (pin_mv * 204 + 50) / 100;
    *level_percent = voltage_to_percent(battery_mv);
    ESP_LOGI("card_battery", "battery=%dmV level=%d%%", battery_mv,
             *level_percent);
    return ESP_OK;
}
