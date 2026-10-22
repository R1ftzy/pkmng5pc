#include "check.h"

#include "pkmn/render/backend.h"
#include "pkmn/render/ds_frame_policy.h"

#include <cstdint>
#include <vector>

using namespace pkmn::render;

namespace {

constexpr uint16_t bgr555(uint16_t r, uint16_t g, uint16_t b)
{
    return static_cast<uint16_t>((r << 10) | (g << 5) | b);
}

constexpr uint16_t kRed = bgr555(0x1F, 0, 0);
constexpr uint16_t kGreen = bgr555(0, 0x1F, 0);
constexpr uint16_t kBlue = bgr555(0, 0, 0x1F);

void check_pixel(
    const CanvasBuffer& canvas, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b)
{
    const std::size_t at = (static_cast<std::size_t>(y) * canvas.width + x) * 4;
    CHECK(canvas.bytes[at + 0] == b);
    CHECK(canvas.bytes[at + 1] == g);
    CHECK(canvas.bytes[at + 2] == r);
    CHECK(canvas.bytes[at + 3] == 0xFF);
}

} // namespace

TEST_CASE(render_bgr555_expands_to_bgra8)
{
    const uint16_t src[4] = { 0x7FFF, kBlue, kGreen, kRed };
    uint8_t dst[16] = {};
    bgr555_to_bgra8(src, dst, 4);

    CHECK(dst[0] == 255 && dst[1] == 255 && dst[2] == 255 && dst[3] == 255); // white
    CHECK(dst[4] == 255 && dst[5] == 0 && dst[6] == 0 && dst[7] == 255); // blue
    CHECK(dst[8] == 0 && dst[9] == 255 && dst[10] == 0 && dst[11] == 255); // green
    CHECK(dst[12] == 0 && dst[13] == 0 && dst[14] == 255 && dst[15] == 255); // red
}

TEST_CASE(render_stacked_layout_places_both_screens)
{
    std::vector<uint16_t> top(kScreenWidth * kScreenHeight, kRed);
    std::vector<uint16_t> bottom(kScreenWidth * kScreenHeight, kBlue);

    const CanvasBuffer canvas = compose_screens(
        top.data(), bottom.data(), ScreenLayout::Stacked, kScreenWidth, kScreenHeight * 2);

    CHECK(canvas.width == kScreenWidth);
    CHECK(canvas.height == kScreenHeight * 2);
    check_pixel(canvas, 0, 0, 255, 0, 0);
    check_pixel(canvas, kScreenWidth - 1, kScreenHeight - 1, 255, 0, 0);
    check_pixel(canvas, 0, kScreenHeight, 0, 0, 255);
    check_pixel(canvas, kScreenWidth - 1, kScreenHeight * 2 - 1, 0, 0, 255);
}

TEST_CASE(render_letterboxes_when_aspect_differs)
{
    std::vector<uint16_t> top(kScreenWidth * kScreenHeight, kRed);
    std::vector<uint16_t> bottom(kScreenWidth * kScreenHeight, kBlue);

    // 2x as wide as the native 256x384 stack: a 128 px bar on each side.
    const CanvasBuffer canvas = compose_screens(
        top.data(), bottom.data(), ScreenLayout::Stacked, kScreenWidth * 2, kScreenHeight * 2);

    check_pixel(canvas, 0, 0, 12, 12, 16); // left bar
    check_pixel(canvas, kScreenWidth * 2 - 1, 0, 12, 12, 16); // right bar
    check_pixel(canvas, kScreenWidth, 0, 255, 0, 0); // top screen starts at x=128
    check_pixel(canvas, kScreenWidth, kScreenHeight, 0, 0, 255);
}

TEST_CASE(render_side_by_side_layout)
{
    std::vector<uint16_t> top(kScreenWidth * kScreenHeight, kRed);
    std::vector<uint16_t> bottom(kScreenWidth * kScreenHeight, kGreen);

    const CanvasBuffer canvas = compose_screens(
        top.data(), bottom.data(), ScreenLayout::SideBySide, kScreenWidth * 2, kScreenHeight);

    check_pixel(canvas, 0, 0, 255, 0, 0);
    check_pixel(canvas, kScreenWidth - 1, kScreenHeight - 1, 255, 0, 0);
    check_pixel(canvas, kScreenWidth, 0, 0, 255, 0);
    check_pixel(canvas, kScreenWidth * 2 - 1, kScreenHeight - 1, 0, 255, 0);
}

namespace {

// A screen in the runner's own surface format: ARGB8888, i.e. B,G,R,A bytes.
std::vector<uint8_t> bgra8_screen(uint32_t width, uint8_t r, uint8_t g, uint8_t b)
{
    std::vector<uint8_t> screen(static_cast<std::size_t>(width) * kScreenHeight * 4);
    for (std::size_t pixel = 0; pixel < screen.size() / 4; ++pixel) {
        screen[pixel * 4 + 0] = b;
        screen[pixel * 4 + 1] = g;
        screen[pixel * 4 + 2] = r;
        screen[pixel * 4 + 3] = 0xFF;
    }
    return screen;
}

} // namespace

TEST_CASE(render_bgra8_stacked_matches_the_runners_canvas)
{
    // What -Native actually presents: two 256x192 surfaces into the runner's
    // 512x768 stacked window, i.e. an exact 2x with no letterbox.
    const std::vector<uint8_t> top = bgra8_screen(kScreenWidth, 255, 0, 0);
    const std::vector<uint8_t> bottom = bgra8_screen(kScreenWidth, 0, 0, 255);

    const CanvasBuffer canvas = compose_screens_bgra8(top.data(), kScreenWidth, bottom.data(),
        kScreenWidth, ScreenLayout::Stacked, kScreenWidth * 2, kScreenHeight * 2 * 2);

    CHECK(canvas.width == kScreenWidth * 2);
    CHECK(canvas.height == kScreenHeight * 2 * 2);
    check_pixel(canvas, 0, 0, 255, 0, 0);
    check_pixel(canvas, kScreenWidth * 2 - 1, kScreenHeight * 2 - 1, 255, 0, 0);
    check_pixel(canvas, 0, kScreenHeight * 2, 0, 0, 255);
    check_pixel(canvas, kScreenWidth * 2 - 1, canvas.height - 1, 0, 0, 255);
}

TEST_CASE(render_bgra8_letterboxes_a_wider_canvas)
{
    const std::vector<uint8_t> top = bgra8_screen(kScreenWidth, 255, 0, 0);
    const std::vector<uint8_t> bottom = bgra8_screen(kScreenWidth, 0, 0, 255);

    // 600x768 still holds the 2x stack, so the 88 px slack is two 44 px bars.
    const CanvasBuffer canvas = compose_screens_bgra8(top.data(), kScreenWidth, bottom.data(),
        kScreenWidth, ScreenLayout::Stacked, 600, kScreenHeight * 2 * 2);

    check_pixel(canvas, 0, 0, 12, 12, 16);
    check_pixel(canvas, 43, 0, 12, 12, 16);
    check_pixel(canvas, 44, 0, 255, 0, 0);
    check_pixel(canvas, 555, 0, 255, 0, 0);
    check_pixel(canvas, 556, 0, 12, 12, 16);
    check_pixel(canvas, 44, kScreenHeight * 2, 0, 0, 255);
}

TEST_CASE(render_bgra8_keeps_a_widened_top_screen_from_stretching_the_bottom)
{
    // Adaptive wide output (re/config, NDS_ADAPTIVE_TOP): the top surface is
    // 448 wide while the bottom stays native. At 1x the bottom is centred
    // inside the widened stack instead of being stretched to 448.
    const std::vector<uint8_t> top = bgra8_screen(448, 255, 0, 0);
    const std::vector<uint8_t> bottom = bgra8_screen(kScreenWidth, 0, 0, 255);

    const CanvasBuffer canvas = compose_screens_bgra8(top.data(), 448, bottom.data(), kScreenWidth,
        ScreenLayout::Stacked, 448, kScreenHeight * 2);

    check_pixel(canvas, 447, 0, 255, 0, 0);
    check_pixel(canvas, 0, 191, 255, 0, 0);
    check_pixel(canvas, 95, kScreenHeight, 12, 12, 16);
    check_pixel(canvas, 96, kScreenHeight, 0, 0, 255);
    check_pixel(canvas, 351, canvas.height - 1, 0, 0, 255);
    check_pixel(canvas, 352, kScreenHeight, 12, 12, 16);
}

TEST_CASE(render_stack_placement_matches_the_composed_letterbox)
{
    // The stack rectangle is what the compositor draws inside, so the pointer
    // mapping and the pixels cannot disagree about where a screen is.
    const StackPlacement exact = stack_placement(
        kScreenWidth, kScreenWidth, ScreenLayout::Stacked, kScreenWidth * 2, kScreenHeight * 4);
    CHECK(exact.visible);
    CHECK(exact.x == 0 && exact.y == 0);
    CHECK(exact.width == kScreenWidth * 2);
    CHECK(exact.height == kScreenHeight * 4);
    CHECK(exact.scale == 2);

    // 600x768: the 2x stack is 512 wide, leaving 44 px bars either side.
    const StackPlacement wide = stack_placement(
        kScreenWidth, kScreenWidth, ScreenLayout::Stacked, 600, kScreenHeight * 4);
    CHECK(wide.x == 44 && wide.y == 0);
    CHECK(wide.width == kScreenWidth * 2);
    CHECK(wide.scale == 2);

    // A widened top screen sets the stack width, and the bottom is centred in it.
    const StackPlacement adaptive
        = stack_placement(448, kScreenWidth, ScreenLayout::Stacked, 448, kScreenHeight * 2);
    CHECK(adaptive.x == 0 && adaptive.scale == 1);
    CHECK(adaptive.width == 448);

    const StackPlacement side_by_side
        = stack_placement(kScreenWidth, kScreenWidth, ScreenLayout::SideBySide, 512, 192);
    CHECK(side_by_side.width == 512 && side_by_side.height == 192 && side_by_side.scale == 1);

    const StackPlacement degenerate
        = stack_placement(kScreenWidth, kScreenWidth, ScreenLayout::Stacked, 0, 0);
    CHECK(!degenerate.visible);
}

TEST_CASE(render_bgra8_into_reuses_the_canvas_and_matches_the_allocating_path)
{
    const std::vector<uint8_t> top = bgra8_screen(kScreenWidth, 255, 0, 0);
    const std::vector<uint8_t> bottom = bgra8_screen(kScreenWidth, 0, 0, 255);

    CanvasBuffer reused;
    compose_screens_bgra8_into(reused, top.data(), kScreenWidth, bottom.data(), kScreenWidth,
        ScreenLayout::Stacked, kScreenWidth * 2, kScreenHeight * 4);
    const CanvasBuffer allocated = compose_screens_bgra8(top.data(), kScreenWidth, bottom.data(),
        kScreenWidth, ScreenLayout::Stacked, kScreenWidth * 2, kScreenHeight * 4);

    CHECK(reused.width == allocated.width && reused.height == allocated.height);
    CHECK(reused.bytes == allocated.bytes);

    const uint8_t* storage = reused.bytes.data();
    const std::size_t capacity = reused.bytes.capacity();
    compose_screens_bgra8_into(reused, bottom.data(), kScreenWidth, top.data(), kScreenWidth,
        ScreenLayout::Stacked, kScreenWidth * 2, kScreenHeight * 4);
    CHECK(reused.bytes.data() == storage);
    CHECK(reused.bytes.capacity() == capacity);
    check_pixel(reused, 0, 0, 0, 0, 255);
    check_pixel(reused, 0, kScreenHeight * 2, 255, 0, 0);
}

TEST_CASE(render_single_screen_compose_and_fit_placement)
{
    const std::vector<uint8_t> screen = bgra8_screen(kScreenWidth, 0, 255, 0);

    // Native-size canvas: no bars, the whole surface is the screen.
    const CanvasBuffer native
        = compose_screen_bgra8(screen.data(), kScreenWidth, kScreenWidth, kScreenHeight);
    CHECK(native.width == kScreenWidth && native.height == kScreenHeight);
    check_pixel(native, 0, 0, 0, 255, 0);
    check_pixel(native, kScreenWidth - 1, kScreenHeight - 1, 0, 255, 0);

    // Twice as wide as the screen: equal bars either side at 1x.
    const CanvasBuffer wide
        = compose_screen_bgra8(screen.data(), kScreenWidth, kScreenWidth * 3, kScreenHeight);
    check_pixel(wide, 0, 0, 12, 12, 16);
    check_pixel(wide, kScreenWidth - 1, 0, 12, 12, 16);
    check_pixel(wide, kScreenWidth, 0, 0, 255, 0);
    check_pixel(wide, kScreenWidth * 2 - 1, 0, 0, 255, 0);
    check_pixel(wide, kScreenWidth * 2, 0, 12, 12, 16);

    const StackPlacement fit
        = fit_placement(kScreenWidth, kScreenHeight, kScreenWidth * 4, kScreenHeight * 4);
    CHECK(fit.visible);
    CHECK(fit.scale == 4);
    CHECK(fit.x == 0 && fit.y == 0);
    CHECK(fit.width == kScreenWidth * 4 && fit.height == kScreenHeight * 4);

    const StackPlacement centred
        = fit_placement(kScreenWidth, kScreenHeight, kScreenWidth * 4 + 8, kScreenHeight * 4);
    CHECK(centred.x == 4 && centred.y == 0);
    CHECK(centred.scale == 4);

    CHECK(!fit_placement(0, kScreenHeight, 100, 100).visible);
}

TEST_CASE(ds_sink_failure_resumes_the_cpu_path)
{
    ds::SinkFrameState state;
    CHECK(ds::sink_frame_needs_cpu(state));

    state.initialized = true;
    CHECK(!ds::sink_frame_needs_cpu(state));

    state.failed = true;
    CHECK(ds::sink_frame_needs_cpu(state));

    state.failed = false;
    state.capture = true;
    CHECK(ds::sink_frame_needs_cpu(state));
}
