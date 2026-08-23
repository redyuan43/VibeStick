#include "vibe_cardputer_opt.h"

#include "vibe_cardputer_asr_board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TCA8418_ADDRESS 0x34
#define TCA8418_REG_CFG 0x01
#define TCA8418_REG_INT_STAT 0x02
#define TCA8418_REG_KEY_LCK_EC 0x03
#define TCA8418_REG_KEY_EVENT_A 0x04
#define TCA8418_REG_GPIO_INT_STAT_1 0x11
#define TCA8418_REG_GPIO_INT_STAT_2 0x12
#define TCA8418_REG_GPIO_INT_STAT_3 0x13
#define TCA8418_REG_GPIO_INT_EN_1 0x1a
#define TCA8418_REG_KP_GPIO_1 0x1d
#define TCA8418_REG_KP_GPIO_2 0x1e
#define TCA8418_REG_KP_GPIO_3 0x1f
#define TCA8418_REG_GPI_EM_1 0x20
#define TCA8418_REG_GPIO_DIR_1 0x23
#define TCA8418_REG_GPIO_INT_LVL_1 0x26
#define TCA8418_REG_DEBOUNCE_DIS_1 0x29
#define TCA8418_CFG_GPI_IEN 0x02
#define TCA8418_CFG_KE_IEN 0x01
#define TCA8418_EVENT_PRESSED 0x80
#define TCA8418_EVENT_CODE_MASK 0x7f
#define TCA8418_EVENT_FIFO_MAX 10
#define CARDPUTER_KEYBOARD_INT_GPIO GPIO_NUM_11

static i2c_master_dev_handle_t s_keyboard;
static TaskHandle_t s_task;
static vibe_cardputer_opt_callback_t s_callback;
static void *s_context;

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_keyboard, data, sizeof(data), 100);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_keyboard, &reg, 1, value, 1, 100);
}

static void dispatch_event(uint8_t raw)
{
    if ((raw & TCA8418_EVENT_PRESSED) == 0) {
        return;
    }
    uint8_t code = raw & TCA8418_EVENT_CODE_MASK;
    if (code == 0) {
        return;
    }
    code--;
    const uint8_t matrix_row = code / 10;
    const uint8_t matrix_col = code % 10;
    if (matrix_row >= 7 || matrix_col >= 8) {
        return;
    }
    const uint8_t column = matrix_row * 2 + (matrix_col > 3 ? 1 : 0);
    const uint8_t row = (matrix_col + 4) % 4;
    if (row == 3 && column == 1 && s_callback) {
        s_callback(s_context);
    }
}

static void drain_events(void)
{
    uint8_t count = 0;
    if (read_reg(TCA8418_REG_KEY_LCK_EC, &count) != ESP_OK) {
        return;
    }
    count &= 0x0f;
    if (count > TCA8418_EVENT_FIFO_MAX) {
        count = TCA8418_EVENT_FIFO_MAX;
    }
    for (uint8_t index = 0; index < count; ++index) {
        uint8_t raw = 0;
        if (read_reg(TCA8418_REG_KEY_EVENT_A, &raw) != ESP_OK) {
            break;
        }
        dispatch_event(raw);
    }
    if (count > 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(TCA8418_REG_INT_STAT, 0x01));
    }
}

static void IRAM_ATTR keyboard_isr(void *context)
{
    BaseType_t wake_task = pdFALSE;
    vTaskNotifyGiveFromISR((TaskHandle_t)context, &wake_task);
    if (wake_task == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void keyboard_task(void *arg)
{
    (void)arg;
    while (true) {
        drain_events();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    }
}

static esp_err_t configure_keyboard(void)
{
    for (uint8_t index = 0; index < 3; ++index) {
        ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_GPIO_DIR_1 + index, 0x00),
                            "card_asr_opt", "gpio direction");
        ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_GPI_EM_1 + index, 0xff),
                            "card_asr_opt", "gpio event mode");
        ESP_RETURN_ON_ERROR(
            write_reg(TCA8418_REG_GPIO_INT_LVL_1 + index, 0x00),
            "card_asr_opt", "gpio interrupt level");
        ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_GPIO_INT_EN_1 + index, 0xff),
                            "card_asr_opt", "gpio interrupt enable");
        ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_DEBOUNCE_DIS_1 + index, 0x00),
                            "card_asr_opt", "key debounce");
    }
    ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_KP_GPIO_1, 0x7f),
                        "card_asr_opt", "matrix rows");
    ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_KP_GPIO_2, 0xff),
                        "card_asr_opt", "matrix columns");
    ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_KP_GPIO_3, 0x00),
                        "card_asr_opt", "matrix columns high");

    uint8_t raw = 0;
    do {
        ESP_RETURN_ON_ERROR(read_reg(TCA8418_REG_KEY_EVENT_A, &raw),
                            "card_asr_opt", "flush key events");
    } while (raw != 0);
    uint8_t ignored = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        read_reg(TCA8418_REG_GPIO_INT_STAT_1, &ignored));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        read_reg(TCA8418_REG_GPIO_INT_STAT_2, &ignored));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        read_reg(TCA8418_REG_GPIO_INT_STAT_3, &ignored));
    ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_INT_STAT, 0x03),
                        "card_asr_opt", "clear interrupts");

    uint8_t cfg = 0;
    ESP_RETURN_ON_ERROR(read_reg(TCA8418_REG_CFG, &cfg),
                        "card_asr_opt", "read config");
    return write_reg(TCA8418_REG_CFG,
                     cfg | TCA8418_CFG_GPI_IEN | TCA8418_CFG_KE_IEN);
}

esp_err_t vibe_cardputer_opt_init(vibe_cardputer_opt_callback_t callback,
                                  void *context)
{
    ESP_RETURN_ON_FALSE(callback != NULL, ESP_ERR_INVALID_ARG,
                        "card_asr_opt", "callback");
    i2c_master_bus_handle_t bus = vibe_cardputer_asr_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE,
                        "card_asr_opt", "i2c bus");

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA8418_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &s_keyboard),
                        "card_asr_opt", "add tca8418");
    ESP_RETURN_ON_ERROR(configure_keyboard(), "card_asr_opt",
                        "configure tca8418");

    const gpio_config_t interrupt_config = {
        .pin_bit_mask = 1ULL << CARDPUTER_KEYBOARD_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&interrupt_config), "card_asr_opt",
                        "keyboard interrupt pin");

    s_callback = callback;
    s_context = context;
    BaseType_t started = xTaskCreatePinnedToCore(
        keyboard_task, "opt_input", 3072, NULL, 5, &s_task, 0);
    ESP_RETURN_ON_FALSE(started == pdPASS, ESP_ERR_NO_MEM,
                        "card_asr_opt", "keyboard task");
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        return isr_err;
    }
    return gpio_isr_handler_add(CARDPUTER_KEYBOARD_INT_GPIO, keyboard_isr,
                                s_task);
}
