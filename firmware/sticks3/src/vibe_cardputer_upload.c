#include "vibe_cardputer_upload.h"

#include <stdatomic.h>

#include "vibe_cardputer_capture.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define UPLOAD_BUFFER_BYTES 8192
#define UPLOAD_BATCH_CHUNKS 4
#define UPLOAD_READ_TIMEOUT_MS 250
#define UPLOAD_RETRY_COUNT 3
#define UPLOAD_RETRY_DELAY_MS 120

static const char *TAG = "card_upload";
static uint8_t s_buffer[UPLOAD_BUFFER_BYTES];
static SemaphoreHandle_t s_completion;
static TaskHandle_t s_task;
static vibe_cardputer_upload_post_fn s_post;
static void *s_context;
static atomic_bool s_active;
static atomic_bool s_failed;
static uint32_t s_next_chunk_id;
static size_t s_posts;
static size_t s_bytes;
static size_t s_wire_bytes;

static void upload_task(void *arg)
{
    (void)arg;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (!atomic_load(&s_failed) &&
               (vibe_cardputer_capture_is_recording() ||
                vibe_cardputer_capture_pending_chunks() > 0)) {
            size_t audio_len = 0;
            esp_err_t err = vibe_cardputer_capture_read_batch(
                s_buffer, sizeof(s_buffer), &audio_len, UPLOAD_BATCH_CHUNKS,
                UPLOAD_READ_TIMEOUT_MS);
            if (err == ESP_ERR_TIMEOUT) {
                continue;
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "read capture queue: %s", esp_err_to_name(err));
                atomic_store(&s_failed, true);
                break;
            }
            const uint32_t chunk_id = s_next_chunk_id++;
            for (unsigned int attempt = 1; attempt <= UPLOAD_RETRY_COUNT;
                 ++attempt) {
                err = s_post(s_buffer, audio_len, chunk_id, s_context);
                if (err == ESP_OK) {
                    break;
                }
                if (attempt < UPLOAD_RETRY_COUNT) {
                    vTaskDelay(pdMS_TO_TICKS(
                        UPLOAD_RETRY_DELAY_MS * attempt));
                }
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "upload chunk=%u failed: %s",
                         (unsigned)chunk_id, esp_err_to_name(err));
                atomic_store(&s_failed, true);
                break;
            }
            s_posts++;
            s_wire_bytes += audio_len;
            s_bytes +=
                (audio_len / VIBE_CARDPUTER_CAPTURE_ENCODED_FRAME_BYTES) *
                VIBE_CARDPUTER_CAPTURE_FRAME_BYTES;
        }
        atomic_store(&s_active, false);
        xSemaphoreGive(s_completion);
    }
}

esp_err_t vibe_cardputer_upload_init(void)
{
    if (s_task) {
        return ESP_OK;
    }
    s_completion = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_completion != NULL, ESP_ERR_NO_MEM, TAG,
                        "upload completion");
    BaseType_t started = xTaskCreatePinnedToCore(
        upload_task, "asr_upload", 5120, NULL, 5, &s_task, 1);
    ESP_RETURN_ON_FALSE(started == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "upload task");
    return ESP_OK;
}

bool vibe_cardputer_upload_start(vibe_cardputer_upload_post_fn post_chunk,
                                 void *context)
{
    if (!s_task || !s_completion || !post_chunk ||
        atomic_exchange(&s_active, true)) {
        return false;
    }
    (void)xSemaphoreTake(s_completion, 0);
    s_post = post_chunk;
    s_context = context;
    s_next_chunk_id = 0;
    s_posts = 0;
    s_bytes = 0;
    s_wire_bytes = 0;
    atomic_store(&s_failed, false);
    xTaskNotifyGive(s_task);
    return true;
}

void vibe_cardputer_upload_wait(void)
{
    if (atomic_load(&s_active)) {
        xSemaphoreTake(s_completion, portMAX_DELAY);
    }
}

bool vibe_cardputer_upload_failed(void)
{
    return atomic_load(&s_failed);
}

void vibe_cardputer_upload_totals(size_t *posts, size_t *bytes)
{
    if (posts) {
        *posts = s_posts;
    }
    if (bytes) {
        *bytes = s_bytes;
    }
}

size_t vibe_cardputer_upload_wire_bytes(void)
{
    return s_wire_bytes;
}
