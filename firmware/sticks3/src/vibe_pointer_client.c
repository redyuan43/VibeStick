#include "vibe_pointer_client.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define POINTER_QUEUE_LENGTH 32
#define POINTER_TASK_STACK_BYTES 6144
#define POINTER_TASK_PRIORITY 3
#define POINTER_NETWORK_CORE 1

typedef struct {
    int16_t dx;
    int16_t dy;
    int16_t wheel;
    uint8_t buttons;
    bool force;
} pointer_report_t;

static const char *TAG = "vibe_pointer";
static vibe_pointer_client_config_t s_config;
static QueueHandle_t s_queue;
static atomic_uchar s_buttons;

static int16_t clamp_delta(int32_t value, int16_t limit)
{
    if (value > limit) {
        return limit;
    }
    if (value < -limit) {
        return (int16_t)-limit;
    }
    return (int16_t)value;
}

static void queue_report(int16_t dx, int16_t dy, int16_t wheel,
                         uint8_t buttons, bool force)
{
    if (!s_queue || (!force && dx == 0 && dy == 0 && wheel == 0)) {
        return;
    }
    const pointer_report_t report = {
        .dx = dx,
        .dy = dy,
        .wheel = wheel,
        .buttons = buttons,
        .force = force,
    };
    if (xQueueSend(s_queue, &report, 0) != pdTRUE) {
        ESP_LOGW(TAG, "pointer report queue overflow; resyncing");
        xQueueReset(s_queue);
        (void)xQueueSend(s_queue, &report, 0);
    }
}

static void pointer_task(void *arg)
{
    (void)arg;
    char session_id[24];
    snprintf(session_id, sizeof(session_id), "%08lx-%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    uint32_t sequence = 0;
    bool suspended_until_release = false;
    uint8_t last_sent_buttons = 0;
    int64_t last_failure_log_ms = 0;

    while (true) {
        pointer_report_t report = {0};
        const BaseType_t received = xQueueReceive(
            s_queue, &report, pdMS_TO_TICKS(s_config.heartbeat_ms));
        if (received != pdTRUE) {
            report.buttons = atomic_load(&s_buttons);
            report.force = report.buttons != 0;
        } else {
            pointer_report_t next;
            while (xQueueReceive(s_queue, &next, 0) == pdTRUE) {
                report.dx = clamp_delta((int32_t)report.dx + next.dx, 2048);
                report.dy = clamp_delta((int32_t)report.dy + next.dy, 2048);
                report.wheel =
                    clamp_delta((int32_t)report.wheel + next.wheel, 32);
                report.buttons = next.buttons;
                report.force |= next.force;
            }
        }

        if (!report.force && report.dx == 0 && report.dy == 0 &&
            report.wheel == 0 && report.buttons == last_sent_buttons) {
            continue;
        }
        if (!s_config.online(s_config.context)) {
            suspended_until_release |= report.buttons != 0;
            continue;
        }
        if (suspended_until_release) {
            if (report.buttons != 0) {
                continue;
            }
            suspended_until_release = false;
            report.dx = 0;
            report.dy = 0;
            report.wheel = 0;
            report.force = true;
        }

        char body[224];
        snprintf(body, sizeof(body),
                 "{\"protocol_version\":1,\"session_id\":\"%s\","
                 "\"sequence\":%lu,\"dx\":%d,\"dy\":%d,"
                 "\"wheel\":%d,\"buttons\":%u}",
                 session_id, (unsigned long)sequence++, (int)report.dx,
                 (int)report.dy, (int)report.wheel,
                 (unsigned)report.buttons);
        char response[128] = {0};
        esp_err_t err = s_config.post(
            s_config.path, body, response, sizeof(response),
            s_config.timeout_ms, s_config.context);
        if (err != ESP_OK) {
            suspended_until_release |= report.buttons != 0;
            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - last_failure_log_ms >= 5000) {
                ESP_LOGW(TAG, "pointer report failed: %s",
                         esp_err_to_name(err));
                last_failure_log_ms = now_ms;
            }
            continue;
        }
        last_sent_buttons = report.buttons;
    }
}

esp_err_t vibe_pointer_client_init(
    const vibe_pointer_client_config_t *config)
{
    if (!config || !config->path || !config->post || !config->online ||
        config->timeout_ms <= 0 || config->heartbeat_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_queue) {
        return ESP_OK;
    }
    s_config = *config;
    s_queue = xQueueCreate(POINTER_QUEUE_LENGTH, sizeof(pointer_report_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t created = xTaskCreatePinnedToCore(
        pointer_task, "pointer_report", POINTER_TASK_STACK_BYTES, NULL,
        POINTER_TASK_PRIORITY, NULL, POINTER_NETWORK_CORE);
    if (created != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void vibe_pointer_client_report(int16_t dx, int16_t dy, int16_t wheel)
{
    queue_report(dx, dy, wheel, atomic_load(&s_buttons), false);
}

void vibe_pointer_client_set_button(uint8_t mask, bool pressed)
{
    uint8_t previous = atomic_load(&s_buttons);
    uint8_t updated;
    do {
        updated = pressed ? (uint8_t)(previous | mask)
                          : (uint8_t)(previous & (uint8_t)~mask);
        if (updated == previous) {
            return;
        }
    } while (!atomic_compare_exchange_weak(&s_buttons, &previous, updated));
    queue_report(0, 0, 0, updated, true);
}

void vibe_pointer_client_release_all(void)
{
    const uint8_t previous = atomic_exchange(&s_buttons, 0);
    queue_report(0, 0, 0, 0, previous != 0);
}

uint8_t vibe_pointer_client_buttons(void)
{
    return atomic_load(&s_buttons);
}
