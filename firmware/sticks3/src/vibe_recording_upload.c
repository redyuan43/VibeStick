#include "vibe_recording_upload.h"

#include <stdatomic.h>
#include <string.h>

#include "vibe_audio.h"
#include "vibe_recording_policy.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "vibe_stick";

static vibe_recording_upload_config_t s_config;
static vibe_recording_upload_stats_t s_stats;
static atomic_bool s_active;
static atomic_bool s_failed;
static SemaphoreHandle_t s_completion;

static void set_failed(void)
{
    atomic_store(&s_failed, true);
}

static void finish_upload_task(void)
{
    xSemaphoreGive(s_completion);
    vTaskDelete(NULL);
}

static void upload_task(void *arg)
{
    (void)arg;
    uint8_t *buffer = heap_caps_malloc(s_config.buffer_bytes, MALLOC_CAP_8BIT);
    if (!buffer) {
        ESP_LOGW(TAG, "recording upload buffer allocation failed");
        set_failed();
        finish_upload_task();
        return;
    }

    while (vibe_audio_is_recording() || vibe_audio_pending_chunks() > 0) {
        vibe_recording_upload_stats_note_pending(
            &s_stats, vibe_audio_pending_chunks());

        size_t audio_len = 0;
        esp_err_t err = vibe_audio_read_batch(
            buffer, s_config.buffer_bytes, &audio_len, s_config.batch_chunks,
            s_config.read_timeout_ms);
        if (err == ESP_ERR_TIMEOUT) {
            vibe_recording_upload_stats_note_read(&s_stats, true);
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "audio read for upload failed: %s",
                     esp_err_to_name(err));
            vibe_recording_upload_stats_note_read(&s_stats, false);
            set_failed();
            continue;
        }

        int64_t post_start_ms = esp_timer_get_time() / 1000;
        err = s_config.post_chunk(buffer, audio_len, s_config.context);
        int64_t post_duration_ms =
            esp_timer_get_time() / 1000 - post_start_ms;
        vibe_recording_upload_stats_note_post(
            &s_stats, post_duration_ms, audio_len, err == ESP_OK);
        if (err != ESP_OK) {
            set_failed();
        }
    }

    heap_caps_free(buffer);
    ESP_LOGI(TAG,
             "recording upload task done posts=%u bytes=%u failures=%u failed=%d",
             (unsigned)s_stats.upload_posts,
             (unsigned)s_stats.uploaded_bytes,
             (unsigned)s_stats.upload_failures,
             vibe_recording_upload_failed());
    finish_upload_task();
}

bool vibe_recording_upload_start(const vibe_recording_upload_config_t *config,
                                 int start_rssi,
                                 int unknown_rssi)
{
    if (!config || config->buffer_bytes == 0 || config->batch_chunks == 0 ||
        config->read_timeout_ms == 0 || config->task_stack_bytes == 0 ||
        !config->post_chunk || atomic_exchange(&s_active, true)) {
        return false;
    }

    s_completion = xSemaphoreCreateBinary();
    if (!s_completion) {
        atomic_store(&s_active, false);
        return false;
    }
    s_config = *config;
    vibe_recording_upload_stats_reset(&s_stats, start_rssi, unknown_rssi);
    atomic_store(&s_failed, false);
    BaseType_t ok = xTaskCreatePinnedToCore(
        upload_task, "recording_upload", config->task_stack_bytes, NULL,
        config->task_priority, NULL, config->task_core);
    if (ok != pdPASS) {
        set_failed();
        atomic_store(&s_active, false);
        vSemaphoreDelete(s_completion);
        s_completion = NULL;
        ESP_LOGW(TAG, "task create failed");
        return false;
    }
    return true;
}

void vibe_recording_upload_wait(void)
{
    if (!atomic_load(&s_active) || !s_completion) {
        return;
    }
    xSemaphoreTake(s_completion, portMAX_DELAY);
    vSemaphoreDelete(s_completion);
    s_completion = NULL;
    atomic_store(&s_active, false);
}

bool vibe_recording_upload_active(void)
{
    return atomic_load(&s_active);
}

bool vibe_recording_upload_failed(void)
{
    return atomic_load(&s_failed);
}

void vibe_recording_upload_log_diagnostics(const char *board_name, int stop_rssi)
{
    vibe_audio_stats_t audio_stats = {0};
    vibe_audio_stats(&audio_stats);
    s_stats.stop_rssi = stop_rssi;
    const int64_t min_post_ms =
        s_stats.post_duration_min_ms >= 0 ? s_stats.post_duration_min_ms : 0;
    ESP_LOGI(TAG,
             "recording diagnostics board=%s audio_read_chunks=%u audio_queued_chunks=%u "
             "audio_dropped_chunks=%u audio_dropped_bytes=%u upload_posts=%u "
             "uploaded_bytes=%u upload_failures=%u read_failures=%u "
             "read_timeouts=%u max_pending=%u post_ms_min=%lld "
             "post_ms_avg=%lld post_ms_max=%lld rssi_start=%d rssi_stop=%d",
             board_name ? board_name : "unknown",
             (unsigned)audio_stats.chunks_read,
             (unsigned)audio_stats.chunks_queued,
             (unsigned)audio_stats.chunks_dropped,
             (unsigned)audio_stats.bytes_dropped,
             (unsigned)s_stats.upload_posts,
             (unsigned)s_stats.uploaded_bytes,
             (unsigned)s_stats.upload_failures,
             (unsigned)s_stats.read_failures,
             (unsigned)s_stats.read_timeouts,
             (unsigned)s_stats.max_pending_chunks,
             (long long)min_post_ms,
             (long long)vibe_recording_upload_stats_average_post_ms(&s_stats),
             (long long)s_stats.post_duration_max_ms,
             s_stats.start_rssi,
             s_stats.stop_rssi);
}
