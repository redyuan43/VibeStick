#include "telemetry_board.h"
#include "sticks3_battery_curve.h"

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"

#define PIN_I2C_SDA 47
#define PIN_I2C_SCL 48
#define I2C_FREQ_HZ 100000
#define M5PM1_ADDR 0x6e
#define M5PM1_REG_DEVICE_ID 0x00
#define M5PM1_REG_PWR_CFG 0x06
#define M5PM1_REG_HOLD_CFG 0x07
#define M5PM1_REG_GPIO_MODE 0x10
#define M5PM1_REG_GPIO_OUT 0x11
#define M5PM1_REG_GPIO_IN 0x12
#define M5PM1_REG_GPIO_DRV 0x13
#define M5PM1_REG_GPIO_FUNC0 0x16
#define M5PM1_REG_VBAT_L 0x22
#define M5PM1_REG_VIN_L 0x24
#define M5PM1_PWR_CFG_LDO_EN BIT(2)
#define M5PM1_PWR_CFG_LED_CTRL BIT(4)
#define M5PM1_HOLD_CFG_LDO_HOLD BIT(5)
#define M5PM1_GPIO2_L3B_POWER_EN BIT(2)

static const char *TAG = "board_sticks3";
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_pmic_dev;

static esp_err_t i2c_read_u8(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_pmic_dev, &reg, 1, value, 1, 100);
}

static esp_err_t i2c_write_u8(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(s_pmic_dev, data, sizeof(data), 100);
}

static esp_err_t update_reg(uint8_t reg, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(i2c_read_u8(reg, &value), TAG, "read pmic reg");
    value = (uint8_t)((value & ~clear_mask) | set_mask);
    return i2c_write_u8(reg, value);
}

static esp_err_t i2c_read_u16_le(uint8_t reg, uint16_t *value)
{
    uint8_t data[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(
        s_pmic_dev, &reg, 1, data, sizeof(data), 100);
    if (err != ESP_OK) {
        return err;
    }
    *value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return ESP_OK;
}

static esp_err_t init_i2c_on(i2c_port_t port, gpio_num_t sda, gpio_num_t scl)
{
    if (s_i2c_bus) {
        (void)i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        s_pmic_dev = NULL;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(
        i2c_new_master_bus(&bus_config, &s_i2c_bus), TAG, "i2c bus");

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = M5PM1_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_pmic_dev);
}

static esp_err_t init_i2c(void)
{
    const struct {
        i2c_port_t port;
        gpio_num_t sda;
        gpio_num_t scl;
    } candidates[] = {
        {I2C_NUM_1, PIN_I2C_SDA, PIN_I2C_SCL},
        {I2C_NUM_1, PIN_I2C_SCL, PIN_I2C_SDA},
        {I2C_NUM_0, PIN_I2C_SDA, PIN_I2C_SCL},
        {I2C_NUM_0, PIN_I2C_SCL, PIN_I2C_SDA},
    };

    esp_err_t last_err = ESP_FAIL;
    for (size_t index = 0; index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
        last_err = init_i2c_on(
            candidates[index].port, candidates[index].sda, candidates[index].scl);
        if (last_err != ESP_OK) {
            continue;
        }
        uint8_t device_id = 0;
        uint8_t power_config = 0;
        uint8_t hold_config = 0;
        last_err = i2c_read_u8(M5PM1_REG_DEVICE_ID, &device_id);
        if (last_err == ESP_OK) {
            last_err = i2c_read_u8(M5PM1_REG_PWR_CFG, &power_config);
        }
        if (last_err == ESP_OK) {
            last_err = i2c_read_u8(M5PM1_REG_HOLD_CFG, &hold_config);
        }
        if (last_err == ESP_OK) {
            ESP_LOGI(
                TAG,
                "M5PM1 found id=0x%02x port=%d sda=%d scl=%d",
                device_id,
                candidates[index].port,
                candidates[index].sda,
                candidates[index].scl);
            return ESP_OK;
        }
    }
    return last_err;
}

static int percent_from_mv(int mv)
{
    return vibe_sticks3_battery_percent(mv);
}

esp_err_t telemetry_board_init(void)
{
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "find M5PM1");
    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_PWR_CFG, M5PM1_PWR_CFG_LED_CTRL,
                                   M5PM1_PWR_CFG_LDO_EN),
                        TAG, "enable PMIC LDO");
    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_HOLD_CFG, 0, M5PM1_HOLD_CFG_LDO_HOLD),
                        TAG, "hold PMIC power");
    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_GPIO_FUNC0, M5PM1_GPIO2_L3B_POWER_EN, 0),
                        TAG, "select display power GPIO");
    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_GPIO_MODE, 0, M5PM1_GPIO2_L3B_POWER_EN),
                        TAG, "display power output");
    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_GPIO_DRV, M5PM1_GPIO2_L3B_POWER_EN, 0),
                        TAG, "display power drive");
    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_GPIO_OUT, 0, M5PM1_GPIO2_L3B_POWER_EN),
                        TAG, "display power on");
    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_GPIO_FUNC0, 0x03, 0),
                        TAG, "charging status GPIO");
    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_GPIO_MODE, BIT(0), 0),
                        TAG, "charging status input");

    const telemetry_display_config_t display = {
        .h_res = 135,
        .v_res = 240,
        .x_gap = 52,
        .y_gap = 40,
        .spi_host = SPI2_HOST,
        .pin_mosi = 39,
        .pin_sclk = 40,
        .pin_cs = 41,
        .pin_dc = 45,
        .pin_rst = 21,
        .pin_bl = 38,
        .backlight_active_high = true,
    };
    return telemetry_display_init_white(&display);
}

esp_err_t telemetry_board_read_battery(telemetry_battery_sample_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "missing output");
    *out = (telemetry_battery_sample_t){0};

    uint16_t vbat = 0;
    esp_err_t err = i2c_read_u16_le(M5PM1_REG_VBAT_L, &vbat);
    if (err != ESP_OK) {
        return err;
    }
    out->voltage_mv_valid = true;
    out->voltage_mv = (int)vbat;
    out->percent_valid = true;
    out->percent = percent_from_mv(out->voltage_mv);

    uint16_t vin = 0;
    if (i2c_read_u16_le(M5PM1_REG_VIN_L, &vin) == ESP_OK) {
        out->usb_voltage_mv_valid = true;
        out->usb_voltage_mv = vin;
        out->usb_powered_valid = true;
        out->usb_powered = vin > 4500;
    }
    uint8_t gpio_in = 0;
    if (i2c_read_u8(M5PM1_REG_GPIO_IN, &gpio_in) == ESP_OK) {
        out->charging_valid = true;
        out->charging = (gpio_in & BIT(0)) == 0;
    }
    return ESP_OK;
}

const char *telemetry_board_pmic_name(void)
{
    return "m5pm1";
}
