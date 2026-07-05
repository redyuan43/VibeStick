#include "vibe_board.h"

#include "vibe_board_profile.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define I2C_FREQ_HZ 400000

static const char *TAG = "vibe_board";
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_pmic_dev;

#if VIBE_BOARD_HAS_ES8311

#define M5PM1_ADDR 0x6e
#define M5PM1_REG_DEVICE_ID 0x00
#define M5PM1_REG_PWR_CFG 0x06
#define M5PM1_REG_HOLD_CFG 0x07
#define M5PM1_REG_I2C_CFG 0x09
#define M5PM1_REG_GPIO_MODE 0x10
#define M5PM1_REG_GPIO_OUT 0x11
#define M5PM1_REG_GPIO_IN 0x12
#define M5PM1_REG_GPIO_DRV 0x13
#define M5PM1_REG_GPIO_FUNC0 0x16
#define M5PM1_REG_BAT_L 0x22
#define M5PM1_REG_VIN_L 0x24
#define M5PM1_REG_IRQ_STATUS1 0x40
#define M5PM1_REG_IRQ_STATUS2 0x41
#define M5PM1_REG_IRQ_STATUS3 0x42
#define M5PM1_REG_IRQ_MASK1 0x43
#define M5PM1_REG_IRQ_MASK2 0x44
#define M5PM1_REG_IRQ_MASK3 0x45
#define M5PM1_REG_AW8737A_PULSE 0x53

#define M5PM1_PWR_CFG_LDO_EN BIT(2)
#define M5PM1_PWR_CFG_LED_CTRL BIT(4)
#define M5PM1_HOLD_CFG_LDO_HOLD BIT(5)
#define M5PM1_GPIO2_L3B_POWER_EN BIT(2)
#define M5PM1_GPIO3_SPK_PULSE BIT(3)
#define M5PM1_GPIO_FUNC_MASK(pin) (0x03 << ((pin) * 2))
#define M5PM1_GPIO_FUNC_GPIO(pin) (0x00 << ((pin) * 2))
#define M5PM1_GPIO_FUNC_IRQ(pin)  (0x01 << ((pin) * 2))

#else

#define AXP192_ADDR 0x34
#define AXP192_REG_INPUT_STATUS 0x00
#define AXP192_REG_POWER_STATUS 0x01
#define AXP192_REG_OUTPUT_CTRL 0x12
#define AXP192_REG_VBUS_IPSOUT_PATH 0x30
#define AXP192_REG_CHARGE_CTRL1 0x33
#define AXP192_REG_BACKUP_CHARGE_CTRL 0x35
#define AXP192_REG_PEK 0x36
#define AXP192_REG_TEMP_PROTECT 0x39
#define AXP192_REG_ADC_ENABLE1 0x82
#define AXP192_REG_GPIO0_CTRL 0x90
#define AXP192_REG_GPIO0_LDO_VOLT 0x91
#define AXP192_REG_LDO23_VOLT 0x28
#define AXP192_REG_VBUS_VOLTAGE 0x5a
#define AXP192_REG_BAT_VOLTAGE 0x78

#endif

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_pmic_dev, &reg, 1, value, 1, 100);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_pmic_dev, &reg, 1, data, len, 100);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_pmic_dev, data, sizeof(data), 100);
}

static esp_err_t update_reg(uint8_t reg, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(read_reg(reg, &value), TAG, "read reg");
    value &= ~clear_mask;
    value |= set_mask;
    return write_reg(reg, value);
}

static esp_err_t init_i2c_on(i2c_port_t port, gpio_num_t sda, gpio_num_t scl, uint8_t address)
{
    if (s_i2c_bus) {
        i2c_del_master_bus(s_i2c_bus);
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
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_i2c_bus), TAG, "i2c bus");

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_pmic_dev);
}

static int voltage_to_percent(int voltage_mv)
{
    int level = (voltage_mv - 3300) * 100 / (4150 - 3350);
    if (level < 0) {
        return 0;
    }
    if (level > 100) {
        return 100;
    }
    return level;
}

#if VIBE_BOARD_HAS_ES8311

static esp_err_t init_i2c(void)
{
    const struct {
        i2c_port_t port;
        gpio_num_t sda;
        gpio_num_t scl;
    } candidates[] = {
        {VIBE_BOARD_I2C_PORT, VIBE_BOARD_PIN_I2C_SDA, VIBE_BOARD_PIN_I2C_SCL},
        {VIBE_BOARD_I2C_PORT, VIBE_BOARD_PIN_I2C_SCL, VIBE_BOARD_PIN_I2C_SDA},
        {I2C_NUM_0, VIBE_BOARD_PIN_I2C_SDA, VIBE_BOARD_PIN_I2C_SCL},
        {I2C_NUM_0, VIBE_BOARD_PIN_I2C_SCL, VIBE_BOARD_PIN_I2C_SDA},
    };
    esp_err_t last_err = ESP_FAIL;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        last_err = init_i2c_on(candidates[i].port, candidates[i].sda, candidates[i].scl, M5PM1_ADDR);
        if (last_err != ESP_OK) {
            continue;
        }
        uint8_t id = 0;
        last_err = read_reg(M5PM1_REG_DEVICE_ID, &id);
        if (last_err == ESP_OK) {
            ESP_LOGI(TAG, "M5PM1 found id=0x%02x", id);
            return ESP_OK;
        }
    }
    return last_err;
}

esp_err_t vibe_board_init_power(void)
{
    if (s_pmic_dev) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "init i2c");

    uint8_t pwr_cfg = 0;
    uint8_t hold_cfg = 0;
    ESP_RETURN_ON_ERROR(read_reg(M5PM1_REG_PWR_CFG, &pwr_cfg), TAG, "read pwr");
    ESP_RETURN_ON_ERROR(read_reg(M5PM1_REG_HOLD_CFG, &hold_cfg), TAG, "read hold");

    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(M5PM1_REG_PWR_CFG,
                                            (pwr_cfg | M5PM1_PWR_CFG_LDO_EN) &
                                                ~M5PM1_PWR_CFG_LED_CTRL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(M5PM1_REG_HOLD_CFG,
                                            hold_cfg | M5PM1_HOLD_CFG_LDO_HOLD));
    ESP_ERROR_CHECK_WITHOUT_ABORT(update_reg(M5PM1_REG_GPIO_FUNC0,
                                             M5PM1_GPIO2_L3B_POWER_EN, 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(update_reg(M5PM1_REG_GPIO_MODE,
                                             0, M5PM1_GPIO2_L3B_POWER_EN));
    ESP_ERROR_CHECK_WITHOUT_ABORT(update_reg(M5PM1_REG_GPIO_DRV,
                                             M5PM1_GPIO2_L3B_POWER_EN, 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(update_reg(M5PM1_REG_GPIO_OUT,
                                             0, M5PM1_GPIO2_L3B_POWER_EN));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(M5PM1_REG_I2C_CFG, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(update_reg(M5PM1_REG_GPIO_FUNC0,
                                             M5PM1_GPIO_FUNC_MASK(0),
                                             M5PM1_GPIO_FUNC_GPIO(0)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(update_reg(M5PM1_REG_GPIO_MODE, BIT(0), 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(M5PM1_REG_IRQ_MASK1, 0x1F));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(M5PM1_REG_IRQ_MASK3, 0x07));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(M5PM1_REG_IRQ_STATUS1, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(M5PM1_REG_IRQ_STATUS2, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(M5PM1_REG_IRQ_STATUS3, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(M5PM1_REG_IRQ_MASK2, 0x3F));
    ESP_ERROR_CHECK_WITHOUT_ABORT(update_reg(M5PM1_REG_GPIO_FUNC0,
                                             M5PM1_GPIO_FUNC_MASK(1),
                                             M5PM1_GPIO_FUNC_IRQ(1)));

    ESP_LOGI(TAG, "power hold enabled");
    return ESP_OK;
}

esp_err_t vibe_board_battery_level(int *level_percent)
{
    ESP_RETURN_ON_FALSE(level_percent != NULL, ESP_ERR_INVALID_ARG, TAG, "null level");
    ESP_RETURN_ON_FALSE(s_pmic_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "pmic missing");

    uint8_t data[2] = {0};
    ESP_RETURN_ON_ERROR(read_regs(M5PM1_REG_BAT_L, data, sizeof(data)), TAG, "read bat");
    *level_percent = voltage_to_percent((data[1] << 8) | data[0]);
    return ESP_OK;
}

esp_err_t vibe_board_battery_charging(bool *charging)
{
    ESP_RETURN_ON_FALSE(charging != NULL, ESP_ERR_INVALID_ARG, TAG, "null charging");
    ESP_RETURN_ON_FALSE(s_pmic_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "pmic missing");

    uint8_t gpio_in = 0;
    ESP_RETURN_ON_ERROR(read_reg(M5PM1_REG_GPIO_IN, &gpio_in), TAG, "read gpio in");
    *charging = (gpio_in & BIT(0)) == 0;
    return ESP_OK;
}

esp_err_t vibe_board_usb_powered(bool *usb_powered)
{
    ESP_RETURN_ON_FALSE(usb_powered != NULL, ESP_ERR_INVALID_ARG, TAG, "null usb powered");
    ESP_RETURN_ON_FALSE(s_pmic_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "pmic missing");

    uint8_t data[2] = {0};
    ESP_RETURN_ON_ERROR(read_regs(M5PM1_REG_VIN_L, data, sizeof(data)), TAG, "read vin");
    *usb_powered = (((int)data[1] << 8) | data[0]) > 4500;
    return ESP_OK;
}

esp_err_t vibe_board_speaker_set_enabled(bool enabled)
{
    ESP_RETURN_ON_FALSE(s_pmic_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "pmic missing");

    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_GPIO_FUNC0,
                                   M5PM1_GPIO_FUNC_MASK(3),
                                   M5PM1_GPIO_FUNC_GPIO(3)),
                        TAG, "speaker gpio func");
    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_GPIO_MODE, 0, M5PM1_GPIO3_SPK_PULSE),
                        TAG, "speaker gpio mode");
    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_GPIO_DRV, M5PM1_GPIO3_SPK_PULSE, 0),
                        TAG, "speaker gpio drive");

    uint8_t pulses = enabled ? 2 : 0;
    uint8_t pulse_reg = 0x80 | (uint8_t)(pulses << 5) | 3;
    ESP_RETURN_ON_ERROR(write_reg(M5PM1_REG_AW8737A_PULSE, pulse_reg),
                        TAG, "speaker aw8737a pulse");
    esp_rom_delay_us(20000);

    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_GPIO_OUT,
                                   enabled ? 0 : M5PM1_GPIO3_SPK_PULSE,
                                   enabled ? M5PM1_GPIO3_SPK_PULSE : 0),
                        TAG, "speaker gpio out");
    return ESP_OK;
}

esp_err_t vibe_board_set_lcd_brightness(uint8_t brightness)
{
    (void)brightness;
    return ESP_OK;
}

#else

static uint16_t read_12bit_adc(uint8_t reg)
{
    uint8_t data[2] = {0};
    if (read_regs(reg, data, sizeof(data)) != ESP_OK) {
        return 0;
    }
    return ((uint16_t)data[0] << 4) | (data[1] & 0x0f);
}

static esp_err_t init_i2c(void)
{
    ESP_RETURN_ON_ERROR(init_i2c_on(VIBE_BOARD_I2C_PORT,
                                    VIBE_BOARD_PIN_I2C_SDA,
                                    VIBE_BOARD_PIN_I2C_SCL,
                                    AXP192_ADDR),
                        TAG, "i2c axp192");
    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(read_reg(AXP192_REG_INPUT_STATUS, &status), TAG, "read axp192");
    ESP_LOGI(TAG, "AXP192 found input_status=0x%02x", status);
    return ESP_OK;
}

esp_err_t vibe_board_init_power(void)
{
    if (s_pmic_dev) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "init i2c");

    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(AXP192_REG_LDO23_VOLT, 0xcc));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(AXP192_REG_ADC_ENABLE1, 0xff));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(AXP192_REG_CHARGE_CTRL1, 0xc0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(update_reg(AXP192_REG_OUTPUT_CTRL, BIT(4), 0x4d));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(AXP192_REG_PEK, 0x0c));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(AXP192_REG_GPIO0_LDO_VOLT, 0xf0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(AXP192_REG_GPIO0_CTRL, 0x02));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(AXP192_REG_VBUS_IPSOUT_PATH, 0x80));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(AXP192_REG_TEMP_PROTECT, 0xfc));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(AXP192_REG_BACKUP_CHARGE_CTRL, 0xa2));
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(0x32, 0x46));
    ESP_LOGI(TAG, "AXP192 power initialized");
    return ESP_OK;
}

i2c_master_bus_handle_t vibe_board_i2c_bus(void)
{
    return s_i2c_bus;
}

esp_err_t vibe_board_battery_level(int *level_percent)
{
    ESP_RETURN_ON_FALSE(level_percent != NULL, ESP_ERR_INVALID_ARG, TAG, "null level");
    ESP_RETURN_ON_FALSE(s_pmic_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "pmic missing");

    int voltage_mv = (int)((read_12bit_adc(AXP192_REG_BAT_VOLTAGE) * 11 + 5) / 10);
    *level_percent = voltage_to_percent(voltage_mv);
    return ESP_OK;
}

esp_err_t vibe_board_battery_charging(bool *charging)
{
    ESP_RETURN_ON_FALSE(charging != NULL, ESP_ERR_INVALID_ARG, TAG, "null charging");
    ESP_RETURN_ON_FALSE(s_pmic_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "pmic missing");

    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(read_reg(AXP192_REG_POWER_STATUS, &status), TAG, "read power status");
    *charging = (status & BIT(6)) != 0;
    return ESP_OK;
}

esp_err_t vibe_board_usb_powered(bool *usb_powered)
{
    ESP_RETURN_ON_FALSE(usb_powered != NULL, ESP_ERR_INVALID_ARG, TAG, "null usb powered");
    ESP_RETURN_ON_FALSE(s_pmic_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "pmic missing");

    int voltage_mv = (int)((read_12bit_adc(AXP192_REG_VBUS_VOLTAGE) * 17 + 5) / 10);
    *usb_powered = voltage_mv > 4500;
    return ESP_OK;
}

esp_err_t vibe_board_speaker_set_enabled(bool enabled)
{
    (void)enabled;
    return ESP_OK;
}

esp_err_t vibe_board_set_lcd_brightness(uint8_t brightness)
{
    ESP_RETURN_ON_FALSE(s_pmic_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "pmic missing");
    uint8_t reg = 0;
    ESP_RETURN_ON_ERROR(read_reg(AXP192_REG_LDO23_VOLT, &reg), TAG, "read ldo23");
    if (brightness == 0) {
        return write_reg(AXP192_REG_LDO23_VOLT, reg & 0x0f);
    }
    int percent = (brightness * 100) / 255;
    int voltage_mv = 2500 + ((3200 - 2500) * percent) / 100;
    int encoded = (voltage_mv - 1800) / 100;
    if (encoded < 0) {
        encoded = 0;
    } else if (encoded > 0x0f) {
        encoded = 0x0f;
    }
    return write_reg(AXP192_REG_LDO23_VOLT, (reg & 0x0f) | ((uint8_t)encoded << 4));
}

#endif

#if VIBE_BOARD_HAS_ES8311
i2c_master_bus_handle_t vibe_board_i2c_bus(void)
{
    return s_i2c_bus;
}
#endif
