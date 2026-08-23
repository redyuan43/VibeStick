#include "vibe_cardputer_keyboard_bridge.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define KEYBOARD_REPORT_QUEUE_LENGTH 8
#define KEYBOARD_REPORT_INTERVAL_MS 350

typedef struct {
    uint8_t modifiers;
    uint8_t keys[6];
    uint8_t key_count;
} keyboard_report_t;

static vibe_cardputer_keyboard_bridge_config_t s_config;
static QueueHandle_t s_reports;
static uint8_t s_usages[4][14];
static uint8_t s_modifiers;
static atomic_bool s_suspended;
static bool s_initialized;

static bool has_keys(const keyboard_report_t *report)
{
    return report && (report->modifiers != 0 || report->key_count != 0);
}

static void queue_current_report(void)
{
    if (!s_reports || atomic_load(&s_suspended)) {
        return;
    }
    keyboard_report_t report = {
        .modifiers = s_modifiers,
    };
    for (size_t row = 0; row < 4 && report.key_count < sizeof(report.keys);
         ++row) {
        for (size_t column = 0;
             column < 14 && report.key_count < sizeof(report.keys);
             ++column) {
            if (s_usages[row][column] != 0) {
                report.keys[report.key_count++] = s_usages[row][column];
            }
        }
    }
    if (xQueueSend(s_reports, &report, 0) != pdTRUE) {
        xQueueReset(s_reports);
        (void)xQueueSend(s_reports, &report, 0);
    }
}

static esp_err_t post_report(const keyboard_report_t *report,
                             const char *session_id, uint32_t sequence)
{
    char keys[48] = {0};
    size_t used = 0;
    for (uint8_t index = 0; index < report->key_count; ++index) {
        int written = snprintf(keys + used, sizeof(keys) - used, "%s%u",
                               index == 0 ? "" : ",",
                               (unsigned)report->keys[index]);
        if (written < 0 || (size_t)written >= sizeof(keys) - used) {
            return ESP_ERR_INVALID_SIZE;
        }
        used += (size_t)written;
    }
    char body[224];
    snprintf(body, sizeof(body),
             "{\"protocol_version\":1,\"session_id\":\"%s\","
             "\"sequence\":%lu,\"modifiers\":%u,\"keys\":[%s]}",
             session_id, (unsigned long)sequence,
             (unsigned)report->modifiers, keys);
    return s_config.post(body, s_config.context);
}

static void keyboard_report_task(void *arg)
{
    (void)arg;
    char session_id[24];
    snprintf(session_id, sizeof(session_id), "%08lx-%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    uint32_t sequence = 0;
    keyboard_report_t current = {0};
    bool send_current = false;
    bool hold_until_release = false;

    while (true) {
        keyboard_report_t next = {0};
        BaseType_t received = xQueueReceive(
            s_reports, &next, pdMS_TO_TICKS(KEYBOARD_REPORT_INTERVAL_MS));
        if (atomic_load(&s_suspended)) {
            current = (keyboard_report_t){0};
            send_current = false;
            hold_until_release = false;
            continue;
        }
        if (received == pdTRUE) {
            current = next;
            send_current = true;
        } else if (!has_keys(&current)) {
            continue;
        } else {
            send_current = true;
        }
        if (!send_current || !s_config.online(s_config.context)) {
            continue;
        }
        if (hold_until_release && has_keys(&current)) {
            continue;
        }
        if (hold_until_release) {
            hold_until_release = false;
        }
        if (post_report(&current, session_id, sequence++) != ESP_OK &&
            has_keys(&current)) {
            hold_until_release = true;
        }
        send_current = false;
    }
}

esp_err_t vibe_cardputer_keyboard_bridge_init(
    const vibe_cardputer_keyboard_bridge_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && config->post && config->online,
                        ESP_ERR_INVALID_ARG, "card_keyboard",
                        "keyboard bridge config");
    if (s_initialized) {
        return ESP_OK;
    }
    s_config = *config;
    s_reports = xQueueCreate(KEYBOARD_REPORT_QUEUE_LENGTH,
                             sizeof(keyboard_report_t));
    ESP_RETURN_ON_FALSE(s_reports != NULL, ESP_ERR_NO_MEM, "card_keyboard",
                        "keyboard queue");
    atomic_init(&s_suspended, false);
    BaseType_t started = xTaskCreatePinnedToCore(
        keyboard_report_task, "key_report", 4096, NULL, 2, NULL, 0);
    ESP_RETURN_ON_FALSE(started == pdPASS, ESP_ERR_NO_MEM, "card_keyboard",
                        "keyboard task");
    s_initialized = true;
    return ESP_OK;
}

void vibe_cardputer_keyboard_bridge_handle(
    const vibe_cardputer_key_event_t *event)
{
    if (!s_initialized || !event || event->row >= 4 || event->column >= 14 ||
        atomic_load(&s_suspended) || event->key == VIBE_CARDPUTER_KEY_OPT ||
        event->key == VIBE_CARDPUTER_KEY_FN) {
        return;
    }
    s_modifiers = event->hid_modifiers;
    if (event->hid_usage != 0) {
        s_usages[event->row][event->column] =
            event->pressed ? event->hid_usage : 0;
    }
    queue_current_report();
}

void vibe_cardputer_keyboard_bridge_suspend(void)
{
    if (!s_initialized) {
        return;
    }
    atomic_store(&s_suspended, true);
    memset(s_usages, 0, sizeof(s_usages));
    s_modifiers = 0;
    xQueueReset(s_reports);
}

void vibe_cardputer_keyboard_bridge_resume(void)
{
    if (s_initialized) {
        atomic_store(&s_suspended, false);
    }
}
