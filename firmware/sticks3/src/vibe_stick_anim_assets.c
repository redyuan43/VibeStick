#include "vibe_stick_anim_assets.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

#define ANIM_PARTITION_LABEL "anim"
#define ANIM_MAGIC 0x31415356u
#define ANIM_HEADER_SIZE 16
#define ANIM_ASSET_ENTRY_SIZE 56
#define ANIM_FRAME_ENTRY_SIZE 8
#define ANIM_NAME_BYTES 40

static const char *TAG = "anim_assets";

typedef struct {
    char name[ANIM_NAME_BYTES];
    uint32_t frame_count;
    uint32_t frame_table_offset;
} anim_asset_entry_t;

static const esp_partition_t *s_partition;
static uint16_t s_asset_count;
static uint16_t s_fps;
static uint32_t s_asset_table_offset;
static uint8_t *s_image_data;
static char s_name_buf[ANIM_NAME_BYTES];

lv_image_dsc_t vibe_stick_anim_image = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .flags = 0,
        .w = VIBE_STICK_ANIM_WIDTH,
        .h = VIBE_STICK_ANIM_HEIGHT,
        .stride = VIBE_STICK_ANIM_WIDTH * 2,
        .reserved_2 = 0,
    },
    .data_size = VIBE_STICK_ANIM_PIXEL_BYTES,
    .data = NULL,
    .reserved = NULL,
};

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static bool read_asset_entry(int asset_id, anim_asset_entry_t *entry)
{
    if (!s_partition || !entry || asset_id < 0 || asset_id >= s_asset_count) {
        return false;
    }
    uint8_t raw[ANIM_ASSET_ENTRY_SIZE] = {0};
    esp_err_t err = esp_partition_read(s_partition,
                                       s_asset_table_offset + (uint32_t)asset_id * ANIM_ASSET_ENTRY_SIZE,
                                       raw,
                                       sizeof(raw));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "asset entry read failed id=%d err=%s", asset_id, esp_err_to_name(err));
        return false;
    }
    memcpy(entry->name, raw, ANIM_NAME_BYTES);
    entry->name[ANIM_NAME_BYTES - 1] = '\0';
    entry->frame_count = read_u32(raw + 40);
    entry->frame_table_offset = read_u32(raw + 44);
    return true;
}

bool vibe_stick_anim_assets_init(void)
{
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                           ESP_PARTITION_SUBTYPE_ANY,
                                           ANIM_PARTITION_LABEL);
    if (!s_partition) {
        ESP_LOGW(TAG, "partition '%s' not found", ANIM_PARTITION_LABEL);
        return false;
    }

    uint8_t header[ANIM_HEADER_SIZE] = {0};
    esp_err_t err = esp_partition_read(s_partition, 0, header, sizeof(header));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "header read failed: %s", esp_err_to_name(err));
        return false;
    }
    if (read_u32(header) != ANIM_MAGIC) {
        ESP_LOGW(TAG, "invalid asset magic");
        return false;
    }
    s_asset_count = read_u16(header + 4);
    uint16_t width = read_u16(header + 6);
    uint16_t height = read_u16(header + 8);
    s_fps = read_u16(header + 10);
    s_asset_table_offset = read_u32(header + 12);
    if (width != VIBE_STICK_ANIM_WIDTH || height != VIBE_STICK_ANIM_HEIGHT ||
        s_asset_count == 0 || s_fps == 0) {
        ESP_LOGW(TAG, "invalid asset header count=%u size=%ux%u fps=%u",
                 s_asset_count, width, height, s_fps);
        return false;
    }
    ESP_LOGI(TAG, "loaded partition assets count=%u fps=%u size=%u",
             s_asset_count, s_fps, (unsigned)s_partition->size);
    return true;
}

int vibe_stick_anim_asset_count(void)
{
    return s_asset_count;
}

int vibe_stick_anim_frame_count(int asset_id)
{
    anim_asset_entry_t entry;
    if (!read_asset_entry(asset_id, &entry)) {
        return 0;
    }
    return (int)entry.frame_count;
}

int vibe_stick_anim_fps(void)
{
    return s_fps > 0 ? s_fps : 15;
}

const char *vibe_stick_anim_asset_name(int asset_id)
{
    anim_asset_entry_t entry;
    if (!read_asset_entry(asset_id, &entry)) {
        return "";
    }
    strlcpy(s_name_buf, entry.name, sizeof(s_name_buf));
    return s_name_buf;
}

bool vibe_stick_anim_decode_frame(int asset_id, int frame_id, uint8_t *dest, size_t dest_size)
{
    if (!dest || dest_size < VIBE_STICK_ANIM_PIXEL_BYTES) {
        return false;
    }

    anim_asset_entry_t asset;
    if (!read_asset_entry(asset_id, &asset) || frame_id < 0 || frame_id >= (int)asset.frame_count) {
        return false;
    }

    uint8_t frame_raw[ANIM_FRAME_ENTRY_SIZE] = {0};
    esp_err_t err = esp_partition_read(s_partition,
                                       asset.frame_table_offset + (uint32_t)frame_id * ANIM_FRAME_ENTRY_SIZE,
                                       frame_raw,
                                       sizeof(frame_raw));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "frame entry read failed asset=%d frame=%d err=%s",
                 asset_id, frame_id, esp_err_to_name(err));
        return false;
    }
    uint32_t data_offset = read_u32(frame_raw);
    uint32_t data_size = read_u32(frame_raw + 4);
    if (data_size == 0 || data_offset + data_size > s_partition->size) {
        ESP_LOGW(TAG, "invalid frame range asset=%d frame=%d off=%u size=%u",
                 asset_id, frame_id, data_offset, data_size);
        return false;
    }

    uint8_t *data = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = heap_caps_malloc(data_size, MALLOC_CAP_8BIT);
    }
    if (!data) {
        ESP_LOGW(TAG, "frame buffer allocation failed size=%u", data_size);
        return false;
    }
    err = esp_partition_read(s_partition, data_offset, data, data_size);
    if (err != ESP_OK) {
        heap_caps_free(data);
        ESP_LOGW(TAG, "frame data read failed: %s", esp_err_to_name(err));
        return false;
    }

    size_t in = 0;
    size_t out = 0;
    while (in < data_size && out < VIBE_STICK_ANIM_PIXEL_BYTES) {
        uint8_t control = data[in++];
        size_t count = (control & 0x7f) + 1;
        if (control & 0x80) {
            if (in + 2 > data_size || out + count * 2 > VIBE_STICK_ANIM_PIXEL_BYTES) {
                heap_caps_free(data);
                return false;
            }
            uint8_t lo = data[in++];
            uint8_t hi = data[in++];
            for (size_t i = 0; i < count; ++i) {
                dest[out++] = lo;
                dest[out++] = hi;
            }
        } else {
            size_t bytes = count * 2;
            if (in + bytes > data_size || out + bytes > VIBE_STICK_ANIM_PIXEL_BYTES) {
                heap_caps_free(data);
                return false;
            }
            memcpy(dest + out, data + in, bytes);
            in += bytes;
            out += bytes;
        }
    }
    heap_caps_free(data);
    return out == VIBE_STICK_ANIM_PIXEL_BYTES;
}

void vibe_stick_anim_set_image_data(uint8_t *data)
{
    s_image_data = data;
    vibe_stick_anim_image.data = s_image_data;
}
