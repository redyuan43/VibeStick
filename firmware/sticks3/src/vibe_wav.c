#include "vibe_wav.h"

#include <string.h>

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

bool vibe_wav_pcm_payload(const uint8_t *data,
                          size_t len,
                          uint32_t expected_sample_rate,
                          uint16_t expected_channels,
                          uint16_t expected_bits_per_sample,
                          const uint8_t **pcm,
                          size_t *pcm_len)
{
    if (!data || len < 44 || !pcm || !pcm_len) {
        return false;
    }
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }

    size_t offset = 12;
    bool format_ok = false;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    while (offset + 8 <= len) {
        const uint8_t *chunk = data + offset;
        uint32_t chunk_size = read_le32(chunk + 4);
        size_t data_offset = offset + 8;
        if (chunk_size > len - data_offset) {
            return false;
        }
        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            uint16_t audio_format = read_le16(data + data_offset);
            uint16_t channels = read_le16(data + data_offset + 2);
            uint32_t sample_rate = read_le32(data + data_offset + 4);
            uint16_t bits_per_sample = read_le16(data + data_offset + 14);
            format_ok = audio_format == 1 &&
                        channels == expected_channels &&
                        sample_rate == expected_sample_rate &&
                        bits_per_sample == expected_bits_per_sample;
        } else if (memcmp(chunk, "data", 4) == 0) {
            payload = data + data_offset;
            payload_len = chunk_size;
        }
        offset = data_offset + chunk_size + (chunk_size & 1u);
    }
    if (!format_ok || !payload || payload_len == 0 || (payload_len % 2) != 0) {
        return false;
    }
    *pcm = payload;
    *pcm_len = payload_len;
    return true;
}
