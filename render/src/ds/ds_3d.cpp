#include "ds_3d.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "GPU.h"
#include "GPU3D_Soft.h"
#include "GPU3D_Texcache.h"

#include "gpu2d.h"
#include "pkmn/render/ds_frame_policy.h"
#include "src/gpu3d.h"
#include "vulkan_context.h"
#include "vulkan_functions.h"

// SPIR-V blobs are generated at build time with external, unqualified symbols
// (`pkmng5pc_<name>_spv`); these declarations must stay at global scope so the
// linker binds them to the generated definitions. The name list is generated
// from render/src/ds/ds_shaders.txt, the same file the GLSL generator and the
// build rules read.
#include "ds_shader_ids.h"

#define DS_SHADER_DECL(name)                                                                       \
    extern const unsigned char pkmng5pc_##name##_spv[];                                            \
    extern const std::size_t pkmng5pc_##name##_spv_size;

PKMN_DS_SHADER_LIST(DS_SHADER_DECL)

#undef DS_SHADER_DECL

struct ShaderBlob {
    const unsigned char* data;
    std::size_t size;
};

#define DS_SHADER_BLOB(name) { pkmng5pc_##name##_spv, pkmng5pc_##name##_spv_size },

const std::array<ShaderBlob, pkmn::render::ds::kNumShaders> kShaderBlobs
    = { { PKMN_DS_SHADER_LIST(DS_SHADER_BLOB) } };

#undef DS_SHADER_BLOB

namespace pkmn::render::ds {
namespace {

    using namespace pkmn::render::vk;

    // Native DS band geometry. The raster scale (V1 of ADR 0008) multiplies
    // these at init: screen_width = render_width * scale, screen_height =
    // 192 * scale. The compute shaders read the same values as specialization
    // constants, specialized per pipeline at creation.
    constexpr uint32_t kWidth = 256;
    constexpr uint32_t kHeight = 192;
    constexpr uint32_t kTileSize = 8;
    constexpr uint32_t kMaxVariants = 256;
    constexpr uint32_t kMaxPolygons = 2048;
    constexpr uint32_t kBaseYSpanSetups = 6144 * 2;
    constexpr uint32_t kBaseYSpanIndices = 64 * 2048;
    constexpr uint32_t kCoarseTileCountX = 8;
    constexpr uint32_t kCoarseTileCountY = 4;
    constexpr uint32_t kCoarseTileW = kCoarseTileCountX * kTileSize;
    constexpr uint32_t kCoarseTileH = kCoarseTileCountY * kTileSize;
    constexpr uint32_t kBinStride = 2048 / 32;
    constexpr uint32_t kCoarseBinStride = kBinStride / 32;
    constexpr uint32_t kMaxScale = 8;

    constexpr unsigned kPipelineInterpSpansZ = shader_index(ShaderId::ds_interp_spans_z);
    constexpr unsigned kPipelineInterpSpansW = shader_index(ShaderId::ds_interp_spans_w);
    constexpr unsigned kPipelineBinCombined = shader_index(ShaderId::ds_bin_combined);
    constexpr unsigned kPipelineDepthBlendZ = shader_index(ShaderId::ds_depth_blend_z);
    constexpr unsigned kPipelineDepthBlendW = shader_index(ShaderId::ds_depth_blend_w);
    constexpr unsigned kPipelineClearCoarse = shader_index(ShaderId::ds_clear_coarse);
    constexpr unsigned kPipelineClearWorkCount = shader_index(ShaderId::ds_clear_workcount);
    constexpr unsigned kPipelineCalcOffsets = shader_index(ShaderId::ds_calc_offsets);
    constexpr unsigned kPipelineSortWork = shader_index(ShaderId::ds_sort_work);

    ShaderId raster_shader(bool textured, uint32_t blend, bool wbuffer, bool highlight)
    {
        const bool w = wbuffer;
        if (!textured) {
            switch (blend) {
            case 0:
            case 1:
            case 3:
                return w ? ShaderId::ds_raster_no_tex_w : ShaderId::ds_raster_no_tex_z;
            case 2:
                if (highlight) {
                    return w ? ShaderId::ds_raster_no_tex_highlight_w
                             : ShaderId::ds_raster_no_tex_highlight_z;
                }
                return w ? ShaderId::ds_raster_no_tex_toon_w : ShaderId::ds_raster_no_tex_toon_z;
            case 4:
                return w ? ShaderId::ds_raster_shadow_w : ShaderId::ds_raster_shadow_z;
            default:
                return ShaderId::Count;
            }
        }
        switch (blend) {
        case 0:
            return w ? ShaderId::ds_raster_tex_modulate_w : ShaderId::ds_raster_tex_modulate_z;
        case 1:
        case 3:
            return w ? ShaderId::ds_raster_tex_decal_w : ShaderId::ds_raster_tex_decal_z;
        case 2:
            if (highlight) {
                return w ? ShaderId::ds_raster_tex_highlight_w
                         : ShaderId::ds_raster_tex_highlight_z;
            }
            return w ? ShaderId::ds_raster_tex_toon_w : ShaderId::ds_raster_tex_toon_z;
        case 4:
            return w ? ShaderId::ds_raster_shadow_w : ShaderId::ds_raster_shadow_z;
        default:
            return ShaderId::Count;
        }
    }

    ShaderId final_pass_shader(uint32_t flags)
    {
        switch (flags & 0x7u) {
        case 0:
            return ShaderId::ds_final_0;
        case 1:
            return ShaderId::ds_final_edge;
        case 2:
            return ShaderId::ds_final_fog;
        case 3:
            return ShaderId::ds_final_edge_fog;
        case 4:
            return ShaderId::ds_final_aa;
        case 5:
            return ShaderId::ds_final_aa_edge;
        case 6:
            return ShaderId::ds_final_aa_fog;
        default:
            return ShaderId::ds_final_aa_edge_fog;
        }
    }

    constexpr VkDeviceSize kMetaBufferSize = 592;

    // Per-init buffer sizes, derived from the raster scale and band width.
    // The base tile/bin counts come from the scale-1 native surface; the
    // members multiply them out in set_dimensions().
    constexpr VkDeviceSize kBinBaseSize = 5136;
    constexpr VkDeviceSize kMetaOrReadbackAlign = 4;

    struct ScreenDims {
        uint32_t scale = 1;
        uint32_t render_width = kWidth;
        uint32_t screen_width = kWidth;
        uint32_t screen_height = kHeight;
        uint32_t tiles_per_line = kWidth / kTileSize;
        uint32_t tile_lines = kHeight / kTileSize;
        uint32_t max_work_tiles = (kWidth / kTileSize) * (kHeight / kTileSize) * 16;
        uint32_t max_y_span_setups = kBaseYSpanSetups;
        uint32_t max_y_span_indices = kBaseYSpanIndices;
        VkDeviceSize bin_buffer_size = 0;
        VkDeviceSize workdesc_buffer_size = 0;
        VkDeviceSize result_buffer_size = 0;
        VkDeviceSize tile_buffer_size = 0;
        VkDeviceSize readback_size = kWidth * kHeight * 4;
    };

    struct SpanSetupY {
        int32_t Z0, Z1, W0, W1;
        int32_t ColorR0, ColorG0, ColorB0;
        int32_t ColorR1, ColorG1, ColorB1;
        int32_t TexcoordU0, TexcoordV0;
        int32_t TexcoordU1, TexcoordV1;
        int32_t I0, I1;
        int32_t Linear;
        int32_t IRecip;
        int32_t W0n, W0d, W1d;
        int32_t Increment;
        int32_t X0, X1, Y0, Y1;
        int32_t XMin, XMax;
        int32_t DxInitial;
        int32_t XCovIncr;
        uint32_t IsDummy;
    };

    struct SpanSetupX {
        int32_t X0, X1;
        int32_t EdgeLenL, EdgeLenR, EdgeCovL, EdgeCovR;
        int32_t XRecip;
        uint32_t Flags;
        int32_t Z0, Z1, W0, W1;
        int32_t ColorR0, ColorG0, ColorB0;
        int32_t ColorR1, ColorG1, ColorB1;
        int32_t TexcoordU0, TexcoordV0;
        int32_t TexcoordU1, TexcoordV1;
        int32_t CovLInitial, CovRInitial;
    };

    struct SetupIndices {
        uint32_t PolyIdx, SpanIdxL, SpanIdxR, Y;
    };

    struct RenderPolygon {
        uint32_t FirstXSpan;
        int32_t YTop, YBot;
        int32_t XMin, XMax;
        int32_t XMinY, XMaxY;
        uint32_t Variant;
        uint32_t Attr;
        float TextureLayer;
    };

    struct MetaUniform {
        uint32_t NumPolygons;
        uint32_t NumVariants;
        uint32_t AlphaRef;
        uint32_t DispCnt;
        uint32_t ToonTable[4 * 34];
        uint32_t ClearColor, ClearDepth, ClearAttr;
        uint32_t FogOffset, FogShift, FogColor;
    };

    struct Variant {
        uint32_t Texture = 0;
        uint8_t Sampler = 0;
        uint8_t BlendMode = 0;
        uint16_t Width = 0;
        uint16_t Height = 0;

        bool operator==(const Variant& other) const
        {
            return Texture == other.Texture && Sampler == other.Sampler
                && BlendMode == other.BlendMode;
        }
    };

    struct PushConstants {
        uint32_t CurVariant;
        uint32_t Pad;
        float InvTextureWidth;
        float InvTextureHeight;
    };

    static_assert(sizeof(SpanSetupY) == 124);
    static_assert(sizeof(SpanSetupX) == 96);
    static_assert(sizeof(SetupIndices) == 16);
    static_assert(sizeof(RenderPolygon) == 40);
    static_assert(sizeof(MetaUniform) == 584);
    static_assert(sizeof(PushConstants) == 16);

    constexpr VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
    };

    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct Texture {
        Image image;
        uint32_t layers = 0;
        bool batch_transitioned = false;
        bool has_data = false;
    };

    bool create_host_buffer(
        VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out)
    {
        VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(context.device(), &info, nullptr, &out.buffer) != VK_SUCCESS) {
            return false;
        }

        VkMemoryRequirements requirements {};
        vkGetBufferMemoryRequirements(context.device(), out.buffer, &requirements);
        const uint32_t memory_type = context.find_memory_type(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memory_type == UINT32_MAX) {
            vkDestroyBuffer(context.device(), out.buffer, nullptr);
            out.buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memory_type;
        if (vkAllocateMemory(context.device(), &allocate, nullptr, &out.memory) != VK_SUCCESS) {
            vkDestroyBuffer(context.device(), out.buffer, nullptr);
            out.buffer = VK_NULL_HANDLE;
            return false;
        }
        if (vkBindBufferMemory(context.device(), out.buffer, out.memory, 0) != VK_SUCCESS) {
            vkFreeMemory(context.device(), out.memory, nullptr);
            vkDestroyBuffer(context.device(), out.buffer, nullptr);
            out = {};
            return false;
        }

        if (vkMapMemory(context.device(), out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped)
            != VK_SUCCESS) {
            vkFreeMemory(context.device(), out.memory, nullptr);
            vkDestroyBuffer(context.device(), out.buffer, nullptr);
            out = {};
            return false;
        }
        std::memset(out.mapped, 0, static_cast<std::size_t>(size));
        out.size = size;
        return true;
    }

    void destroy_buffer(VulkanContext& context, Buffer& buffer)
    {
        if (buffer.mapped) {
            vkUnmapMemory(context.device(), buffer.memory);
        }
        if (buffer.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(context.device(), buffer.buffer, nullptr);
        }
        if (buffer.memory != VK_NULL_HANDLE) {
            vkFreeMemory(context.device(), buffer.memory, nullptr);
        }
        buffer = {};
    }

    bool create_device_image(VulkanContext& context, uint32_t width, uint32_t height,
        uint32_t layers, VkFormat format, VkImageUsageFlags usage, VkImageViewType view_type,
        Image& out)
    {
        VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(context.device(), &info, nullptr, &out.image) != VK_SUCCESS) {
            return false;
        }

        VkMemoryRequirements requirements {};
        vkGetImageMemoryRequirements(context.device(), out.image, &requirements);
        const uint32_t memory_type = context.find_memory_type(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memory_type == UINT32_MAX) {
            vkDestroyImage(context.device(), out.image, nullptr);
            out = {};
            return false;
        }

        VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memory_type;
        if (vkAllocateMemory(context.device(), &allocate, nullptr, &out.memory) != VK_SUCCESS) {
            vkDestroyImage(context.device(), out.image, nullptr);
            out = {};
            return false;
        }
        if (vkBindImageMemory(context.device(), out.image, out.memory, 0) != VK_SUCCESS) {
            vkFreeMemory(context.device(), out.memory, nullptr);
            vkDestroyImage(context.device(), out.image, nullptr);
            out = {};
            return false;
        }

        VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = out.image;
        view.viewType = view_type;
        view.format = format;
        view.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.baseMipLevel = 0;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.baseArrayLayer = 0;
        view.subresourceRange.layerCount = layers;
        if (vkCreateImageView(context.device(), &view, nullptr, &out.view) != VK_SUCCESS) {
            vkFreeMemory(context.device(), out.memory, nullptr);
            vkDestroyImage(context.device(), out.image, nullptr);
            out = {};
            return false;
        }

        out.width = width;
        out.height = height;
        out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    void destroy_image(VulkanContext& context, Image& image)
    {
        if (image.view != VK_NULL_HANDLE) {
            vkDestroyImageView(context.device(), image.view, nullptr);
        }
        if (image.image != VK_NULL_HANDLE) {
            vkDestroyImage(context.device(), image.image, nullptr);
        }
        if (image.memory != VK_NULL_HANDLE) {
            vkFreeMemory(context.device(), image.memory, nullptr);
        }
        image = {};
    }

    void image_barrier(VkCommandBuffer command, VkImage image, VkImageLayout old_layout,
        VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
        VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage, uint32_t layers)
    {
        VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = src_access;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = layers;
        vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void memory_barrier(VkCommandBuffer command)
    {
        VkMemoryBarrier barrier { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
            | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1,
            &barrier, 0, nullptr, 0, nullptr);
    }

    class CommandRunner {
    public:
        CommandRunner() = default;
        ~CommandRunner() { shutdown(); }

        CommandRunner(const CommandRunner&) = delete;
        CommandRunner& operator=(const CommandRunner&) = delete;

        bool init(VulkanContext& context)
        {
            context_ = &context;
            VkCommandPoolCreateInfo pool_info { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool_info.queueFamilyIndex = context.queue_family();
            if (vkCreateCommandPool(context.device(), &pool_info, nullptr, &pool_) != VK_SUCCESS) {
                return false;
            }

            VkCommandBufferAllocateInfo command_info {
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
            };
            command_info.commandPool = pool_;
            command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            command_info.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(context.device(), &command_info, &command_)
                != VK_SUCCESS) {
                return false;
            }

            VkFenceCreateInfo fence_info { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            if (vkCreateFence(context.device(), &fence_info, nullptr, &fence_) != VK_SUCCESS) {
                return false;
            }
            return true;
        }

        bool run(const std::function<void(VkCommandBuffer)>& record)
        {
            if (!context_) {
                return false;
            }
            if (pending_) {
                if (!wait()) {
                    return false;
                }
            }

            if (vkResetCommandBuffer(command_, 0) != VK_SUCCESS) {
                return false;
            }
            VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(command_, &begin) != VK_SUCCESS) {
                return false;
            }
            record(command_);
            if (vkEndCommandBuffer(command_) != VK_SUCCESS) {
                return false;
            }

            VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command_;
            if (vkResetFences(context_->device(), 1, &fence_) != VK_SUCCESS) {
                return false;
            }
            if (vkQueueSubmit(context_->queue(), 1, &submit, fence_) != VK_SUCCESS) {
                return false;
            }
            pending_ = true;
            return wait();
        }

        bool wait()
        {
            if (!pending_) {
                return true;
            }
            if (vkWaitForFences(context_->device(), 1, &fence_, VK_TRUE, UINT64_MAX)
                != VK_SUCCESS) {
                return false;
            }
            pending_ = false;
            return true;
        }

        void shutdown()
        {
            if (!context_) {
                return;
            }
            if (pending_) {
                wait();
            }
            if (fence_ != VK_NULL_HANDLE) {
                vkDestroyFence(context_->device(), fence_, nullptr);
                fence_ = VK_NULL_HANDLE;
            }
            if (pool_ != VK_NULL_HANDLE) {
                vkDestroyCommandPool(context_->device(), pool_, nullptr);
                pool_ = VK_NULL_HANDLE;
            }
            command_ = VK_NULL_HANDLE;
            context_ = nullptr;
        }

    private:
        VulkanContext* context_ = nullptr;
        VkCommandPool pool_ = VK_NULL_HANDLE;
        VkCommandBuffer command_ = VK_NULL_HANDLE;
        VkFence fence_ = VK_NULL_HANDLE;
        bool pending_ = false;
    };

    class TextureStore {
    public:
        struct PendingUpload {
            Buffer staging;
            uint32_t id = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t layer = 0;
        };

        bool init(VulkanContext& context)
        {
            context_ = &context;
            return true;
        }

        uint32_t create_texture(uint32_t width, uint32_t height, uint32_t layers)
        {
            if (!context_ || width == 0 || height == 0 || layers == 0) {
                return 0;
            }
            auto texture = std::make_unique<Texture>();
            if (!create_device_image(*context_, width, height, layers, VK_FORMAT_R8G8B8A8_UINT,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_VIEW_TYPE_2D_ARRAY, texture->image)) {
                return 0;
            }
            texture->layers = layers;
            const uint32_t id = next_id_++;
            textures_.push_back(std::move(texture));
            return id;
        }

        bool upload_texture(
            uint32_t id, uint32_t width, uint32_t height, uint32_t layer, const void* data)
        {
            Texture* texture = get(id);
            if (!texture || !data || width != texture->image.width
                || height != texture->image.height || layer >= texture->layers) {
                return false;
            }

            Buffer staging;
            if (!create_host_buffer(*context_, static_cast<VkDeviceSize>(width) * height * 4,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) {
                return false;
            }
            std::memcpy(staging.mapped, data, static_cast<std::size_t>(width) * height * 4);
            pending_.push_back(PendingUpload { staging, id, width, height, layer });
            texture->has_data = true;
            return true;
        }

        void record_pending_uploads(VkCommandBuffer command)
        {
            abort_pending_uploads();
            if (pending_.empty()) {
                return;
            }
            for (const PendingUpload& upload : pending_) {
                Texture* texture = get(upload.id);
                if (!texture) {
                    continue;
                }
                if (!texture->batch_transitioned) {
                    const VkImageLayout old_layout = texture->image.layout;
                    const VkAccessFlags old_access
                        = old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
                    image_barrier(command, texture->image.image, old_layout,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, old_access,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                            : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, texture->layers);
                    texture->batch_transitioned = true;
                    batch_ids_.push_back(upload.id);
                }

                VkBufferImageCopy copy {};
                copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                copy.imageSubresource.mipLevel = 0;
                copy.imageSubresource.baseArrayLayer = upload.layer;
                copy.imageSubresource.layerCount = 1;
                copy.imageExtent = { upload.width, upload.height, 1 };
                vkCmdCopyBufferToImage(command, upload.staging.buffer, texture->image.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            }
            for (uint32_t id : batch_ids_) {
                Texture* texture = get(id);
                if (!texture) {
                    continue;
                }
                image_barrier(command, texture->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, texture->layers);
            }
        }

        void finish_pending_uploads()
        {
            for (PendingUpload& upload : pending_) {
                destroy_buffer(*context_, upload.staging);
            }
            pending_.clear();
            for (uint32_t id : batch_ids_) {
                Texture* texture = get(id);
                if (!texture) {
                    continue;
                }
                texture->image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                texture->batch_transitioned = false;
            }
            batch_ids_.clear();
        }

        void abort_pending_uploads()
        {
            for (uint32_t id : batch_ids_) {
                if (Texture* texture = get(id)) {
                    texture->batch_transitioned = false;
                }
            }
            batch_ids_.clear();
        }

        void destroy_texture(uint32_t id)
        {
            if (id == 0 || id > textures_.size() || !textures_[id - 1]) {
                return;
            }
            destroy_image(*context_, textures_[id - 1]->image);
            textures_[id - 1].reset();
        }

        Texture* get(uint32_t id)
        {
            if (id == 0 || id > textures_.size()) {
                return nullptr;
            }
            return textures_[id - 1].get();
        }

        void shutdown()
        {
            if (!context_) {
                return;
            }
            for (PendingUpload& upload : pending_) {
                destroy_buffer(*context_, upload.staging);
            }
            pending_.clear();
            batch_ids_.clear();
            for (auto& texture : textures_) {
                if (texture) {
                    destroy_image(*context_, texture->image);
                }
            }
            textures_.clear();
            context_ = nullptr;
        }

    private:
        VulkanContext* context_ = nullptr;
        std::vector<std::unique_ptr<Texture>> textures_;
        std::vector<PendingUpload> pending_;
        std::vector<uint32_t> batch_ids_;
        uint32_t next_id_ = 1;
    };

    struct VulkanTextureLoader {
        std::shared_ptr<TextureStore> store;
        std::function<void(uint32_t, uint32_t, uint32_t, uint32_t, const void*)> on_upload;

        uint32_t GenerateTexture(uint32_t width, uint32_t height, uint32_t layers) const
        {
            return store->create_texture(width, height, layers);
        }

        void UploadTexture(
            uint32_t id, uint32_t width, uint32_t height, uint32_t layer, const void* data) const
        {
            store->upload_texture(id, width, height, layer, data);
            if (on_upload) {
                on_upload(id, width, height, layer, data);
            }
        }

        void DeleteTexture(uint32_t id) const { store->destroy_texture(id); }
    };

} // namespace

struct VulkanComputeRenderer::Impl {
    VulkanContext* context = nullptr;
    std::unique_ptr<CommandRunner> upload_runner;
    std::shared_ptr<TextureStore> texture_store;
    std::unique_ptr<melonDS::Texcache<VulkanTextureLoader, uint32_t>> texcache;

    Buffer yspan_buffer;
    Buffer xspan_buffer;
    Buffer polygon_buffer;
    Buffer result_buffer;
    Buffer bin_buffer;
    Buffer workdesc_buffer;
    Buffer color_tile_buffer;
    Buffer depth_tile_buffer;
    Buffer attr_tile_buffer;
    Buffer meta_buffer;
    Buffer setup_index_buffer;
    Buffer color_readback;
    Buffer attr_readback;

    Image final_fb;
    Image lowres_fb;
    Image attr_fb;

    std::vector<SpanSetupY> yspans;
    std::vector<SetupIndices> setup_indices;
    std::vector<RenderPolygon> render_polygons;
    std::array<Variant, kMaxVariants> variants_ {};
    uint32_t yspan_count = 0;
    uint32_t setup_index_count = 0;

    // Raster scale and the derived surface geometry (ADR 0008, slice V1).
    // Fixed at init: the shaders specialize to the same values per pipeline.
    ScreenDims dims;
    bool scale_logged = false;

    VkSampler samplers[9] {};
    uint32_t dummy_texture = 0;

    VkDescriptorSetLayout set0_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout set1_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout set2_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;

    VkDescriptorPool common_pool = VK_NULL_HANDLE;
    VkDescriptorPool texture_pool = VK_NULL_HANDLE;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    VkDescriptorSet set1_dummy = VK_NULL_HANDLE;
    VkDescriptorSet set2 = VK_NULL_HANDLE;

    std::array<VkPipeline, kNumShaders> pipelines {};
    VkCommandPool frame_pool = VK_NULL_HANDLE;
    VkCommandBuffer frame_command = VK_NULL_HANDLE;
    VkFence frame_fence = VK_NULL_HANDLE;

    bool initialized = false;
    bool failed = false;
    bool verify = false;
    bool cpu_lines = false;
    bool frame_submitted = false;
    bool have_frame = false;
    bool logged_failure = false;
    uint32_t gpu_dispcnt = 0;

    std::unique_ptr<melonDS::SoftRenderer> soft;
    uint64_t verify_frames = 0;
    uint64_t verify_skipped = 0;
    uint64_t verify_polygon_frames = 0;
    uint64_t verify_bad_frames = 0;
    uint64_t verify_attr_bad_pixels = 0;
    uint32_t verify_max_channel_diff = 0;
    bool verify_logged_detail = false;

    std::string dump_dir;
    uint64_t dump_frame = 0;
    uint64_t dump_every = 30;
    uint64_t frame_counter = 0;
    bool dump_uploads = false;
    void dump_upload(
        uint32_t id, uint32_t width, uint32_t height, uint32_t layer, const void* data);
    void dump_frame_tables(uint32_t num_variants, uint32_t num_polygons, bool wbuffer);

    bool initialize(SDL_Window* window, const Config& config);
    void shutdown();

    void reset(melonDS::GPU& gpu);
    void render_frame(melonDS::GPU& gpu);
    void render_cpu(melonDS::GPU& gpu);
    void verify_frame(melonDS::GPU& gpu);

    bool create_samplers();
    bool create_buffers();
    bool create_images();
    bool create_descriptor_layouts();
    bool create_descriptor_pools();
    bool create_common_descriptors();
    bool create_pipelines();
    bool create_frame_resources();
    bool create_dummy_texture();

    bool wait_frame();
    void fail(const char* what);

    void setup_attrs(SpanSetupY* span, const melonDS::Polygon* poly, int from, int to);
    void setup_y_span_dummy(RenderPolygon* rp, SpanSetupY* span, const melonDS::Polygon* poly,
        int vertex, int side, const int32_t positions[10][2]);
    void setup_y_span(RenderPolygon* rp, SpanSetupY* span, const melonDS::Polygon* poly, int from,
        int to, int side, const int32_t positions[10][2]);

    bool build_spans(melonDS::GPU& gpu, uint32_t& num_variants, bool& wbuffer);
    void build_meta(const melonDS::GPU& gpu, const Variant* variants, uint32_t num_variants);
    bool build_variant_sets(
        const Variant* variants, uint32_t num_variants, std::vector<VkDescriptorSet>& out_sets);
    bool record_frame(uint32_t num_variants, const Variant* variants,
        const std::vector<VkDescriptorSet>& variant_sets, bool wbuffer, uint32_t num_polygons);
};

bool VulkanComputeRenderer::Impl::initialize(SDL_Window* window, const Config& config)
{
    (void)window;
    (void)config;

    context = shared_context();
    if (!context) {
        std::fprintf(stderr,
            "[native-render] no shared Vulkan context; the presenter must start before the 3D "
            "sink\n");
        return false;
    }
    if (!context->compute_capable()) {
        std::fprintf(
            stderr, "[native-render] the shared Vulkan device has no compute-capable queue\n");
        return false;
    }

    upload_runner = std::make_unique<CommandRunner>();
    if (!upload_runner->init(*context)) {
        std::fprintf(stderr, "[native-render] upload command runner creation failed\n");
        return false;
    }

    texture_store = std::make_shared<TextureStore>();
    if (!texture_store->init(*context)) {
        std::fprintf(stderr, "[native-render] texture store creation failed\n");
        return false;
    }
    if (const char* dir = std::getenv("PKMNG5PC_DS3D_DUMP_DIR")) {
        dump_dir = dir;
    }
    if (const char* value = std::getenv("PKMNG5PC_DS3D_DUMP_FRAME")) {
        dump_frame = std::strtoull(value, nullptr, 10);
    }
    if (const char* value = std::getenv("PKMNG5PC_DS3D_DUMP_EVERY")) {
        dump_every = std::strtoull(value, nullptr, 10);
    }
    dump_uploads = std::getenv("PKMNG5PC_DS3D_DUMP_UPLOADS") != nullptr;

    // Raster scale (ADR 0008, slice V1). Fixed for the life of the renderer;
    // the compute pipelines specialize to the same values.
    dims.scale = 1;
    if (const char* value = std::getenv("PKMNG5PC_RENDER_SCALE")) {
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (!end || *end != '\0' || parsed < 1 || parsed > static_cast<long>(kMaxScale)) {
            std::fprintf(stderr,
                "[native-render] invalid PKMNG5PC_RENDER_SCALE '%s' (expected 1..%u)\n", value,
                static_cast<unsigned>(kMaxScale));
            return false;
        }
        dims.scale = static_cast<uint32_t>(parsed);
    }
    dims.render_width = kWidth;
    if (melonDS::GPU3D* gpu3d = nds_gpu3d_device()) {
        dims.render_width = gpu3d->GetRenderWidth();
    }
    if (dims.render_width < kWidth || dims.render_width > 448 || (dims.render_width & 1) != 0) {
        std::fprintf(stderr,
            "[native-render] render width %u is out of range (expected an even 256..448)\n",
            static_cast<unsigned>(dims.render_width));
        return false;
    }
    dims.screen_width = dims.render_width * dims.scale;
    dims.screen_height = kHeight * dims.scale;
    dims.tiles_per_line = dims.screen_width / kTileSize;
    dims.tile_lines = dims.screen_height / kTileSize;
    dims.max_work_tiles = dims.tiles_per_line * dims.tile_lines * 16;
    dims.max_y_span_setups = kBaseYSpanSetups * dims.scale;
    dims.max_y_span_indices = kBaseYSpanIndices * dims.scale;
    dims.bin_buffer_size = kBinBaseSize
        + static_cast<VkDeviceSize>(dims.tiles_per_line) * dims.tile_lines * kCoarseBinStride * 4
        + static_cast<VkDeviceSize>(dims.tiles_per_line) * dims.tile_lines * kBinStride * 4 * 2;
    dims.workdesc_buffer_size = static_cast<VkDeviceSize>(dims.max_work_tiles) * 2 * 4 * 2;
    dims.result_buffer_size
        = static_cast<VkDeviceSize>(4) * 3 * 2 * dims.screen_width * dims.screen_height;
    dims.tile_buffer_size
        = static_cast<VkDeviceSize>(4) * kTileSize * kTileSize * dims.max_work_tiles;
    dims.readback_size = static_cast<VkDeviceSize>(dims.render_width) * kHeight * 4;
    // The compute shaders dispatch whole coarse tiles, so the hi-res surface
    // must tile exactly; 342 (16:9) stays a faithful-path width.
    if ((dims.screen_width % kCoarseTileW) != 0 || (dims.screen_height % kCoarseTileH) != 0) {
        std::fprintf(stderr,
            "[native-render] render width %u at %ux scale is unsupported; the hi-res "
            "surface must be a %u-px multiple (use the faithful renderer for this width)\n",
            static_cast<unsigned>(dims.render_width), static_cast<unsigned>(dims.scale),
            static_cast<unsigned>(kCoarseTileW));
        return false;
    }
    texcache = std::make_unique<melonDS::Texcache<VulkanTextureLoader, uint32_t>>(
        VulkanTextureLoader { texture_store,
            [this](uint32_t id, uint32_t width, uint32_t height, uint32_t layer, const void* data) {
                dump_upload(id, width, height, layer, data);
            } });

    yspans.resize(dims.max_y_span_setups);
    setup_indices.resize(dims.max_y_span_indices);
    render_polygons.resize(kMaxPolygons);

    if (!create_samplers() || !create_buffers() || !create_images() || !create_dummy_texture()
        || !create_descriptor_layouts() || !create_descriptor_pools()
        || !create_common_descriptors() || !create_pipelines() || !create_frame_resources()) {
        std::fprintf(stderr, "[native-render] Vulkan DS renderer resource creation failed\n");
        return false;
    }

    bool images_ok = false;
    if (!upload_runner->run([&](VkCommandBuffer command) {
            image_barrier(command, final_fb.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1);
            image_barrier(command, lowres_fb.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1);
            image_barrier(command, attr_fb.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1);
            images_ok = true;
        })) {
        return false;
    }
    if (!images_ok) {
        return false;
    }
    final_fb.layout = VK_IMAGE_LAYOUT_GENERAL;
    lowres_fb.layout = VK_IMAGE_LAYOUT_GENERAL;
    attr_fb.layout = VK_IMAGE_LAYOUT_GENERAL;

    soft = std::make_unique<melonDS::SoftRenderer>();
    verify = std::getenv("PKMNG5PC_DS3D_VERIFY") != nullptr;
    if (verify) {
        std::fprintf(stderr, "[ds3d-verify] soft-reference verification enabled\n");
    }

    initialized = true;
    return true;
}

void VulkanComputeRenderer::Impl::shutdown()
{
    if (!context || !context->device()) {
        context = nullptr;
        return;
    }
    wait_frame();
    if (verify) {
        std::fprintf(stderr,
            "[ds3d-verify] summary frames=%llu skipped=%llu polygon_frames=%llu bad_frames=%llu "
            "max_channel_diff=%u attr_bad_pixels=%llu\n",
            static_cast<unsigned long long>(verify_frames),
            static_cast<unsigned long long>(verify_skipped),
            static_cast<unsigned long long>(verify_polygon_frames),
            static_cast<unsigned long long>(verify_bad_frames), verify_max_channel_diff,
            static_cast<unsigned long long>(verify_attr_bad_pixels));
    }
    soft.reset();
    if (texcache) {
        texcache->Reset();
    }
    if (texture_store) {
        texture_store->shutdown();
    }
    if (upload_runner) {
        upload_runner->shutdown();
    }

    for (VkSampler& sampler : samplers) {
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(context->device(), sampler, nullptr);
            sampler = VK_NULL_HANDLE;
        }
    }
    for (VkPipeline& pipeline : pipelines) {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(context->device(), pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
    if (pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(context->device(), pipeline_layout, nullptr);
        pipeline_layout = VK_NULL_HANDLE;
    }
    if (set0_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(context->device(), set0_layout, nullptr);
        set0_layout = VK_NULL_HANDLE;
    }
    if (set1_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(context->device(), set1_layout, nullptr);
        set1_layout = VK_NULL_HANDLE;
    }
    if (set2_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(context->device(), set2_layout, nullptr);
        set2_layout = VK_NULL_HANDLE;
    }
    if (common_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context->device(), common_pool, nullptr);
        common_pool = VK_NULL_HANDLE;
    }
    if (texture_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context->device(), texture_pool, nullptr);
        texture_pool = VK_NULL_HANDLE;
    }
    if (frame_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(context->device(), frame_pool, nullptr);
        frame_pool = VK_NULL_HANDLE;
    }
    if (frame_fence != VK_NULL_HANDLE) {
        vkDestroyFence(context->device(), frame_fence, nullptr);
        frame_fence = VK_NULL_HANDLE;
    }

    destroy_image(*context, final_fb);
    destroy_image(*context, lowres_fb);
    destroy_image(*context, attr_fb);
    destroy_buffer(*context, yspan_buffer);
    destroy_buffer(*context, xspan_buffer);
    destroy_buffer(*context, polygon_buffer);
    destroy_buffer(*context, result_buffer);
    destroy_buffer(*context, bin_buffer);
    destroy_buffer(*context, workdesc_buffer);
    destroy_buffer(*context, color_tile_buffer);
    destroy_buffer(*context, depth_tile_buffer);
    destroy_buffer(*context, attr_tile_buffer);
    destroy_buffer(*context, meta_buffer);
    destroy_buffer(*context, setup_index_buffer);
    destroy_buffer(*context, color_readback);
    destroy_buffer(*context, attr_readback);

    context->wait_idle();
    context = nullptr;
    initialized = false;
    frame_submitted = false;
}

bool VulkanComputeRenderer::Impl::create_samplers()
{
    const VkSamplerAddressMode modes[3] = { VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT };
    for (uint32_t y = 0; y < 3; ++y) {
        for (uint32_t x = 0; x < 3; ++x) {
            VkSamplerCreateInfo info { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            info.magFilter = VK_FILTER_NEAREST;
            info.minFilter = VK_FILTER_NEAREST;
            info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            info.addressModeU = modes[x];
            info.addressModeV = modes[y];
            info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.minLod = 0.0f;
            info.maxLod = 0.0f;
            info.maxAnisotropy = 1.0f;
            if (vkCreateSampler(context->device(), &info, nullptr, &samplers[x + y * 3])
                != VK_SUCCESS) {
                return false;
            }
        }
    }
    return true;
}

bool VulkanComputeRenderer::Impl::create_buffers()
{
    const VkBufferUsageFlags ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!create_host_buffer(
            *context, sizeof(SpanSetupY) * dims.max_y_span_setups, ssbo, yspan_buffer)
        || !create_host_buffer(
            *context, sizeof(SpanSetupX) * dims.max_y_span_indices, ssbo, xspan_buffer)
        || !create_host_buffer(*context, sizeof(RenderPolygon) * kMaxPolygons, ssbo, polygon_buffer)
        || !create_host_buffer(*context, dims.result_buffer_size, ssbo, result_buffer)
        || !create_host_buffer(
            *context, dims.bin_buffer_size, ssbo | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, bin_buffer)
        || !create_host_buffer(*context, dims.workdesc_buffer_size, ssbo, workdesc_buffer)
        || !create_host_buffer(*context, dims.tile_buffer_size, ssbo, color_tile_buffer)
        || !create_host_buffer(*context, dims.tile_buffer_size, ssbo, depth_tile_buffer)
        || !create_host_buffer(*context, dims.tile_buffer_size, ssbo, attr_tile_buffer)
        || !create_host_buffer(
            *context, kMetaBufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, meta_buffer)
        || !create_host_buffer(
            *context, sizeof(SetupIndices) * dims.max_y_span_indices, ssbo, setup_index_buffer)
        || !create_host_buffer(
            *context, dims.readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, color_readback)
        || !create_host_buffer(
            *context, dims.readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, attr_readback)) {
        return false;
    }
    return true;
}

bool VulkanComputeRenderer::Impl::create_images()
{
    // FinalFB carries the hi-res color surface (the HD-composite source, ADR
    // 0008 slice V2); LowResFB and AttrFB are the faithful 1x planes the
    // FinalPass writes at block boundaries, whatever the scale. The readback
    // is point-sampled from them: averaging the hi-res surface before the CPU
    // composite blurs the 2D stack (tried in slice V1 and reverted).
    if (!create_device_image(*context, dims.screen_width, dims.screen_height, 1,
            VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_VIEW_TYPE_2D, final_fb)
        || !create_device_image(*context, dims.render_width, kHeight, 1, VK_FORMAT_R8G8B8A8_UINT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_VIEW_TYPE_2D,
            lowres_fb)
        || !create_device_image(*context, dims.render_width, kHeight, 1, VK_FORMAT_R32_UINT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_VIEW_TYPE_2D,
            attr_fb)) {
        return false;
    }
    return true;
}

bool VulkanComputeRenderer::Impl::create_descriptor_layouts()
{
    {
        std::array<VkDescriptorSetLayoutBinding, 11> bindings {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        for (uint32_t i = 1; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(context->device(), &info, nullptr, &set0_layout)
            != VK_SUCCESS) {
            return false;
        }
    }
    {
        VkDescriptorSetLayoutBinding binding {};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo info {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };
        info.bindingCount = 1;
        info.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(context->device(), &info, nullptr, &set1_layout)
            != VK_SUCCESS) {
            return false;
        }
    }
    {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings {};
        for (uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(context->device(), &info, nullptr, &set2_layout)
            != VK_SUCCESS) {
            return false;
        }
    }

    const VkDescriptorSetLayout layouts[3] = { set0_layout, set1_layout, set2_layout };
    VkPushConstantRange push {};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.offset = 0;
    push.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo info { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    info.setLayoutCount = 3;
    info.pSetLayouts = layouts;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    return vkCreatePipelineLayout(context->device(), &info, nullptr, &pipeline_layout)
        == VK_SUCCESS;
}

bool VulkanComputeRenderer::Impl::create_descriptor_pools()
{
    {
        VkDescriptorPoolSize sizes[4] {};
        sizes[0] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
        sizes[1] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10 };
        sizes[2] = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 };
        sizes[3] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo info { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        info.maxSets = 3;
        info.poolSizeCount = 4;
        info.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(context->device(), &info, nullptr, &common_pool) != VK_SUCCESS) {
            return false;
        }
    }
    {
        VkDescriptorPoolSize size { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxVariants };
        VkDescriptorPoolCreateInfo info { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        info.maxSets = kMaxVariants;
        info.poolSizeCount = 1;
        info.pPoolSizes = &size;
        if (vkCreateDescriptorPool(context->device(), &info, nullptr, &texture_pool)
            != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool VulkanComputeRenderer::Impl::create_common_descriptors()
{
    VkDescriptorSetLayout layouts[2] = { set0_layout, set2_layout };
    VkDescriptorSet sets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDescriptorSetAllocateInfo allocate { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocate.descriptorPool = common_pool;
    allocate.descriptorSetCount = 2;
    allocate.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(context->device(), &allocate, sets) != VK_SUCCESS) {
        return false;
    }
    set0 = sets[0];
    set2 = sets[1];

    VkDescriptorSetAllocateInfo dummy_allocate { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dummy_allocate.descriptorPool = common_pool;
    dummy_allocate.descriptorSetCount = 1;
    dummy_allocate.pSetLayouts = &set1_layout;
    if (vkAllocateDescriptorSets(context->device(), &dummy_allocate, &set1_dummy) != VK_SUCCESS) {
        return false;
    }

    const Buffer* buffers[11] = { &meta_buffer, &polygon_buffer, &xspan_buffer, &yspan_buffer,
        &result_buffer, &bin_buffer, &workdesc_buffer, &color_tile_buffer, &depth_tile_buffer,
        &attr_tile_buffer, &setup_index_buffer };
    VkDescriptorBufferInfo buffer_info[11] {};
    VkWriteDescriptorSet writes[11] {};
    for (uint32_t i = 0; i < 11; ++i) {
        buffer_info[i].buffer = buffers[i]->buffer;
        buffer_info[i].offset = 0;
        buffer_info[i].range = i == 0 ? kMetaBufferSize : buffers[i]->size;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set0;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType
            = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffer_info[i];
    }
    vkUpdateDescriptorSets(context->device(), 11, writes, 0, nullptr);

    const Image* images[3] = { &final_fb, &lowres_fb, &attr_fb };
    VkDescriptorImageInfo image_info[3] {};
    VkWriteDescriptorSet image_writes[3] {};
    for (uint32_t i = 0; i < 3; ++i) {
        image_info[i].imageView = images[i]->view;
        image_info[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        image_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        image_writes[i].dstSet = set2;
        image_writes[i].dstBinding = i;
        image_writes[i].descriptorCount = 1;
        image_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        image_writes[i].pImageInfo = &image_info[i];
    }
    vkUpdateDescriptorSets(context->device(), 3, image_writes, 0, nullptr);

    Texture* dummy = texture_store->get(dummy_texture);
    if (!dummy) {
        return false;
    }
    VkDescriptorImageInfo dummy_info {};
    dummy_info.sampler = samplers[0];
    dummy_info.imageView = dummy->image.view;
    dummy_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet dummy_write { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    dummy_write.dstSet = set1_dummy;
    dummy_write.dstBinding = 0;
    dummy_write.descriptorCount = 1;
    dummy_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dummy_write.pImageInfo = &dummy_info;
    vkUpdateDescriptorSets(context->device(), 1, &dummy_write, 0, nullptr);
    return true;
}

bool VulkanComputeRenderer::Impl::create_pipelines()
{
    // Specialize ScreenWidth/ScreenHeight/RenderScale/MaxWorkTiles (spec
    // constants in the generated GLSL) to this renderer's geometry. The
    // entries must stay in constant_id order (0..3), ints.
    const uint32_t spec_values[4]
        = { dims.screen_width, dims.screen_height, dims.scale, dims.max_work_tiles };
    VkSpecializationMapEntry spec_entries[4] {};
    for (uint32_t i = 0; i < 4; ++i) {
        spec_entries[i].constantID = i;
        spec_entries[i].offset = i * sizeof(uint32_t);
        spec_entries[i].size = sizeof(uint32_t);
    }
    VkSpecializationInfo spec_info {};
    spec_info.mapEntryCount = 4;
    spec_info.pMapEntries = spec_entries;
    spec_info.dataSize = sizeof(spec_values);
    spec_info.pData = spec_values;

    for (uint32_t i = 0; i < kNumShaders; ++i) {
        VkShaderModuleCreateInfo module_info { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        module_info.codeSize = kShaderBlobs[i].size;
        module_info.pCode = reinterpret_cast<const uint32_t*>(kShaderBlobs[i].data);
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(context->device(), &module_info, nullptr, &module) != VK_SUCCESS) {
            return false;
        }

        VkPipelineShaderStageCreateInfo stage {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";
        stage.pSpecializationInfo = &spec_info;

        VkComputePipelineCreateInfo info { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        info.stage = stage;
        info.layout = pipeline_layout;
        const VkResult result = vkCreateComputePipelines(
            context->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipelines[i]);
        vkDestroyShaderModule(context->device(), module, nullptr);
        if (result != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool VulkanComputeRenderer::Impl::create_frame_resources()
{
    VkCommandPoolCreateInfo pool_info { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = context->queue_family();
    if (vkCreateCommandPool(context->device(), &pool_info, nullptr, &frame_pool) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferAllocateInfo command_info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    command_info.commandPool = frame_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(context->device(), &command_info, &frame_command) != VK_SUCCESS) {
        return false;
    }
    VkFenceCreateInfo fence_info { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    return vkCreateFence(context->device(), &fence_info, nullptr, &frame_fence) == VK_SUCCESS;
}

bool VulkanComputeRenderer::Impl::create_dummy_texture()
{
    dummy_texture = texture_store->create_texture(1, 1, 1);
    if (dummy_texture == 0) {
        return false;
    }
    const uint32_t pixel = 0xFFFFFFFFu;
    return texture_store->upload_texture(dummy_texture, 1, 1, 0, &pixel);
}

bool VulkanComputeRenderer::Impl::wait_frame()
{
    if (!frame_submitted) {
        return true;
    }
    if (vkWaitForFences(context->device(), 1, &frame_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        fail("frame fence wait");
        return false;
    }
    frame_submitted = false;
    if (texture_store) {
        texture_store->finish_pending_uploads();
    }
    return true;
}

void VulkanComputeRenderer::Impl::fail(const char* what)
{
    failed = true;
    cpu_lines = true;
    if (!logged_failure) {
        std::fprintf(stderr,
            "[native-render] Vulkan DS renderer failed at %s; falling back to the CPU renderer "
            "for the rest of this run\n",
            what);
        logged_failure = true;
    }
    std::memset(color_readback.mapped, 0, static_cast<std::size_t>(dims.readback_size));
    std::memset(attr_readback.mapped, 0, static_cast<std::size_t>(dims.readback_size));
}

void VulkanComputeRenderer::Impl::reset(melonDS::GPU& gpu)
{
    if (!wait_frame() && !failed) {
        return;
    }
    if (texcache) {
        texcache->Reset();
    }
    if (soft) {
        soft->SetRenderWidth(gpu.GPU3D.GetRenderWidth());
        soft->SetThreaded(false, gpu);
        soft->Reset(gpu);
    }
    cpu_lines = failed;
    have_frame = false;
    std::memset(color_readback.mapped, 0, static_cast<std::size_t>(dims.readback_size));
    std::memset(attr_readback.mapped, 0, static_cast<std::size_t>(dims.readback_size));
}

void VulkanComputeRenderer::Impl::dump_upload(
    uint32_t id, uint32_t width, uint32_t height, uint32_t layer, const void* data)
{
    if (dump_dir.empty() || !dump_uploads || !data) {
        return;
    }
    static uint64_t sequence = 0;
    char path[1024];
    std::snprintf(path, sizeof(path), "%s\\tex-%04llu-%u-%u-%ux%u.bin", dump_dir.c_str(),
        static_cast<unsigned long long>(sequence), id, layer, width, height);
    if (FILE* file = std::fopen(path, "wb")) {
        std::fwrite(data, 4, static_cast<std::size_t>(width) * height, file);
        std::fclose(file);
    }
    std::snprintf(path, sizeof(path), "%s\\uploads.txt", dump_dir.c_str());
    if (FILE* file = std::fopen(path, "ab")) {
        std::fprintf(file, "%llu %u %u %ux%u\n", static_cast<unsigned long long>(sequence), id,
            layer, width, height);
        std::fclose(file);
    }
    ++sequence;
}

void VulkanComputeRenderer::Impl::dump_frame_tables(
    uint32_t num_variants, uint32_t num_polygons, bool wbuffer)
{
    if (dump_dir.empty()) {
        return;
    }
    char path[1024];
    std::snprintf(path, sizeof(path), "%s\\frame-%llu-meta.txt", dump_dir.c_str(),
        static_cast<unsigned long long>(frame_counter));
    if (FILE* file = std::fopen(path, "wb")) {
        std::fprintf(file, "wbuffer %d\nnum_variants %u\n", wbuffer ? 1 : 0, num_variants);
        std::fclose(file);
    }
    std::snprintf(path, sizeof(path), "%s\\frame-%llu-variants.txt", dump_dir.c_str(),
        static_cast<unsigned long long>(frame_counter));
    if (FILE* file = std::fopen(path, "wb")) {
        for (uint32_t i = 0; i < num_variants; ++i) {
            std::fprintf(file, "%u tex=%u layer? sampler=%u blend=%u %ux%u\n", i,
                variants_[i].Texture, variants_[i].Sampler, variants_[i].BlendMode,
                variants_[i].Width, variants_[i].Height);
        }
        std::fclose(file);
    }
    std::snprintf(path, sizeof(path), "%s\\frame-%llu-polys.txt", dump_dir.c_str(),
        static_cast<unsigned long long>(frame_counter));
    if (FILE* file = std::fopen(path, "wb")) {
        for (uint32_t i = 0; i < num_polygons; ++i) {
            const RenderPolygon& p = render_polygons[i];
            std::fprintf(file, "%u attr=%08X variant=%u layer=%.0f y=%d..%d x=%d..%d spans=%u\n", i,
                p.Attr, p.Variant, p.TextureLayer, p.YTop, p.YBot, p.XMin, p.XMax, p.FirstXSpan);
        }
        std::fclose(file);
    }
}

void VulkanComputeRenderer::Impl::verify_frame(melonDS::GPU& gpu)
{
    if (!wait_frame()) {
        return;
    }
    soft->RenderFrame(gpu);
    ++verify_frames;
    if (gpu.GPU3D.RenderNumPolygons > 0) {
        ++verify_polygon_frames;
    }
    if (dims.scale > 1) {
        // The readback is the faithful block-boundary plane; at scale > 1 the
        // hi-res raster should reproduce it exactly. Any diff lands in the
        // same summary counters, logged once with the scale so it is not
        // confused with a scale-1 regression.
        if (!scale_logged) {
            std::fprintf(stderr, "[ds3d-verify] comparing at %ux scale (hires-coordinate raster)\n",
                static_cast<unsigned>(dims.scale));
            scale_logged = true;
        }
    }

    uint32_t max_diff = 0;
    uint64_t attr_bad = 0;
    int detail_line = -1;
    int detail_x = -1;
    uint32_t detail_vk = 0;
    uint32_t detail_soft = 0;
    uint32_t detail_vk_attr = 0;
    uint32_t detail_soft_attr = 0;

    for (int y = 0; y < static_cast<int>(kHeight); ++y) {
        const uint32_t* vk_color = static_cast<const uint32_t*>(color_readback.mapped)
            + static_cast<std::size_t>(y) * dims.render_width;
        const uint32_t* vk_attr = static_cast<const uint32_t*>(attr_readback.mapped)
            + static_cast<std::size_t>(y) * dims.render_width;
        const uint32_t* soft_color = soft->GetLine(y);
        const uint32_t* soft_attr = soft->GetAttrLine(y);
        if (!soft_color || !soft_attr) {
            continue;
        }
        for (int x = 0; x < static_cast<int>(dims.render_width); ++x) {
            const uint32_t vk = vk_color[x] & 0x1F3F3F3Fu;
            const uint32_t soft = soft_color[x] & 0x1F3F3F3Fu;
            const uint32_t dr = static_cast<uint32_t>(std::abs(
                static_cast<int>((vk >> 16) & 0x3Fu) - static_cast<int>((soft >> 16) & 0x3Fu)));
            const uint32_t dg = static_cast<uint32_t>(std::abs(
                static_cast<int>((vk >> 8) & 0x3Fu) - static_cast<int>((soft >> 8) & 0x3Fu)));
            const uint32_t db = static_cast<uint32_t>(
                std::abs(static_cast<int>(vk & 0x3Fu) - static_cast<int>(soft & 0x3Fu)));
            const uint32_t da = static_cast<uint32_t>(std::abs(
                static_cast<int>((vk >> 24) & 0x1Fu) - static_cast<int>((soft >> 24) & 0x1Fu)));
            const uint32_t diff = std::max(std::max(dr, dg), std::max(db, da));
            if (diff > max_diff) {
                max_diff = diff;
            }
            if (vk_attr[x] != soft_attr[x]) {
                ++attr_bad;
            }
            if (detail_line < 0 && (diff > 1 || vk_attr[x] != soft_attr[x])) {
                detail_line = y;
                detail_x = x;
                detail_vk = vk;
                detail_soft = soft;
                detail_vk_attr = vk_attr[x];
                detail_soft_attr = soft_attr[x];
            }
        }
    }

    verify_max_channel_diff = std::max(verify_max_channel_diff, max_diff);
    verify_attr_bad_pixels += attr_bad;

    if (const char* dump = std::getenv("PKMNG5PC_DS3D_VERIFY_DUMP")) {
        uint64_t every = 30;
        if (const char* value = std::getenv("PKMNG5PC_DS3D_VERIFY_DUMP_EVERY")) {
            every = std::strtoull(value, nullptr, 10);
        }
        if (verify_frames == 1 || (every != 0 && (verify_frames % every) == 0)) {
            std::vector<uint32_t> soft_color(kWidth * kHeight);
            std::vector<uint32_t> soft_attr(kWidth * kHeight);
            for (int y = 0; y < static_cast<int>(kHeight); ++y) {
                std::memcpy(&soft_color[static_cast<std::size_t>(y) * kWidth], soft->GetLine(y),
                    kWidth * 4);
                std::memcpy(&soft_attr[static_cast<std::size_t>(y) * kWidth], soft->GetAttrLine(y),
                    kWidth * 4);
            }
            char path[1024];
            auto write = [&](const char* suffix, const void* data) {
                std::snprintf(path, sizeof(path), "%s-%llu-%s.bin", dump,
                    static_cast<unsigned long long>(verify_frames), suffix);
                if (FILE* file = std::fopen(path, "wb")) {
                    std::fwrite(data, 1, static_cast<std::size_t>(dims.readback_size), file);
                    std::fclose(file);
                }
            };
            write("vk-color", color_readback.mapped);
            write("vk-attr", attr_readback.mapped);
            write("soft-color", soft_color.data());
            write("soft-attr", soft_attr.data());
        }
    }
    if (max_diff > 1 || attr_bad != 0) {
        ++verify_bad_frames;
        if (!verify_logged_detail) {
            std::fprintf(stderr,
                "[ds3d-verify] mismatch: polys=%u max_channel_diff=%u attr_bad=%llu "
                "line=%d x=%d vk=%08X soft=%08X vk_attr=%08X soft_attr=%08X\n",
                gpu.GPU3D.RenderNumPolygons, max_diff, static_cast<unsigned long long>(attr_bad),
                detail_line, detail_x, detail_vk, detail_soft, detail_vk_attr, detail_soft_attr);
            verify_logged_detail = true;
        }
    }
    if ((verify_frames % 120) == 0) {
        std::fprintf(stderr,
            "[ds3d-verify] frames=%llu skipped=%llu polygon_frames=%llu bad_frames=%llu "
            "max_channel_diff=%u attr_bad_pixels=%llu\n",
            static_cast<unsigned long long>(verify_frames),
            static_cast<unsigned long long>(verify_skipped),
            static_cast<unsigned long long>(verify_polygon_frames),
            static_cast<unsigned long long>(verify_bad_frames), verify_max_channel_diff,
            static_cast<unsigned long long>(verify_attr_bad_pixels));
    }
}

void VulkanComputeRenderer::Impl::build_meta(
    const melonDS::GPU& gpu, const Variant* variants, uint32_t num_variants)
{
    MetaUniform meta {};
    (void)variants;
    meta.DispCnt = gpu.GPU3D.RenderDispCnt;
    meta.NumPolygons = gpu.GPU3D.RenderNumPolygons;
    meta.NumVariants = num_variants;
    meta.AlphaRef = gpu.GPU3D.RenderAlphaRef;

    const uint32_t r = (gpu.GPU3D.RenderClearAttr1 << 1) & 0x3E;
    const uint32_t g = (gpu.GPU3D.RenderClearAttr1 >> 4) & 0x3E;
    const uint32_t b = (gpu.GPU3D.RenderClearAttr1 >> 9) & 0x3E;
    const uint32_t a = (gpu.GPU3D.RenderClearAttr1 >> 16) & 0x1F;
    meta.ClearColor
        = (r ? r + 1 : 0) | ((g ? g + 1 : 0) << 8) | ((b ? b + 1 : 0) << 16) | (a << 24);
    meta.ClearDepth = ((gpu.GPU3D.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;
    meta.ClearAttr = gpu.GPU3D.RenderClearAttr1 & 0x3F008000;

    for (uint32_t i = 0; i < 32; ++i) {
        const uint32_t color = gpu.GPU3D.RenderToonTable[i];
        const uint32_t tr = (color << 1) & 0x3E;
        const uint32_t tg = (color >> 4) & 0x3E;
        const uint32_t tb = (color >> 9) & 0x3E;
        meta.ToonTable[i * 4 + 0]
            = (tr ? tr + 1 : 0) | ((tg ? tg + 1 : 0) << 8) | ((tb ? tb + 1 : 0) << 16);
    }
    for (uint32_t i = 0; i < 34; ++i) {
        meta.ToonTable[i * 4 + 1] = gpu.GPU3D.RenderFogDensityTable[i];
    }
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t color = gpu.GPU3D.RenderEdgeTable[i];
        const uint32_t er = (color << 1) & 0x3E;
        const uint32_t eg = (color >> 4) & 0x3E;
        const uint32_t eb = (color >> 9) & 0x3E;
        meta.ToonTable[i * 4 + 2]
            = (er ? er + 1 : 0) | ((eg ? eg + 1 : 0) << 8) | ((eb ? eb + 1 : 0) << 16);
    }
    meta.FogOffset = gpu.GPU3D.RenderFogOffset;
    meta.FogShift = gpu.GPU3D.RenderFogShift;
    const uint32_t fog_r = (gpu.GPU3D.RenderFogColor << 1) & 0x3E;
    const uint32_t fog_g = (gpu.GPU3D.RenderFogColor >> 4) & 0x3E;
    const uint32_t fog_b = (gpu.GPU3D.RenderFogColor >> 9) & 0x3E;
    const uint32_t fog_a = (gpu.GPU3D.RenderFogColor >> 16) & 0x1F;
    meta.FogColor = (fog_r ? fog_r + 1 : 0) | ((fog_g ? fog_g + 1 : 0) << 8)
        | ((fog_b ? fog_b + 1 : 0) << 16) | (fog_a << 24);

    std::memset(meta_buffer.mapped, 0, static_cast<std::size_t>(meta_buffer.size));
    std::memcpy(meta_buffer.mapped, &meta, sizeof(meta));
}

void VulkanComputeRenderer::Impl::setup_attrs(
    SpanSetupY* span, const melonDS::Polygon* poly, int from, int to)
{
    span->Z0 = poly->FinalZ[from];
    span->W0 = poly->FinalW[from];
    span->Z1 = poly->FinalZ[to];
    span->W1 = poly->FinalW[to];
    span->ColorR0 = poly->Vertices[from]->FinalColor[0];
    span->ColorG0 = poly->Vertices[from]->FinalColor[1];
    span->ColorB0 = poly->Vertices[from]->FinalColor[2];
    span->ColorR1 = poly->Vertices[to]->FinalColor[0];
    span->ColorG1 = poly->Vertices[to]->FinalColor[1];
    span->ColorB1 = poly->Vertices[to]->FinalColor[2];
    span->TexcoordU0 = poly->Vertices[from]->TexCoords[0];
    span->TexcoordV0 = poly->Vertices[from]->TexCoords[1];
    span->TexcoordU1 = poly->Vertices[to]->TexCoords[0];
    span->TexcoordV1 = poly->Vertices[to]->TexCoords[1];
}

void VulkanComputeRenderer::Impl::setup_y_span_dummy(RenderPolygon* rp, SpanSetupY* span,
    const melonDS::Polygon* poly, int vertex, int side, const int32_t positions[10][2])
{
    int32_t x0 = positions[vertex][0];
    if (side) {
        span->DxInitial = -0x40000;
        --x0;
    } else {
        span->DxInitial = 0;
    }

    span->X0 = span->X1 = x0;
    span->XMin = x0;
    span->XMax = x0;
    span->Y0 = span->Y1 = positions[vertex][1];

    if (span->XMin < rp->XMin) {
        rp->XMin = span->XMin;
        rp->XMinY = span->Y0;
    }
    if (span->XMax > rp->XMax) {
        rp->XMax = span->XMax;
        rp->XMaxY = span->Y0;
    }

    span->Increment = 0;
    span->I0 = span->I1 = span->IRecip = 0;
    span->Linear = true;
    span->XCovIncr = 0;
    span->IsDummy = true;
    setup_attrs(span, poly, vertex, vertex);
}

void VulkanComputeRenderer::Impl::setup_y_span(RenderPolygon* rp, SpanSetupY* span,
    const melonDS::Polygon* poly, int from, int to, int side, const int32_t positions[10][2])
{
    span->X0 = positions[from][0];
    span->X1 = positions[to][0];
    span->Y0 = positions[from][1];
    span->Y1 = positions[to][1];

    setup_attrs(span, poly, from, to);

    int32_t min_xy = 0;
    int32_t max_xy = 0;
    bool negative = false;
    if (span->X1 > span->X0) {
        span->XMin = span->X0;
        span->XMax = span->X1 - 1;
        min_xy = span->Y0;
        max_xy = span->Y1;
    } else if (span->X1 < span->X0) {
        span->XMin = span->X1;
        span->XMax = span->X0 - 1;
        negative = true;
        min_xy = span->Y1;
        max_xy = span->Y0;
    } else {
        span->XMin = span->X0;
        if (side) {
            --span->XMin;
        }
        span->XMax = span->XMin;
        min_xy = span->Y0;
        max_xy = span->Y0;
    }

    if (span->XMin < rp->XMin) {
        rp->XMin = span->XMin;
        rp->XMinY = min_xy;
    }
    if (span->XMax > rp->XMax) {
        rp->XMax = span->XMax;
        rp->XMaxY = max_xy;
    }

    span->IsDummy = false;

    const int32_t xlen = span->XMax + 1 - span->XMin;
    const int32_t ylen = span->Y1 - span->Y0;

    if (ylen == 0) {
        span->Increment = 0;
    } else if (ylen == xlen) {
        span->Increment = 0x40000;
    } else {
        const int32_t yrecip = (1 << 18) / ylen;
        span->Increment = (span->X1 - span->X0) * yrecip;
        if (span->Increment < 0) {
            span->Increment = -span->Increment;
        }
    }

    const bool x_major = span->Increment > 0x40000;
    if (side) {
        if (x_major) {
            span->DxInitial = negative ? (0x20000 + 0x40000) : (span->Increment - 0x20000);
        } else if (span->Increment != 0) {
            span->DxInitial = negative ? 0x40000 : 0;
        } else {
            span->DxInitial = -0x40000;
        }
    } else {
        if (x_major) {
            span->DxInitial = negative ? ((span->Increment - 0x20000) + 0x40000) : 0x20000;
        } else if (span->Increment != 0) {
            span->DxInitial = negative ? 0x40000 : 0;
        } else {
            span->DxInitial = 0;
        }
    }

    if (x_major) {
        if (side) {
            span->I0 = span->X0 - 1;
            span->I1 = span->X1 - 1;
        } else {
            span->I0 = span->X0;
            span->I1 = span->X1;
        }
        span->XCovIncr = (ylen << 10) / xlen;
    } else {
        span->I0 = span->Y0;
        span->I1 = span->Y1;
    }

    span->IRecip = span->I0 != span->I1 ? (1 << 30) / (span->I1 - span->I0) : 0;
    span->Linear = (span->W0 == span->W1) && !(span->W0 & 0x7E) && !(span->W1 & 0x7E);

    if ((span->W0 & 0x1) && !(span->W1 & 0x1)) {
        span->W0n = (span->W0 - 1) >> 1;
        span->W0d = (span->W0 + 1) >> 1;
        span->W1d = span->W1 >> 1;
    } else {
        span->W0n = span->W0 >> 1;
        span->W0d = span->W0 >> 1;
        span->W1d = span->W1 >> 1;
    }
}

bool VulkanComputeRenderer::Impl::build_spans(
    melonDS::GPU& gpu, uint32_t& num_variants, bool& wbuffer)
{
    uint32_t num_y_spans = 0;
    uint32_t num_setup_indices = 0;
    uint32_t prev_variant = 0;
    uint32_t prev_tex_layer = 0;

    num_variants = 0;
    wbuffer = false;
    const bool enable_texture_maps = (gpu.GPU3D.RenderDispCnt & 1) != 0;
    if (gpu.GPU3D.RenderNumPolygons > kMaxPolygons) {
        fail("polygon overflow");
        return false;
    }
    if (gpu.GPU3D.RenderNumPolygons > 0) {
        wbuffer = gpu.GPU3D.RenderPolygonRAM[0]->WBuffer;
    }

    for (uint32_t i = 0; i < gpu.GPU3D.RenderNumPolygons; ++i) {
        const melonDS::Polygon* polygon = gpu.GPU3D.RenderPolygonRAM[i];
        if (!polygon) {
            fail("null polygon");
            return false;
        }
        const uint32_t nverts = polygon->NumVertices;
        uint32_t cur_vl = polygon->VTop;
        uint32_t cur_vr = polygon->VTop;
        uint32_t next_vl = 0;
        uint32_t next_vr = 0;

        render_polygons[i].FirstXSpan = num_setup_indices;
        render_polygons[i].Attr = polygon->Attr;

        bool found_variant = false;
        if (i > 0) {
            const melonDS::Polygon* previous = gpu.GPU3D.RenderPolygonRAM[i - 1];
            found_variant = previous->TexParam == polygon->TexParam
                && previous->TexPalette == polygon->TexPalette
                && (previous->Attr & 0x30) == (polygon->Attr & 0x30)
                && previous->IsShadowMask == polygon->IsShadowMask;
        }

        if (!found_variant) {
            Variant variant;
            variant.BlendMode
                = polygon->IsShadowMask ? 4 : static_cast<uint8_t>((polygon->Attr >> 4) & 0x3);
            variant.Texture = 0;
            variant.Sampler = 0;
            uint32_t* texture_last_variant = nullptr;
            if (enable_texture_maps && ((polygon->TexParam >> 26) & 0x7)) {
                texcache->GetTexture(gpu, polygon->TexParam, polygon->TexPalette, variant.Texture,
                    prev_tex_layer, texture_last_variant);
                Texture* texture = texture_store->get(variant.Texture);
                if (!texture || !texture->has_data) {
                    fail("texture upload");
                    return false;
                }
                const bool wrap_s = (polygon->TexParam >> 16) & 1;
                const bool wrap_t = (polygon->TexParam >> 17) & 1;
                const bool mirror_s = (polygon->TexParam >> 18) & 1;
                const bool mirror_t = (polygon->TexParam >> 19) & 1;
                variant.Sampler = static_cast<uint8_t>(
                    (wrap_s ? (mirror_s ? 2 : 1) : 0) + (wrap_t ? (mirror_t ? 2 : 1) : 0) * 3);

                if (*texture_last_variant < num_variants
                    && variants_[*texture_last_variant] == variant) {
                    found_variant = true;
                    prev_variant = *texture_last_variant;
                }
            }

            if (!found_variant) {
                for (int j = static_cast<int>(num_variants) - 1; j >= 0; --j) {
                    if (variants_[static_cast<std::size_t>(j)] == variant) {
                        found_variant = true;
                        prev_variant = static_cast<uint32_t>(j);
                        break;
                    }
                }
                if (!found_variant) {
                    if (num_variants >= kMaxVariants) {
                        fail("variant overflow");
                        return false;
                    }
                    prev_variant = num_variants;
                    variants_[num_variants] = variant;
                    variants_[num_variants].Width = melonDS::TextureWidth(polygon->TexParam);
                    variants_[num_variants].Height = melonDS::TextureHeight(polygon->TexParam);
                    ++num_variants;
                }
                if (texture_last_variant) {
                    *texture_last_variant = prev_variant;
                }
            }
        }
        render_polygons[i].Variant = prev_variant;
        render_polygons[i].TextureLayer = static_cast<float>(prev_tex_layer);

        if (polygon->FacingView) {
            next_vl = cur_vl + 1;
            if (next_vl >= nverts) {
                next_vl = 0;
            }
            next_vr = cur_vr - 1;
            if (static_cast<int32_t>(next_vr) < 0) {
                next_vr = nverts - 1;
            }
        } else {
            next_vl = cur_vl - 1;
            if (static_cast<int32_t>(next_vl) < 0) {
                next_vl = nverts - 1;
            }
            next_vr = cur_vr + 1;
            if (next_vr >= nverts) {
                next_vr = 0;
            }
        }

        int32_t scaled_positions[10][2] {};
        int32_t ytop = static_cast<int32_t>(dims.screen_height);
        int32_t ybot = 0;
        for (uint32_t v = 0; v < polygon->NumVertices; ++v) {
            if (dims.scale > 1) {
                // Upstream's high-resolution path (GPU3D_Compute.cpp: the
                // 4-bit fractional positions keep the scaled interpolation
                // exact; the GE device model computes them for every vertex).
                scaled_positions[v][0]
                    = (polygon->Vertices[v]->HiresPosition[0] * static_cast<int32_t>(dims.scale))
                    >> 4;
                scaled_positions[v][1]
                    = (polygon->Vertices[v]->HiresPosition[1] * static_cast<int32_t>(dims.scale))
                    >> 4;
            } else {
                scaled_positions[v][0] = polygon->Vertices[v]->FinalPosition[0];
                scaled_positions[v][1] = polygon->Vertices[v]->FinalPosition[1];
            }
            ytop = std::min(scaled_positions[v][1], ytop);
            ybot = std::max(scaled_positions[v][1], ybot);
        }
        render_polygons[i].YTop = ytop;
        render_polygons[i].YBot = ybot;
        render_polygons[i].XMin = static_cast<int32_t>(dims.screen_width);
        render_polygons[i].XMax = 0;

        if (ybot == ytop) {
            cur_vl = 0;
            cur_vr = 0;
            ++render_polygons[i].YBot;

            int j = 1;
            if (scaled_positions[j][0] < scaled_positions[cur_vl][0]) {
                cur_vl = j;
            }
            if (scaled_positions[j][0] > scaled_positions[cur_vr][0]) {
                cur_vr = j;
            }
            j = static_cast<int>(nverts) - 1;
            if (scaled_positions[j][0] < scaled_positions[cur_vl][0]) {
                cur_vl = j;
            }
            if (scaled_positions[j][0] > scaled_positions[cur_vr][0]) {
                cur_vr = j;
            }

            if (num_y_spans + 2 > dims.max_y_span_setups
                || num_setup_indices >= dims.max_y_span_indices) {
                fail("y-span capacity");
                return false;
            }
            const uint32_t span_l = num_y_spans;
            setup_y_span_dummy(
                &render_polygons[i], &yspans[num_y_spans++], polygon, cur_vl, 0, scaled_positions);
            const uint32_t span_r = num_y_spans;
            setup_y_span_dummy(
                &render_polygons[i], &yspans[num_y_spans++], polygon, cur_vr, 1, scaled_positions);

            setup_indices[num_setup_indices++] = { i, span_l, span_r, static_cast<uint32_t>(ytop) };
        } else {
            if (num_y_spans + 2 > dims.max_y_span_setups
                || num_setup_indices >= dims.max_y_span_indices) {
                fail("y-span capacity");
                return false;
            }
            uint32_t span_l = num_y_spans;
            setup_y_span(&render_polygons[i], &yspans[num_y_spans++], polygon, cur_vl, next_vl, 0,
                scaled_positions);
            uint32_t span_r = num_y_spans;
            setup_y_span(&render_polygons[i], &yspans[num_y_spans++], polygon, cur_vr, next_vr, 1,
                scaled_positions);

            for (int32_t y = ytop; y < ybot; ++y) {
                if (y >= scaled_positions[next_vl][1] && cur_vl != polygon->VBottom) {
                    while (y >= scaled_positions[next_vl][1] && cur_vl != polygon->VBottom) {
                        cur_vl = next_vl;
                        if (polygon->FacingView) {
                            next_vl = cur_vl + 1;
                            if (next_vl >= nverts) {
                                next_vl = 0;
                            }
                        } else {
                            next_vl = cur_vl - 1;
                            if (static_cast<int32_t>(next_vl) < 0) {
                                next_vl = nverts - 1;
                            }
                        }
                    }
                    if (num_y_spans >= dims.max_y_span_setups) {
                        fail("y-span capacity");
                        return false;
                    }
                    span_l = num_y_spans;
                    setup_y_span(&render_polygons[i], &yspans[num_y_spans++], polygon, cur_vl,
                        next_vl, 0, scaled_positions);
                }
                if (y >= scaled_positions[next_vr][1] && cur_vr != polygon->VBottom) {
                    while (y >= scaled_positions[next_vr][1] && cur_vr != polygon->VBottom) {
                        cur_vr = next_vr;
                        if (polygon->FacingView) {
                            next_vr = cur_vr - 1;
                            if (static_cast<int32_t>(next_vr) < 0) {
                                next_vr = nverts - 1;
                            }
                        } else {
                            next_vr = cur_vr + 1;
                            if (next_vr >= nverts) {
                                next_vr = 0;
                            }
                        }
                    }
                    if (num_y_spans >= dims.max_y_span_setups) {
                        fail("y-span capacity");
                        return false;
                    }
                    span_r = num_y_spans;
                    setup_y_span(&render_polygons[i], &yspans[num_y_spans++], polygon, cur_vr,
                        next_vr, 1, scaled_positions);
                }
                if (num_setup_indices >= dims.max_y_span_indices) {
                    fail("setup-index capacity");
                    return false;
                }
                setup_indices[num_setup_indices++]
                    = { i, span_l, span_r, static_cast<uint32_t>(y) };
            }
        }
    }

    yspan_count = num_y_spans;
    setup_index_count = num_setup_indices;
    return true;
}

bool VulkanComputeRenderer::Impl::build_variant_sets(
    const Variant* variants, uint32_t num_variants, std::vector<VkDescriptorSet>& out_sets)
{
    out_sets.clear();
    if (num_variants == 0) {
        return true;
    }
    if (vkResetDescriptorPool(context->device(), texture_pool, 0) != VK_SUCCESS) {
        fail("descriptor pool reset");
        return false;
    }

    out_sets.resize(num_variants, VK_NULL_HANDLE);
    std::vector<VkDescriptorSetLayout> layouts(num_variants, set1_layout);
    VkDescriptorSetAllocateInfo allocate { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocate.descriptorPool = texture_pool;
    allocate.descriptorSetCount = num_variants;
    allocate.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(context->device(), &allocate, out_sets.data()) != VK_SUCCESS) {
        fail("descriptor set allocation");
        return false;
    }

    std::vector<VkDescriptorImageInfo> image_infos(num_variants);
    std::vector<VkWriteDescriptorSet> writes(num_variants);
    for (uint32_t i = 0; i < num_variants; ++i) {
        Texture* texture = variants[i].Texture != 0 ? texture_store->get(variants[i].Texture)
                                                    : texture_store->get(dummy_texture);
        if (!texture || !texture->has_data) {
            fail("texture descriptor");
            return false;
        }
        const uint32_t sampler_index = variants[i].Sampler < 9 ? variants[i].Sampler : 0;
        image_infos[i].sampler = samplers[sampler_index];
        image_infos[i].imageView = texture->image.view;
        image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = out_sets[i];
        writes[i].dstBinding = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &image_infos[i];
    }
    vkUpdateDescriptorSets(context->device(), num_variants, writes.data(), 0, nullptr);
    return true;
}

bool VulkanComputeRenderer::Impl::record_frame(uint32_t num_variants, const Variant* variants,
    const std::vector<VkDescriptorSet>& variant_sets, bool wbuffer, uint32_t num_polygons)
{
    if (vkResetCommandBuffer(frame_command, 0) != VK_SUCCESS) {
        fail("command buffer reset");
        return false;
    }
    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(frame_command, &begin) != VK_SUCCESS) {
        fail("command buffer begin");
        return false;
    }

    texture_store->record_pending_uploads(frame_command);

    VkDescriptorSet common_sets[1] = { set0 };
    vkCmdBindDescriptorSets(frame_command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
        common_sets, 0, nullptr);
    vkCmdBindDescriptorSets(
        frame_command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 2, 1, &set2, 0, nullptr);
    vkCmdBindDescriptorSets(frame_command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 1, 1,
        &set1_dummy, 0, nullptr);

    vkCmdBindPipeline(
        frame_command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[kPipelineClearCoarse]);
    vkCmdDispatch(frame_command, (dims.tiles_per_line * dims.tile_lines + 31) / 32, 1, 1);
    memory_barrier(frame_command);

    const bool wbuffer_frame = wbuffer;
    const bool highlight = (gpu_dispcnt & (1u << 1)) != 0;

    if (setup_index_count > 0) {
        vkCmdBindPipeline(
            frame_command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[kPipelineClearWorkCount]);
        vkCmdDispatch(frame_command, (num_variants + 31) / 32, 1, 1);
        vkCmdBindPipeline(frame_command, VK_PIPELINE_BIND_POINT_COMPUTE,
            pipelines[wbuffer_frame ? kPipelineInterpSpansW : kPipelineInterpSpansZ]);
        vkCmdDispatch(frame_command, (setup_index_count + 31) / 32, 1, 1);
        memory_barrier(frame_command);

        const uint32_t bin_groups = (num_polygons + 31) / 32;
        if (bin_groups > 0) {
            vkCmdBindPipeline(
                frame_command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[kPipelineBinCombined]);
            vkCmdDispatch(frame_command, bin_groups, dims.screen_width / kCoarseTileW,
                dims.screen_height / kCoarseTileH);
            memory_barrier(frame_command);
        }

        vkCmdBindPipeline(
            frame_command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[kPipelineCalcOffsets]);
        vkCmdDispatch(frame_command, (num_variants + 31) / 32, 1, 1);
        memory_barrier(frame_command);

        vkCmdBindPipeline(
            frame_command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[kPipelineSortWork]);
        vkCmdDispatchIndirect(frame_command, bin_buffer.buffer, 5120);
        memory_barrier(frame_command);

        for (uint32_t i = 0; i < num_variants; ++i) {
            const ShaderId shader = raster_shader(
                variants[i].Texture != 0, variants[i].BlendMode, wbuffer_frame, highlight);
            if (shader == ShaderId::Count) {
                fail("raster pipeline selection");
                vkEndCommandBuffer(frame_command);
                return false;
            }
            const uint32_t pipeline = shader_index(shader);

            vkCmdBindPipeline(frame_command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pipeline]);
            vkCmdBindDescriptorSets(frame_command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                1, 1, &variant_sets[i], 0, nullptr);
            PushConstants push { i, 0, 1.0f, 1.0f };
            if (variants[i].Texture != 0) {
                push.InvTextureWidth = variants[i].Width != 0 ? 1.0f / variants[i].Width : 1.0f;
                push.InvTextureHeight = variants[i].Height != 0 ? 1.0f / variants[i].Height : 1.0f;
            }
            vkCmdPushConstants(frame_command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(push), &push);
            vkCmdDispatchIndirect(frame_command, bin_buffer.buffer, i * 16);
        }
        memory_barrier(frame_command);
    }

    vkCmdBindPipeline(frame_command, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelines[wbuffer_frame ? kPipelineDepthBlendW : kPipelineDepthBlendZ]);
    vkCmdDispatch(frame_command, dims.tiles_per_line, dims.tile_lines, 1);
    memory_barrier(frame_command);

    uint32_t final_flags = 0;
    if ((gpu_dispcnt & (1u << 4)) != 0) {
        final_flags |= 4;
    }
    if ((gpu_dispcnt & (1u << 7)) != 0) {
        final_flags |= 2;
    }
    if ((gpu_dispcnt & (1u << 5)) != 0) {
        final_flags |= 1;
    }
    vkCmdBindPipeline(frame_command, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelines[shader_index(final_pass_shader(final_flags))]);
    vkCmdDispatch(frame_command, dims.screen_width / 32, dims.screen_height, 1);

    VkMemoryBarrier image_done { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    image_done.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    image_done.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(frame_command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &image_done, 0, nullptr, 0, nullptr);

    image_barrier(frame_command, lowres_fb.image, VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
    image_barrier(frame_command, attr_fb.image, VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 1);

    // LowResFB and AttrFB are the faithful 1x planes at every scale: FinalPass
    // stores them at block boundaries only, so the readback is point-sampled
    // and bit-identical to scale 1. The visible hi-res path is the GPU
    // composite (ADR 0008, slice V2) - averaging here would blur the 2D stack
    // composited over the 3D layer.
    VkBufferImageCopy color_copy {};
    color_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    color_copy.imageSubresource.layerCount = 1;
    color_copy.imageExtent = { dims.render_width, kHeight, 1 };
    vkCmdCopyImageToBuffer(frame_command, lowres_fb.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        color_readback.buffer, 1, &color_copy);

    VkBufferImageCopy attr_copy = color_copy;
    vkCmdCopyImageToBuffer(frame_command, attr_fb.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        attr_readback.buffer, 1, &attr_copy);

    if (vkEndCommandBuffer(frame_command) != VK_SUCCESS) {
        fail("command buffer end");
        return false;
    }

    VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame_command;
    if (vkResetFences(context->device(), 1, &frame_fence) != VK_SUCCESS) {
        fail("frame fence reset");
        return false;
    }
    if (vkQueueSubmit(context->queue(), 1, &submit, frame_fence) != VK_SUCCESS) {
        fail("frame queue submit");
        return false;
    }
    frame_submitted = true;
    return true;
}

void VulkanComputeRenderer::Impl::render_cpu(melonDS::GPU& gpu)
{
    cpu_lines = true;
    if (soft) {
        soft->RenderFrame(gpu);
    }
}

void VulkanComputeRenderer::Impl::render_frame(melonDS::GPU& gpu)
{
    cpu_lines = false;
    ++frame_counter;

    SinkFrameState state;
    state.initialized = initialized;
    state.failed = failed;
    state.capture = nds_gpu2d_capture_active();
    if (sink_frame_needs_cpu(state)) {
        render_cpu(gpu);
        return;
    }

    if (!wait_frame()) {
        render_cpu(gpu);
        return;
    }

    const bool texture_changed = texcache->Update(gpu);
    if (have_frame && !texture_changed && gpu.GPU3D.RenderFrameIdentical) {
        if (verify) {
            render_cpu(gpu);
            ++verify_skipped;
        }
        return;
    }
    if (gpu.GPU3D.AbortFrame) {
        return;
    }

    uint32_t num_variants = 0;
    bool wbuffer = false;
    if (!build_spans(gpu, num_variants, wbuffer)) {
        render_cpu(gpu);
        return;
    }
    if (!dump_dir.empty()
        && (dump_every == 0 || (frame_counter % dump_every) == 0 || frame_counter == dump_frame)) {
        dump_frame_tables(num_variants, gpu.GPU3D.RenderNumPolygons, wbuffer);
    }
    build_meta(gpu, variants_.data(), num_variants);

    if (yspan_count > 0) {
        std::memcpy(yspan_buffer.mapped, yspans.data(),
            static_cast<std::size_t>(yspan_count) * sizeof(SpanSetupY));
    }
    if (setup_index_count > 0) {
        std::memcpy(setup_index_buffer.mapped, setup_indices.data(),
            static_cast<std::size_t>(setup_index_count) * sizeof(SetupIndices));
    }
    if (gpu.GPU3D.RenderNumPolygons > 0) {
        std::memcpy(polygon_buffer.mapped, render_polygons.data(),
            static_cast<std::size_t>(gpu.GPU3D.RenderNumPolygons) * sizeof(RenderPolygon));
    }

    std::vector<VkDescriptorSet> variant_sets;
    if (!build_variant_sets(variants_.data(), num_variants, variant_sets)) {
        render_cpu(gpu);
        return;
    }
    gpu_dispcnt = gpu.GPU3D.RenderDispCnt;
    if (!record_frame(
            num_variants, variants_.data(), variant_sets, wbuffer, gpu.GPU3D.RenderNumPolygons)) {
        render_cpu(gpu);
        return;
    }
    have_frame = true;
    if (verify) {
        verify_frame(gpu);
        cpu_lines = true;
    }
}

VulkanComputeRenderer::VulkanComputeRenderer()
    : Renderer3D(false)
    , impl_(std::make_unique<Impl>())
{
}

VulkanComputeRenderer::~VulkanComputeRenderer()
{
    if (impl_) {
        impl_->shutdown();
    }
}

std::unique_ptr<VulkanComputeRenderer> VulkanComputeRenderer::create(
    SDL_Window* window, const Config& config)
{
    auto renderer = std::unique_ptr<VulkanComputeRenderer>(new VulkanComputeRenderer());
    if (!renderer->impl_->initialize(window, config)) {
        renderer->impl_->shutdown();
        return nullptr;
    }
    return renderer;
}

void VulkanComputeRenderer::Reset(melonDS::GPU& gpu) { impl_->reset(gpu); }

void VulkanComputeRenderer::RenderFrame(melonDS::GPU& gpu) { impl_->render_frame(gpu); }

void VulkanComputeRenderer::RestartFrame(melonDS::GPU& gpu)
{
    if (impl_->cpu_lines && impl_->soft) {
        impl_->soft->RestartFrame(gpu);
    }
}

melonDS::u32* VulkanComputeRenderer::GetLine(int line)
{
    if (line < 0 || line >= static_cast<int>(kHeight)) {
        return nullptr;
    }
    if (impl_->cpu_lines && impl_->soft) {
        return impl_->soft->GetLine(line);
    }
    if (!impl_->color_readback.mapped || !impl_->wait_frame()) {
        return nullptr;
    }
    return static_cast<melonDS::u32*>(impl_->color_readback.mapped)
        + static_cast<std::size_t>(line) * impl_->dims.render_width;
}

const melonDS::u32* VulkanComputeRenderer::GetAttrLine(int line)
{
    if (line < 0 || line >= static_cast<int>(kHeight)) {
        return nullptr;
    }
    if (impl_->cpu_lines && impl_->soft) {
        return impl_->soft->GetAttrLine(line);
    }
    if (!impl_->attr_readback.mapped || !impl_->wait_frame()) {
        return nullptr;
    }
    return static_cast<const melonDS::u32*>(impl_->attr_readback.mapped)
        + static_cast<std::size_t>(line) * impl_->dims.render_width;
}

void VulkanComputeRenderer::SetRenderWidth(melonDS::u32 width)
{
    if (impl_->soft) {
        impl_->soft->SetRenderWidth(width);
    }
    // The band width is fixed before init (the frontend calls
    // nds_gpu3d_set_output_width first) and sizes every buffer and pipeline;
    // a different width falls back to the CPU renderer.
    if (width != impl_->dims.render_width) {
        impl_->fail("SetRenderWidth");
    }
}

void* VulkanComputeRenderer::hires_color_view() const
{
    return impl_->initialized && impl_->dims.scale > 1 ? impl_->final_fb.view : nullptr;
}

uint32_t VulkanComputeRenderer::hires_width() const { return impl_->dims.screen_width; }

uint32_t VulkanComputeRenderer::hires_height() const { return impl_->dims.screen_height; }

uint32_t VulkanComputeRenderer::hires_scale() const { return impl_->dims.scale; }

void VulkanComputeRenderer::shutdown()
{
    if (impl_) {
        impl_->shutdown();
    }
}

} // namespace pkmn::render::ds
