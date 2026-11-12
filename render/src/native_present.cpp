// The runner frontend's native presentation bridge. See
// `pkmn/render/native_present.h` for the contract and
// `docs/adr/0006-ndsrecomp-native-present-hook.md` for why it exists.

#include "pkmn/render/native_present.h"

#include "gpu2d.h"
#include "pkmn/render/backend.h"
extern "C" int pkmng5pc_ds_render_hires_color(
    void** view, unsigned* width, unsigned* height, unsigned* scale);

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
using pkmn::render::Canvas;
using pkmn::render::CanvasBuffer;
using pkmn::render::Config;
using pkmn::render::Presenter;
using pkmn::render::PresentTarget;
using pkmn::render::Scaling;
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

// `WxH` in output pixels, or false when unset/malformed. Written by the
// launcher's window-size choice; each window is sized once at init and stays
// freely resizable afterwards.
bool configured_window_size(int& width, int& height)
{
    const char* value = std::getenv("PKMNG5PC_WINDOW_SIZE");
    if (!value || !*value) {
        return false;
    }
    int parsed_width = 0;
    int parsed_height = 0;
    if (std::sscanf(value, "%dx%d", &parsed_width, &parsed_height) != 2) {
        std::fprintf(stderr, "[native] ignoring window size '%s' (expected WxH)\n", value);
        return false;
    }
    if (parsed_width < 160 || parsed_height < 120 || parsed_width > 7680 || parsed_height > 4320) {
        std::fprintf(stderr, "[native] ignoring window size '%s' (out of range)\n", value);
        return false;
    }
    width = parsed_width;
    height = parsed_height;
    return true;
}

// `integer` (default) keeps whole-pixel blocks and letterboxes; `fit` fills the
// window at an aspect-preserving fractional scale.
Scaling configured_scaling()
{
    const char* value = std::getenv("PKMNG5PC_PRESENT_SCALING");
    return value && std::strcmp(value, "fit") == 0 ? Scaling::Fit : Scaling::Integer;
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

    int window_width = 0;
    int window_height = 0;
    if (configured_window_size(window_width, window_height)) {
        SDL_SetWindowSize(output.window, window_width, window_height);
        SDL_SetWindowPosition(output.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        std::fprintf(
            stderr, "[native] window=%dx%d screen=%d\n", window_width, window_height, index);
    }

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

    std::fprintf(stderr, "[native] layout=%s scaling=%s gpu=%s\n",
        separate_layout ? "separate" : "stacked",
        configured_scaling() == Scaling::Fit ? "fit" : "integer", config.gpu.c_str());
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

        const StackPlacement top_target = pkmn::render::fit_placement(presented_top_width,
            pkmn::render::kScreenHeight, outputs[0].width, outputs[0].height, configured_scaling());
        const StackPlacement bottom_target = pkmn::render::fit_placement(presented_bottom_width,
            pkmn::render::kScreenHeight, outputs[1].width, outputs[1].height, configured_scaling());
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

    const StackPlacement stack
        = pkmn::render::stack_placement(presented_top_width, presented_bottom_width,
            ScreenLayout::Stacked, outputs[0].width, outputs[0].height, configured_scaling());

    // HD composite (ADR 0008, slice V2): when the sink rasterizes above 1x
    // and the compositor emitted the no-3D stack, blend the hi-res surface at
    // the placed rectangle's own resolution instead of presenting the 1x
    // canvas. The rectangle uses fit scaling so the extra density is visible.
    void* hires_view = nullptr;
    unsigned hires_w = 0;
    unsigned hires_h = 0;
    unsigned hires_scale = 0;
    NdsGpu2dHdFrame hd {};
    const bool hd_ok = pkmng5pc_ds_render_hires_color(&hires_view, &hires_w, &hires_h, &hires_scale)
        && nds_gpu2d_hd_frame(&hd) && hd.top_pixels && hd.below_pixels && hd.width == native_width
        && hires_scale > 1;
    if (hd_ok) {
        const StackPlacement fit = pkmn::render::stack_placement(presented_top_width,
            presented_bottom_width, ScreenLayout::Stacked, outputs[0].width, outputs[0].height,
            pkmn::render::Scaling::Fit);

        pkmn::render::HdComposite input {};
        input.hires_view = hires_view;
        input.hires_width = hires_w;
        input.hires_height = hires_h;
        input.scale = hires_scale;
        input.top = hd.top_pixels;
        input.below = hd.below_pixels;
        input.width = hd.width;
        input.fallback_width = presented_bottom_width;
        input.fallback_offset = (native_width - presented_bottom_width) / 2;
        input.content_row0 = 0;
        input.content_rows = pkmn::render::kScreenHeight * 2;
        input.priority_3d = hd.priority_3d;
        input.order_3d = hd.order_3d;
        input.bldcnt = hd.bldcnt;
        input.eva = hd.eva;
        input.evb = hd.evb;
        input.evy = hd.evy;
        input.master_bright = hd.master_bright;

        const Canvas fallback { reinterpret_cast<const uint8_t*>(bottom), presented_bottom_width,
            pkmn::render::kScreenHeight, presented_bottom_width * 4 };
        if (outputs[0].presenter->present_hd(
                input, fallback, PresentTarget { fit.x, fit.y, fit.width, fit.height })) {
            return 1;
        }
        // Composite failed: fall through to the faithful canvas path below.
    }

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
    uint32_t native_w = 0;
    if (separate_layout) {
        const uint32_t screen_width = index == 0 ? presented_top_width : presented_bottom_width;
        native_w = screen_width;
        placement = pkmn::render::fit_placement(screen_width, pkmn::render::kScreenHeight,
            output.width, output.height, configured_scaling());
    } else {
        native_w = std::max(presented_top_width, presented_bottom_width);
        placement = pkmn::render::stack_placement(presented_top_width, presented_bottom_width,
            ScreenLayout::Stacked, output.width, output.height, configured_scaling());
    }
    const uint32_t native_h
        = separate_layout ? pkmn::render::kScreenHeight : pkmn::render::kScreenHeight * 2;
    if (!placement.visible || placement.width == 0 || placement.height == 0 || native_w == 0) {
        return 0;
    }

    // The inverse of the presented rectangle: the stack's top-left corner is
    // logical (0,0), and the composed canvas spans the placement. Deriving the
    // factor from width/height (not the integer `scale`) keeps the mapping
    // exact in both integer and fit modes. Points on the letterbox land outside
    // the logical canvas, which the frontend's own routing then rejects as
    // "not a touch" - the same outcome as the SDL path.
    const float scale_x = static_cast<float>(placement.width) / static_cast<float>(native_w);
    const float scale_y = static_cast<float>(placement.height) / static_cast<float>(native_h);
    *x = static_cast<int>(std::floor((pixel_x - static_cast<float>(placement.x)) / scale_x));
    *y = static_cast<int>(std::floor((pixel_y - static_cast<float>(placement.y)) / scale_y));
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
