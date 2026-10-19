#include "pkmn/render/backend.h"

#include <algorithm>
#include <cstring>

namespace pkmn::render {
namespace {

    constexpr uint8_t kLetterboxR = 12;
    constexpr uint8_t kLetterboxG = 12;
    constexpr uint8_t kLetterboxB = 16;

    void fill_letterbox(CanvasBuffer& canvas)
    {
        for (uint32_t y = 0; y < canvas.height; ++y) {
            uint8_t* row = canvas.bytes.data() + static_cast<std::size_t>(y) * canvas.width * 4;
            for (uint32_t x = 0; x < canvas.width; ++x) {
                row[x * 4 + 0] = kLetterboxB;
                row[x * 4 + 1] = kLetterboxG;
                row[x * 4 + 2] = kLetterboxR;
                row[x * 4 + 3] = 0xFF;
            }
        }
    }

    // Draw one BGR555 screen into a rectangle of a BGRA8 canvas, nearest-neighbour.
    void draw_screen(const uint16_t* src, uint32_t dst_x, uint32_t dst_y, uint32_t dst_w,
        uint32_t dst_h, CanvasBuffer& canvas)
    {
        if (dst_w == 0 || dst_h == 0) {
            return;
        }
        for (uint32_t y = 0; y < dst_h; ++y) {
            const uint32_t sy = std::min(kScreenHeight - 1, y * kScreenHeight / dst_h);
            const uint16_t* src_row = src + static_cast<std::size_t>(sy) * kScreenWidth;
            uint8_t* dst_row = canvas.bytes.data()
                + (static_cast<std::size_t>(dst_y + y) * canvas.width + dst_x) * 4;
            for (uint32_t x = 0; x < dst_w; ++x) {
                const uint32_t sx = std::min(kScreenWidth - 1, x * kScreenWidth / dst_w);
                bgr555_to_bgra8(src_row + sx, dst_row + x * 4, 1);
                dst_row[x * 4 + 3] = 0xFF;
            }
        }
    }

} // namespace

void bgr555_to_bgra8(const uint16_t* src, uint8_t* dst, uint32_t pixels)
{
    for (uint32_t i = 0; i < pixels; ++i) {
        const uint16_t v = src[i];
        const uint8_t b5 = static_cast<uint8_t>(v & 0x1Fu);
        const uint8_t g5 = static_cast<uint8_t>((v >> 5) & 0x1Fu);
        const uint8_t r5 = static_cast<uint8_t>((v >> 10) & 0x1Fu);
        dst[i * 4 + 0] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
        dst[i * 4 + 1] = static_cast<uint8_t>((g5 << 3) | (g5 >> 2));
        dst[i * 4 + 2] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
        dst[i * 4 + 3] = 0xFF;
    }
}

CanvasBuffer compose_screens(const uint16_t* top, const uint16_t* bottom, ScreenLayout layout,
    uint32_t output_width, uint32_t output_height)
{
    CanvasBuffer canvas;
    canvas.width = output_width;
    canvas.height = output_height;
    canvas.bytes.assign(static_cast<std::size_t>(output_width) * output_height * 4, 0);
    fill_letterbox(canvas);

    if (output_width == 0 || output_height == 0) {
        return canvas;
    }

    uint32_t native_w = kScreenWidth;
    uint32_t native_h = kScreenHeight;
    if (layout == ScreenLayout::Stacked) {
        native_h = kScreenHeight * 2;
    } else {
        native_w = kScreenWidth * 2;
    }

    uint32_t scale = std::min(output_width / native_w, output_height / native_h);
    if (scale == 0) {
        scale = 1; // too small to letterbox: fill and clip
    }
    const uint32_t draw_w = std::min(output_width, native_w * scale);
    const uint32_t draw_h = std::min(output_height, native_h * scale);
    const uint32_t origin_x = (output_width - draw_w) / 2;
    const uint32_t origin_y = (output_height - draw_h) / 2;

    if (layout == ScreenLayout::Stacked) {
        const uint32_t screen_h = draw_h / 2;
        draw_screen(top, origin_x, origin_y, draw_w, screen_h, canvas);
        draw_screen(bottom, origin_x, origin_y + screen_h, draw_w, draw_h - screen_h, canvas);
    } else {
        const uint32_t screen_w = draw_w / 2;
        draw_screen(top, origin_x, origin_y, screen_w, draw_h, canvas);
        draw_screen(bottom, origin_x + screen_w, origin_y, draw_w - screen_w, draw_h, canvas);
    }
    return canvas;
}

} // namespace pkmn::render
