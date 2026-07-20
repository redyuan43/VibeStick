#include "vibe_input.h"

#include "button_gpio.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "iot_button.h"
#include "vibe_board_profile.h"

static const char *TAG = "vibe_input";
static vibe_input_callbacks_t s_callbacks;

#define DEFINE_INPUT_ADAPTER(name, field) \
    static void name(void *button_handle, void *unused) \
    { \
        (void)unused; \
        if (s_callbacks.field) { \
            s_callbacks.field(button_handle, s_callbacks.context); \
        } \
    }

DEFINE_INPUT_ADAPTER(front_down_adapter, front_down)
DEFINE_INPUT_ADAPTER(front_single_adapter, front_single)
DEFINE_INPUT_ADAPTER(front_double_adapter, front_double)
DEFINE_INPUT_ADAPTER(front_long_adapter, front_long)
DEFINE_INPUT_ADAPTER(front_confirm_adapter, front_confirm)
DEFINE_INPUT_ADAPTER(front_up_adapter, front_up)
DEFINE_INPUT_ADAPTER(side_up_adapter, side_up)
DEFINE_INPUT_ADAPTER(side_mode_adapter, side_mode_hold)
DEFINE_INPUT_ADAPTER(side_calibration_adapter, side_calibration_hold)

bool vibe_input_front_pressed(void)
{
    return gpio_get_level(VIBE_BOARD_PIN_BUTTON_FRONT) == 0;
}

esp_err_t vibe_input_init(const vibe_input_config_t *config,
                          const vibe_input_callbacks_t *callbacks)
{
    ESP_RETURN_ON_FALSE(config && callbacks, ESP_ERR_INVALID_ARG, TAG,
                        "input config");
    s_callbacks = *callbacks;

    button_handle_t front = NULL;
    button_handle_t side = NULL;
    const button_config_t button_config = {0};
    const button_gpio_config_t front_gpio = {
        .gpio_num = VIBE_BOARD_PIN_BUTTON_FRONT,
        .active_level = 0,
        .enable_power_save = true,
        .disable_pull = VIBE_BOARD_BUTTONS_DISABLE_INTERNAL_PULL,
    };
    ESP_RETURN_ON_ERROR(
        iot_button_new_gpio_device(&button_config, &front_gpio, &front),
        TAG, "front button");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(
                            front, BUTTON_PRESS_DOWN, NULL,
                            front_down_adapter, NULL),
                        TAG, "front down");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(
                            front, BUTTON_SINGLE_CLICK, NULL,
                            front_single_adapter, NULL),
                        TAG, "front single");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(
                            front, BUTTON_DOUBLE_CLICK, NULL,
                            front_double_adapter, NULL),
                        TAG, "front double");
    button_event_args_t front_long = {
        .long_press = {.press_time = config->front_long_ms},
    };
    ESP_RETURN_ON_ERROR(iot_button_register_cb(
                            front, BUTTON_LONG_PRESS_START, &front_long,
                            front_long_adapter, NULL),
                        TAG, "front long");
    button_event_args_t front_confirm = {
        .long_press = {.press_time = config->front_confirm_ms},
    };
    ESP_RETURN_ON_ERROR(iot_button_register_cb(
                            front, BUTTON_LONG_PRESS_START, &front_confirm,
                            front_confirm_adapter, NULL),
                        TAG, "front confirm");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(
                            front, BUTTON_PRESS_UP, NULL,
                            front_up_adapter, NULL),
                        TAG, "front up");

    const button_gpio_config_t side_gpio = {
        .gpio_num = VIBE_BOARD_PIN_BUTTON_SIDE,
        .active_level = 0,
        .enable_power_save = true,
        .disable_pull = VIBE_BOARD_BUTTONS_DISABLE_INTERNAL_PULL,
    };
    ESP_RETURN_ON_ERROR(
        iot_button_new_gpio_device(&button_config, &side_gpio, &side),
        TAG, "side button");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(
                            side, BUTTON_PRESS_UP, NULL,
                            side_up_adapter, NULL),
                        TAG, "side up");
    button_event_args_t side_mode = {
        .long_press = {.press_time = config->side_mode_ms},
    };
    ESP_RETURN_ON_ERROR(iot_button_register_cb(
                            side, BUTTON_LONG_PRESS_START, &side_mode,
                            side_mode_adapter, NULL),
                        TAG, "side mode");
    button_event_args_t side_calibration = {
        .long_press = {.press_time = config->side_calibration_ms},
    };
    ESP_RETURN_ON_ERROR(iot_button_register_cb(
                            side, BUTTON_LONG_PRESS_START, &side_calibration,
                            side_calibration_adapter, NULL),
                        TAG, "side calibration");
    return ESP_OK;
}
