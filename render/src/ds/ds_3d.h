#pragma once

#include <cstdint>
#include <memory>

#include "GPU3D.h"

#include "pkmn/render/backend.h"

struct SDL_Window;

namespace pkmn::render::ds {

// Vulkan-compute implementation of melonDS's accelerated 3D rasterizer. Slice
// 1 deliberately keeps the native 256x192 surface and the CPU readback
// contract: `GetLine` and `GetAttrLine` return the same layouts the soft
// renderer produces, and `Accelerated` stays false so the runner keeps its
// faithful 2D/capture/savestate paths.
class VulkanComputeRenderer final : public melonDS::Renderer3D {
public:
    static std::unique_ptr<VulkanComputeRenderer> create(SDL_Window* window, const Config& config);

    ~VulkanComputeRenderer() override;

    VulkanComputeRenderer(const VulkanComputeRenderer&) = delete;
    VulkanComputeRenderer& operator=(const VulkanComputeRenderer&) = delete;

    void Reset(melonDS::GPU& gpu) override;
    void RenderFrame(melonDS::GPU& gpu) override;
    void RestartFrame(melonDS::GPU& gpu) override;
    melonDS::u32* GetLine(int line) override;
    const melonDS::u32* GetAttrLine(int line) override;
    void SetRenderWidth(melonDS::u32 width) override;

    void shutdown();

private:
    VulkanComputeRenderer();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pkmn::render::ds
