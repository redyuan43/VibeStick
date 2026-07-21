#pragma once

#include "esp_err.h"

typedef enum {
    VIBE_MINIJOY_OTA_CONNECTING,
    VIBE_MINIJOY_OTA_CHECKING,
    VIBE_MINIJOY_OTA_DOWNLOADING,
    VIBE_MINIJOY_OTA_CURRENT,
    VIBE_MINIJOY_OTA_FAILED,
} vibe_minijoy_ota_status_t;

typedef void (*vibe_minijoy_ota_status_fn)(vibe_minijoy_ota_status_t status,
                                           void *context);

/* Runs one maintenance-mode update check. A successful update restarts. */
esp_err_t vibe_minijoy_ota_run(vibe_minijoy_ota_status_fn status_callback,
                               void *context);
