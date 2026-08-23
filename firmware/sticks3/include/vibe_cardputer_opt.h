#pragma once

#include "esp_err.h"

typedef void (*vibe_cardputer_opt_callback_t)(void *context);

esp_err_t vibe_cardputer_opt_init(vibe_cardputer_opt_callback_t callback,
                                  void *context);
