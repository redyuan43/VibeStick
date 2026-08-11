#include "vibe_keyboard.h"

#include "vibe_board.h"
#include "vibe_board_profile.h"

#if defined(VIBE_BOARD_CARDPUTER_ADV)

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
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

static const char *TAG = "vibe_keyboard";
static i2c_master_dev_handle_t s_keyboard_dev;
static vibe_keyboard_callback_t s_callback;
static void *s_callback_context;
static bool s_pressed[4][14];
static uint8_t s_active_usage[4][14];
static char s_active_character[4][14];
static bool s_fn;
static bool s_shift;
static bool s_ctrl;
static bool s_alt;
static bool s_opt;

static const char s_primary[4][14] = {
    {'`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0},
    {0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\\'},
    {0, 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', 0},
    {0, 0, 0, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', ' '},
};

static const char s_shifted[4][14] = {
    {'~', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0},
    {0, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '|'},
    {0, 0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', 0},
    {0, 0, 0, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', ' '},
};

// USB HID Keyboard/Keypad usage IDs from the official M5Cardputer key map.
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
    return i2c_master_transmit(s_keyboard_dev, data, sizeof(data), 100);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_keyboard_dev, &reg, 1, value, 1, 100);
}

static vibe_key_code_t base_special_key(uint8_t row, uint8_t col)
{
    if (row == 0 && col == 13) return VIBE_KEY_BACKSPACE;
    if (row == 1 && col == 0) return VIBE_KEY_TAB;
    if (row == 2 && col == 0) return VIBE_KEY_FN;
    if (row == 2 && col == 1) return VIBE_KEY_SHIFT;
    if (row == 2 && col == 13) return VIBE_KEY_ENTER;
    if (row == 3 && col == 0) return VIBE_KEY_CTRL;
    if (row == 3 && col == 1) return VIBE_KEY_OPT;
    if (row == 3 && col == 2) return VIBE_KEY_ALT;
    return VIBE_KEY_NONE;
}

static vibe_key_code_t fn_special_key(uint8_t row, uint8_t col)
{
    if (row == 0 && col == 0) return VIBE_KEY_ESCAPE;
    if (row == 0 && col == 13) return VIBE_KEY_DELETE;
    if (row == 2 && col == 11) return VIBE_KEY_UP;
    if (row == 3 && col == 10) return VIBE_KEY_LEFT;
    if (row == 3 && col == 11) return VIBE_KEY_DOWN;
    if (row == 3 && col == 12) return VIBE_KEY_RIGHT;
    return VIBE_KEY_NONE;
}

static uint8_t fn_hid_usage(uint8_t row, uint8_t col)
{
    if (row == 0 && col == 0) return 0x29; // Escape
    if (row == 0 && col >= 1 && col <= 10) {
        return (uint8_t)(0x3a + col - 1); // F1-F10
    }
    if (row == 0 && col == 11) return 0x44; // F11
    if (row == 0 && col == 12) return 0x45; // F12
    if (row == 0 && col == 13) return 0x4c; // Delete
    if (row == 2 && col == 11) return 0x52; // Up
    if (row == 3 && col == 10) return 0x50; // Left
    if (row == 3 && col == 11) return 0x51; // Down
    if (row == 3 && col == 12) return 0x4f; // Right
    return 0;
}

static uint8_t hid_modifiers(void)
{
    uint8_t modifiers = 0;
    if (s_ctrl) modifiers |= 0x01;  // Left Control
    if (s_shift) modifiers |= 0x02; // Left Shift / Aa
    if (s_alt) modifiers |= 0x04;   // Left Alt
    if (s_opt) modifiers |= 0x40;   // Right Alt / Option
    return modifiers;
}

static void refresh_modifiers(void)
{
    s_fn = s_pressed[2][0];
    s_shift = s_pressed[2][1];
    s_ctrl = s_pressed[3][0];
    s_opt = s_pressed[3][1];
    s_alt = s_pressed[3][2];
}

static void dispatch_raw_event(uint8_t raw)
{
    uint8_t code = raw & TCA8418_EVENT_CODE_MASK;
    if (code == 0) {
        return;
    }
    code--;
    const uint8_t matrix_row = code / 10;
    const uint8_t matrix_col = code % 10;
    if (matrix_row >= 7 || matrix_col >= 8) {
        ESP_LOGW(TAG, "ignored matrix event raw=0x%02x", raw);
        return;
    }

    const uint8_t col = matrix_row * 2 + (matrix_col > 3 ? 1 : 0);
    const uint8_t row = (matrix_col + 4) % 4;
    if (row >= 4 || col >= 14) {
        return;
    }

    const bool pressed = (raw & TCA8418_EVENT_PRESSED) != 0;
    s_pressed[row][col] = pressed;
    refresh_modifiers();

    vibe_key_event_t event = {
        .pressed = pressed,
        .row = row,
        .column = col,
        .character = 0,
        .key = base_special_key(row, col),
        .hid_usage = 0,
        .hid_modifiers = hid_modifiers(),
        .fn = s_fn,
        .shift = s_shift,
        .ctrl = s_ctrl,
        .alt = s_alt,
        .opt = s_opt,
    };
    if (pressed) {
        if (event.key == VIBE_KEY_NONE && s_fn) {
            event.key = fn_special_key(row, col);
            event.hid_usage = fn_hid_usage(row, col);
        } else if (event.key == VIBE_KEY_NONE) {
            event.character = s_shift ? s_shifted[row][col] : s_primary[row][col];
            event.hid_usage = s_hid_usage[row][col];
        } else if (event.key != VIBE_KEY_FN &&
                   event.key != VIBE_KEY_SHIFT &&
                   event.key != VIBE_KEY_CTRL &&
                   event.key != VIBE_KEY_ALT &&
                   event.key != VIBE_KEY_OPT) {
            event.hid_usage = s_fn ? fn_hid_usage(row, col)
                                   : s_hid_usage[row][col];
        }
        s_active_usage[row][col] = event.hid_usage;
        s_active_character[row][col] = event.character;
    } else {
        event.hid_usage = s_active_usage[row][col];
        event.character = s_active_character[row][col];
        s_active_usage[row][col] = 0;
        s_active_character[row][col] = 0;
    }
    if (s_callback) {
        s_callback(&event, s_callback_context);
    }
}

static void keyboard_task(void *arg)
{
    (void)arg;
    while (true) {
        uint8_t count = 0;
        if (read_reg(TCA8418_REG_KEY_LCK_EC, &count) == ESP_OK) {
            count &= 0x0f;
            if (count > TCA8418_EVENT_FIFO_MAX) {
                count = TCA8418_EVENT_FIFO_MAX;
            }
            for (uint8_t i = 0; i < count; ++i) {
                uint8_t raw = 0;
                if (read_reg(TCA8418_REG_KEY_EVENT_A, &raw) != ESP_OK) {
                    break;
                }
                dispatch_raw_event(raw);
            }
            if (count > 0) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(TCA8418_REG_INT_STAT, 0x01));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t configure_keyboard(void)
{
    for (uint8_t i = 0; i < 3; ++i) {
        ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_GPIO_DIR_1 + i, 0x00),
                            TAG, "gpio direction");
        ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_GPI_EM_1 + i, 0xff),
                            TAG, "gpio event mode");
        ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_GPIO_INT_LVL_1 + i, 0x00),
                            TAG, "gpio interrupt level");
        ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_GPIO_INT_EN_1 + i, 0xff),
                            TAG, "gpio interrupt enable");
        ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_DEBOUNCE_DIS_1 + i, 0x00),
                            TAG, "key debounce");
    }
    ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_KP_GPIO_1, 0x7f), TAG, "matrix rows");
    ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_KP_GPIO_2, 0xff), TAG, "matrix columns");
    ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_KP_GPIO_3, 0x00), TAG, "matrix columns high");

    uint8_t raw = 0;
    do {
        ESP_RETURN_ON_ERROR(read_reg(TCA8418_REG_KEY_EVENT_A, &raw), TAG,
                            "flush key events");
    } while (raw != 0);
    uint8_t ignored = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(read_reg(TCA8418_REG_GPIO_INT_STAT_1, &ignored));
    ESP_ERROR_CHECK_WITHOUT_ABORT(read_reg(TCA8418_REG_GPIO_INT_STAT_2, &ignored));
    ESP_ERROR_CHECK_WITHOUT_ABORT(read_reg(TCA8418_REG_GPIO_INT_STAT_3, &ignored));
    ESP_RETURN_ON_ERROR(write_reg(TCA8418_REG_INT_STAT, 0x03), TAG,
                        "clear interrupts");

    uint8_t cfg = 0;
    ESP_RETURN_ON_ERROR(read_reg(TCA8418_REG_CFG, &cfg), TAG, "read config");
    return write_reg(TCA8418_REG_CFG,
                     cfg | TCA8418_CFG_GPI_IEN | TCA8418_CFG_KE_IEN);
}

esp_err_t vibe_keyboard_init(vibe_keyboard_callback_t callback, void *context)
{
    ESP_RETURN_ON_FALSE(callback != NULL, ESP_ERR_INVALID_ARG, TAG, "callback");
    i2c_master_bus_handle_t bus = vibe_board_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "i2c bus");

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA8418_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &s_keyboard_dev),
                        TAG, "add TCA8418");
    ESP_RETURN_ON_ERROR(configure_keyboard(), TAG, "configure TCA8418");

    gpio_config_t int_config = {
        .pin_bit_mask = 1ULL << VIBE_BOARD_PIN_KEYBOARD_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_config), TAG, "keyboard interrupt pin");

    s_callback = callback;
    s_callback_context = context;
    memset(s_pressed, 0, sizeof(s_pressed));
    memset(s_active_usage, 0, sizeof(s_active_usage));
    memset(s_active_character, 0, sizeof(s_active_character));
    BaseType_t ok = xTaskCreatePinnedToCore(keyboard_task, "keyboard", 4096,
                                            NULL, 5, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "keyboard task");
    ESP_LOGI(TAG, "TCA8418 keyboard ready address=0x%02x int_gpio=%d",
             TCA8418_ADDRESS, VIBE_BOARD_PIN_KEYBOARD_INT);
    return ESP_OK;
}

#else

esp_err_t vibe_keyboard_init(vibe_keyboard_callback_t callback, void *context)
{
    (void)callback;
    (void)context;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
