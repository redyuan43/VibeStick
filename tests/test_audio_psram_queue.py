"""Execute the production queue allocator with deterministic heap/RTOS failures."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "firmware/sticks3/src/vibe_audio.c"
INCLUDE = ROOT / "firmware/sticks3/include"


def test_audio_queue_board_gating_and_allocation_fallback(tmp_path):
    source = SOURCE.read_text()
    begin = source.index("static esp_err_t create_audio_queue(")
    end = source.index("\n#if VIBE_BOARD_HAS_ES8311", begin)
    allocator = source[begin:end]
    harness = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "vibe_audio_buffer_policy.h"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define AUDIO_FRAME_MS 60
#define VIBE_AUDIO_TRANSPORT_IMA_ADPCM 1
#define ESP_ERR_NO_MEM -1
#define ESP_OK 0
#define ESP_RETURN_ON_FALSE(c,e,...) do { if (!(c)) return (e); } while(0)
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
typedef int vibe_audio_transport_t;
typedef void *QueueHandle_t;
typedef int StaticQueue_t;
typedef struct {size_t len; uint8_t data[1920];} audio_chunk_t;
static QueueHandle_t s_audio_queue;
static size_t s_audio_queue_item_size, s_audio_queue_depth;
static vibe_audio_transport_t s_audio_transport;
#if VIBE_AUDIO_PSRAM_BUDGET_BYTES > 0 && defined(CONFIG_SPIRAM)
static StaticQueue_t s_audio_queue_control;
static uint8_t *s_audio_queue_storage;
#endif
static size_t free_bytes = 8*1024*1024, requested_caps, static_calls, dynamic_calls;
static int fail_heap, fail_queue;
static size_t audio_wire_frame_bytes(int t) {return t ? 484 : 1920;}
static size_t audio_queue_depth(int t) {
#ifdef VIBE_BOARD_STICKS3
    return t ? 96 : 12;
#else
    return t ? 48 : 12;
#endif
}
static size_t heap_caps_get_free_size(int caps) {return free_bytes;}
static void *heap_caps_malloc(size_t n,int caps) {
    requested_caps = caps; return fail_heap ? NULL : malloc(n);
}
static void heap_caps_free(void *p) {free(p);}
static void *xQueueCreateStatic(size_t n,size_t size,uint8_t *data,StaticQueue_t *control) {
    static_calls++; return fail_queue ? NULL : control;
}
static void *xQueueCreate(size_t n,size_t size) {dynamic_calls++; return (void*)1;}
'''
    main = r'''
int main(void) {
    assert(create_audio_queue(0) == ESP_OK);
    assert(dynamic_calls == 1 && static_calls == 0); /* PCM stays small. */
    assert(create_audio_queue(1) == ESP_OK);
#if VIBE_AUDIO_PSRAM_BUDGET_BYTES > 0 && defined(CONFIG_SPIRAM)
    assert(requested_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(static_calls == 1 && dynamic_calls == 1);
    assert(s_audio_queue_depth * s_audio_queue_item_size <= 1024*1024);
    assert(s_audio_queue_depth * AUDIO_FRAME_MS >= 120000);
    heap_caps_free(s_audio_queue_storage); s_audio_queue_storage = NULL;
    free_bytes = 1024*1024; /* Preserve reserve: never consume last PSRAM. */
    assert(create_audio_queue(1) == ESP_OK && s_audio_queue_depth == 96);
    free_bytes = 8*1024*1024; fail_heap = 1;
    assert(create_audio_queue(1) == ESP_OK && s_audio_queue_depth == 96);
    fail_heap = 0; fail_queue = 1;
    assert(create_audio_queue(1) == ESP_OK && s_audio_queue_depth == 96);
    assert(s_audio_queue_storage == NULL);
#else
    assert(static_calls == 0 && dynamic_calls == 2);
    assert(s_audio_queue_depth == audio_queue_depth(1));
#endif
    return 0;
}
'''
    cfile = tmp_path / "queue.c"
    cfile.write_text(harness + allocator + main)
    for board in ("STICKS3", "CARDPUTER_ADV", "STICKC_PLUS", "STICKC_PLUS_SE", "UNKNOWN"):
        for spiram in (False, True):
            exe = tmp_path / (board + str(spiram))
            command = ["cc", "-std=c11", "-I", str(INCLUDE), "-DVIBE_BOARD_" + board,
                       str(cfile), "-o", str(exe)]
            if spiram:
                command.append("-DCONFIG_SPIRAM=1")
            subprocess.run(command, check=True, capture_output=True)
            subprocess.run([str(exe)], check=True)
