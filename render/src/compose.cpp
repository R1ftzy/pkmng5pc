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

    CanvasBuffer make_canvas(uint32_t width, uint32_t height)
    {
        CanvasBuffer canvas;
        canvas.width = width;
        canvas.height = height;
        canvas.bytes.assign(static_cast<std::size_t>(width) * height * 4, 0);
        fill_letterbox(canvas);
        return canvas;
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

    // Already-BGRA8 source. `src_width` is the DS width or the adaptive wide
    // width; the rows are tightly packed.
    void draw_screen_bgra8(const uint8_t* src, uint32_t src_width, uint32_t dst_x, uint32_t dst_y,
        uint32_t dst_w, uint32_t dst_h, CanvasBuffer& canvas)
    {
        if (!src || src_width == 0 || dst_w == 0 || dst_h == 0) {
            return;
        }
        for (uint32_t y = 0; y < dst_h; ++y) {
            const uint32_t sy = std::min(kScreenHeight - 1, y * kScreenHeight / dst_h);
            const uint8_t* src_row = src + static_cast<std::size_t>(sy) * src_width * 4;
            uint8_t* dst_row = canvas.bytes.data()
                + (static_cast<std::size_t>(dst_y + y) * canvas.width + dst_x) * 4;
            for (uint32_t x = 0; x < dst_w; ++x) {
                const uint32_t sx = std::min(src_width - 1, x * src_width / dst_w);
                std::memcpy(dst_row + x * 4, src_row + static_cast<std::size_t>(sx) * 4, 4);
            }
        }
    }

    // Where one screen lands in the canvas. Internal: the public API exposes
    // only the enclosing stack (`StackPlacement`), because that is all the
    // pointer mapping needs to invert the layout.
    struct ScreenPlacement {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool visible = false;
    };

    // The one placement rule for the already-composed stack: integer scale to
    // fit, centred, letterboxed. Shared by both source formats.
    StackPlacement place_stack(
        uint32_t native_w, uint32_t native_h, uint32_t output_width, uint32_t output_height)
    {
        StackPlacement placement;
        uint32_t scale = std::min(output_width / native_w, output_height / native_h);
        if (scale == 0) {
            scale = 1; // too small to letterbox: fill and clip
        }
        placement.scale = scale;
        placement.width = std::min(output_width, native_w * scale);
        placement.height = std::min(output_height, native_h * scale);
        placement.x = (output_width - placement.width) / 2;
        placement.y = (output_height - placement.height) / 2;
        placement.visible = placement.width != 0 && placement.height != 0;
        return placement;
    }

    // The placement of both screens in the composed canvas. `compose_screens_bgra8()`
    // draws from it and `stack_placement()` reports the enclosing stack, so a
    // pointer maps back through the very rectangle the pixels were written into.
    void place_screens(uint32_t top_width, uint32_t bottom_width, ScreenLayout layout,
        uint32_t output_width, uint32_t output_height, StackPlacement& stack, ScreenPlacement& top,
        ScreenPlacement& bottom)
    {
        stack = StackPlacement {};
        top = ScreenPlacement {};
        bottom = ScreenPlacement {};
        if (output_width == 0 || output_height == 0) {
            return;
        }

        if (top_width == 0) {
            top_width = kScreenWidth;
        }
        if (bottom_width == 0) {
            bottom_width = kScreenWidth;
        }

        const bool stacked = layout == ScreenLayout::Stacked;
        const uint32_t native_w
            = stacked ? std::max(top_width, bottom_width) : top_width + bottom_width;
        const uint32_t native_h = stacked ? kScreenHeight * 2 : kScreenHeight;

        stack = place_stack(native_w, native_h, output_width, output_height);
        const uint32_t stack_w = stack.width;
        const uint32_t stack_h = stack.height;

        // Each screen keeps its own native width at the shared integer scale, so
        // a widened (adaptive) top screen does not stretch the untouched bottom
        // one.
        if (stacked) {
            const uint32_t screen_h = std::min(stack_h / 2, kScreenHeight * stack.scale);
            const uint32_t top_w = std::min(stack_w, top_width * stack.scale);
            const uint32_t bottom_w = std::min(stack_w, bottom_width * stack.scale);
            top = ScreenPlacement { stack.x + (stack_w - top_w) / 2, stack.y, top_w, screen_h,
                top_w != 0 && screen_h != 0 };
            bottom = ScreenPlacement { stack.x + (stack_w - bottom_w) / 2,
                stack.y + (stack_h - screen_h), bottom_w, screen_h,
                bottom_w != 0 && screen_h != 0 };
        } else {
            const uint32_t top_w = std::min(stack_w, top_width * stack.scale);
            const uint32_t bottom_w = std::min(stack_w - top_w, bottom_width * stack.scale);
            top = ScreenPlacement { stack.x, stack.y, top_w, stack_h, top_w != 0 && stack_h != 0 };
            bottom = ScreenPlacement { stack.x + top_w, stack.y, bottom_w, stack_h,
                bottom_w != 0 && stack_h != 0 };
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
    CanvasBuffer canvas = make_canvas(output_width, output_height);

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

    const StackPlacement placement = place_stack(native_w, native_h, output_width, output_height);
    const uint32_t draw_w = placement.width;
    const uint32_t draw_h = placement.height;
    const uint32_t origin_x = placement.x;
    const uint32_t origin_y = placement.y;

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

CanvasBuffer compose_screens_bgra8(const uint8_t* top, uint32_t top_width, const uint8_t* bottom,
    uint32_t bottom_width, ScreenLayout layout, uint32_t output_width, uint32_t output_height)
{
    CanvasBuffer canvas = make_canvas(output_width, output_height);

    StackPlacement stack;
    ScreenPlacement top_placement;
    ScreenPlacement bottom_placement;
    place_screens(top_width, bottom_width, layout, output_width, output_height, stack,
        top_placement, bottom_placement);

    draw_screen_bgra8(top, top_width ? top_width : kScreenWidth, top_placement.x, top_placement.y,
        top_placement.width, top_placement.height, canvas);
    draw_screen_bgra8(bottom, bottom_width ? bottom_width : kScreenWidth, bottom_placement.x,
        bottom_placement.y, bottom_placement.width, bottom_placement.height, canvas);
    return canvas;
}

StackPlacement stack_placement(uint32_t top_width, uint32_t bottom_width, ScreenLayout layout,
    uint32_t output_width, uint32_t output_height)
{
    StackPlacement stack;
    ScreenPlacement top_placement;
    ScreenPlacement bottom_placement;
    place_screens(top_width, bottom_width, layout, output_width, output_height, stack,
        top_placement, bottom_placement);
    return stack;
}

} // namespace pkmn::render
