#include "vibe_cardputer_messages.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
#define MESSAGE_FONT_PATH MESSAGE_SYSTEM_DIR "/DroidSansFallback.ttf"
#define MESSAGE_NOTIFY_PATH MESSAGE_SYSTEM_DIR "/F1_New_SMS.wav"
#define MESSAGE_MAX_COUNT 100
#define MESSAGE_PAGE_SIZE 4
#define MESSAGE_SYNC_RESPONSE_BYTES (24 * 1024)
#define MESSAGE_MAX_RESOURCE_BYTES (8 * 1024 * 1024)
#define MESSAGE_SYNC_INTERVAL_MS 10000
#define MESSAGE_TASK_STACK_BYTES 12288
#define MESSAGE_RESOURCE_PATH_PREFIX "/device/messages/resource?"
#define MESSAGE_AUDIO_PATH_PREFIX "/device/messages/resource?kind=audio&id="

typedef struct {
    uint32_t cursor;
    bool read;
    char title[64];
    char summary[160];
    char audio_id[32];
    char audio_sha256[65];
    size_t audio_size;
} stored_message_t;

static const char *TAG = "card_messages";
static vibe_cardputer_messages_config_t s_config;
static stored_message_t s_messages[MESSAGE_MAX_COUNT];
static size_t s_message_count;
static uint32_t s_cursor;
static SemaphoreHandle_t s_lock;
static bool s_storage_ready;
static bool s_active;
static size_t s_selected;
static lv_obj_t *s_layer;
static lv_obj_t *s_header;
static lv_obj_t *s_tiles[MESSAGE_PAGE_SIZE];
static lv_obj_t *s_labels[MESSAGE_PAGE_SIZE];
static lv_timer_t *s_blink_timer;
static bool s_blink_on;
static lv_font_t *s_chinese_font;

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
    uint8_t buffer[2048];
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

static void load_index(void)
{
    FILE *file = fopen(MESSAGE_INDEX_PATH, "rb");
    if (!file) return;
    char line[1024];
    while (s_message_count < MESSAGE_MAX_COUNT && fgets(line, sizeof(line), file)) {
        cJSON *item = cJSON_Parse(line);
        if (!item) continue;
        stored_message_t *message = &s_messages[s_message_count];
        memset(message, 0, sizeof(*message));
        cJSON *cursor = cJSON_GetObjectItemCaseSensitive(item, "cursor");
        cJSON *read = cJSON_GetObjectItemCaseSensitive(item, "read");
        cJSON *size = cJSON_GetObjectItemCaseSensitive(item, "audio_size");
        if (cJSON_IsNumber(cursor) && cursor->valuedouble > 0) {
            message->cursor = (uint32_t)cursor->valuedouble;
            message->read = cJSON_IsTrue(read);
            message->audio_size = cJSON_IsNumber(size) ? (size_t)size->valuedouble : 0;
            copy_json_string(item, "title", message->title, sizeof(message->title));
            copy_json_string(item, "summary", message->summary, sizeof(message->summary));
            copy_json_string(item, "audio_id", message->audio_id, sizeof(message->audio_id));
            copy_json_string(item, "audio_sha256", message->audio_sha256,
                             sizeof(message->audio_sha256));
            if (message->cursor > s_cursor) s_cursor = message->cursor;
            s_message_count++;
        }
        cJSON_Delete(item);
    }
    fclose(file);
}

static esp_err_t save_index(void)
{
    FILE *file = fopen(MESSAGE_INDEX_TMP, "wb");
    if (!file) return ESP_FAIL;
    for (size_t index = 0; index < s_message_count; index++) {
        const stored_message_t *message = &s_messages[index];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "cursor", message->cursor);
        cJSON_AddBoolToObject(item, "read", message->read);
        cJSON_AddStringToObject(item, "title", message->title);
        cJSON_AddStringToObject(item, "summary", message->summary);
        cJSON_AddStringToObject(item, "audio_id", message->audio_id);
        cJSON_AddStringToObject(item, "audio_sha256", message->audio_sha256);
        cJSON_AddNumberToObject(item, "audio_size", (double)message->audio_size);
        char *line = cJSON_PrintUnformatted(item);
        cJSON_Delete(item);
        if (!line || fprintf(file, "%s\n", line) < 0) {
            cJSON_free(line);
            fclose(file);
            unlink(MESSAGE_INDEX_TMP);
            return ESP_FAIL;
        }
        cJSON_free(line);
    }
    if (fclose(file) != 0 || rename(MESSAGE_INDEX_TMP, MESSAGE_INDEX_PATH) != 0) {
        unlink(MESSAGE_INDEX_TMP);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void audio_path(const char *audio_id, char *path, size_t path_size)
{
    snprintf(path, path_size, MESSAGE_AUDIO_DIR "/%s.wav", audio_id);
}

static void remove_message_at(size_t index)
{
    if (index >= s_message_count) return;
    char path[160];
    audio_path(s_messages[index].audio_id, path, sizeof(path));
    unlink(path);
    if (index + 1 < s_message_count) {
        memmove(&s_messages[index], &s_messages[index + 1],
                (s_message_count - index - 1) * sizeof(s_messages[0]));
    }
    s_message_count--;
}

static void prune_messages(void)
{
    while (s_message_count >= MESSAGE_MAX_COUNT) {
        size_t remove_index = 0;
        for (size_t index = 0; index < s_message_count; index++) {
            if (s_messages[index].read) {
                remove_index = index;
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

static esp_err_t download_message_audio(cJSON *item, stored_message_t *message)
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
    uint8_t header[512];
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
    uint8_t buffer[2048];
    size_t remaining = pcm_len;
    while (err == ESP_OK && remaining > 0) {
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

static void sync_once(void)
{
    if (!s_config.request || !s_config.download || !s_storage_ready) return;
    if (s_config.audio_busy && s_config.audio_busy()) return;
    bool received_new = false;
    for (int page = 0; page < 6; page++) {
        char path[96];
        snprintf(path, sizeof(path), "/device/messages/sync?after=%lu&limit=20",
                 (unsigned long)s_cursor);
        char *response = malloc(MESSAGE_SYNC_RESPONSE_BYTES);
        if (!response) return;
        esp_err_t request_err = s_config.request("GET", path, NULL, response,
                                                 MESSAGE_SYNC_RESPONSE_BYTES);
        if (request_err != ESP_OK) {
            free(response);
            return;
        }
        cJSON *root = cJSON_Parse(response);
        free(response);
        if (!root) return;
        cJSON *resources = cJSON_GetObjectItemCaseSensitive(root, "resources");
        if (cJSON_IsObject(resources)) {
            (void)ensure_resource(cJSON_GetObjectItemCaseSensitive(resources, "font"),
                                  MESSAGE_FONT_PATH);
            (void)ensure_resource(cJSON_GetObjectItemCaseSensitive(resources, "notification"),
                                  MESSAGE_NOTIFY_PATH);
        }
        cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "messages");
        size_t received = 0;
        bool page_failed = false;
        cJSON *item;
        cJSON_ArrayForEach(item, items) {
            cJSON *cursor_value = cJSON_GetObjectItemCaseSensitive(item, "cursor");
            uint32_t cursor = cJSON_IsNumber(cursor_value)
                                  ? (uint32_t)cursor_value->valuedouble
                                  : 0;
            if (cursor <= s_cursor) continue;
            stored_message_t message = {.cursor = cursor, .read = false};
            copy_json_string(item, "title", message.title, sizeof(message.title));
            copy_json_string(item, "summary", message.summary, sizeof(message.summary));
            if (download_message_audio(item, &message) != ESP_OK) {
                page_failed = true;
                break;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            prune_messages();
            s_messages[s_message_count++] = message;
            s_cursor = cursor;
            (void)save_index();
            xSemaphoreGive(s_lock);
            received++;
            received_new = true;
        }
        cJSON_Delete(root);
        if (page_failed) break;
        if (received == 0) break;
    }
    if (s_cursor > 0) {
        char body[48];
        snprintf(body, sizeof(body), "{\"cursor\":%lu}",
                 (unsigned long)s_cursor);
        char response[128];
        (void)s_config.request("POST", "/device/messages/ack", body,
                               response, sizeof(response));
    }
    if (received_new && s_config.activity) s_config.activity();
    if (received_new && access(MESSAGE_NOTIFY_PATH, R_OK) == 0 &&
        (!s_config.audio_busy || !s_config.audio_busy())) {
        (void)play_wav_file(MESSAGE_NOTIFY_PATH);
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
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
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
    s_storage_ready = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "SD ready capacity=%lluMB messages=%u cursor=%lu stack_free=%u",
             (unsigned long long)(card->csd.capacity * card->csd.sector_size /
                                  (1024 * 1024)),
             (unsigned)s_message_count, (unsigned long)s_cursor,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (true) {
        sync_once();
        vTaskDelay(pdMS_TO_TICKS(MESSAGE_SYNC_INTERVAL_MS));
    }
}

static const lv_font_t *message_font(void)
{
    if (!s_chinese_font && access(MESSAGE_FONT_PATH, R_OK) == 0) {
        s_chinese_font = lv_tiny_ttf_create_file_ex(
            "A:/vibestick/system/DroidSansFallback.ttf", 14,
            LV_FONT_KERNING_NORMAL, 48);
    }
    return s_chinese_font ? s_chinese_font : LV_FONT_DEFAULT;
}

static void render_messages(void)
{
    if (!s_layer) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t count = s_message_count;
    if (count > 0 && s_selected >= count) s_selected = count - 1;
    const size_t page_start = (s_selected / MESSAGE_PAGE_SIZE) * MESSAGE_PAGE_SIZE;
    const size_t page_count = count > page_start
                                  ? ((count - page_start) < MESSAGE_PAGE_SIZE
                                         ? count - page_start
                                         : MESSAGE_PAGE_SIZE)
                                  : 0;
    char header[48];
    snprintf(header, sizeof(header), "消息 %u  %u/%u",
             (unsigned)count,
             count ? (unsigned)(page_start / MESSAGE_PAGE_SIZE + 1) : 0,
             (unsigned)((count + MESSAGE_PAGE_SIZE - 1) / MESSAGE_PAGE_SIZE));
    lv_label_set_text(s_header, header);
    const lv_font_t *font = message_font();
    for (size_t tile = 0; tile < MESSAGE_PAGE_SIZE; tile++) {
        if (tile >= page_count) {
            lv_obj_add_flag(s_tiles[tile], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_tiles[tile], LV_OBJ_FLAG_HIDDEN);
        size_t index = page_start + tile;
        const stored_message_t *message = &s_messages[index];
        char text[240];
        snprintf(text, sizeof(text), "%s%s\n%s",
                 message->read ? "" : "● ", message->title,
                 message->summary);
        lv_obj_set_style_text_font(s_labels[tile], font, 0);
        lv_label_set_text(s_labels[tile], text);
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
    xSemaphoreGive(s_lock);
}

static void blink_timer(lv_timer_t *timer)
{
    (void)timer;
    if (!s_active) return;
    s_blink_on = !s_blink_on;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t page_start = (s_selected / MESSAGE_PAGE_SIZE) * MESSAGE_PAGE_SIZE;
    for (size_t tile = 0; tile < MESSAGE_PAGE_SIZE; tile++) {
        size_t index = page_start + tile;
        if (index < s_message_count && !s_messages[index].read) {
            lv_obj_set_style_bg_opa(s_tiles[tile],
                                    s_blink_on ? LV_OPA_60 : LV_OPA_20, 0);
        } else {
            lv_obj_set_style_bg_opa(s_tiles[tile], LV_OPA_30, 0);
        }
    }
    xSemaphoreGive(s_lock);
}

static void open_messages(void)
{
    display_lock();
    if (!s_layer) {
        s_layer = lv_obj_create(lv_screen_active());
        lv_obj_set_size(s_layer, 240, 135);
        lv_obj_align(s_layer, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(s_layer, lv_color_hex(0x070b10), 0);
        lv_obj_set_style_border_width(s_layer, 0, 0);
        lv_obj_set_style_pad_all(s_layer, 0, 0);
        s_header = lv_label_create(s_layer);
        lv_obj_set_style_text_color(s_header, lv_color_hex(0xe5e7eb), 0);
        lv_obj_align(s_header, LV_ALIGN_TOP_LEFT, 10, 8);
        for (size_t index = 0; index < MESSAGE_PAGE_SIZE; index++) {
            s_tiles[index] = lv_obj_create(s_layer);
            lv_obj_set_style_bg_color(s_tiles[index], lv_color_hex(0x17202b), 0);
            lv_obj_set_style_radius(s_tiles[index], 5, 0);
            lv_obj_set_style_pad_all(s_tiles[index], 5, 0);
            s_labels[index] = lv_label_create(s_tiles[index]);
            lv_label_set_long_mode(s_labels[index], LV_LABEL_LONG_WRAP);
            lv_obj_set_width(s_labels[index], LV_PCT(100));
            lv_obj_set_style_text_color(s_labels[index], lv_color_hex(0xf3f4f6), 0);
        }
        s_blink_timer = lv_timer_create(blink_timer, 500, NULL);
    }
    if (s_message_count > 0) s_selected = s_message_count - 1;
    s_active = true;
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    render_messages();
    display_unlock();
}

static void close_messages(void)
{
    display_lock();
    s_active = false;
    if (s_layer) lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    display_unlock();
    if (s_config.restore_home) s_config.restore_home();
}

static void play_selected(void)
{
    if (s_config.audio_busy && s_config.audio_busy()) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_selected >= s_message_count) {
        xSemaphoreGive(s_lock);
        return;
    }
    char path[160];
    audio_path(s_messages[s_selected].audio_id, path, sizeof(path));
    xSemaphoreGive(s_lock);
    esp_err_t err = play_wav_file(path);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_selected < s_message_count) s_messages[s_selected].read = true;
        (void)save_index();
        xSemaphoreGive(s_lock);
        display_lock();
        render_messages();
        display_unlock();
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
    return ESP_OK;
}

esp_err_t vibe_cardputer_messages_start(void)
{
    BaseType_t result = xTaskCreatePinnedToCore(sync_task, "card_messages",
                                                MESSAGE_TASK_STACK_BYTES, NULL,
                                                2, NULL, 0);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool vibe_cardputer_messages_handle_key(const vibe_key_event_t *event)
{
    if (!event) return false;
    if (!s_active && s_storage_ready && event->pressed && event->fn && event->row == 3 &&
        event->column == 8) {
        if (s_config.activity) s_config.activity();
        open_messages();
        return true;
    }
    if (!s_active) return false;
    if (!event->pressed) return true;
    if (s_config.activity) s_config.activity();
    display_lock();
    if (event->key == VIBE_KEY_ESCAPE) {
        display_unlock();
        close_messages();
        return true;
    }
    if (s_message_count > 0) {
        if (event->key == VIBE_KEY_LEFT && s_selected > 0) s_selected--;
        if (event->key == VIBE_KEY_RIGHT && s_selected + 1 < s_message_count) s_selected++;
        if (event->key == VIBE_KEY_UP && s_selected >= 2) s_selected -= 2;
        if (event->key == VIBE_KEY_DOWN && s_selected + 2 < s_message_count) s_selected += 2;
    }
    bool play = event->key == VIBE_KEY_ENTER;
    render_messages();
    display_unlock();
    if (play) play_selected();
    return true;
}

bool vibe_cardputer_messages_active(void)
{
    return s_active;
}

bool vibe_cardputer_messages_storage_ready(void)
{
    return s_storage_ready;
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

#endif
