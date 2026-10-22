#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "GPU3D.h"
#include "src/gpu3d.h"

#include "pkmn/render/backend.h"
#include "pkmn/render/native_present.h"

#include "ds/ds_3d.h"

namespace {

using pkmn::render::BackendKind;
using pkmn::render::Config;

enum class RendererRequest {
    Off,
    ThreeD,
    TwoD,
    Both,
    Invalid,
};

RendererRequest renderer_request()
{
    const char* value = std::getenv("PKMNG5PC_RENDERER");
    if (!value || !*value || std::strcmp(value, "off") == 0) {
        return RendererRequest::Off;
    }
    if (std::strcmp(value, "native") == 0 || std::strcmp(value, "3d") == 0) {
        return RendererRequest::ThreeD;
    }
    if (std::strcmp(value, "2d") == 0) {
        return RendererRequest::TwoD;
    }
    if (std::strcmp(value, "both") == 0) {
        return RendererRequest::Both;
    }
    return RendererRequest::Invalid;
}

Config sink_config()
{
    Config config;
    config.kind = BackendKind::Vulkan;
    if (const char* gpu = std::getenv("PKMNG5PC_GPU"); gpu && *gpu) {
        config.gpu = gpu;
    }
    return config;
}

melonDS::Renderer3D* installed_sink = nullptr;

} // namespace

extern "C" int pkmng5pc_ds_render_init(void* top_window, void* bottom_window, int separate)
{
    (void)bottom_window;
    (void)separate;

    const RendererRequest request = renderer_request();
    if (request == RendererRequest::Off) {
        return 1;
    }
    if (!pkmng5pc_native_present_enabled()) {
        std::fprintf(stderr,
            "[native-render] PKMNG5PC_RENDERER requires "
            "PKMNG5PC_PRESENT=native\n");
        return 0;
    }
    if (request == RendererRequest::Invalid) {
        std::fprintf(stderr,
            "[native-render] PKMNG5PC_RENDERER must be off, native, 3d, 2d, "
            "or both\n");
        return 0;
    }
    if (request == RendererRequest::TwoD) {
        std::fprintf(stderr,
            "[native-render] the 2D sink is not implemented in this build; "
            "use native, 3d, or both\n");
        return 0;
    }
    if (request == RendererRequest::Both) {
        std::fprintf(stderr,
            "[native-render] the 2D sink is not implemented in this build; "
            "both is unavailable, use native or 3d\n");
        return 0;
    }
    if (installed_sink) {
        return 1;
    }
    if (!top_window) {
        std::fprintf(stderr, "[native-render] no top-level window for the 3D sink\n");
        return 0;
    }

    melonDS::GPU3D* const gpu3d = nds_gpu3d_device();
    if (!gpu3d) {
        std::fprintf(stderr, "[native-render] the GPU3D device is unavailable\n");
        return 0;
    }
    if (gpu3d->GetRenderWidth() != 256u) {
        std::fprintf(stderr,
            "[native-render] the 3D sink currently supports 4:3 (256 px) "
            "only; requested width is %u\n",
            gpu3d->GetRenderWidth());
        return 0;
    }

    auto renderer = pkmn::render::ds::VulkanComputeRenderer::create(
        static_cast<SDL_Window*>(top_window), sink_config());
    if (!renderer) {
        std::fprintf(stderr,
            "[native-render] Vulkan compute 3D sink initialization failed; "
            "the CPU renderer is unchanged\n");
        return 0;
    }

    melonDS::Renderer3D* const raw = renderer.get();
    gpu3d->SetCurrentRenderer(std::move(renderer));
    installed_sink = raw;
    std::fprintf(stderr, "[native-render] 3D sink active (Vulkan compute, 256x192)\n");
    return 1;
}

extern "C" void pkmng5pc_ds_render_shutdown()
{
    if (!installed_sink) {
        return;
    }
    if (melonDS::GPU3D* const gpu3d = nds_gpu3d_device()) {
        if (&gpu3d->GetCurrentRenderer() == installed_sink) {
            nds_gpu3d_restore_soft_renderer();
        }
    }
    installed_sink = nullptr;
}

extern "C" int pkmng5pc_ds_render_consumes_hd() { return 0; }
