#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "vibe_bt_ui_renderer.h"

#define TEST_WIDTH 87
#define TEST_HEIGHT 80
#define TEST_PIXELS (TEST_WIDTH * TEST_HEIGHT)

static uint32_t surface_hash(const uint16_t *pixels, size_t count)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; ++i) {
        hash ^= pixels[i] & 0xff;
        hash *= 16777619u;
        hash ^= pixels[i] >> 8;
        hash *= 16777619u;
    }
    return hash;
}

static void test_surface_bounds(void)
{
    uint16_t storage[TEST_PIXELS + 2];
    storage[0] = 0x55aa;
    storage[TEST_PIXELS + 1] = 0xaa55;
    vibe_bt_ui_surface_t surface;
    assert(vibe_bt_ui_surface_init(&surface, &storage[1], TEST_PIXELS,
                                   TEST_WIDTH, TEST_HEIGHT));
    vibe_bt_ui_surface_clear(&surface, 0x1234);
    vibe_bt_ui_surface_fill_rect(&surface, -4, -3, 12, 10, 0xabcd);
    vibe_bt_ui_surface_fill_rect(&surface, TEST_WIDTH - 4, TEST_HEIGHT - 3,
                                 12, 10, 0xbeef);
    vibe_bt_ui_surface_fill_rounded_rect(&surface, 10, 10, 6, 12, 3,
                                         0x0f0f);
    assert(storage[0] == 0x55aa);
    assert(storage[TEST_PIXELS + 1] == 0xaa55);
    assert(storage[1] == 0xabcd);
    assert(storage[TEST_PIXELS] == 0xbeef);
    assert(storage[1 + 10 * TEST_WIDTH + 10] != 0x0f0f);
    assert(storage[1 + 11 * TEST_WIDTH + 12] == 0x0f0f);
    assert(!vibe_bt_ui_surface_init(&surface, &storage[1], TEST_PIXELS - 1,
                                    TEST_WIDTH, TEST_HEIGHT));
}

static void test_text(void)
{
    uint16_t pixels[TEST_PIXELS];
    vibe_bt_ui_surface_t surface;
    assert(vibe_bt_ui_surface_init(&surface, pixels, TEST_PIXELS, TEST_WIDTH,
                                   TEST_HEIGHT));
    vibe_bt_ui_surface_clear(&surface, 0);
    assert(vibe_bt_ui_text_width("PAIR", 1) == 23);
    assert(vibe_bt_ui_text_width("MIC LIVE", 2) == 94);
    assert(vibe_bt_ui_text_width("PLEASE WAIT", 2) == 130);
    vibe_bt_ui_surface_draw_text(&surface, 0, 0, "PAIR", 0xffff, 1);
    bool has_foreground = false;
    for (size_t i = 0; i < TEST_PIXELS; ++i) {
        has_foreground |= pixels[i] == 0xffff;
    }
    assert(has_foreground);
}

static void test_pet_frames(void)
{
    uint16_t pixels[TEST_PIXELS];
    uint32_t hashes[VIBE_MINIJOY_PET_FRAME_COUNT] = {0};
    vibe_bt_ui_surface_t surface;
    assert(vibe_bt_ui_surface_init(&surface, pixels, TEST_PIXELS, TEST_WIDTH,
                                   TEST_HEIGHT));
    for (int frame = 0; frame < VIBE_MINIJOY_PET_FRAME_COUNT; ++frame) {
        vibe_bt_ui_surface_clear(&surface, 0x1234);
        assert(vibe_bt_ui_surface_draw_pet(
            &surface, 3, 0, (vibe_minijoy_pet_frame_id_t)frame));
        hashes[frame] = surface_hash(pixels, TEST_PIXELS);
        for (int previous = 0; previous < frame; ++previous) {
            assert(hashes[frame] != hashes[previous]);
        }
    }
    assert(!vibe_bt_ui_surface_draw_pet(
        &surface, 8, 0, VIBE_MINIJOY_PET_FRAME_IDLE));
}

static void test_loading_animation(void)
{
    int start[5];
    vibe_bt_ui_loading_heights(0, start);
    for (int i = 0; i < 5; ++i) {
        assert(start[i] >= 10 && start[i] <= 18);
        int at_delay[5];
        int at_peak[5];
        int at_period[5];
        uint32_t delay = (uint32_t)i * 70;
        vibe_bt_ui_loading_heights(delay, at_delay);
        vibe_bt_ui_loading_heights(delay + 460, at_peak);
        vibe_bt_ui_loading_heights(delay + 920, at_period);
        assert(at_delay[i] == start[i]);
        assert(at_period[i] == start[i]);
        assert(at_peak[i] > at_delay[i]);
    }
}

int main(void)
{
    test_surface_bounds();
    test_text();
    test_pet_frames();
    test_loading_animation();
    return 0;
}
