#pragma once

/* Board capability, not CPU family: Cardputer-Adv also uses an ESP32-S3.
 * Unknown targets stay on internal memory until explicitly qualified.
 */
#if defined(VIBE_BOARD_STICKS3)
#define VIBE_AUDIO_PSRAM_BUDGET_BYTES (1024U * 1024U)
#else
#define VIBE_AUDIO_PSRAM_BUDGET_BYTES 0U
#endif
#define VIBE_AUDIO_PSRAM_RESERVE_BYTES (1024U * 1024U)
