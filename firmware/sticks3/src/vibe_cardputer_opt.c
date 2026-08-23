#include "vibe_cardputer_opt.h"

#include <string.h>

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
static vibe_cardputer_key_callback_t s_callback;
static void *s_context;
static bool s_pressed[4][14];
static uint8_t s_active_usage[4][14];
static bool s_fn;
static bool s_shift;
static bool s_ctrl;
static bool s_alt;

static const uint8_t s_hid_usage[4][14] = {
    {0x35, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23,
     0x24, 0x25, 0x26, 0x27, 0x2d, 0x2e, 0x2a},
    {0x2b, 0x14, 0x1a, 0x08, 0x15, 0x17, 0x1c,
     0x18, 0x0c, 0x12, 0x13, 0x2f, 0x30, 0x31},
    {0x00, 0x00, 0x04, 0x16, 0x07, 0x09, 0x0a,
     0x0b, 0x0d, 0x0e, 0x0f, 0x33, 0x34, 0x28},
    {0x00, 0x00, 0x00, 0x1d, 0x1b, 0x06, 0x19,
     0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0x2c},
};

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_keyboard, data, sizeof(data), 100);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_keyboard, &reg, 1, value, 1, 100);
}

static vibe_cardputer_key_t special_key(uint8_t row, uint8_t column)
{
    if (row == 0 && column == 13) return VIBE_CARDPUTER_KEY_BACKSPACE;
    if (row == 1 && column == 0) return VIBE_CARDPUTER_KEY_TAB;
    if (row == 2 && column == 0) return VIBE_CARDPUTER_KEY_FN;
    if (row == 2 && column == 1) return VIBE_CARDPUTER_KEY_SHIFT;
    if (row == 2 && column == 13) return VIBE_CARDPUTER_KEY_ENTER;
    if (row == 3 && column == 0) return VIBE_CARDPUTER_KEY_CTRL;
    if (row == 3 && column == 1) return VIBE_CARDPUTER_KEY_OPT;
    if (row == 3 && column == 2) return VIBE_CARDPUTER_KEY_ALT;
    return VIBE_CARDPUTER_KEY_NONE;
}

static vibe_cardputer_key_t fn_key(uint8_t row, uint8_t column)
{
    if (row == 0 && column == 0) return VIBE_CARDPUTER_KEY_ESCAPE;
    if (row == 0 && column == 13) return VIBE_CARDPUTER_KEY_DELETE;
    if (row == 2 && column == 11) return VIBE_CARDPUTER_KEY_UP;
    if (row == 3 && column == 10) return VIBE_CARDPUTER_KEY_LEFT;
    if (row == 3 && column == 11) return VIBE_CARDPUTER_KEY_DOWN;
    if (row == 3 && column == 12) return VIBE_CARDPUTER_KEY_RIGHT;
    return VIBE_CARDPUTER_KEY_NONE;
}

static uint8_t fn_usage(uint8_t row, uint8_t column)
{
    if (row == 0 && column == 0) return 0x29;
    if (row == 0 && column >= 1 && column <= 10) {
        return (uint8_t)(0x3a + column - 1);
    }
    if (row == 0 && column == 11) return 0x44;
    if (row == 0 && column == 12) return 0x45;
    if (row == 0 && column == 13) return 0x4c;
    if (row == 2 && column == 11) return 0x52;
    if (row == 3 && column == 10) return 0x50;
    if (row == 3 && column == 11) return 0x51;
    if (row == 3 && column == 12) return 0x4f;
    return 0;
}

static uint8_t modifiers(void)
{
    uint8_t value = 0;
    if (s_ctrl) value |= 0x01;
    if (s_shift) value |= 0x02;
    if (s_alt) value |= 0x04;
    return value;
}

static void refresh_modifiers(void)
{
    s_fn = s_pressed[2][0];
    s_shift = s_pressed[2][1];
    s_ctrl = s_pressed[3][0];
    s_alt = s_pressed[3][2];
}

static void dispatch_event(uint8_t raw)
{
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
    if (row >= 4 || column >= 14) {
        return;
    }

    const bool pressed = (raw & TCA8418_EVENT_PRESSED) != 0;
    s_pressed[row][column] = pressed;
    refresh_modifiers();
    vibe_cardputer_key_event_t event = {
        .pressed = pressed,
        .row = row,
        .column = column,
        .key = special_key(row, column),
        .hid_modifiers = modifiers(),
    };
    if (pressed) {
        if (event.key == VIBE_CARDPUTER_KEY_NONE && s_fn) {
            event.key = fn_key(row, column);
            event.hid_usage = fn_usage(row, column);
        } else if (event.key == VIBE_CARDPUTER_KEY_NONE) {
            event.hid_usage = s_hid_usage[row][column];
        } else if (event.key != VIBE_CARDPUTER_KEY_FN &&
                   event.key != VIBE_CARDPUTER_KEY_SHIFT &&
                   event.key != VIBE_CARDPUTER_KEY_CTRL &&
                   event.key != VIBE_CARDPUTER_KEY_ALT &&
                   event.key != VIBE_CARDPUTER_KEY_OPT) {
            event.hid_usage = s_fn ? fn_usage(row, column)
                                    : s_hid_usage[row][column];
        }
        s_active_usage[row][column] = event.hid_usage;
    } else {
        event.hid_usage = s_active_usage[row][column];
        s_active_usage[row][column] = 0;
    }
    if (s_callback) {
        s_callback(&event, s_context);
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

esp_err_t vibe_cardputer_opt_init(vibe_cardputer_key_callback_t callback,
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
    memset(s_pressed, 0, sizeof(s_pressed));
    memset(s_active_usage, 0, sizeof(s_active_usage));
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
