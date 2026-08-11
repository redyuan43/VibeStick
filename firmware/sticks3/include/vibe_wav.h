#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool vibe_wav_pcm_payload(const uint8_t *data,
                          size_t len,
                          uint32_t expected_sample_rate,
                          uint16_t expected_channels,
                          uint16_t expected_bits_per_sample,
                          const uint8_t **pcm,
                          size_t *pcm_len);

bool vibe_wav_pcm_stream_info(const uint8_t *header,
                              size_t header_len,
                              size_t total_len,
                              uint32_t expected_sample_rate,
                              uint16_t expected_channels,
                              uint16_t expected_bits_per_sample,
                              size_t *pcm_offset,
                              size_t *pcm_len);
