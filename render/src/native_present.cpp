// The runner frontend's native presentation bridge. See
// `pkmn/render/native_present.h` for the contract and
// `docs/adr/0006-ndsrecomp-native-present-hook.md` for why it exists.

#include "pkmn/render/native_present.h"

#include "pkmn/render/backend.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

using pkmn::render::BackendKind;
using pkmn::render::CanvasBuffer;
using pkmn::render::Config;
using pkmn::render::Presenter;
using pkmn::render::ScreenLayout;
using pkmn::render::Window;

// -1 unknown, 0 off, 1 on. Latched once: the environment does not change
// under a running frontend.
int enabled_state = -1;

std::unique_ptr<Presenter> presenter;
CanvasBuffer canvas;
SDL_Window* output_window = nullptr;
uint32_t output_width = 0;
uint32_t output_height = 0;
// The surfaces the last frame was composed from. The pointer mapping reads them
// so a widened (adaptive) top screen keeps the same rule the pixels were laid
// out with.
uint32_t presented_top_width = pkmn::render::kScreenWidth;
uint32_t presented_bottom_width = pkmn::render::kScreenWidth;

int native_enabled()
{
    if (enabled_state < 0) {
        const char* value = std::getenv("PKMNG5PC_PRESENT");
        enabled_state
            = (value && (std::strcmp(value, "native") == 0 || std::strcmp(value, "vulkan") == 0))
            ? 1
            : 0;
    }
    return enabled_state;
}

// Keep the swapchain at the window's pixel size. The presenter requires the
// canvas to match the swapchain extent exactly, so a resize is applied here
// rather than guessed at present time.
void sync_output_size()
{
    if (!presenter || !output_window) {
        return;
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(output_window, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }
    if (static_cast<uint32_t>(width) == output_width
        && static_cast<uint32_t>(height) == output_height) {
        return;
    }
    if (presenter->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height))) {
        output_width = static_cast<uint32_t>(width);
        output_height = static_cast<uint32_t>(height);
    }
}

} // namespace

extern "C" int pkmng5pc_native_present_enabled(void) { return native_enabled(); }

extern "C" int pkmng5pc_native_present_init(void* sdl_window)
{
    if (!sdl_window) {
        return 0;
    }
    output_window = static_cast<SDL_Window*>(sdl_window);
    presenter = pkmn::render::create_presenter(BackendKind::Vulkan);
    if (!presenter) {
        return 0;
    }

    Config config;
    config.kind = BackendKind::Vulkan;
    config.vsync = true;

    Window handle;
    handle.sdl_window = sdl_window;
    if (!presenter->init(handle, config)) {
        std::fprintf(stderr, "[native] presenter init failed (%s)\n",
            pkmn::render::to_string(presenter->kind()));
        presenter->shutdown();
        presenter.reset();
        output_window = nullptr;
        return 0;
    }

    output_width = presenter->width();
    output_height = presenter->height();
    std::fprintf(stderr, "[native] presenter=%s size=%ux%u\n",
        pkmn::render::to_string(presenter->kind()), output_width, output_height);
    return 1;
}

extern "C" int pkmng5pc_native_present_frame(
    const unsigned int* top, int top_width, const unsigned int* bottom, int bottom_width)
{
    if (!presenter || !top || !bottom) {
        return 0;
    }
    sync_output_size();
    if (output_width == 0 || output_height == 0) {
        return 0;
    }

    presented_top_width
        = static_cast<uint32_t>(top_width > 0 ? top_width : pkmn::render::kScreenWidth);
    presented_bottom_width
        = static_cast<uint32_t>(bottom_width > 0 ? bottom_width : pkmn::render::kScreenWidth);

    canvas = pkmn::render::compose_screens_bgra8(reinterpret_cast<const uint8_t*>(top),
        presented_top_width, reinterpret_cast<const uint8_t*>(bottom), presented_bottom_width,
        ScreenLayout::Stacked, output_width, output_height);

    if (!presenter->present(canvas.view())) {
        // Recoverable (out of date / suboptimal): re-derive the extent and drop
        // this frame rather than presenting a mismatched canvas.
        sync_output_size();
    }
    return 1;
}

extern "C" int pkmng5pc_native_present_logical_point(float window_x, float window_y, int* x, int* y)
{
    if (!presenter || !output_window || !x || !y || output_width == 0 || output_height == 0) {
        return 0;
    }

    // SDL reports pointer positions in window points; the canvas is built in
    // pixels. They differ on a high-DPI display, and SDL_RenderCoordinatesFromWindow
    // (which this replaces) applies exactly this conversion.
    float pixel_x = window_x;
    float pixel_y = window_y;
    int window_width = 0;
    int window_height = 0;
    if (SDL_GetWindowSize(output_window, &window_width, &window_height) && window_width > 0
        && window_height > 0) {
        pixel_x = window_x * static_cast<float>(output_width) / static_cast<float>(window_width);
        pixel_y = window_y * static_cast<float>(output_height) / static_cast<float>(window_height);
    }
    if (pixel_x < 0.0f || pixel_y < 0.0f) {
        return 0;
    }

    const pkmn::render::StackPlacement stack = pkmn::render::stack_placement(presented_top_width,
        presented_bottom_width, ScreenLayout::Stacked, output_width, output_height);
    if (!stack.visible || stack.scale == 0) {
        return 0;
    }

    // The inverse of the compositor's integer scale: the stack's top-left corner
    // is logical (0,0), and each scale pixels is one logical pixel. Points on the
    // letterbox land outside the logical canvas, which the frontend's own
    // routing then rejects as "not a touch" - the same outcome as the SDL path.
    *x = static_cast<int>(
        std::floor((pixel_x - static_cast<float>(stack.x)) / static_cast<float>(stack.scale)));
    *y = static_cast<int>(
        std::floor((pixel_y - static_cast<float>(stack.y)) / static_cast<float>(stack.scale)));
    return 1;
}

extern "C" void pkmng5pc_native_present_shutdown(void)
{
    if (presenter) {
        presenter->shutdown();
        presenter.reset();
    }
    canvas = CanvasBuffer {};
    output_window = nullptr;
    output_width = 0;
    output_height = 0;
}
