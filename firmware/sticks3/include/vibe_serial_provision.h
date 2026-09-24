#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "vibe_bridge_registry.h"
#include "vibe_wifi_runtime.h"

// Out-of-band provisioning over the USB Serial/JTAG CDC port.
//
// Line protocol (all responses are single lines terminated by '\n'; ESP_LOG
// output may interleave, so hosts must match line prefixes exactly):
//
//   VSPROV {"ssid":"...","password":"...","apply":true,
//           "bridge":{"id":"...","label":"...","host":"...","port":8765,
//                     "token":"..."}}
//     -> stores the Wi-Fi profile in NVS (vibe_wifi namespace), optionally
//        upserts a bridge profile for that SSID, and when "apply" is true
//        switches the radio to the new profile and waits (up to ~12 s) for
//        the association to complete.
//   <- VSPROV_OK {"connected":true,"ip":"...","ssid":"...","profiles":N}
//   <- VSPROV_ERR {"error":"..."}
//
//   VSGET
//     -> VSOK {"connected":false,"ssid":"","ip":"","bridge_host":"","bridge_port":0}

#define VIBE_SERIAL_PROVISION_DEFAULT_STACK_BYTES 6144
#define VIBE_SERIAL_PROVISION_DEFAULT_PRIORITY 2
#define VIBE_SERIAL_PROVISION_CONNECT_WAIT_MS 12000

typedef struct {
    vibe_wifi_runtime_t *wifi;
    vibe_bridge_registry_t *registry;
    int task_stack_bytes;
    int task_priority;
} vibe_serial_provision_config_t;

esp_err_t vibe_serial_provision_start(
    const vibe_serial_provision_config_t *config);
