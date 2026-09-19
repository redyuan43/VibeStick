#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "vibe_audio.h"
#include "vibe_board.h"
#include "vibe_board_profile.h"
#include "vibe_sticks3_asr_transport.h"
#include "vibe_sticks3_status.h"
#include "vibe_wifi_runtime.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "sticks3_asr";
static vibe_wifi_runtime_t s_wifi;
static TaskHandle_t s_upload_task;
static SemaphoreHandle_t s_upload_done;
static atomic_bool s_recording, s_failed;
static char s_session[33];
static uint8_t s_upload_buffer[VIBE_STICK_AUDIO_ADPCM_FRAME_BYTES * 4];
static size_t s_posts, s_pcm_bytes;

static void upload_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (vibe_audio_is_recording() || vibe_audio_pending_chunks()) {
            size_t n = 0;
            esp_err_t err = vibe_audio_read_batch(s_upload_buffer, sizeof(s_upload_buffer), &n, 4, 250);
            if (err == ESP_ERR_TIMEOUT) continue;
            if (err != ESP_OK || sticks3_transport_audio(s_session, s_upload_buffer, n, s_posts) != ESP_OK) {
                ESP_LOGE(TAG, "upload failed chunk=%u queued_frames=%u", (unsigned)s_posts,
                         (unsigned)vibe_audio_pending_chunks());
                atomic_store(&s_failed, true);
                break;
            }
            ++s_posts;
            s_pcm_bytes += vibe_audio_pcm_bytes_for_wire(n);
        }
        xSemaphoreGive(s_upload_done);
    }
}

static void stop_recording(void)
{
    if (!atomic_load(&s_recording)) return;
    vibe_sticks3_status_set_recording_animation(false);
    vibe_sticks3_status_show(VIBE_STICKS3_STATUS_SENDING);
    vibe_audio_stop();
    /* Keep the session and transport alive until every queued frame settles. */
    xSemaphoreTake(s_upload_done, portMAX_DELAY);
    vibe_audio_stats_t stats = {0};
    vibe_audio_stats(&stats);
    bool failed = atomic_load(&s_failed) || stats.chunks_dropped > 0;
    esp_err_t err = sticks3_transport_stop(s_session, s_posts, s_pcm_bytes, failed);
    ESP_LOGI(TAG, "summary session=%s posts=%u pcm_bytes=%u dropped_frames=%u peak_pending=%u retries=%u failed=%d stop=%s",
             s_session, (unsigned)s_posts, (unsigned)s_pcm_bytes, (unsigned)stats.chunks_dropped,
             (unsigned)stats.peak_pending_chunks, sticks3_transport_retries(), failed, esp_err_to_name(err));
    sticks3_transport_close();
    vibe_audio_clear();
    atomic_store(&s_recording, false);
    vibe_wifi_runtime_set_performance(&s_wifi, false);
    vibe_sticks3_status_show(!failed && err == ESP_OK ? VIBE_STICKS3_STATUS_DONE : VIBE_STICKS3_STATUS_ERROR);
}

static void start_recording(void)
{
    if (!vibe_wifi_runtime_connected(&s_wifi)) {
        vibe_sticks3_status_show(VIBE_STICKS3_STATUS_WIFI);
        return;
    }
    s_posts = 0; s_pcm_bytes = 0;
    atomic_store(&s_failed, false);
    xSemaphoreTake(s_upload_done, 0);
    snprintf(s_session, sizeof(s_session), "%08lx%08lx%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random());
    vibe_sticks3_status_show(VIBE_STICKS3_STATUS_SENDING);
    vibe_wifi_runtime_set_performance(&s_wifi, true);
    /* Allocate the compressed queue and transport before opening a session. */
    esp_err_t err = vibe_audio_set_transport(VIBE_AUDIO_TRANSPORT_IMA_ADPCM);
    if (err == ESP_OK) err = sticks3_transport_open();
    bool bridge_started = false;
    if (err == ESP_OK) {
        err = sticks3_transport_start(s_session);
        bridge_started = err == ESP_OK;
    }
    if (err == ESP_OK) err = vibe_audio_start();
    if (err != ESP_OK) {
        if (bridge_started) sticks3_transport_stop(s_session, 0, 0, true);
        sticks3_transport_close();
        vibe_audio_clear();
        vibe_wifi_runtime_set_performance(&s_wifi, false);
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        vibe_sticks3_status_show(VIBE_STICKS3_STATUS_ERROR);
        return;
    }
    atomic_store(&s_recording, true);
    xTaskNotifyGive(s_upload_task);
    vibe_sticks3_status_show(VIBE_STICKS3_STATUS_RECORDING);
    vibe_sticks3_status_set_recording_animation(true);
    ESP_LOGI(TAG, "recording session=%s", s_session);
}

static void wifi_changed(bool connected, const char *ip, void *context)
{
    (void)context;
    ESP_LOGI(TAG, "wifi connected=%d ip=%s", connected, ip ? ip : "");
    if (!atomic_load(&s_recording)) vibe_sticks3_status_show(connected ? VIBE_STICKS3_STATUS_READY : VIBE_STICKS3_STATUS_WIFI);
}

static void control_task(void *arg)
{
    (void)arg;
    bool previous = false;
    int64_t battery_at = 0;
    while (true) {
        bool pressed = gpio_get_level(VIBE_BOARD_PIN_BUTTON_FRONT) == 0;
        uint8_t command = 0;
        usb_serial_jtag_read_bytes(&command, 1, 0);
        bool toggle = command == 'r';
        if (pressed && !previous) {
            vTaskDelay(pdMS_TO_TICKS(35));
            toggle |= gpio_get_level(VIBE_BOARD_PIN_BUTTON_FRONT) == 0;
        }
        previous = pressed;
        if (toggle) {
            if (atomic_load(&s_recording)) stop_recording();
            else start_recording();
        }
        if (atomic_load(&s_recording) && atomic_load(&s_failed)) stop_recording();
        int64_t now = esp_timer_get_time();
        if (now >= battery_at && !atomic_load(&s_recording)) {
            int level;
            if (vibe_board_battery_level(&level) == ESP_OK) vibe_sticks3_status_set_battery_level(level);
            battery_at = now + 30000000;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "StickS3 maintained ASR %s", esp_app_get_description()->version);
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(vibe_board_init_power());
    ESP_ERROR_CHECK(vibe_sticks3_status_init());
    ESP_ERROR_CHECK(vibe_audio_init());
    ESP_ERROR_CHECK(vibe_audio_set_transport(VIBE_AUDIO_TRANSPORT_IMA_ADPCM));
    s_upload_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_upload_done ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(upload_task, "asr_upload", 6144, NULL, 4,
                                          &s_upload_task, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    const vibe_wifi_runtime_config_t wifi = {
        .idle_power_save = WIFI_PS_MIN_MODEM,
        .max_tx_power = VIBE_BOARD_WIFI_MAX_TX_POWER, .status_changed = wifi_changed,
    };
    /* Retain Wi-Fi credentials in NVS instead of copying them into releases. */
    ESP_ERROR_CHECK(vibe_wifi_runtime_init(&s_wifi, &wifi));
    gpio_config_t button = {.pin_bit_mask = 1ULL << VIBE_BOARD_PIN_BUTTON_FRONT,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&button));
    usb_serial_jtag_driver_config_t serial = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&serial);
    ESP_ERROR_CHECK(err == ESP_ERR_INVALID_STATE ? ESP_OK : err);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(control_task, "asr_control", 6144, NULL, 3,
                                          NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
