#include "vibe_cardputer_messages.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "lvgl.h"
#include "mbedtls/sha256.h"
#include "sdmmc_cmd.h"
#include "vibe_audio.h"
#include "vibe_board_profile.h"
#include "vibe_wav.h"

#if defined(VIBE_BOARD_CARDPUTER_ADV)

#define MESSAGE_MOUNT_POINT "/sdcard"
#define MESSAGE_ROOT MESSAGE_MOUNT_POINT "/vibestick"
#define MESSAGE_AUDIO_DIR MESSAGE_ROOT "/audio"
#define MESSAGE_SYSTEM_DIR MESSAGE_ROOT "/system"
#define MESSAGE_INDEX_PATH MESSAGE_ROOT "/messages.jsonl"
#define MESSAGE_INDEX_TMP MESSAGE_ROOT "/messages.jsonl.tmp"
#define MESSAGE_NOTIFY_PATH MESSAGE_SYSTEM_DIR "/F1_New_SMS.wav"
#define MESSAGE_FONT_PATH MESSAGE_SYSTEM_DIR "/DroidSansFallback.ttf"
#define MESSAGE_MAX_COUNT 100
#define MESSAGE_PAGE_SIZE 4
#define MESSAGE_SYNC_RESPONSE_BYTES (4 * 1024)
#define MESSAGE_SYNC_BATCH_SIZE 3
#define MESSAGE_MAX_RESOURCE_BYTES (8 * 1024 * 1024)
#define MESSAGE_ACTIVE_SYNC_INTERVAL_MS 10000
#define MESSAGE_TASK_STACK_BYTES (12 * 1024)
#define MESSAGE_ACTION_QUEUE_LENGTH 12
#define MESSAGE_IO_BUFFER_BYTES 512
#define MESSAGE_RESOURCE_PATH_PREFIX "/device/messages/resource?"
#define MESSAGE_AUDIO_PATH_PREFIX "/device/messages/resource?kind=audio&id="
#define MESSAGE_FONT_PARTITION_LABEL "anim"
#define MESSAGE_FONT_MAGIC 0x56464e54u
#define MESSAGE_FONT_FORMAT_VERSION 1u
#define MESSAGE_FONT_DATA_OFFSET 0x1000u
#define MESSAGE_FONT_CACHE_GLYPHS 8
#define MESSAGE_INDEX_SCHEMA_VERSION 2
#define MESSAGE_DETAIL_START_DELAY_MS 800
#define MESSAGE_DETAIL_SCROLL_PERIOD_MS 100
#define MESSAGE_DETAIL_MANUAL_PAUSE_MS 1500
#define MESSAGE_DETAIL_MANUAL_STEP 24
#define MESSAGE_DOUBLE_PRESS_WINDOW_MS 400

typedef struct {
    uint32_t magic;
    uint32_t format_version;
    uint32_t font_size;
    uint8_t sha256[32];
} message_font_header_t;

typedef struct {
    uint32_t cursor;
    bool read;
    char title[64];
    char summary[160];
    char spoken_text[513];
    char audio_id[32];
    char audio_sha256[65];
    size_t audio_size;
} stored_message_t;

/* Keep only the four visible records in RAM. The complete message metadata
 * remains as JSONL on SD; the cursor/read table is deliberately tiny. */
typedef struct {
    uint32_t cursor;
    bool read;
} message_ref_t;

typedef enum {
    MESSAGE_ACTION_PREPARE = 0,
    MESSAGE_ACTION_OPEN,
    MESSAGE_ACTION_CLOSE,
    MESSAGE_ACTION_LEFT,
    MESSAGE_ACTION_RIGHT,
    MESSAGE_ACTION_UP,
    MESSAGE_ACTION_DOWN,
    MESSAGE_ACTION_PLAY,
    MESSAGE_ACTION_PLAY_UNREAD,
    MESSAGE_ACTION_RELEASE,
} message_action_t;

static const char *TAG = "card_messages";
static vibe_cardputer_messages_config_t s_config;
static stored_message_t s_messages[MESSAGE_PAGE_SIZE];
static message_ref_t s_message_refs[MESSAGE_MAX_COUNT];
static size_t s_message_count;
static uint32_t s_cursor;
static uint32_t s_oldest_cursor;
static bool s_history_has_more;
static size_t s_loaded_page_start = SIZE_MAX;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_action_queue;
static SemaphoreHandle_t s_release_done;
static TimerHandle_t s_enter_single_timer;
static atomic_bool s_storage_ready;
static atomic_bool s_active;
static atomic_bool s_play_queued;
static atomic_bool s_detail_active;
static atomic_bool s_play_cancel;
static atomic_bool s_auto_session_active;
static atomic_bool s_auto_playing;
static atomic_bool s_auto_play_queued;
static atomic_bool s_auto_paused;
static atomic_bool s_auto_start_pending;
static atomic_bool s_enter_single_pending;
static atomic_bool s_sync_paused;
static atomic_bool s_sync_in_progress;
static atomic_bool s_sync_load_older;
static TaskHandle_t s_sync_task_handle;
static size_t s_selected;
static uint32_t s_auto_queue[MESSAGE_MAX_COUNT];
static size_t s_auto_queue_count;
static int64_t s_space_last_press_ms;
static int64_t s_enter_last_press_ms;
static lv_obj_t *s_layer;
static lv_obj_t *s_header;
static lv_obj_t *s_tiles[MESSAGE_PAGE_SIZE];
static lv_obj_t *s_unread_dots[MESSAGE_PAGE_SIZE];
static lv_obj_t *s_labels[MESSAGE_PAGE_SIZE];
static lv_timer_t *s_blink_timer;
static lv_obj_t *s_detail_view;
static lv_obj_t *s_detail_label;
static lv_timer_t *s_detail_scroll_timer;
static int64_t s_detail_scroll_start_ms;
static int64_t s_detail_manual_until_ms;
static bool s_blink_on;
static bool s_system_resources_ready;
static bool s_font_resource_ready;
static size_t s_rendered_count;
static uint32_t s_rendered_cursor;
static lv_font_t *s_chinese_font;
static const void *s_font_mapping;
static esp_partition_mmap_handle_t s_font_mapping_handle;

static void blink_timer(lv_timer_t *timer);
static void detail_scroll_timer(lv_timer_t *timer);
static int64_t message_now_ms(void);
static void prepare_messages(void);
static void render_messages(void);
static void handle_action(message_action_t action);
static void sync_once(void);
static void sync_once_inner(void);
static void auto_enqueue_unread_messages(void);
static void auto_enqueue_message(uint32_t cursor);
static void auto_request_playback(void);
static void request_selected_playback(void);

static void message_sync_task(void *argument)
{
    (void)argument;
    while (true) {
        /* The home screen never polls messages. Once Fn+N owns the display,
         * it is a dedicated message mode and may refresh without competing
         * with live voice capture. */
        TickType_t timeout = atomic_load(&s_active)
                                 ? pdMS_TO_TICKS(MESSAGE_ACTIVE_SYNC_INTERVAL_MS)
                                 : portMAX_DELAY;
        (void)ulTaskNotifyTake(pdTRUE, timeout);
        if (atomic_load(&s_active)) sync_once();
    }
}

static bool enqueue_action(message_action_t action)
{
    if (!s_action_queue ||
        xQueueSend(s_action_queue, &action, 0) != pdTRUE) {
        ESP_LOGW(TAG, "message action queue full action=%d", (int)action);
        return false;
    }
    return true;
}

static void request_selected_playback(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_play_queued, &expected, true)) {
        return;
    }
    if (!enqueue_action(MESSAGE_ACTION_PLAY)) {
        atomic_store(&s_play_queued, false);
    }
}

static void enter_single_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (!atomic_exchange(&s_enter_single_pending, false) ||
        !atomic_load(&s_active) || atomic_load(&s_auto_paused) ||
        atomic_load(&s_auto_playing)) {
        return;
    }
    request_selected_playback();
}

static void display_lock(void)
{
    if (s_config.display_lock) s_config.display_lock();
}

static void display_unlock(void)
{
    if (s_config.display_unlock) s_config.display_unlock();
}

static void mkdir_if_missing(const char *path)
{
    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir failed path=%s errno=%d", path, errno);
    }
}

static bool file_sha256_matches(const char *path, const char *expected,
                                size_t expected_size)
{
    struct stat stat_value;
    if (!expected || strlen(expected) != 64 || stat(path, &stat_value) != 0 ||
        (expected_size > 0 && (size_t)stat_value.st_size != expected_size)) {
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    mbedtls_sha256_starts(&context, 0);
    uint8_t buffer[MESSAGE_IO_BUFFER_BYTES];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        mbedtls_sha256_update(&context, buffer, count);
    }
    fclose(file);
    uint8_t digest[32];
    mbedtls_sha256_finish(&context, digest);
    mbedtls_sha256_free(&context);
    char actual[65];
    for (size_t index = 0; index < sizeof(digest); index++) {
        snprintf(actual + index * 2, 3, "%02x", digest[index]);
    }
    return strcmp(actual, expected) == 0;
}

static void copy_json_string(cJSON *object, const char *key, char *output,
                             size_t output_size)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(value) && value->valuestring) {
        const char *source = value->valuestring;
        size_t source_len = strlen(source);
        size_t copy_len = source_len < output_size - 1 ? source_len : output_size - 1;
        if (copy_len < source_len) {
            while (copy_len > 0 && ((unsigned char)source[copy_len] & 0xc0) == 0x80) {
                copy_len--;
            }
        }
        memcpy(output, source, copy_len);
        output[copy_len] = '\0';
    }
}

static void clear_legacy_message_cache(void)
{
    DIR *directory = opendir(MESSAGE_AUDIO_DIR);
    if (directory) {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char path[320];
            snprintf(path, sizeof(path), MESSAGE_AUDIO_DIR "/%s",
                     entry->d_name);
            unlink(path);
        }
        closedir(directory);
    }
    unlink(MESSAGE_INDEX_PATH);
    s_message_count = 0;
    s_cursor = 0;
    s_oldest_cursor = 0;
    ESP_LOGI(TAG, "legacy message cache cleared for schema v%d",
             MESSAGE_INDEX_SCHEMA_VERSION);
}

static bool parse_stored_message(cJSON *item, stored_message_t *message)
{
    cJSON *cursor = cJSON_GetObjectItemCaseSensitive(item, "cursor");
    cJSON *read = cJSON_GetObjectItemCaseSensitive(item, "read");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(item, "audio_size");
    if (!cJSON_IsNumber(cursor) || cursor->valuedouble <= 0) return false;
    memset(message, 0, sizeof(*message));
    message->cursor = (uint32_t)cursor->valuedouble;
    message->read = cJSON_IsTrue(read);
    message->audio_size = cJSON_IsNumber(size) ? (size_t)size->valuedouble : 0;
    copy_json_string(item, "title", message->title, sizeof(message->title));
    copy_json_string(item, "summary", message->summary, sizeof(message->summary));
    copy_json_string(item, "spoken_text", message->spoken_text,
                     sizeof(message->spoken_text));
    copy_json_string(item, "audio_id", message->audio_id, sizeof(message->audio_id));
    copy_json_string(item, "audio_sha256", message->audio_sha256,
                     sizeof(message->audio_sha256));
    return true;
}

static int message_ref_index(uint32_t cursor)
{
    for (size_t index = 0; index < s_message_count; index++) {
        if (s_message_refs[index].cursor == cursor) return (int)index;
    }
    return -1;
}

static void message_refs_insert(uint32_t cursor, bool read)
{
    if (message_ref_index(cursor) >= 0 || s_message_count >= MESSAGE_MAX_COUNT) return;
    size_t index = 0;
    while (index < s_message_count && s_message_refs[index].cursor > cursor) index++;
    if (index < s_message_count) {
        memmove(&s_message_refs[index + 1], &s_message_refs[index],
                (s_message_count - index) * sizeof(s_message_refs[0]));
    }
    s_message_refs[index] = (message_ref_t){.cursor = cursor, .read = read};
    s_message_count++;
    if (cursor > s_cursor) s_cursor = cursor;
    s_oldest_cursor = s_message_refs[s_message_count - 1].cursor;
}

static bool write_message_json(FILE *file, const stored_message_t *message)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "schema_version", MESSAGE_INDEX_SCHEMA_VERSION);
    cJSON_AddNumberToObject(item, "cursor", message->cursor);
    cJSON_AddBoolToObject(item, "read", message->read);
    cJSON_AddStringToObject(item, "title", message->title);
    cJSON_AddStringToObject(item, "summary", message->summary);
    cJSON_AddStringToObject(item, "spoken_text", message->spoken_text);
    cJSON_AddStringToObject(item, "audio_id", message->audio_id);
    cJSON_AddStringToObject(item, "audio_sha256", message->audio_sha256);
    cJSON_AddNumberToObject(item, "audio_size", (double)message->audio_size);
    char *line = cJSON_PrintUnformatted(item);
    cJSON_Delete(item);
    bool ok = line && fprintf(file, "%s\n", line) >= 0;
    cJSON_free(line);
    return ok;
}

static bool load_message_record(uint32_t cursor, stored_message_t *message)
{
    FILE *file = fopen(MESSAGE_INDEX_PATH, "rb");
    if (!file) return false;
    char line[2048];
    bool found = false;
    while (fgets(line, sizeof(line), file)) {
        cJSON *item = cJSON_Parse(line);
        if (item && parse_stored_message(item, message) && message->cursor == cursor) {
            found = true;
            cJSON_Delete(item);
            break;
        }
        cJSON_Delete(item);
    }
    fclose(file);
    return found;
}

static void load_index(void)
{
    FILE *file = fopen(MESSAGE_INDEX_PATH, "rb");
    if (!file) return;
    bool legacy_schema = false;
    char line[2048];
    while (fgets(line, sizeof(line), file)) {
        cJSON *item = cJSON_Parse(line);
        if (!item) continue;
        cJSON *schema = cJSON_GetObjectItemCaseSensitive(item, "schema_version");
        stored_message_t message;
        if (!cJSON_IsNumber(schema) || schema->valueint != MESSAGE_INDEX_SCHEMA_VERSION) {
            legacy_schema = true;
        } else if (parse_stored_message(item, &message)) {
            message_refs_insert(message.cursor, message.read);
        }
        cJSON_Delete(item);
        if (legacy_schema) break;
    }
    fclose(file);
    if (legacy_schema) clear_legacy_message_cache();
}

static esp_err_t save_index(void)
{
    FILE *source = fopen(MESSAGE_INDEX_PATH, "rb");
    FILE *target = fopen(MESSAGE_INDEX_TMP, "wb");
    if (!target) {
        if (source) fclose(source);
        return ESP_FAIL;
    }
    char line[2048];
    bool ok = true;
    while (source && fgets(line, sizeof(line), source)) {
        cJSON *item = cJSON_Parse(line);
        stored_message_t message;
        if (item && parse_stored_message(item, &message)) {
            int ref_index = message_ref_index(message.cursor);
            if (ref_index >= 0) {
                message.read = s_message_refs[ref_index].read;
                ok = write_message_json(target, &message);
            }
        }
        cJSON_Delete(item);
        if (!ok) break;
    }
    if (source) fclose(source);
    if (fclose(target) != 0) ok = false;
    /* FATFS rename does not replace an existing destination. Commit only
     * after the temporary file has been completely written and closed. */
    if (ok && unlink(MESSAGE_INDEX_PATH) != 0 && errno != ENOENT) ok = false;
    if (!ok || rename(MESSAGE_INDEX_TMP, MESSAGE_INDEX_PATH) != 0) {
        unlink(MESSAGE_INDEX_TMP);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t append_message_record(const stored_message_t *message)
{
    FILE *file = fopen(MESSAGE_INDEX_PATH, "ab");
    if (!file) return ESP_FAIL;
    bool ok = write_message_json(file, message) && fclose(file) == 0;
    if (!ok) return ESP_FAIL;
    message_refs_insert(message->cursor, message->read);
    return ESP_OK;
}

static void audio_path(const char *audio_id, char *path, size_t path_size)
{
    snprintf(path, path_size, MESSAGE_AUDIO_DIR "/%s.wav", audio_id);
}

static void remove_message_at(size_t index)
{
    if (index >= s_message_count) return;
    stored_message_t message;
    if (load_message_record(s_message_refs[index].cursor, &message)) {
        char path[160];
        audio_path(message.audio_id, path, sizeof(path));
        unlink(path);
    }
    if (index + 1 < s_message_count) {
        memmove(&s_message_refs[index], &s_message_refs[index + 1],
                (s_message_count - index - 1) * sizeof(s_message_refs[0]));
    }
    s_message_count--;
    s_oldest_cursor = s_message_count ? s_message_refs[s_message_count - 1].cursor : 0;
}

static void prune_messages(void)
{
    while (s_message_count >= MESSAGE_MAX_COUNT) {
        size_t remove_index = s_message_count - 1;
        for (size_t index = s_message_count; index > 0; index--) {
            if (s_message_refs[index - 1].read) {
                remove_index = index - 1;
                break;
            }
        }
        remove_message_at(remove_index);
    }
}

static esp_err_t ensure_resource(cJSON *resource, const char *destination)
{
    if (!cJSON_IsObject(resource)) return ESP_ERR_INVALID_RESPONSE;
    char url[192] = {0};
    char sha[65] = {0};
    copy_json_string(resource, "url", url, sizeof(url));
    copy_json_string(resource, "sha256", sha, sizeof(sha));
    cJSON *size_value = cJSON_GetObjectItemCaseSensitive(resource, "size");
    size_t size = cJSON_IsNumber(size_value) ? (size_t)size_value->valuedouble : 0;
    if (strncmp(url, MESSAGE_RESOURCE_PATH_PREFIX,
                strlen(MESSAGE_RESOURCE_PATH_PREFIX)) != 0 || size == 0 ||
        size > MESSAGE_MAX_RESOURCE_BYTES || strlen(sha) != 64) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (file_sha256_matches(destination, sha, size)) return ESP_OK;
    char temporary[192];
    snprintf(temporary, sizeof(temporary), "%s.part", destination);
    unlink(temporary);
    ESP_RETURN_ON_ERROR(s_config.download(url, temporary, size), TAG,
                        "download system resource");
    if (!file_sha256_matches(temporary, sha, size)) {
        unlink(temporary);
        return ESP_ERR_INVALID_CRC;
    }
    if (rename(temporary, destination) != 0) {
        unlink(temporary);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool hex_digest(const char *text, uint8_t digest[32])
{
    if (!text || strlen(text) != 64) return false;
    for (size_t index = 0; index < 32; index++) {
        unsigned value = 0;
        if (sscanf(&text[index * 2], "%2x", &value) != 1) return false;
        digest[index] = (uint8_t)value;
    }
    return true;
}

static const esp_partition_t *font_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    (esp_partition_subtype_t)0x40,
                                    MESSAGE_FONT_PARTITION_LABEL);
}

static void release_cached_font(void)
{
    display_lock();
    if (s_layer) {
        lv_obj_set_style_text_font(s_header, LV_FONT_DEFAULT, 0);
        for (size_t index = 0; index < MESSAGE_PAGE_SIZE; index++) {
            lv_obj_set_style_text_font(s_labels[index], LV_FONT_DEFAULT, 0);
        }
        if (s_detail_label) {
            lv_obj_set_style_text_font(s_detail_label, LV_FONT_DEFAULT, 0);
        }
    }
    if (s_chinese_font) {
        lv_tiny_ttf_destroy(s_chinese_font);
        s_chinese_font = NULL;
    }
    if (s_font_mapping_handle) {
        esp_partition_munmap(s_font_mapping_handle);
        s_font_mapping_handle = 0;
        s_font_mapping = NULL;
    }
    display_unlock();
}

static esp_err_t load_cached_font(void)
{
    if (s_chinese_font) return ESP_OK;
    const esp_partition_t *partition = font_partition();
    ESP_RETURN_ON_FALSE(partition, ESP_ERR_NOT_FOUND, TAG,
                        "font partition missing");
    message_font_header_t header;
    ESP_RETURN_ON_ERROR(esp_partition_read(partition, 0, &header,
                                           sizeof(header)),
                        TAG, "read font header");
    ESP_RETURN_ON_FALSE(
        header.magic == MESSAGE_FONT_MAGIC &&
            header.format_version == MESSAGE_FONT_FORMAT_VERSION &&
            header.font_size > 0 &&
            header.font_size <= partition->size - MESSAGE_FONT_DATA_OFFSET,
        ESP_ERR_NOT_FOUND, TAG, "cached font invalid");
    ESP_RETURN_ON_ERROR(
        esp_partition_mmap(partition, MESSAGE_FONT_DATA_OFFSET,
                           header.font_size, ESP_PARTITION_MMAP_DATA,
                           &s_font_mapping, &s_font_mapping_handle),
        TAG, "map cached font");
    display_lock();
    s_chinese_font = lv_tiny_ttf_create_data_ex(
        s_font_mapping, header.font_size, 14, LV_FONT_KERNING_NORMAL,
        MESSAGE_FONT_CACHE_GLYPHS);
    if (s_chinese_font) {
        /* DroidSansFallback covers CJK but intentionally omits most ASCII. */
        s_chinese_font->fallback = &lv_font_montserrat_14;
        if (s_layer) {
            lv_obj_set_style_text_font(s_header, s_chinese_font, 0);
            for (size_t index = 0; index < MESSAGE_PAGE_SIZE; index++) {
                lv_obj_set_style_text_font(s_labels[index], s_chinese_font, 0);
            }
            if (s_detail_label) {
                lv_obj_set_style_text_font(s_detail_label, s_chinese_font, 0);
            }
        }
    }
    display_unlock();
    if (!s_chinese_font) {
        esp_partition_munmap(s_font_mapping_handle);
        s_font_mapping_handle = 0;
        s_font_mapping = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "cached Chinese font loaded size=%lu",
             (unsigned long)header.font_size);
    return ESP_OK;
}

static esp_err_t install_cached_font(const char *path, const char *sha_text,
                                     size_t font_size)
{
    const esp_partition_t *partition = font_partition();
    ESP_RETURN_ON_FALSE(partition, ESP_ERR_NOT_FOUND, TAG,
                        "font partition missing");
    ESP_RETURN_ON_FALSE(font_size > 0 &&
                            font_size <= partition->size -
                                             MESSAGE_FONT_DATA_OFFSET,
                        ESP_ERR_INVALID_SIZE, TAG, "font too large");
    uint8_t expected_sha[32];
    ESP_RETURN_ON_FALSE(hex_digest(sha_text, expected_sha),
                        ESP_ERR_INVALID_ARG, TAG, "invalid font hash");

    message_font_header_t current;
    if (esp_partition_read(partition, 0, &current, sizeof(current)) == ESP_OK &&
        current.magic == MESSAGE_FONT_MAGIC &&
        current.format_version == MESSAGE_FONT_FORMAT_VERSION &&
        current.font_size == font_size &&
        memcmp(current.sha256, expected_sha, sizeof(expected_sha)) == 0) {
        return ESP_OK;
    }

    release_cached_font();
    FILE *file = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(file, ESP_ERR_NOT_FOUND, TAG, "open downloaded font");
    size_t erase_size = MESSAGE_FONT_DATA_OFFSET + font_size;
    erase_size = (erase_size + 0xfff) & ~((size_t)0xfff);
    esp_err_t err = esp_partition_erase_range(partition, 0, erase_size);
    uint8_t *buffer = NULL;
    if (err == ESP_OK) {
        buffer = malloc(4096);
        if (!buffer) err = ESP_ERR_NO_MEM;
    }
    size_t written = 0;
    while (err == ESP_OK && written < font_size) {
        size_t request = font_size - written;
        if (request > 4096) request = 4096;
        size_t count = fread(buffer, 1, request, file);
        if (count != request) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = esp_partition_write(partition,
                                  MESSAGE_FONT_DATA_OFFSET + written,
                                  buffer, count);
        written += count;
        vTaskDelay(1);
    }
    free(buffer);
    fclose(file);
    ESP_RETURN_ON_ERROR(err, TAG, "write font partition");

    const void *verify_mapping = NULL;
    esp_partition_mmap_handle_t verify_handle = 0;
    ESP_RETURN_ON_ERROR(
        esp_partition_mmap(partition, MESSAGE_FONT_DATA_OFFSET, font_size,
                           ESP_PARTITION_MMAP_DATA, &verify_mapping,
                           &verify_handle),
        TAG, "map font for verification");
    uint8_t actual_sha[32];
    int sha_result = mbedtls_sha256(verify_mapping, font_size, actual_sha, 0);
    esp_partition_munmap(verify_handle);
    ESP_RETURN_ON_FALSE(sha_result == 0 &&
                            memcmp(actual_sha, expected_sha,
                                   sizeof(actual_sha)) == 0,
                        ESP_ERR_INVALID_CRC, TAG, "font flash hash mismatch");

    message_font_header_t header = {
        .magic = MESSAGE_FONT_MAGIC,
        .format_version = MESSAGE_FONT_FORMAT_VERSION,
        .font_size = font_size,
    };
    memcpy(header.sha256, expected_sha, sizeof(expected_sha));
    ESP_RETURN_ON_ERROR(esp_partition_write(partition, 0, &header,
                                            sizeof(header)),
                        TAG, "commit font header");
    ESP_LOGI(TAG, "Chinese font installed size=%lu",
             (unsigned long)font_size);
    return ESP_OK;
}

static esp_err_t parse_message_audio_metadata(cJSON *item, stored_message_t *message)
{
    char url[192] = {0};
    copy_json_string(item, "audio_url", url, sizeof(url));
    copy_json_string(item, "audio_id", message->audio_id,
                     sizeof(message->audio_id));
    copy_json_string(item, "audio_sha256", message->audio_sha256,
                     sizeof(message->audio_sha256));
    cJSON *size_value = cJSON_GetObjectItemCaseSensitive(item, "audio_size");
    message->audio_size = cJSON_IsNumber(size_value)
                              ? (size_t)size_value->valuedouble
                              : 0;
    if (strncmp(url, MESSAGE_AUDIO_PATH_PREFIX,
                strlen(MESSAGE_AUDIO_PATH_PREFIX)) != 0 ||
        !message->audio_id[0] || strlen(message->audio_sha256) != 64 ||
        message->audio_size == 0 || message->audio_size > MESSAGE_MAX_RESOURCE_BYTES) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t download_message_audio(const stored_message_t *message)
{
    ESP_RETURN_ON_FALSE(message && message->audio_id[0] &&
                            strlen(message->audio_sha256) == 64 &&
                            message->audio_size > 0 &&
                            message->audio_size <= MESSAGE_MAX_RESOURCE_BYTES,
                        ESP_ERR_INVALID_ARG, TAG, "invalid message audio metadata");
    char url[160];
    snprintf(url, sizeof(url), "%s%s", MESSAGE_AUDIO_PATH_PREFIX,
             message->audio_id);
    char destination[160];
    audio_path(message->audio_id, destination, sizeof(destination));
    if (file_sha256_matches(destination, message->audio_sha256,
                            message->audio_size)) {
        return ESP_OK;
    }
    char temporary[176];
    snprintf(temporary, sizeof(temporary), "%s.part", destination);
    unlink(temporary);
    ESP_RETURN_ON_ERROR(s_config.download(url, temporary, message->audio_size),
                        TAG, "download message audio");
    if (!file_sha256_matches(temporary, message->audio_sha256,
                             message->audio_size)) {
        unlink(temporary);
        return ESP_ERR_INVALID_CRC;
    }
    if (rename(temporary, destination) != 0) {
        unlink(temporary);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t play_wav_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    struct stat info;
    if (stat(path, &info) != 0 || info.st_size <= 44) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t header[MESSAGE_IO_BUFFER_BYTES];
    size_t header_len = fread(header, 1, sizeof(header), file);
    size_t pcm_offset = 0;
    size_t pcm_len = 0;
    if (!vibe_wav_pcm_stream_info(header, header_len, (size_t)info.st_size,
                                  16000, 1, 16, &pcm_offset, &pcm_len) ||
        fseek(file, (long)pcm_offset, SEEK_SET) != 0) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }
    esp_err_t err = vibe_audio_play_stream_begin();
    uint8_t buffer[MESSAGE_IO_BUFFER_BYTES];
    size_t remaining = pcm_len;
    while (err == ESP_OK && remaining > 0) {
        if (atomic_load(&s_play_cancel)) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        size_t request = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        size_t count = fread(buffer, 1, request, file);
        if (count == 0) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = vibe_audio_play_stream_write(buffer, count);
        remaining -= count;
    }
    esp_err_t end_err = vibe_audio_play_stream_end();
    fclose(file);
    return err == ESP_OK ? end_err : err;
}

static bool sync_interrupted(void)
{
    return atomic_load(&s_sync_paused) ||
           (s_config.audio_busy && s_config.audio_busy());
}

static void sync_once_inner(void)
{
    if (!s_config.request || !s_config.download ||
        !atomic_load(&s_storage_ready) || sync_interrupted()) return;
    const bool loading_older = atomic_exchange(&s_sync_load_older, false);
    const bool initial_load = !loading_older && s_message_count < 20;
    if (loading_older && (s_oldest_cursor == 0 || !s_history_has_more)) return;
    bool received_new = false;
    char path[112];
    if (loading_older) {
        snprintf(path, sizeof(path), "/device/messages/sync?before=%lu&limit=%u",
                 (unsigned long)s_oldest_cursor, MESSAGE_SYNC_BATCH_SIZE);
    } else {
        snprintf(path, sizeof(path), "/device/messages/sync?after=%lu&limit=%u%s",
                 (unsigned long)(initial_load ? 0 : s_cursor),
                 MESSAGE_SYNC_BATCH_SIZE,
                 initial_load ? "&bootstrap=3" : "");
    }
    char *response = malloc(MESSAGE_SYNC_RESPONSE_BYTES);
    if (!response) return;
    esp_err_t request_err = s_config.request("GET", path, NULL, response,
                                             MESSAGE_SYNC_RESPONSE_BYTES);
    if (request_err != ESP_OK) {
        ESP_LOGW(TAG, "message sync failed: %s", esp_err_to_name(request_err));
        free(response);
        return;
    }
    cJSON *root = cJSON_Parse(response);
    free(response);
    if (!root || sync_interrupted()) {
        cJSON_Delete(root);
        return;
    }
    cJSON *resources = cJSON_GetObjectItemCaseSensitive(root, "resources");
    if (cJSON_IsObject(resources) && !loading_older) {
        if (!s_system_resources_ready) {
            s_system_resources_ready = ensure_resource(
                cJSON_GetObjectItemCaseSensitive(resources, "notification"),
                MESSAGE_NOTIFY_PATH) == ESP_OK;
        }
        if (!s_font_resource_ready && !sync_interrupted()) {
            cJSON *font = cJSON_GetObjectItemCaseSensitive(resources, "font");
            char font_sha[65] = {0};
            copy_json_string(font, "sha256", font_sha, sizeof(font_sha));
            cJSON *font_size = cJSON_GetObjectItemCaseSensitive(font, "size");
            esp_err_t font_err = ensure_resource(font, MESSAGE_FONT_PATH);
            if (font_err == ESP_OK) {
                font_err = install_cached_font(MESSAGE_FONT_PATH, font_sha,
                    cJSON_IsNumber(font_size) ? (size_t)font_size->valuedouble : 0);
            }
            s_font_resource_ready = font_err == ESP_OK;
        }
    }
    bool index_dirty = false;
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "messages");
    cJSON *item;
    cJSON_ArrayForEach(item, items) {
        if (sync_interrupted()) break;
        stored_message_t message = {.read = false};
        cJSON *cursor = cJSON_GetObjectItemCaseSensitive(item, "cursor");
        if (!cJSON_IsNumber(cursor) || cursor->valuedouble <= 0) continue;
        message.cursor = (uint32_t)cursor->valuedouble;
        if (message_ref_index(message.cursor) >= 0) continue;
        copy_json_string(item, "title", message.title, sizeof(message.title));
        copy_json_string(item, "summary", message.summary, sizeof(message.summary));
        copy_json_string(item, "spoken_text", message.spoken_text, sizeof(message.spoken_text));
        if (!message.spoken_text[0]) strlcpy(message.spoken_text, message.summary,
                                             sizeof(message.spoken_text));
        if (parse_message_audio_metadata(item, &message) != ESP_OK) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        prune_messages();
        esp_err_t store_err = append_message_record(&message);
        xSemaphoreGive(s_lock);
        if (store_err != ESP_OK) break;
        if (atomic_load(&s_auto_session_active)) {
            auto_enqueue_message(message.cursor);
        }
        index_dirty = true;
        received_new |= !loading_older;
    }
    cJSON *has_more = cJSON_GetObjectItemCaseSensitive(root, "has_more");
    if (loading_older || initial_load) s_history_has_more = cJSON_IsTrue(has_more);
    cJSON_Delete(root);
    if (index_dirty) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_err_t save_err = save_index();
        s_loaded_page_start = SIZE_MAX;
        xSemaphoreGive(s_lock);
        if (save_err != ESP_OK) ESP_LOGW(TAG, "message index save failed: %s",
                                         esp_err_to_name(save_err));
    }
    if (s_cursor > 0 && !sync_interrupted() && !loading_older) {
        char body[48];
        snprintf(body, sizeof(body), "{\"cursor\":%lu}",
                 (unsigned long)s_cursor);
        char response[128];
        esp_err_t ack_err = s_config.request(
            "POST", "/device/messages/ack", body, response, sizeof(response));
        ESP_LOGI(TAG, "message ACK cursor=%lu result=%s",
                 (unsigned long)s_cursor, esp_err_to_name(ack_err));
    }
    if (received_new) (void)enqueue_action(MESSAGE_ACTION_PREPARE);
    if (received_new && s_config.activity) s_config.activity();
    if (received_new && !atomic_load(&s_auto_playing) &&
        access(MESSAGE_NOTIFY_PATH, R_OK) == 0 &&
        (!s_config.audio_busy || !s_config.audio_busy())) {
        (void)play_wav_file(MESSAGE_NOTIFY_PATH);
    }
    if (received_new && atomic_load(&s_auto_session_active) &&
        !atomic_load(&s_auto_paused)) {
        auto_request_playback();
    }
}

static void sync_once(void)
{
    if (atomic_load(&s_sync_paused)) return;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_sync_in_progress, &expected, true)) {
        return;
    }
    if (!atomic_load(&s_sync_paused)) sync_once_inner();
    atomic_store(&s_sync_in_progress, false);
    if (atomic_exchange(&s_auto_start_pending, false) &&
        atomic_load(&s_auto_session_active)) {
        auto_enqueue_unread_messages();
        auto_request_playback();
    }
}

static void sync_task(void *argument)
{
    (void)argument;
    spi_bus_config_t bus = {
        .mosi_io_num = VIBE_BOARD_PIN_SD_MOSI,
        .miso_io_num = VIBE_BOARD_PIN_SD_MISO,
        .sclk_io_num = VIBE_BOARD_PIN_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD SPI bus init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = VIBE_BOARD_PIN_SD_CS;
    slot.host_id = SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    ESP_LOGI(TAG, "mounting SD heap_free=%u heap_largest=%u stack_free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    err = esp_vfs_fat_sdspi_mount(MESSAGE_MOUNT_POINT, &host, &slot,
                                  &mount, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        vTaskDelete(NULL);
        return;
    }
    mkdir_if_missing(MESSAGE_ROOT);
    mkdir_if_missing(MESSAGE_AUDIO_DIR);
    mkdir_if_missing(MESSAGE_SYSTEM_DIR);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    load_index();
    size_t unread_count = 0;
    for (size_t index = 0; index < s_message_count; index++) {
        if (!s_message_refs[index].read) unread_count++;
    }
    atomic_store(&s_storage_ready, true);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "SD ready capacity=%lluMB messages=%u unread=%u cursor=%lu stack_free=%u heap_largest=%u",
             ((unsigned long long)card->csd.capacity * card->csd.sector_size /
                                  (1024 * 1024)),
             (unsigned)s_message_count, (unsigned)unread_count,
             (unsigned long)s_cursor,
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (s_message_count > 0) (void)enqueue_action(MESSAGE_ACTION_PREPARE);
    BaseType_t sync_result = xTaskCreatePinnedToCore(
        message_sync_task, "card_msg_sync", MESSAGE_TASK_STACK_BYTES,
        NULL, 1, &s_sync_task_handle, 0);
    if (sync_result != pdPASS) {
        ESP_LOGW(TAG, "message sync task create failed");
    }
    while (true) {
        message_action_t action;
        if (xQueueReceive(s_action_queue, &action, portMAX_DELAY) == pdTRUE) {
            handle_action(action);
        }
    }
}

static const lv_font_t *message_font(void)
{
    return s_chinese_font ? s_chinese_font : LV_FONT_DEFAULT;
}

static void ensure_message_layer(void)
{
    if (s_layer) return;
    s_layer = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_layer, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_layer, lv_color_hex(0x070b10), 0);
    lv_obj_set_style_border_width(s_layer, 0, 0);
    lv_obj_set_style_pad_all(s_layer, 0, 0);
    s_header = lv_label_create(s_layer);
    lv_obj_set_style_text_font(s_header, message_font(), 0);
    lv_obj_set_style_text_color(s_header, lv_color_hex(0xe5e7eb), 0);
    lv_obj_align(s_header, LV_ALIGN_TOP_LEFT, 10, 8);
    for (size_t index = 0; index < MESSAGE_PAGE_SIZE; index++) {
        s_tiles[index] = lv_obj_create(s_layer);
        lv_obj_set_style_bg_color(s_tiles[index], lv_color_hex(0x17202b), 0);
        lv_obj_set_style_radius(s_tiles[index], 5, 0);
        lv_obj_set_style_pad_all(s_tiles[index], 5, 0);
        s_unread_dots[index] = lv_obj_create(s_tiles[index]);
        lv_obj_set_size(s_unread_dots[index], 6, 6);
        lv_obj_align(s_unread_dots[index], LV_ALIGN_TOP_LEFT, 0, 4);
        lv_obj_set_style_radius(s_unread_dots[index], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_unread_dots[index],
                                  lv_color_hex(0x38bdf8), 0);
        lv_obj_set_style_border_width(s_unread_dots[index], 0, 0);
        lv_obj_set_style_pad_all(s_unread_dots[index], 0, 0);
        s_labels[index] = lv_label_create(s_tiles[index]);
        lv_label_set_long_mode(s_labels[index], LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_labels[index], LV_PCT(90));
        lv_obj_align(s_labels[index], LV_ALIGN_TOP_LEFT, 10, 0);
        lv_obj_set_style_text_color(s_labels[index], lv_color_hex(0xf3f4f6), 0);
    }
    s_blink_timer = lv_timer_create(blink_timer, 500, NULL);

    s_detail_view = lv_obj_create(s_layer);
    lv_obj_set_size(s_detail_view, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_detail_view, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_detail_view, lv_color_hex(0x070b10), 0);
    lv_obj_set_style_border_width(s_detail_view, 0, 0);
    lv_obj_set_style_pad_all(s_detail_view, 10, 0);
    lv_obj_set_scroll_dir(s_detail_view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_detail_view, LV_SCROLLBAR_MODE_OFF);
    s_detail_label = lv_label_create(s_detail_view);
    lv_obj_set_width(s_detail_label, 220);
    lv_label_set_long_mode(s_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_detail_label, message_font(), 0);
    lv_obj_set_style_text_color(s_detail_label, lv_color_hex(0xf3f4f6), 0);
    lv_obj_align(s_detail_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    s_detail_scroll_timer = lv_timer_create(
        detail_scroll_timer, MESSAGE_DETAIL_SCROLL_PERIOD_MS, NULL);
}

static int64_t message_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void detail_scroll_timer(lv_timer_t *timer)
{
    (void)timer;
    if (!atomic_load(&s_detail_active) || !s_detail_view) return;
    int64_t now = message_now_ms();
    if (now < s_detail_scroll_start_ms || now < s_detail_manual_until_ms ||
        lv_obj_get_scroll_bottom(s_detail_view) <= 0) {
        return;
    }
    lv_obj_scroll_to_y(s_detail_view,
                       lv_obj_get_scroll_y(s_detail_view) + 1,
                       LV_ANIM_OFF);
}

static void show_detail_view(const char *text)
{
    lv_obj_add_flag(s_header, LV_OBJ_FLAG_HIDDEN);
    for (size_t tile = 0; tile < MESSAGE_PAGE_SIZE; tile++) {
        lv_obj_add_flag(s_tiles[tile], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_font(s_detail_label, message_font(), 0);
    lv_label_set_text(s_detail_label, text && text[0] ? text : "消息内容为空");
    lv_obj_clear_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_detail_view);
    lv_obj_scroll_to_y(s_detail_view, 0, LV_ANIM_OFF);
    s_detail_scroll_start_ms = message_now_ms() + MESSAGE_DETAIL_START_DELAY_MS;
    s_detail_manual_until_ms = 0;
    atomic_store(&s_detail_active, true);
}

static void hide_detail_view(void)
{
    atomic_store(&s_detail_active, false);
    if (s_detail_view) lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_header, LV_OBJ_FLAG_HIDDEN);
    render_messages();
}

static void scroll_detail_manually(int delta)
{
    display_lock();
    if (s_detail_view) {
        int32_t target = lv_obj_get_scroll_y(s_detail_view) + delta;
        if (target < 0) target = 0;
        lv_obj_scroll_to_y(s_detail_view, target, LV_ANIM_OFF);
        s_detail_manual_until_ms =
            message_now_ms() + MESSAGE_DETAIL_MANUAL_PAUSE_MS;
    }
    display_unlock();
}

static bool load_visible_page(size_t page_start)
{
    if (page_start == s_loaded_page_start) return true;
    memset(s_messages, 0, sizeof(s_messages));
    for (size_t slot = 0; slot < MESSAGE_PAGE_SIZE; slot++) {
        size_t index = page_start + slot;
        if (index >= s_message_count) break;
        if (!load_message_record(s_message_refs[index].cursor, &s_messages[slot])) {
            ESP_LOGW(TAG, "message record missing cursor=%lu",
                     (unsigned long)s_message_refs[index].cursor);
            return false;
        }
        s_messages[slot].read = s_message_refs[index].read;
    }
    s_loaded_page_start = page_start;
    return true;
}

static void render_messages(void)
{
    if (!s_layer) return;
    lv_obj_set_size(s_layer, LV_PCT(100), LV_PCT(100));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t count = s_message_count;
    if (count > 0 && s_selected >= count) s_selected = count - 1;
    const size_t page_start = (s_selected / MESSAGE_PAGE_SIZE) * MESSAGE_PAGE_SIZE;
    const size_t page_count = count > page_start
                                  ? ((count - page_start) < MESSAGE_PAGE_SIZE
                                         ? count - page_start
                                         : MESSAGE_PAGE_SIZE)
                                  : 0;
    (void)load_visible_page(page_start);
    char header[48];
    if (s_chinese_font) {
        snprintf(header, sizeof(header), "消息 %u  %u/%u",
                 (unsigned)count,
                 count ? (unsigned)(page_start / MESSAGE_PAGE_SIZE + 1) : 0,
                 (unsigned)((count + MESSAGE_PAGE_SIZE - 1) /
                            MESSAGE_PAGE_SIZE));
    } else {
        snprintf(header, sizeof(header), "MESSAGES %u  %u/%u",
                 (unsigned)count,
                 count ? (unsigned)(page_start / MESSAGE_PAGE_SIZE + 1) : 0,
                 (unsigned)((count + MESSAGE_PAGE_SIZE - 1) /
                            MESSAGE_PAGE_SIZE));
    }
    lv_obj_set_style_text_font(s_header, message_font(), 0);
    lv_label_set_text(s_header, header);
    const lv_font_t *font = message_font();
    for (size_t tile = 0; tile < MESSAGE_PAGE_SIZE; tile++) {
        if (tile >= page_count) {
            lv_obj_add_flag(s_tiles[tile], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_tiles[tile], LV_OBJ_FLAG_HIDDEN);
        size_t index = page_start + tile;
        const stored_message_t *message = &s_messages[tile];
        char title[40];
        size_t title_len = strlen(message->title);
        bool title_truncated = title_len > 30;
        if (title_truncated) {
            title_len = 30;
            while (title_len > 0 &&
                   ((unsigned char)message->title[title_len] & 0xc0) == 0x80) {
                title_len--;
            }
        }
        if (s_chinese_font) {
            memcpy(title, message->title, title_len);
            title[title_len] = '\0';
            if (title_truncated) strlcat(title, "...", sizeof(title));
        } else {
            strlcpy(title, "FONT SYNCING", sizeof(title));
        }
        if (message->read) {
            lv_obj_add_flag(s_unread_dots[tile], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_unread_dots[tile], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_text_font(s_labels[tile], font, 0);
        lv_label_set_text(s_labels[tile], title);
        lv_obj_set_style_border_color(
            s_tiles[tile], index == s_selected ? lv_color_hex(0x38bdf8)
                                               : lv_color_hex(0x39414d), 0);
        lv_obj_set_style_border_width(s_tiles[tile], index == s_selected ? 2 : 1, 0);
        int width = page_count == 1 ? 220 : 106;
        int height = page_count <= 2 ? 94 : 45;
        int column = page_count == 1 ? 0 : (int)(tile % 2);
        int row = page_count <= 2 ? 0 : (int)(tile / 2);
        lv_obj_set_size(s_tiles[tile], width, height);
        lv_obj_align(s_tiles[tile], LV_ALIGN_TOP_LEFT,
                     10 + column * 114, 32 + row * 49);
    }
    s_rendered_count = count;
    s_rendered_cursor = s_cursor;
    xSemaphoreGive(s_lock);
}

static void blink_timer(lv_timer_t *timer)
{
    (void)timer;
    if (!atomic_load(&s_active)) return;
    s_blink_on = !s_blink_on;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t page_start = (s_selected / MESSAGE_PAGE_SIZE) * MESSAGE_PAGE_SIZE;
    for (size_t tile = 0; tile < MESSAGE_PAGE_SIZE; tile++) {
        size_t index = page_start + tile;
        if (index < s_message_count && !s_message_refs[index].read) {
            lv_obj_set_style_opa(s_unread_dots[tile],
                                 s_blink_on ? LV_OPA_COVER : LV_OPA_20, 0);
        }
    }
    xSemaphoreGive(s_lock);
}

static void open_messages(void)
{
    esp_err_t font_err = load_cached_font();
    if (font_err != ESP_OK) {
        ESP_LOGW(TAG, "message font load skipped: %s",
                 esp_err_to_name(font_err));
    }
    display_lock();
    if (s_config.set_landscape && s_config.set_landscape(true) != ESP_OK) {
        display_unlock();
        atomic_store(&s_active, false);
        return;
    }
    ensure_message_layer();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_message_count > 0) s_selected = 0;
    const size_t count = s_message_count;
    const size_t selected = s_selected;
    const uint32_t cursor = s_cursor;
    xSemaphoreGive(s_lock);
    atomic_store(&s_active, true);
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    if (s_rendered_count != count || s_rendered_cursor != cursor) {
        render_messages();
    }
    /* Cached messages are already on SD. Start their unread queue immediately
     * instead of making playback depend on the first network refresh. */
    auto_enqueue_unread_messages();
    auto_request_playback();
    ESP_LOGI(TAG, "message center rendered count=%u selected=%u",
             (unsigned)count, (unsigned)selected);
    display_unlock();
}

static void prepare_messages(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_message_count > 0) s_selected = 0;
    xSemaphoreGive(s_lock);
    s_rendered_count = SIZE_MAX;
    s_rendered_cursor = UINT32_MAX;
    ESP_LOGI(TAG, "message center prepared count=%u cursor=%lu",
             (unsigned)s_message_count, (unsigned long)s_cursor);
}

static void close_messages(void)
{
    atomic_store(&s_active, false);
    atomic_store(&s_detail_active, false);
    atomic_store(&s_auto_session_active, false);
    atomic_store(&s_auto_paused, false);
    atomic_store(&s_auto_start_pending, false);
    atomic_store(&s_play_cancel, true);
    atomic_store(&s_enter_single_pending, false);
    s_space_last_press_ms = 0;
    s_enter_last_press_ms = 0;
    if (s_enter_single_timer) (void)xTimerStop(s_enter_single_timer, 0);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_auto_queue_count = 0;
    xSemaphoreGive(s_lock);
    display_lock();
    if (s_blink_timer) {
        lv_timer_delete(s_blink_timer);
        s_blink_timer = NULL;
    }
    if (s_detail_scroll_timer) {
        lv_timer_delete(s_detail_scroll_timer);
        s_detail_scroll_timer = NULL;
    }
    if (s_layer) {
        lv_obj_delete(s_layer);
        s_layer = NULL;
        s_header = NULL;
        s_detail_view = NULL;
        s_detail_label = NULL;
        memset(s_tiles, 0, sizeof(s_tiles));
        memset(s_unread_dots, 0, sizeof(s_unread_dots));
        memset(s_labels, 0, sizeof(s_labels));
    }
    if (s_config.set_landscape) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(s_config.set_landscape(false));
    }
    display_unlock();
    release_cached_font();
    s_rendered_count = SIZE_MAX;
    s_rendered_cursor = UINT32_MAX;
    if (s_config.restore_home) s_config.restore_home();
}

static void mark_message_read(uint32_t cursor)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int ref_index = message_ref_index(cursor);
    if (ref_index >= 0 && !s_message_refs[ref_index].read) {
        s_message_refs[ref_index].read = true;
        if (save_index() != ESP_OK) {
            s_message_refs[ref_index].read = false;
            ESP_LOGW(TAG, "message read state save failed cursor=%lu",
                     (unsigned long)cursor);
        } else {
            s_loaded_page_start = SIZE_MAX;
            ESP_LOGI(TAG, "message marked read cursor=%lu",
                     (unsigned long)cursor);
        }
    }
    xSemaphoreGive(s_lock);
}

static bool auto_queue_contains_locked(uint32_t cursor)
{
    for (size_t index = 0; index < s_auto_queue_count; index++) {
        if (s_auto_queue[index] == cursor) return true;
    }
    return false;
}

static void auto_enqueue_message(uint32_t cursor)
{
    if (!cursor) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int ref_index = message_ref_index(cursor);
    if (ref_index >= 0 && !s_message_refs[ref_index].read &&
        s_auto_queue_count < MESSAGE_MAX_COUNT &&
        !auto_queue_contains_locked(cursor)) {
        s_auto_queue[s_auto_queue_count++] = cursor;
        ESP_LOGI(TAG, "message auto queued cursor=%lu count=%u",
                 (unsigned long)cursor, (unsigned)s_auto_queue_count);
    }
    xSemaphoreGive(s_lock);
}

static void auto_enqueue_unread_messages(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t index = 0;
         index < s_message_count && s_auto_queue_count < MESSAGE_MAX_COUNT;
         index++) {
        uint32_t cursor = s_message_refs[index].cursor;
        if (!s_message_refs[index].read && !auto_queue_contains_locked(cursor)) {
            s_auto_queue[s_auto_queue_count++] = cursor;
        }
    }
    size_t count = s_auto_queue_count;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "message auto queue prepared count=%u", (unsigned)count);
}

static bool auto_peek_message(uint32_t *cursor)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    while (s_auto_queue_count > 0) {
        uint32_t candidate = s_auto_queue[0];
        int ref_index = message_ref_index(candidate);
        if (ref_index >= 0 && !s_message_refs[ref_index].read) {
            *cursor = candidate;
            found = true;
            break;
        }
        memmove(s_auto_queue, &s_auto_queue[1],
                (s_auto_queue_count - 1) * sizeof(s_auto_queue[0]));
        s_auto_queue_count--;
    }
    xSemaphoreGive(s_lock);
    return found;
}

static void auto_remove_message(uint32_t cursor)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t index = 0; index < s_auto_queue_count; index++) {
        if (s_auto_queue[index] != cursor) continue;
        memmove(&s_auto_queue[index], &s_auto_queue[index + 1],
                (s_auto_queue_count - index - 1) * sizeof(s_auto_queue[0]));
        s_auto_queue_count--;
        break;
    }
    xSemaphoreGive(s_lock);
}

static bool auto_queue_has_messages(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool has_messages = s_auto_queue_count > 0;
    xSemaphoreGive(s_lock);
    return has_messages;
}

static void auto_request_playback(void)
{
    if (!atomic_load(&s_auto_session_active) ||
        atomic_load(&s_auto_paused)) return;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_auto_play_queued, &expected, true)) {
        return;
    }
    if (!enqueue_action(MESSAGE_ACTION_PLAY_UNREAD)) {
        atomic_store(&s_auto_play_queued, false);
    }
}

static esp_err_t play_message(uint32_t cursor, bool mark_on_open,
                              bool mark_on_complete)
{
    if (s_config.audio_busy && s_config.audio_busy()) {
        ESP_LOGW(TAG, "message playback skipped: audio busy");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int ref_index = message_ref_index(cursor);
    if (ref_index < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    stored_message_t message;
    char spoken_text[sizeof(s_messages[0].spoken_text)];
    if (!load_message_record(cursor, &message)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(spoken_text, message.spoken_text,
            sizeof(spoken_text));
    if (!spoken_text[0]) {
        strlcpy(spoken_text, message.summary,
                sizeof(spoken_text));
    }
    xSemaphoreGive(s_lock);
    /* Text is already on SD: present it immediately instead of making the
     * user wait for the optional audio file to cross Wi-Fi. */
    atomic_store(&s_play_cancel, false);
    display_lock();
    show_detail_view(spoken_text);
    display_unlock();
    if (mark_on_open) mark_message_read(cursor);
    esp_err_t download_err = download_message_audio(&message);
    if (download_err != ESP_OK) {
        ESP_LOGW(TAG, "message audio download failed cursor=%lu result=%s",
                 (unsigned long)cursor, esp_err_to_name(download_err));
        display_lock();
        hide_detail_view();
        display_unlock();
        return download_err;
    }
    char path[160];
    audio_path(message.audio_id, path, sizeof(path));
    ESP_LOGI(TAG, "message playback start cursor=%lu",
             (unsigned long)cursor);
    esp_err_t err = play_wav_file(path);
    ESP_LOGI(TAG, "message playback cursor=%lu result=%s",
             (unsigned long)cursor, esp_err_to_name(err));
    display_lock();
    hide_detail_view();
    display_unlock();
    if (err == ESP_OK && mark_on_complete) mark_message_read(cursor);
    return err;
}

static void play_selected(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t cursor = s_selected < s_message_count
                          ? s_message_refs[s_selected].cursor
                          : 0;
    xSemaphoreGive(s_lock);
    if (!cursor) return;
    atomic_store(&s_play_cancel, false);
    esp_err_t err = play_message(cursor, true, false);
    ESP_LOGI(TAG, "message playback cursor=%lu result=%s",
             (unsigned long)cursor, esp_err_to_name(err));
    atomic_store(&s_play_cancel, false);
}

static void play_unread_messages(void)
{
    atomic_store(&s_auto_playing, true);
    while (atomic_load(&s_auto_session_active) && atomic_load(&s_active) &&
           !atomic_load(&s_auto_paused)) {
        if (s_config.audio_busy && s_config.audio_busy()) break;
        uint32_t cursor = 0;
        if (!auto_peek_message(&cursor)) break;
        atomic_store(&s_play_cancel, false);
        ESP_LOGI(TAG, "message auto playback start cursor=%lu",
                 (unsigned long)cursor);
        esp_err_t err = play_message(cursor, false, true);
        bool cancelled = atomic_load(&s_play_cancel) ||
                         atomic_load(&s_auto_paused) ||
                         !atomic_load(&s_active);
        if (cancelled) {
            ESP_LOGI(TAG, "message auto playback paused cursor=%lu",
                     (unsigned long)cursor);
            break;
        }
        auto_remove_message(cursor);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "message auto playback skipped cursor=%lu result=%s",
                     (unsigned long)cursor, esp_err_to_name(err));
        }
    }
    atomic_store(&s_auto_playing, false);
    atomic_store(&s_play_cancel, false);
    atomic_store(&s_auto_play_queued, false);
    if (!atomic_load(&s_auto_paused) && auto_queue_has_messages() &&
        (!s_config.audio_busy || !s_config.audio_busy())) {
        auto_request_playback();
    }
}

static void move_selection(message_action_t action)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t old_selected = s_selected;
    uint32_t old_cursor = s_selected < s_message_count
                              ? s_message_refs[s_selected].cursor
                              : 0;
    bool load_older = false;
    if (s_message_count > 0) {
        if (action == MESSAGE_ACTION_LEFT && s_selected > 0) s_selected--;
        if (action == MESSAGE_ACTION_RIGHT && s_selected + 1 < s_message_count) {
            s_selected++;
        } else if (action == MESSAGE_ACTION_RIGHT) {
            load_older = s_history_has_more;
        }
        if (action == MESSAGE_ACTION_UP && s_selected >= 2) s_selected -= 2;
        if (action == MESSAGE_ACTION_DOWN && s_selected + 2 < s_message_count) {
            s_selected += 2;
        } else if (action == MESSAGE_ACTION_DOWN) {
            load_older = s_history_has_more;
        }
    }
    uint32_t new_cursor = s_selected < s_message_count
                              ? s_message_refs[s_selected].cursor
                              : 0;
    size_t page = s_selected / MESSAGE_PAGE_SIZE + 1;
    size_t new_selected = s_selected;
    xSemaphoreGive(s_lock);

    display_lock();
    if (old_selected / MESSAGE_PAGE_SIZE !=
        new_selected / MESSAGE_PAGE_SIZE) {
        render_messages();
    } else {
        size_t page_start =
            (new_selected / MESSAGE_PAGE_SIZE) * MESSAGE_PAGE_SIZE;
        for (size_t tile = 0; tile < MESSAGE_PAGE_SIZE; tile++) {
            size_t index = page_start + tile;
            lv_obj_set_style_border_color(
                s_tiles[tile],
                index == new_selected ? lv_color_hex(0x38bdf8)
                                      : lv_color_hex(0x39414d),
                0);
            lv_obj_set_style_border_width(
                s_tiles[tile], index == new_selected ? 2 : 1, 0);
        }
    }
    display_unlock();
    ESP_LOGI(TAG,
             "message selection action=%d cursor=%lu->%lu page=%u stack_free=%u",
             (int)action, (unsigned long)old_cursor, (unsigned long)new_cursor,
             (unsigned)page,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (load_older && s_sync_task_handle) {
        atomic_store(&s_sync_load_older, true);
        xTaskNotifyGive(s_sync_task_handle);
        ESP_LOGI(TAG, "message history load requested before=%lu",
                 (unsigned long)s_oldest_cursor);
    }
}

static void handle_action(message_action_t action)
{
    if (action != MESSAGE_ACTION_PREPARE && s_config.activity) {
        s_config.activity();
    }
    switch (action) {
    case MESSAGE_ACTION_PREPARE:
        if (atomic_load(&s_active)) {
            display_lock();
            render_messages();
            display_unlock();
        } else {
            prepare_messages();
        }
        break;
    case MESSAGE_ACTION_OPEN:
        open_messages();
        break;
    case MESSAGE_ACTION_CLOSE:
        close_messages();
        break;
    case MESSAGE_ACTION_LEFT:
    case MESSAGE_ACTION_RIGHT:
    case MESSAGE_ACTION_UP:
    case MESSAGE_ACTION_DOWN:
        move_selection(action);
        break;
    case MESSAGE_ACTION_PLAY:
        ESP_LOGI(TAG, "message play action stack_free=%u",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        play_selected();
        atomic_store(&s_play_queued, false);
        break;
    case MESSAGE_ACTION_PLAY_UNREAD:
        ESP_LOGI(TAG, "message auto play action stack_free=%u",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        play_unread_messages();
        break;
    case MESSAGE_ACTION_RELEASE:
        close_messages();
        atomic_store(&s_play_cancel, false);
        if (s_release_done) xSemaphoreGive(s_release_done);
        break;
    }
}

esp_err_t vibe_cardputer_messages_init(
    const vibe_cardputer_messages_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && config->request && config->download,
                        ESP_ERR_INVALID_ARG, TAG, "invalid config");
    s_config = *config;
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "message lock");
    s_action_queue = xQueueCreate(MESSAGE_ACTION_QUEUE_LENGTH,
                                  sizeof(message_action_t));
    ESP_RETURN_ON_FALSE(s_action_queue, ESP_ERR_NO_MEM, TAG,
                        "message action queue");
    s_release_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_release_done, ESP_ERR_NO_MEM, TAG,
                        "message release semaphore");
    atomic_store(&s_storage_ready, false);
    atomic_store(&s_active, false);
    atomic_store(&s_play_queued, false);
    atomic_store(&s_auto_session_active, false);
    atomic_store(&s_auto_playing, false);
    atomic_store(&s_auto_play_queued, false);
    atomic_store(&s_auto_paused, false);
    atomic_store(&s_auto_start_pending, false);
    atomic_store(&s_enter_single_pending, false);
    atomic_store(&s_sync_paused, false);
    atomic_store(&s_sync_in_progress, false);
    atomic_store(&s_sync_load_older, false);
    s_enter_single_timer = xTimerCreate("msg_enter", pdMS_TO_TICKS(
        MESSAGE_DOUBLE_PRESS_WINDOW_MS), pdFALSE, NULL, enter_single_timer_cb);
    ESP_RETURN_ON_FALSE(s_enter_single_timer, ESP_ERR_NO_MEM, TAG,
                        "message enter timer");
    return ESP_OK;
}

esp_err_t vibe_cardputer_messages_start(void)
{
    BaseType_t sync_result = xTaskCreatePinnedToCore(
        sync_task, "card_messages", MESSAGE_TASK_STACK_BYTES,
        NULL, 2, NULL, 0);
    return sync_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool vibe_cardputer_messages_handle_key(const vibe_key_event_t *event)
{
    if (!event) return false;
    const bool entering_messages = !atomic_load(&s_active) && event->pressed &&
        event->fn && event->row == 3 && event->column == 8;
    if (event->pressed && (atomic_load(&s_active) || entering_messages) &&
        s_config.activity) {
        /* A message-page key is also the wake gesture after automatic display
         * off. Wake before queuing the action so the first key is not lost. */
        s_config.activity();
    }
    if (entering_messages) {
        bool storage_ready = atomic_load(&s_storage_ready);
        ESP_LOGI(TAG, "Fn+N shortcut storage_ready=%d",
                 storage_ready ? 1 : 0);
        if (!storage_ready) return true;
        atomic_store(&s_active, true);
        atomic_store(&s_auto_session_active, true);
        atomic_store(&s_auto_paused, false);
        atomic_store(&s_auto_start_pending, true);
        s_space_last_press_ms = 0;
        s_enter_last_press_ms = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_auto_queue_count = 0;
        xSemaphoreGive(s_lock);
        if (!enqueue_action(MESSAGE_ACTION_OPEN)) {
            atomic_store(&s_active, false);
            atomic_store(&s_auto_session_active, false);
        } else if (s_sync_task_handle) {
            /* Refresh only after the user deliberately enters the message
             * center. This prevents all message HTTP traffic on the home UI. */
            xTaskNotifyGive(s_sync_task_handle);
        }
        return true;
    }
    if (!atomic_load(&s_active)) return false;
    if (event->pressed && event->character == ' ') {
        int64_t now = message_now_ms();
        if (now - s_space_last_press_ms <= MESSAGE_DOUBLE_PRESS_WINDOW_MS) {
            s_space_last_press_ms = 0;
            atomic_store(&s_auto_paused, true);
            atomic_store(&s_play_cancel, true);
            ESP_LOGI(TAG, "message auto playback paused by double space");
        } else {
            s_space_last_press_ms = now;
        }
        return true;
    }
    if (event->pressed && event->key == VIBE_KEY_ENTER) {
        int64_t now = message_now_ms();
        if (now - s_enter_last_press_ms <= MESSAGE_DOUBLE_PRESS_WINDOW_MS) {
            s_enter_last_press_ms = 0;
            atomic_store(&s_enter_single_pending, false);
            if (s_enter_single_timer) (void)xTimerStop(s_enter_single_timer, 0);
            atomic_store(&s_auto_paused, false);
            atomic_store(&s_play_cancel, false);
            auto_enqueue_unread_messages();
            auto_request_playback();
            ESP_LOGI(TAG, "message auto playback resumed by double enter");
        } else {
            s_enter_last_press_ms = now;
            atomic_store(&s_enter_single_pending, true);
            if (s_enter_single_timer) {
                (void)xTimerReset(s_enter_single_timer, 0);
            }
        }
        return true;
    }
    if (atomic_load(&s_detail_active)) {
        if (!event->pressed) return true;
        if (event->key == VIBE_KEY_ESCAPE) {
            atomic_store(&s_play_cancel, true);
            if (atomic_load(&s_auto_playing)) {
                atomic_store(&s_auto_paused, true);
            }
        } else if (event->key == VIBE_KEY_UP) {
            scroll_detail_manually(-MESSAGE_DETAIL_MANUAL_STEP);
        } else if (event->key == VIBE_KEY_DOWN) {
            scroll_detail_manually(MESSAGE_DETAIL_MANUAL_STEP);
        }
        return true;
    }
    if (!event->pressed) return true;
    message_action_t action;
    if (event->key == VIBE_KEY_ESCAPE) {
        action = MESSAGE_ACTION_CLOSE;
    } else if (event->key == VIBE_KEY_LEFT) {
        action = MESSAGE_ACTION_LEFT;
    } else if (event->key == VIBE_KEY_RIGHT) {
        action = MESSAGE_ACTION_RIGHT;
    } else if (event->key == VIBE_KEY_UP) {
        action = MESSAGE_ACTION_UP;
    } else if (event->key == VIBE_KEY_DOWN) {
        action = MESSAGE_ACTION_DOWN;
    } else {
        return true;
    }
    if (action == MESSAGE_ACTION_CLOSE) {
        atomic_store(&s_active, false);
        if (!enqueue_action(action)) atomic_store(&s_active, true);
    } else {
        (void)enqueue_action(action);
    }
    return true;
}

bool vibe_cardputer_messages_active(void)
{
    return atomic_load(&s_active);
}

bool vibe_cardputer_messages_storage_ready(void)
{
    return atomic_load(&s_storage_ready);
}

bool vibe_cardputer_messages_pause_sync(uint32_t timeout_ms)
{
    atomic_store(&s_sync_paused, true);
    const int64_t deadline_ms = message_now_ms() + timeout_ms;
    while (atomic_load(&s_sync_in_progress)) {
        if (message_now_ms() >= deadline_ms) {
            /* The in-flight request has a short timeout and will observe the
             * pause before doing more work. Recording must never be rejected
             * merely because an old sync request is winding down. */
            ESP_LOGW(TAG, "message sync yielding to recording");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "message sync paused for recording");
    return true;
}

void vibe_cardputer_messages_resume_sync(void)
{
    atomic_store(&s_sync_paused, false);
    ESP_LOGI(TAG, "message sync resumed");
}

void vibe_cardputer_messages_release_display_resources(void)
{
    if (!atomic_load(&s_active) && !s_layer && !s_chinese_font) return;
    atomic_store(&s_play_cancel, true);
    atomic_store(&s_active, false);
    while (xSemaphoreTake(s_release_done, 0) == pdTRUE) {}
    const message_action_t action = MESSAGE_ACTION_RELEASE;
    if (xQueueSend(s_action_queue, &action, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGE(TAG, "message release action queue full");
        return;
    }
    if (xSemaphoreTake(s_release_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "message release timed out");
    }
}

#else

esp_err_t vibe_cardputer_messages_init(
    const vibe_cardputer_messages_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t vibe_cardputer_messages_start(void) { return ESP_ERR_NOT_SUPPORTED; }
bool vibe_cardputer_messages_handle_key(const vibe_key_event_t *event)
{
    (void)event;
    return false;
}
bool vibe_cardputer_messages_active(void) { return false; }
bool vibe_cardputer_messages_storage_ready(void) { return false; }
bool vibe_cardputer_messages_pause_sync(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true;
}
void vibe_cardputer_messages_resume_sync(void) {}
void vibe_cardputer_messages_release_display_resources(void) {}

#endif
