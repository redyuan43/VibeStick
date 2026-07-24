#include "vibe_bt_ui_renderer.h"

#include <string.h>

bool vibe_bt_ui_surface_init(vibe_bt_ui_surface_t *surface, uint16_t *pixels,
                             size_t capacity_pixels, int width, int height)
{
    if (!surface || !pixels || width <= 0 || height <= 0 ||
        (size_t)width * (size_t)height > capacity_pixels) {
        return false;
    }
    *surface = (vibe_bt_ui_surface_t){
        .pixels = pixels,
        .width = width,
        .height = height,
        .stride = width,
    };
    return true;
}

void vibe_bt_ui_surface_clear(vibe_bt_ui_surface_t *surface, uint16_t color)
{
    if (!surface || !surface->pixels) {
        return;
    }
    for (int y = 0; y < surface->height; ++y) {
        for (int x = 0; x < surface->width; ++x) {
            surface->pixels[y * surface->stride + x] = color;
        }
    }
}

void vibe_bt_ui_surface_fill_rect(vibe_bt_ui_surface_t *surface, int x, int y,
                                  int width, int height, uint16_t color)
{
    if (!surface || !surface->pixels || width <= 0 || height <= 0) {
        return;
    }
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > surface->width) {
        width = surface->width - x;
    }
    if (y + height > surface->height) {
        height = surface->height - y;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        uint16_t *output =
            &surface->pixels[(y + row) * surface->stride + x];
        for (int column = 0; column < width; ++column) {
            output[column] = color;
        }
    }
}

void vibe_bt_ui_surface_fill_rounded_rect(vibe_bt_ui_surface_t *surface,
                                          int x, int y, int width,
                                          int height, int radius,
                                          uint16_t color)
{
    if (!surface || !surface->pixels || width <= 0 || height <= 0) {
        return;
    }
    if (radius < 0) {
        radius = 0;
    }
    int max_radius = width / 2;
    if (height / 2 < max_radius) {
        max_radius = height / 2;
    }
    if (radius > max_radius) {
        radius = max_radius;
    }
    for (int row = 0; row < height; ++row) {
        int edge = row;
        if (height - 1 - row < edge) {
            edge = height - 1 - row;
        }
        int inset = edge < radius ? radius - edge - 1 : 0;
        vibe_bt_ui_surface_fill_rect(surface, x + inset, y + row,
                                     width - inset * 2, 1, color);
    }
}

static uint8_t glyph_column(char character, int column)
{
    static const struct {
        char character;
        uint8_t columns[5];
    } glyphs[] = {
        {'0', {0x3e, 0x51, 0x49, 0x45, 0x3e}},
        {'1', {0x00, 0x42, 0x7f, 0x40, 0x00}},
        {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
        {'3', {0x21, 0x41, 0x45, 0x4b, 0x31}},
        {'4', {0x18, 0x14, 0x12, 0x7f, 0x10}},
        {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
        {'6', {0x3c, 0x4a, 0x49, 0x49, 0x30}},
        {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
        {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
        {'9', {0x06, 0x49, 0x49, 0x29, 0x1e}},
        {'%', {0x63, 0x13, 0x08, 0x64, 0x63}},
        {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
        {'A', {0x7e, 0x11, 0x11, 0x11, 0x7e}},
        {'B', {0x7f, 0x49, 0x49, 0x49, 0x36}},
        {'C', {0x3e, 0x41, 0x41, 0x41, 0x22}},
        {'D', {0x7f, 0x41, 0x41, 0x22, 0x1c}},
        {'E', {0x7f, 0x49, 0x49, 0x49, 0x41}},
        {'F', {0x7f, 0x09, 0x09, 0x09, 0x01}},
        {'G', {0x3e, 0x41, 0x49, 0x49, 0x7a}},
        {'H', {0x7f, 0x08, 0x08, 0x08, 0x7f}},
        {'I', {0x00, 0x41, 0x7f, 0x41, 0x00}},
        {'J', {0x20, 0x40, 0x41, 0x3f, 0x01}},
        {'K', {0x7f, 0x08, 0x14, 0x22, 0x41}},
        {'L', {0x7f, 0x40, 0x40, 0x40, 0x40}},
        {'M', {0x7f, 0x02, 0x0c, 0x02, 0x7f}},
        {'N', {0x7f, 0x04, 0x08, 0x10, 0x7f}},
        {'O', {0x3e, 0x41, 0x41, 0x41, 0x3e}},
        {'P', {0x7f, 0x09, 0x09, 0x09, 0x06}},
        {'R', {0x7f, 0x09, 0x19, 0x29, 0x46}},
        {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
        {'T', {0x01, 0x01, 0x7f, 0x01, 0x01}},
        {'U', {0x3f, 0x40, 0x40, 0x40, 0x3f}},
        {'V', {0x1f, 0x20, 0x40, 0x20, 0x1f}},
        {'W', {0x3f, 0x40, 0x38, 0x40, 0x3f}},
        {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    };
    for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i) {
        if (glyphs[i].character == character) {
            return glyphs[i].columns[column];
        }
    }
    return 0;
}

int vibe_bt_ui_text_width(const char *text, int scale)
{
    if (!text || scale <= 0) {
        return 0;
    }
    size_t length = strlen(text);
    return length == 0 ? 0 : (int)(length * 6 * (size_t)scale - scale);
}

void vibe_bt_ui_surface_draw_text(vibe_bt_ui_surface_t *surface, int x, int y,
                                  const char *text, uint16_t color, int scale)
{
    if (!surface || !text || scale <= 0) {
        return;
    }
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor != ' ') {
            for (int column = 0; column < 5; ++column) {
                uint8_t bits = glyph_column(*cursor, column);
                for (int row = 0; row < 7; ++row) {
                    if (bits & (1u << row)) {
                        vibe_bt_ui_surface_fill_rect(
                            surface, x + column * scale, y + row * scale,
                            scale, scale, color);
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

bool vibe_bt_ui_surface_draw_pet(vibe_bt_ui_surface_t *surface, int x, int y,
                                 vibe_minijoy_pet_frame_id_t frame_id)
{
    if (!surface || !surface->pixels || x < 0 || y < 0 ||
        x + VIBE_MINIJOY_PET_WIDTH > surface->width ||
        y + VIBE_MINIJOY_PET_HEIGHT > surface->height) {
        return false;
    }
    const vibe_minijoy_pet_rle_frame_t *frame =
        vibe_minijoy_pet_frame(frame_id);
    if (!frame) {
        return false;
    }

    size_t input = 0;
    size_t output = 0;
    const size_t pixel_count =
        VIBE_MINIJOY_PET_WIDTH * VIBE_MINIJOY_PET_HEIGHT;
    while (input < frame->size && output < pixel_count) {
        uint8_t control = frame->data[input++];
        size_t count = (control & 0x7f) + 1;
        if (output + count > pixel_count) {
            return false;
        }
        if (control & 0x80) {
            if (input + 2 > frame->size) {
                return false;
            }
            uint8_t lo = frame->data[input++];
            uint8_t hi = frame->data[input++];
            uint16_t color = ((uint16_t)lo << 8) | hi;
            for (size_t i = 0; i < count; ++i, ++output) {
                int px = (int)(output % VIBE_MINIJOY_PET_WIDTH);
                int py = (int)(output / VIBE_MINIJOY_PET_WIDTH);
                surface->pixels[(y + py) * surface->stride + x + px] = color;
            }
        } else {
            if (input + count * 2 > frame->size) {
                return false;
            }
            for (size_t i = 0; i < count; ++i, ++output) {
                uint8_t lo = frame->data[input++];
                uint8_t hi = frame->data[input++];
                int px = (int)(output % VIBE_MINIJOY_PET_WIDTH);
                int py = (int)(output / VIBE_MINIJOY_PET_WIDTH);
                surface->pixels[(y + py) * surface->stride + x + px] =
                    ((uint16_t)lo << 8) | hi;
            }
        }
    }
    return output == pixel_count && input == frame->size;
}

void vibe_bt_ui_loading_heights(uint32_t elapsed_ms, int heights[5])
{
    static const int min_heights[5] = {10, 14, 18, 14, 10};
    static const int max_heights[5] = {24, 34, 48, 34, 24};
    const uint32_t half_period_ms = 460;
    const uint32_t period_ms = half_period_ms * 2;
    const uint32_t delay_ms = 70;
    if (!heights) {
        return;
    }
    for (int i = 0; i < 5; ++i) {
        uint32_t delay = (uint32_t)i * delay_ms;
        if (elapsed_ms < delay) {
            heights[i] = min_heights[i];
            continue;
        }
        uint32_t phase = (elapsed_ms - delay) % period_ms;
        uint32_t progress = phase <= half_period_ms
                                ? phase
                                : period_ms - phase;
        heights[i] = min_heights[i] +
                     (max_heights[i] - min_heights[i]) * (int)progress /
                         (int)half_period_ms;
    }
}
