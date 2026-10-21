// The runner frontend's native presentation bridge. See
// `pkmn/render/native_present.h` for the contract and
// `docs/adr/0006-ndsrecomp-native-present-hook.md` for why it exists.

#include "pkmn/render/native_present.h"

#include "pkmn/render/backend.h"

#include <SDL3/SDL.h>

#include <algorithm>
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
using pkmn::render::PresentTarget;
using pkmn::render::ScreenLayout;
using pkmn::render::StackPlacement;
using pkmn::render::Window;

// -1 unknown, 0 off, 1 on. Latched once: the environment does not change
// under a running frontend.
int enabled_state = -1;

struct Output {
    std::unique_ptr<Presenter> presenter;
    CanvasBuffer canvas;
    SDL_Window* window = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

Output outputs[2];
bool separate_layout = false;
// The surfaces the last frame was composed from. The pointer mapping reads them
// so a widened (adaptive) screen keeps the same rule the pixels were laid out
// with.
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

const char* configured_gpu()
{
    const char* value = std::getenv("PKMNG5PC_GPU");
    return value && *value ? value : "auto";
}

// Keep each swapchain at its window's pixel size. The canvas is kept at the
// native screen size and the GPU scales it into the swapchain, so a window
// resize costs one swapchain rebuild, not a per-frame full-window composite.
void sync_output_size(int index)
{
    Output& output = outputs[index];
    if (!output.presenter || !output.window) {
        return;
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(output.window, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }
    if (static_cast<uint32_t>(width) == output.width
        && static_cast<uint32_t>(height) == output.height) {
        return;
    }
    if (output.presenter->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height))) {
        output.width = static_cast<uint32_t>(width);
        output.height = static_cast<uint32_t>(height);
    }
}

bool init_output(int index, void* sdl_window, const Config& config)
{
    if (!sdl_window) {
        return false;
    }
    Output& output = outputs[index];
    output.window = static_cast<SDL_Window*>(sdl_window);
    output.presenter = pkmn::render::create_presenter(BackendKind::Vulkan);
    if (!output.presenter) {
        return false;
    }

    Window handle;
    handle.sdl_window = sdl_window;
    if (!output.presenter->init(handle, config)) {
        std::fprintf(stderr, "[native] presenter init failed (%s)\n",
            pkmn::render::to_string(output.presenter->kind()));
        output.presenter->shutdown();
        output.presenter.reset();
        output.window = nullptr;
        return false;
    }

    output.width = output.presenter->width();
    output.height = output.presenter->height();
    std::fprintf(stderr, "[native] presenter=%s size=%ux%u screen=%d\n",
        pkmn::render::to_string(output.presenter->kind()), output.width, output.height, index);
    return true;
}

bool present_one(int index, PresentTarget target)
{
    Output& output = outputs[index];
    if (!output.presenter) {
        return false;
    }
    if (!output.presenter->present(output.canvas.view(), target)) {
        // Recoverable (out of date / suboptimal): re-derive the extent and drop
        // this frame rather than presenting a mismatched canvas.
        sync_output_size(index);
        return false;
    }
    return true;
}

} // namespace

extern "C" int pkmng5pc_native_present_enabled(void) { return native_enabled(); }

extern "C" int pkmng5pc_native_present_init(void* top_window, void* bottom_window, int separate)
{
    if (!top_window) {
        return 0;
    }

    Config config;
    config.kind = BackendKind::Vulkan;
    config.vsync = true;
    config.gpu = configured_gpu();
    separate_layout = separate != 0;

    if (!init_output(0, top_window, config)) {
        return 0;
    }
    if (separate_layout && !init_output(1, bottom_window, config)) {
        outputs[0].presenter->shutdown();
        outputs[0].presenter.reset();
        outputs[0].window = nullptr;
        return 0;
    }

    std::fprintf(stderr, "[native] layout=%s gpu=%s\n", separate_layout ? "separate" : "stacked",
        config.gpu.c_str());
    return 1;
}

extern "C" int pkmng5pc_native_present_frame(
    const unsigned int* top, int top_width, const unsigned int* bottom, int bottom_width)
{
    if (!outputs[0].presenter || !top || !bottom) {
        return 0;
    }

    presented_top_width
        = static_cast<uint32_t>(top_width > 0 ? top_width : pkmn::render::kScreenWidth);
    presented_bottom_width
        = static_cast<uint32_t>(bottom_width > 0 ? bottom_width : pkmn::render::kScreenWidth);

    if (separate_layout && outputs[1].presenter) {
        sync_output_size(0);
        sync_output_size(1);

        pkmn::render::compose_screen_bgra8_into(outputs[0].canvas,
            reinterpret_cast<const uint8_t*>(top), presented_top_width, presented_top_width,
            pkmn::render::kScreenHeight);
        pkmn::render::compose_screen_bgra8_into(outputs[1].canvas,
            reinterpret_cast<const uint8_t*>(bottom), presented_bottom_width,
            presented_bottom_width, pkmn::render::kScreenHeight);

        const StackPlacement top_target = pkmn::render::fit_placement(
            presented_top_width, pkmn::render::kScreenHeight, outputs[0].width, outputs[0].height);
        const StackPlacement bottom_target = pkmn::render::fit_placement(presented_bottom_width,
            pkmn::render::kScreenHeight, outputs[1].width, outputs[1].height);
        present_one(
            0, PresentTarget { top_target.x, top_target.y, top_target.width, top_target.height });
        present_one(1,
            PresentTarget {
                bottom_target.x, bottom_target.y, bottom_target.width, bottom_target.height });
        return 1;
    }

    sync_output_size(0);
    if (outputs[0].width == 0 || outputs[0].height == 0) {
        return 0;
    }

    const uint32_t native_width = std::max(presented_top_width, presented_bottom_width);
    pkmn::render::compose_screens_bgra8_into(outputs[0].canvas,
        reinterpret_cast<const uint8_t*>(top), presented_top_width,
        reinterpret_cast<const uint8_t*>(bottom), presented_bottom_width, ScreenLayout::Stacked,
        native_width, pkmn::render::kScreenHeight * 2);

    const StackPlacement stack = pkmn::render::stack_placement(presented_top_width,
        presented_bottom_width, ScreenLayout::Stacked, outputs[0].width, outputs[0].height);
    present_one(0, PresentTarget { stack.x, stack.y, stack.width, stack.height });
    return 1;
}

extern "C" int pkmng5pc_native_present_logical_point(
    unsigned int window_id, float window_x, float window_y, int* x, int* y)
{
    if (!x || !y) {
        return 0;
    }

    int index = -1;
    if (separate_layout) {
        for (int candidate = 0; candidate < 2; ++candidate) {
            if (outputs[candidate].window
                && SDL_GetWindowID(outputs[candidate].window) == window_id) {
                index = candidate;
                break;
            }
        }
    } else if (outputs[0].window) {
        index = 0;
    }
    if (index < 0) {
        return 0;
    }

    Output& output = outputs[index];
    if (!output.presenter || !output.window || output.width == 0 || output.height == 0) {
        return 0;
    }

    // SDL reports pointer positions in window points; the canvas is built in
    // pixels. They differ on a high-DPI display, and SDL_RenderCoordinatesFromWindow
    // (which this replaces) applies exactly this conversion.
    float pixel_x = window_x;
    float pixel_y = window_y;
    int window_width = 0;
    int window_height = 0;
    if (SDL_GetWindowSize(output.window, &window_width, &window_height) && window_width > 0
        && window_height > 0) {
        pixel_x = window_x * static_cast<float>(output.width) / static_cast<float>(window_width);
        pixel_y = window_y * static_cast<float>(output.height) / static_cast<float>(window_height);
    }
    if (pixel_x < 0.0f || pixel_y < 0.0f) {
        return 0;
    }

    StackPlacement placement;
    if (separate_layout) {
        const uint32_t screen_width = index == 0 ? presented_top_width : presented_bottom_width;
        placement = pkmn::render::fit_placement(
            screen_width, pkmn::render::kScreenHeight, output.width, output.height);
    } else {
        placement = pkmn::render::stack_placement(presented_top_width, presented_bottom_width,
            ScreenLayout::Stacked, output.width, output.height);
    }
    if (!placement.visible || placement.scale == 0) {
        return 0;
    }

    // The inverse of the compositor's integer scale: the stack's top-left corner
    // is logical (0,0), and each scale pixels is one logical pixel. Points on the
    // letterbox land outside the logical canvas, which the frontend's own
    // routing then rejects as "not a touch" - the same outcome as the SDL path.
    *x = static_cast<int>(std::floor(
        (pixel_x - static_cast<float>(placement.x)) / static_cast<float>(placement.scale)));
    *y = static_cast<int>(std::floor(
        (pixel_y - static_cast<float>(placement.y)) / static_cast<float>(placement.scale)));
    return 1;
}

extern "C" void pkmng5pc_native_present_shutdown(void)
{
    for (Output& output : outputs) {
        if (output.presenter) {
            output.presenter->shutdown();
            output.presenter.reset();
        }
        output.canvas = CanvasBuffer {};
        output.window = nullptr;
        output.width = 0;
        output.height = 0;
    }
    separate_layout = false;
    presented_top_width = pkmn::render::kScreenWidth;
    presented_bottom_width = pkmn::render::kScreenWidth;
}
