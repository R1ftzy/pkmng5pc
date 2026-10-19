// The runner frontend's native presentation bridge. See
// `pkmn/render/native_present.h` for the contract and
// `docs/adr/0006-ndsrecomp-native-present-hook.md` for why it exists.

#include "pkmn/render/native_present.h"

#include "pkmn/render/backend.h"

#include <SDL3/SDL.h>

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

    canvas = pkmn::render::compose_screens_bgra8(reinterpret_cast<const uint8_t*>(top),
        static_cast<uint32_t>(top_width > 0 ? top_width : pkmn::render::kScreenWidth),
        reinterpret_cast<const uint8_t*>(bottom),
        static_cast<uint32_t>(bottom_width > 0 ? bottom_width : pkmn::render::kScreenWidth),
        ScreenLayout::Stacked, output_width, output_height);

    if (!presenter->present(canvas.view())) {
        // Recoverable (out of date / suboptimal): re-derive the extent and drop
        // this frame rather than presenting a mismatched canvas.
        sync_output_size();
    }
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
