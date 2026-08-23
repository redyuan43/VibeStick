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
#define RECORDING_UPLOAD_RETRY_COUNT 3
#define RECORDING_UPLOAD_RETRY_DELAY_MS 120
#define RECORDING_UPLOAD_BUFFER_CAPACITY 8192
#define RECORDING_UPLOAD_MAX_PARALLEL 1

static vibe_recording_upload_config_t s_config;
static vibe_recording_upload_stats_t s_stats;
static atomic_bool s_active;
static atomic_bool s_failed;
static atomic_uint s_next_chunk_id;
static atomic_uint s_live_workers;
static SemaphoreHandle_t s_completion;
static SemaphoreHandle_t s_stats_mutex;
static uint8_t s_upload_buffers[RECORDING_UPLOAD_MAX_PARALLEL]
                               [RECORDING_UPLOAD_BUFFER_CAPACITY / RECORDING_UPLOAD_MAX_PARALLEL];

static void set_failed(void)
{
    atomic_store(&s_failed, true);
}

static void note_pending(size_t pending_chunks)
{
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    vibe_recording_upload_stats_note_pending(&s_stats, pending_chunks);
    xSemaphoreGive(s_stats_mutex);
}

static void note_read(bool timed_out)
{
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    vibe_recording_upload_stats_note_read(&s_stats, timed_out);
    xSemaphoreGive(s_stats_mutex);
}

static void note_post(int64_t duration_ms, size_t audio_len, bool succeeded)
{
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    vibe_recording_upload_stats_note_post(
        &s_stats, duration_ms, audio_len, succeeded);
    xSemaphoreGive(s_stats_mutex);
}

static void finish_upload_worker(void)
{
    if (atomic_fetch_sub(&s_live_workers, 1) == 1) {
        xSemaphoreGive(s_completion);
    }
    vTaskDelete(NULL);
}

static void upload_task(void *arg)
{
    const unsigned int worker_index = (unsigned int)(uintptr_t)arg;
    uint8_t *buffer = s_upload_buffers[worker_index];

    while (!atomic_load(&s_failed) &&
           (vibe_audio_is_recording() || vibe_audio_pending_chunks() > 0)) {
        note_pending(vibe_audio_pending_chunks());
        size_t audio_len = 0;
        const int64_t read_start_ms = esp_timer_get_time() / 1000;
        esp_err_t err = vibe_audio_read_batch(
            buffer, s_config.buffer_bytes, &audio_len, s_config.batch_chunks,
            s_config.read_timeout_ms);
        const int64_t read_duration_ms =
            esp_timer_get_time() / 1000 - read_start_ms;
        if (err == ESP_ERR_TIMEOUT) {
            note_read(true);
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "audio read for upload failed: %s",
                     esp_err_to_name(err));
            note_read(false);
            set_failed();
            break;
        }

        const uint32_t chunk_id =
            atomic_fetch_add(&s_next_chunk_id, 1);
        int64_t post_start_ms = esp_timer_get_time() / 1000;
        for (int attempt = 1; attempt <= RECORDING_UPLOAD_RETRY_COUNT; ++attempt) {
            err = s_config.post_chunk(
                buffer, audio_len, chunk_id, s_config.context);
            if (err == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "audio upload worker=%u chunk=%u attempt %d/%d failed: %s",
                     worker_index, (unsigned)chunk_id, attempt,
                     RECORDING_UPLOAD_RETRY_COUNT, esp_err_to_name(err));
            if (attempt < RECORDING_UPLOAD_RETRY_COUNT) {
                vTaskDelay(pdMS_TO_TICKS(
                    RECORDING_UPLOAD_RETRY_DELAY_MS * attempt));
            }
        }
        int64_t post_duration_ms =
            esp_timer_get_time() / 1000 - post_start_ms;
#if VIBE_STICK_SERIAL_DEBUG_ENABLED
        ESP_LOGI(TAG,
                 "recording upload worker=%u chunk=%u bytes=%u read_ms=%lld post_ms=%lld",
                 worker_index, (unsigned)chunk_id, (unsigned)audio_len,
                 (long long)read_duration_ms,
                 (long long)post_duration_ms);
#else
        (void)worker_index;
        (void)read_duration_ms;
#endif
        note_post(post_duration_ms, audio_len, err == ESP_OK);
        if (err != ESP_OK) {
            set_failed();
            break;
        }
    }

    finish_upload_worker();
}

bool vibe_recording_upload_start(const vibe_recording_upload_config_t *config,
                                 int start_rssi,
                                 int unknown_rssi)
{
    if (!config || config->buffer_bytes == 0 || config->batch_chunks == 0 ||
        config->parallel_uploads == 0 ||
        config->parallel_uploads > RECORDING_UPLOAD_MAX_PARALLEL ||
        config->buffer_bytes >
            sizeof(s_upload_buffers[0]) ||
        config->read_timeout_ms == 0 || config->task_stack_bytes == 0 ||
        !config->post_chunk ||
        atomic_exchange(&s_active, true)) {
        return false;
    }

    s_completion = xSemaphoreCreateBinary();
    s_stats_mutex = xSemaphoreCreateMutex();
    if (!s_completion || !s_stats_mutex) {
        if (s_completion) {
            vSemaphoreDelete(s_completion);
            s_completion = NULL;
        }
        if (s_stats_mutex) {
            vSemaphoreDelete(s_stats_mutex);
            s_stats_mutex = NULL;
        }
        atomic_store(&s_active, false);
        return false;
    }
    s_config = *config;
    vibe_recording_upload_stats_reset(&s_stats, start_rssi, unknown_rssi);
    atomic_store(&s_failed, false);
    atomic_store(&s_next_chunk_id, 0);
    atomic_store(&s_live_workers, 0);
    for (unsigned int index = 0; index < config->parallel_uploads; ++index) {
        atomic_fetch_add(&s_live_workers, 1);
        BaseType_t ok = xTaskCreatePinnedToCore(
            upload_task, "recording_upload", config->task_stack_bytes,
            (void *)(uintptr_t)index, config->task_priority, NULL,
            config->task_core);
        if (ok != pdPASS) {
            atomic_fetch_sub(&s_live_workers, 1);
            ESP_LOGW(TAG, "recording upload worker=%u task create failed",
                     index);
            set_failed();
            break;
        }
    }
    if (atomic_load(&s_live_workers) == 0) {
        atomic_store(&s_active, false);
        vSemaphoreDelete(s_completion);
        s_completion = NULL;
        vSemaphoreDelete(s_stats_mutex);
        s_stats_mutex = NULL;
        return false;
    }
#if VIBE_STICK_SERIAL_DEBUG_ENABLED
    ESP_LOGI(TAG,
             "recording upload started workers=%u buffer_bytes=%u heap_free=%u heap_largest=%u",
             (unsigned)config->parallel_uploads,
             (unsigned)config->buffer_bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#endif
    return true;
}

void vibe_recording_upload_wait(void)
{
    if (!atomic_load(&s_active) || !s_completion) {
        return;
    }
    xSemaphoreTake(s_completion, portMAX_DELAY);
#if VIBE_STICK_SERIAL_DEBUG_ENABLED
    ESP_LOGI(TAG,
             "recording upload workers done posts=%u bytes=%u failures=%u failed=%d",
             (unsigned)s_stats.upload_posts,
             (unsigned)s_stats.uploaded_bytes,
             (unsigned)s_stats.upload_failures,
             vibe_recording_upload_failed());
#endif
    vSemaphoreDelete(s_completion);
    s_completion = NULL;
    vSemaphoreDelete(s_stats_mutex);
    s_stats_mutex = NULL;
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

void vibe_recording_upload_totals(size_t *posts, size_t *bytes)
{
    if (!s_stats_mutex) {
        if (posts) {
            *posts = s_stats.upload_posts;
        }
        if (bytes) {
            *bytes = s_stats.uploaded_bytes;
        }
        return;
    }
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    if (posts) {
        *posts = s_stats.upload_posts;
    }
    if (bytes) {
        *bytes = s_stats.uploaded_bytes;
    }
    xSemaphoreGive(s_stats_mutex);
}

void vibe_recording_upload_diagnostics(
    vibe_recording_upload_diagnostics_t *diagnostics)
{
    if (!diagnostics) {
        return;
    }
    memset(diagnostics, 0, sizeof(*diagnostics));
    vibe_audio_stats(&diagnostics->audio);
    if (s_stats_mutex) {
        xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    }
    diagnostics->upload = s_stats;
    if (s_stats_mutex) {
        xSemaphoreGive(s_stats_mutex);
    }
}

void vibe_recording_upload_log_diagnostics(const char *board_name, int stop_rssi)
{
    vibe_recording_upload_diagnostics_t diagnostics = {0};
    if (s_stats_mutex) {
        xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    }
    s_stats.stop_rssi = stop_rssi;
    if (s_stats_mutex) {
        xSemaphoreGive(s_stats_mutex);
    }
    vibe_recording_upload_diagnostics(&diagnostics);
    const vibe_audio_stats_t *audio_stats = &diagnostics.audio;
    const vibe_recording_upload_stats_t *upload_stats = &diagnostics.upload;
    const int64_t min_post_ms =
        upload_stats->post_duration_min_ms >= 0
            ? upload_stats->post_duration_min_ms
            : 0;
    ESP_LOGI(TAG,
             "recording diagnostics board=%s audio_read_chunks=%u audio_queued_chunks=%u "
             "audio_dropped_chunks=%u audio_dropped_bytes=%u upload_posts=%u "
             "uploaded_bytes=%u upload_failures=%u read_failures=%u "
             "read_timeouts=%u max_pending=%u post_ms_min=%lld "
             "post_ms_avg=%lld post_ms_max=%lld rssi_start=%d rssi_stop=%d",
             board_name ? board_name : "unknown",
             (unsigned)audio_stats->chunks_read,
             (unsigned)audio_stats->chunks_queued,
             (unsigned)audio_stats->chunks_dropped,
             (unsigned)audio_stats->bytes_dropped,
             (unsigned)upload_stats->upload_posts,
             (unsigned)upload_stats->uploaded_bytes,
             (unsigned)upload_stats->upload_failures,
             (unsigned)upload_stats->read_failures,
             (unsigned)upload_stats->read_timeouts,
             (unsigned)upload_stats->max_pending_chunks,
             (long long)min_post_ms,
             (long long)vibe_recording_upload_stats_average_post_ms(upload_stats),
             (long long)upload_stats->post_duration_max_ms,
             upload_stats->start_rssi,
             upload_stats->stop_rssi);
}
