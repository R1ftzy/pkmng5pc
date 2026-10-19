// Standalone smoke driver for the render seam.
//
// It does not touch the game: it builds two synthetic BGR555 DS screens,
// composes them through the layout code, and presents them through the chosen
// backend. That exercises device creation, the swapchain, the upload path and
// out-of-date recovery without a ROM, so `render/` can be built and validated
// in isolation.

#include "pkmn/render/backend.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pkmn::render;

namespace {

BackendKind parse_backend(const std::string& name)
{
    return name == "null" ? BackendKind::Null : BackendKind::Vulkan;
}

void fill_test_screen(std::vector<uint16_t>& screen, uint16_t base, uint32_t frame)
{
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            const uint16_t r = static_cast<uint16_t>((x + frame * 4) & 0x1F);
            const uint16_t g = static_cast<uint16_t>((y + frame * 2) & 0x1F);
            const uint16_t b = static_cast<uint16_t>(base & 0x1F);
            screen[y * kScreenWidth + x] = static_cast<uint16_t>(r | (g << 5) | (b << 10));
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::string backend_name = "vulkan";
    uint32_t frames = 120;
    uint32_t width = 512;
    uint32_t height = 768;
    bool vsync = true;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--backend") {
            backend_name = next();
        } else if (arg == "--frames") {
            frames = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
        } else if (arg == "--width") {
            width = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
        } else if (arg == "--height") {
            height = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
        } else if (arg == "--no-vsync") {
            vsync = false;
        } else if (arg == "--validation") {
            // handled below through Config
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }
    const bool validation = [&] {
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--validation") == 0) {
                return true;
            }
        }
        return false;
    }();

    const BackendKind kind = parse_backend(backend_name);
    SDL_Window* window = nullptr;
    if (kind != BackendKind::Null) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::fprintf(stderr, "SDL video init failed: %s\n", SDL_GetError());
            return 1;
        }
        window = SDL_CreateWindow("pkmng5pc render smoke", static_cast<int>(width),
            static_cast<int>(height), SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!window) {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            return 1;
        }
    }

    std::unique_ptr<Presenter> presenter = create_presenter(kind);
    Config config;
    config.kind = kind;
    config.vsync = vsync;
    config.validation = validation;

    Window handle;
    handle.sdl_window = window;
    if (!presenter->init(handle, config)) {
        std::fprintf(
            stderr, "presenter init failed for backend '%s'\n", to_string(presenter->kind()));
        if (window) {
            SDL_DestroyWindow(window);
        }
        return 1;
    }
    if (!presenter->resize(width, height)) {
        std::fprintf(stderr, "presenter resize failed\n");
        presenter->shutdown();
        return 1;
    }

    std::vector<uint16_t> top(kScreenWidth * kScreenHeight);
    std::vector<uint16_t> bottom(kScreenWidth * kScreenHeight);

    uint32_t presented = 0;
    bool quit = false;
    while (!quit && presented < frames) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                quit = true;
            } else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                const uint32_t new_w = static_cast<uint32_t>(event.window.data1);
                const uint32_t new_h = static_cast<uint32_t>(event.window.data2);
                if (new_w && new_h
                    && (new_w != presenter->width() || new_h != presenter->height())) {
                    presenter->resize(new_w, new_h);
                }
            }
        }

        fill_test_screen(top, 0x1F, presented);
        fill_test_screen(bottom, 0x10, presented);
        CanvasBuffer canvas = compose_screens(top.data(), bottom.data(), ScreenLayout::Stacked,
            presenter->width(), presenter->height());
        if (!presenter->present(canvas.view())) {
            // Swapchain out of date: re-derive the size and try again.
            int w = 0;
            int h = 0;
            if (window) {
                SDL_GetWindowSizeInPixels(window, &w, &h);
            }
            if (w > 0 && h > 0) {
                presenter->resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
            }
            continue;
        }
        ++presented;
    }

    std::printf("[render] backend=%s presented=%u size=%ux%u\n", to_string(presenter->kind()),
        presented, presenter->width(), presenter->height());
    presenter->shutdown();
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    return presented == 0 ? 1 : 0;
}
