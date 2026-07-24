#include "vibe_minijoyc.h"

#if defined(VIBE_BOARD_STICKC_PLUS) || defined(VIBE_BOARD_STICKC_PLUS_SE)
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define MINIJOYC_ADDR 0x54
#define MINIJOYC_POS_X_10BIT_REG 0x10
#define MINIJOYC_POS_Y_10BIT_REG 0x12
#define MINIJOYC_BUTTON_REG 0x30
#define MINIJOYC_LED_REG 0x40

static const char *TAG = "minijoyc";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static esp_err_t prepare_i2c_pins(void)
{
    /* Recover a slave left mid-transaction, then leave the bus idle. */
    ESP_RETURN_ON_ERROR(gpio_reset_pin(GPIO_NUM_0), TAG,
                        "reset SDA");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(GPIO_NUM_0, GPIO_PULLUP_ONLY),
                        TAG, "pull up SDA");
    ESP_RETURN_ON_ERROR(gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT),
                        TAG, "release SDA");
    ESP_RETURN_ON_ERROR(gpio_reset_pin(GPIO_NUM_26), TAG,
                        "reset SCL");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(GPIO_NUM_26, GPIO_PULLUP_ONLY),
                        TAG, "pull up SCL");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_26, 1), TAG,
                        "preset SCL high");
    ESP_RETURN_ON_ERROR(gpio_set_direction(GPIO_NUM_26, GPIO_MODE_OUTPUT_OD),
                        TAG, "drive recovery SCL");

    for (int pulse = 0; pulse < 9 && gpio_get_level(GPIO_NUM_0) == 0;
         ++pulse) {
        ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_26, 0), TAG,
                            "recovery SCL low");
        esp_rom_delay_us(10);
        ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_26, 1), TAG,
                            "recovery SCL high");
        esp_rom_delay_us(10);
    }

    /* Generate STOP: SDA rises while SCL is high. */
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_26, 0), TAG,
                        "STOP SCL low");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_0, 0), TAG,
                        "STOP SDA low");
    ESP_RETURN_ON_ERROR(gpio_set_direction(GPIO_NUM_0, GPIO_MODE_OUTPUT_OD),
                        TAG, "drive STOP SDA");
    esp_rom_delay_us(10);
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_26, 1), TAG,
                        "STOP SCL high");
    esp_rom_delay_us(10);
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_0, 1), TAG,
                        "STOP SDA high");
    esp_rom_delay_us(10);

    ESP_RETURN_ON_ERROR(gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT), TAG,
                        "release SDA after STOP");
    ESP_RETURN_ON_ERROR(gpio_reset_pin(GPIO_NUM_26), TAG,
                        "reset SCL after STOP");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(GPIO_NUM_26, GPIO_PULLUP_ONLY), TAG,
                        "pull up SCL after STOP");
    ESP_RETURN_ON_ERROR(gpio_set_direction(GPIO_NUM_26, GPIO_MODE_INPUT), TAG,
                        "release SCL after STOP");
    esp_rom_delay_us(20);
    int sda = gpio_get_level(GPIO_NUM_0);
    int scl = gpio_get_level(GPIO_NUM_26);
    if (!sda || !scl) {
        ESP_LOGE(TAG, "I2C bus stuck after recovery SDA=%d SCL=%d", sda, scl);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t vibe_minijoyc_open(void)
{
    if (s_dev) return ESP_OK;
    ESP_RETURN_ON_ERROR(prepare_i2c_pins(), TAG,
                        "prepare I2C pins");
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = GPIO_NUM_0,
        .scl_io_num = GPIO_NUM_26,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus, &s_bus);
    if (err != ESP_OK) return err;
    err = i2c_master_probe(s_bus, MINIJOYC_ADDR, 50);
    if (err != ESP_OK) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return err;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MINIJOYC_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
    if (err != ESP_OK) vibe_minijoyc_close();
    return err;
}

void vibe_minijoyc_close(void)
{
    if (s_dev) i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    if (s_bus) i2c_del_master_bus(s_bus);
    s_bus = NULL;
}

esp_err_t vibe_minijoyc_suspend_for_microphone(void)
{
    vibe_minijoyc_close();

    /*
     * MiniJoy SDA shares GPIO0 with the built-in PDM microphone clock. Keep
     * SCL low while PDM owns GPIO0 so the joystick cannot interpret that
     * clock as I2C START/STOP traffic.
     */
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_26, 0), TAG,
                        "preset SCL low");
    ESP_RETURN_ON_ERROR(gpio_set_direction(GPIO_NUM_26, GPIO_MODE_OUTPUT_OD),
                        TAG, "hold SCL low");
    return gpio_set_level(GPIO_NUM_26, 0);
}

bool vibe_minijoyc_available(void) { return s_dev != NULL; }

esp_err_t vibe_minijoyc_read(vibe_minijoyc_state_t *state)
{
    if (!state || !s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t x_reg = MINIJOYC_POS_X_10BIT_REG;
    uint8_t x_value[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_dev, &x_reg, 1, x_value,
                                                sizeof(x_value), 30);
    if (err != ESP_OK) return err;
    uint8_t y_reg = MINIJOYC_POS_Y_10BIT_REG;
    uint8_t y_value[2] = {0};
    err = i2c_master_transmit_receive(s_dev, &y_reg, 1, y_value,
                                      sizeof(y_value), 30);
    if (err != ESP_OK) return err;
    uint8_t button_reg = MINIJOYC_BUTTON_REG;
    uint8_t button = 1;
    err = i2c_master_transmit_receive(s_dev, &button_reg, 1, &button, 1, 30);
    if (err != ESP_OK) return err;
    state->x = (int16_t)((uint16_t)x_value[0] | ((uint16_t)x_value[1] << 8));
    state->y = (int16_t)((uint16_t)y_value[0] | ((uint16_t)y_value[1] << 8));
    state->button_pressed = button == 0;
    return ESP_OK;
}

esp_err_t vibe_minijoyc_set_led(uint32_t rgb)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t data[] = {MINIJOYC_LED_REG, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb};
    return i2c_master_transmit(s_dev, data, sizeof(data), 30);
}
#else
esp_err_t vibe_minijoyc_open(void) { return ESP_ERR_NOT_SUPPORTED; }
void vibe_minijoyc_close(void) {}
esp_err_t vibe_minijoyc_suspend_for_microphone(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
bool vibe_minijoyc_available(void) { return false; }
esp_err_t vibe_minijoyc_read(vibe_minijoyc_state_t *state)
{
    (void)state;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t vibe_minijoyc_set_led(uint32_t rgb)
{
    (void)rgb;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
