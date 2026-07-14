#include "telemetry_board.h"

#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"

#define I2C_PORT I2C_NUM_0
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define AXP192_ADDR 0x34
#define AXP192_REG_POWER_STATUS 0x00
#define AXP192_REG_CHARGE_STATUS 0x01
#define AXP192_REG_POWER_OUTPUT 0x12
#define AXP192_REG_LDO23_VOLTAGE 0x28
#define AXP192_REG_VBUS_VOLTAGE_H 0x5a
#define AXP192_REG_BATTERY_VOLTAGE_H 0x78

static const char *TAG = "board_stickc_plus_11";

static esp_err_t axp_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(I2C_PORT, AXP192_ADDR, &reg, 1, value, 1, pdMS_TO_TICKS(100));
}

static esp_err_t axp_write(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_write_to_device(I2C_PORT, AXP192_ADDR, data, sizeof(data), pdMS_TO_TICKS(100));
}

static esp_err_t axp_read_u12(uint8_t reg, uint16_t *value)
{
    uint8_t data[2] = {0};
    esp_err_t err = i2c_master_write_read_device(I2C_PORT, AXP192_ADDR, &reg, 1, data, sizeof(data), pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        return err;
    }
    *value = ((uint16_t)data[0] << 4) | (data[1] & 0x0f);
    return ESP_OK;
}

static int percent_from_mv(int mv)
{
    if (mv <= 3300) {
        return 0;
    }
    if (mv >= 4200) {
        return 100;
    }
    return (mv - 3300) * 100 / 900;
}

esp_err_t telemetry_board_init(void)
{
    i2c_config_t i2c = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_PORT, &i2c), TAG, "i2c config");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0), TAG, "i2c install");

    uint8_t output = 0;
    if (axp_read(AXP192_REG_POWER_OUTPUT, &output) == ESP_OK) {
        (void)axp_write(AXP192_REG_LDO23_VOLTAGE, 0xcc);
        (void)axp_write(AXP192_REG_POWER_OUTPUT, output | 0x04);
    }

    const telemetry_display_config_t display = {
        .h_res = 135,
        .v_res = 240,
        .x_gap = 52,
        .y_gap = 40,
        .spi_host = HSPI_HOST,
        .pin_mosi = 15,
        .pin_sclk = 13,
        .pin_cs = 5,
        .pin_dc = 23,
        .pin_rst = 18,
        .pin_bl = -1,
        .backlight_active_high = true,
    };
    return telemetry_display_init_white(&display);
}

esp_err_t telemetry_board_read_battery(telemetry_battery_sample_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "missing output");
    *out = (telemetry_battery_sample_t){0};

    uint16_t raw = 0;
    esp_err_t err = axp_read_u12(AXP192_REG_BATTERY_VOLTAGE_H, &raw);
    if (err != ESP_OK) {
        return err;
    }
    out->voltage_mv_valid = true;
    out->voltage_mv = (int)((raw * 11) / 10);
    out->percent_valid = true;
    out->percent = percent_from_mv(out->voltage_mv);

    uint8_t power = 0;
    uint16_t vbus_raw = 0;
    if (axp_read_u12(AXP192_REG_VBUS_VOLTAGE_H, &vbus_raw) == ESP_OK) {
        out->usb_voltage_mv_valid = true;
        out->usb_voltage_mv = (int)((vbus_raw * 17) / 10);
    }
    if (axp_read(AXP192_REG_POWER_STATUS, &power) == ESP_OK) {
        out->usb_powered_valid = true;
        out->usb_powered = out->usb_voltage_mv_valid
                               ? out->usb_voltage_mv > 4000
                               : (power & 0x20) != 0;
    }
    uint8_t charge = 0;
    if (axp_read(AXP192_REG_CHARGE_STATUS, &charge) == ESP_OK) {
        out->charging_valid = true;
        out->charging = (charge & 0x40) != 0;
    }
    return ESP_OK;
}

const char *telemetry_board_pmic_name(void)
{
    return "axp192";
}
