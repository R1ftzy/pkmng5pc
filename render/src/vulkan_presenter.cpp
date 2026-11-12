#if defined(PKMN_RENDER_VULKAN)

#include "vulkan_presenter.h"

#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "vulkan_context.h"
#include "vulkan_functions.h"

// The HD composite shader, compiled at build time (render/src/present_hd.comp).
extern const unsigned char pkmn_present_hd_spv[];
extern const std::size_t pkmn_present_hd_spv_size;

namespace pkmn::render {
namespace {

    using namespace pkmn::render::vk;

    constexpr uint32_t kFramesInFlight = 2;

    struct StagingBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
    };

    struct CanvasUpload {
        StagingBuffer staging;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory image_memory = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // One SSBO plane of the HD composite (top/below stack, fallback): staged
    // per frame, read by the composite shader.
    struct HdPlane {
        StagingBuffer staging;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize bytes = 0;
        uint32_t width = 0;
    };

    // Mirrors the push-constant block in render/src/present_hd.comp
    // (std430-style packing; keep the order in sync).
    struct HdPush {
        uint32_t canvas_width;
        uint32_t canvas_height;
        uint32_t hd_width;
        uint32_t fallback_width;
        uint32_t fallback_offset;
        uint32_t content_row0;
        uint32_t content_rows;
        uint32_t scale3d;
        uint32_t render_xpos;
        uint32_t priority3d;
        uint32_t order3d;
        uint32_t bldcnt;
        uint32_t eva;
        uint32_t evb;
        uint32_t evy;
        uint32_t master_bright;
        float zoom;
        float pan_x;
        float pan_y;
    };

    // One device, one output surface, one upload path. See backend.h for the
    // contract; the comments here call out only the parts that are easy to get
    // wrong (layout transitions, host-write visibility, out-of-date recovery).
    class VulkanPresenter final : public Presenter {
    public:
        ~VulkanPresenter() override { shutdown(); }

        bool init(const Window& window, const Config& config) override
        {
            window_ = static_cast<SDL_Window*>(window.sdl_window);
            if (!window_) {
                return false;
            }
            if (!SDL_WasInit(SDL_INIT_VIDEO)) {
                if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
                    std::fprintf(stderr, "[render] SDL video init failed: %s\n", SDL_GetError());
                    return false;
                }
            }
            vsync_ = config.vsync;
            linear_filter_ = false;
            if (const char* filter = std::getenv("PKMNG5PC_PRESENT_FILTER")) {
                linear_filter_ = std::strcmp(filter, "linear") == 0;
            }
            context_ = std::make_shared<VulkanContext>();
            if (!context_->init(window_, config)) {
                context_.reset();
                return false;
            }
            publish_shared_context(context_.get());
            if (!create_command_resources() || !create_sync()) {
                return false;
            }
            int w = 0;
            int h = 0;
            SDL_GetWindowSizeInPixels(window_, &w, &h);
            if (!create_swapchain(
                    static_cast<uint32_t>(std::max(w, 1)), static_cast<uint32_t>(std::max(h, 1)))) {
                return false;
            }
            return true;
        }

        void shutdown() override
        {
            if (context_) {
                context_->wait_idle();
                withdraw_shared_context(context_.get());
            }
            destroy_swapchain();
            destroy_uploads();
            destroy_hd();
            const VkDevice dev = device();
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                if (image_available_[i]) {
                    if (dev) {
                        vkDestroySemaphore(dev, image_available_[i], nullptr);
                    }
                    image_available_[i] = VK_NULL_HANDLE;
                }
                if (render_finished_[i]) {
                    if (dev) {
                        vkDestroySemaphore(dev, render_finished_[i], nullptr);
                    }
                    render_finished_[i] = VK_NULL_HANDLE;
                }
                if (in_flight_[i]) {
                    if (dev) {
                        vkDestroyFence(dev, in_flight_[i], nullptr);
                    }
                    in_flight_[i] = VK_NULL_HANDLE;
                }
            }
            if (command_pool_ && dev) {
                vkDestroyCommandPool(dev, command_pool_, nullptr);
            }
            command_pool_ = VK_NULL_HANDLE;
            command_buffers_.clear();
            frame_ = 0;
            context_.reset();
        }

        bool resize(uint32_t width, uint32_t height) override
        {
            if (!device() || width == 0 || height == 0) {
                return false;
            }
            vkDeviceWaitIdle(device());
            destroy_swapchain();
            return create_swapchain(width, height);
        }

        uint32_t width() const override { return extent_.width; }
        uint32_t height() const override { return extent_.height; }
        BackendKind kind() const override { return BackendKind::Vulkan; }

        bool present(const Canvas& canvas, const PresentTarget& target) override
        {
            if (!device() || swapchain_ == VK_NULL_HANDLE) {
                return false;
            }
            if (!canvas.data || canvas.width == 0 || canvas.height == 0 || target.empty()
                || canvas.pitch < static_cast<std::size_t>(canvas.width) * 4) {
                return false;
            }
            if (static_cast<uint64_t>(target.x) + target.width > extent_.width
                || static_cast<uint64_t>(target.y) + target.height > extent_.height) {
                return false;
            }

            const uint32_t frame = frame_;
            vkWaitForFences(device(), 1, &in_flight_[frame], VK_TRUE, UINT64_MAX);

            uint32_t image_index = 0;
            const VkResult acquired = vkAcquireNextImageKHR(device(), swapchain_, UINT64_MAX,
                image_available_[frame], VK_NULL_HANDLE, &image_index);
            if (acquired == VK_ERROR_OUT_OF_DATE_KHR || acquired == VK_SUBOPTIMAL_KHR) {
                return false;
            }
            if (acquired != VK_SUCCESS) {
                std::fprintf(stderr, "[render] vkAcquireNextImageKHR failed (%d)\n", acquired);
                return false;
            }

            vkResetFences(device(), 1, &in_flight_[frame]);

            if (!ensure_upload(frame, canvas.width, canvas.height)) {
                return false;
            }
            CanvasUpload& upload = uploads_[frame];

            // Upload the native-resolution canvas into this frame's staging
            // buffer. Row pitch may exceed width*4; the copy below reads it
            // tightly packed.
            const std::size_t row_bytes = static_cast<std::size_t>(canvas.width) * 4;
            auto* dst = static_cast<uint8_t*>(upload.staging.mapped);
            for (uint32_t y = 0; y < canvas.height; ++y) {
                std::memcpy(dst + static_cast<std::size_t>(y) * row_bytes,
                    canvas.data + static_cast<std::size_t>(y) * canvas.pitch, row_bytes);
            }

            VkCommandBuffer cmd = command_buffers_[frame];
            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo begin {};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
                return false;
            }

            // Host write -> transfer read.
            VkMemoryBarrier host_barrier {};
            host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            host_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            host_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                1, &host_barrier, 0, nullptr, 0, nullptr);

            // Both images are overwritten completely, so their previous
            // contents can be discarded. The swapchain image is also cleared
            // outside the destination rectangle below.
            VkImageMemoryBarrier to_transfer[2] {};
            for (VkImageMemoryBarrier& barrier : to_transfer) {
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                barrier.srcAccessMask = 0;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            }
            to_transfer[0].image = images_[image_index];
            to_transfer[1].image = upload.image;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, to_transfer);

            const uint8_t letterbox_b = static_cast<uint8_t>(kLetterboxBgra & 0xFFu);
            const uint8_t letterbox_g = static_cast<uint8_t>((kLetterboxBgra >> 8) & 0xFFu);
            const uint8_t letterbox_r = static_cast<uint8_t>((kLetterboxBgra >> 16) & 0xFFu);
            VkClearColorValue clear {};
            clear.float32[0] = static_cast<float>(letterbox_r) / 255.0f;
            clear.float32[1] = static_cast<float>(letterbox_g) / 255.0f;
            clear.float32[2] = static_cast<float>(letterbox_b) / 255.0f;
            clear.float32[3] = 1.0f;
            const VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(
                cmd, images_[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

            VkBufferImageCopy region {};
            region.bufferOffset = 0;
            region.bufferRowLength = 0; // tightly packed rows in the staging buffer
            region.bufferImageHeight = 0;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageOffset = { 0, 0, 0 };
            region.imageExtent = { canvas.width, canvas.height, 1 };
            vkCmdCopyBufferToImage(cmd, upload.staging.buffer, upload.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            VkImageMemoryBarrier to_source = to_transfer[1];
            to_source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_source.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_source);

            // Scale the native canvas into its placed rectangle. The target is
            // an integer multiple of the canvas, so nearest filtering keeps the
            // DS pixels crisp while the GPU, not the CPU, does the enlargement.
            VkImageBlit blit {};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[0] = { 0, 0, 0 };
            blit.srcOffsets[1]
                = { static_cast<int32_t>(canvas.width), static_cast<int32_t>(canvas.height), 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.dstOffsets[0]
                = { static_cast<int32_t>(target.x), static_cast<int32_t>(target.y), 0 };
            blit.dstOffsets[1] = { static_cast<int32_t>(target.x + target.width),
                static_cast<int32_t>(target.y + target.height), 1 };
            vkCmdBlitImage(cmd, upload.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                images_[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                linear_filter_ ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

            VkImageMemoryBarrier to_present = to_transfer[0];
            to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_present.dstAccessMask = 0;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_present);

            if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
                return false;
            }

            VkSubmitInfo submit {};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &image_available_[frame];
            submit.pWaitDstStageMask = &wait_stage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &render_finished_[frame];
            if (vkQueueSubmit(queue(), 1, &submit, in_flight_[frame]) != VK_SUCCESS) {
                return false;
            }

            VkPresentInfoKHR present_info {};
            present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present_info.waitSemaphoreCount = 1;
            present_info.pWaitSemaphores = &render_finished_[frame];
            present_info.swapchainCount = 1;
            present_info.pSwapchains = &swapchain_;
            present_info.pImageIndices = &image_index;
            const VkResult presented = vkQueuePresentKHR(queue(), &present_info);
            if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
                return false;
            }
            if (presented != VK_SUCCESS) {
                std::fprintf(stderr, "[render] vkQueuePresentKHR failed (%d)\n", presented);
                return false;
            }

            frame_ = (frame_ + 1) % kFramesInFlight;
            return true;
        }

    private:
        bool create_command_resources()
        {
            VkCommandPoolCreateInfo pool {};
            pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = queue_family();
            if (vkCreateCommandPool(device(), &pool, nullptr, &command_pool_) != VK_SUCCESS) {
                return false;
            }
            command_buffers_.resize(kFramesInFlight, VK_NULL_HANDLE);
            VkCommandBufferAllocateInfo alloc {};
            alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            alloc.commandPool = command_pool_;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = kFramesInFlight;
            return vkAllocateCommandBuffers(device(), &alloc, command_buffers_.data())
                == VK_SUCCESS;
        }

        bool create_sync()
        {
            VkSemaphoreCreateInfo sem {};
            sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkFenceCreateInfo fence {};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                if (vkCreateSemaphore(device(), &sem, nullptr, &image_available_[i]) != VK_SUCCESS
                    || vkCreateSemaphore(device(), &sem, nullptr, &render_finished_[i])
                        != VK_SUCCESS
                    || vkCreateFence(device(), &fence, nullptr, &in_flight_[i]) != VK_SUCCESS) {
                    return false;
                }
            }
            return true;
        }

        VkFormat choose_format() const
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device(), surface(), &count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(
                physical_device(), surface(), &count, formats.data());
            // UNORM keeps the canvas byte-exact; an _SRGB swapchain would apply a
            // second encode the port did not ask for.
            for (const VkSurfaceFormatKHR& f : formats) {
                if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
                    return f.format;
                }
            }
            for (const VkSurfaceFormatKHR& f : formats) {
                if (f.format == VK_FORMAT_B8G8R8A8_SRGB) {
                    return f.format;
                }
            }
            return formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
        }

        VkPresentModeKHR choose_present_mode(bool vsync) const
        {
            if (vsync) {
                return VK_PRESENT_MODE_FIFO_KHR;
            }
            uint32_t count = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                physical_device(), surface(), &count, nullptr);
            std::vector<VkPresentModeKHR> modes(count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                physical_device(), surface(), &count, modes.data());
            for (VkPresentModeKHR mode : modes) {
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                    return mode;
                }
            }
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        bool create_swapchain(uint32_t desired_w, uint32_t desired_h)
        {
            VkSurfaceCapabilitiesKHR caps {};
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device(), surface(), &caps)
                != VK_SUCCESS) {
                return false;
            }
            VkExtent2D extent = caps.currentExtent;
            if (extent.width == UINT32_MAX) {
                extent.width
                    = std::clamp(desired_w, caps.minImageExtent.width, caps.maxImageExtent.width);
                extent.height
                    = std::clamp(desired_h, caps.minImageExtent.height, caps.maxImageExtent.height);
            }

            format_ = choose_format();
            const VkPresentModeKHR present_mode = choose_present_mode(vsync_);

            uint32_t image_count = caps.minImageCount + 1;
            if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
                image_count = caps.maxImageCount;
            }

            VkSwapchainCreateInfoKHR info {};
            info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
            info.surface = surface();
            info.minImageCount = image_count;
            info.imageFormat = format_;
            info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            info.imageExtent = extent;
            info.imageArrayLayers = 1;
            info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.preTransform = caps.currentTransform;
            info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            info.presentMode = present_mode;
            info.clipped = VK_TRUE;
            info.oldSwapchain = VK_NULL_HANDLE;

            if (vkCreateSwapchainKHR(device(), &info, nullptr, &swapchain_) != VK_SUCCESS) {
                std::fprintf(stderr, "[render] vkCreateSwapchainKHR failed\n");
                return false;
            }
            uint32_t count = 0;
            vkGetSwapchainImagesKHR(device(), swapchain_, &count, nullptr);
            images_.resize(count);
            vkGetSwapchainImagesKHR(device(), swapchain_, &count, images_.data());
            extent_ = extent;
            return true;
        }

        void destroy_swapchain()
        {
            if (swapchain_) {
                vkDestroySwapchainKHR(device(), swapchain_, nullptr);
                swapchain_ = VK_NULL_HANDLE;
            }
            images_.clear();
            extent_ = { 0, 0 };
        }

        bool create_staging(StagingBuffer& staging, VkDeviceSize size)
        {
            if (size == 0) {
                return false;
            }
            VkBufferCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = size;
            info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device(), &info, nullptr, &staging.buffer) != VK_SUCCESS) {
                return false;
            }
            VkMemoryRequirements req {};
            vkGetBufferMemoryRequirements(device(), staging.buffer, &req);
            const uint32_t type = context_->find_memory_type(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (type == UINT32_MAX) {
                return false;
            }
            VkMemoryAllocateInfo alloc {};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = type;
            if (vkAllocateMemory(device(), &alloc, nullptr, &staging.memory) != VK_SUCCESS) {
                return false;
            }
            vkBindBufferMemory(device(), staging.buffer, staging.memory, 0);
            if (vkMapMemory(device(), staging.memory, 0, size, 0, &staging.mapped) != VK_SUCCESS) {
                return false;
            }
            staging.size = size;
            return true;
        }

        void destroy_staging(StagingBuffer& staging)
        {
            if (!device()) {
                return;
            }
            if (staging.mapped) {
                vkUnmapMemory(device(), staging.memory);
                staging.mapped = nullptr;
            }
            if (staging.buffer) {
                vkDestroyBuffer(device(), staging.buffer, nullptr);
                staging.buffer = VK_NULL_HANDLE;
            }
            if (staging.memory) {
                vkFreeMemory(device(), staging.memory, nullptr);
                staging.memory = VK_NULL_HANDLE;
            }
            staging.size = 0;
        }

        bool create_canvas_image(CanvasUpload& upload, uint32_t width, uint32_t height)
        {
            VkImageCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = VK_FORMAT_B8G8R8A8_UNORM;
            info.extent = { width, height, 1 };
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                | VK_IMAGE_USAGE_STORAGE_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(device(), &info, nullptr, &upload.image) != VK_SUCCESS) {
                return false;
            }
            VkImageViewCreateInfo view {};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = upload.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = VK_FORMAT_B8G8R8A8_UNORM;
            view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(device(), &view, nullptr, &upload.view) != VK_SUCCESS) {
                return false;
            }

            VkMemoryRequirements req {};
            vkGetImageMemoryRequirements(device(), upload.image, &req);
            uint32_t type = context_->find_memory_type(
                req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (type == UINT32_MAX) {
                type = context_->find_memory_type(req.memoryTypeBits, 0);
            }
            if (type == UINT32_MAX) {
                return false;
            }

            VkMemoryAllocateInfo alloc {};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = type;
            if (vkAllocateMemory(device(), &alloc, nullptr, &upload.image_memory) != VK_SUCCESS) {
                return false;
            }
            if (vkBindImageMemory(device(), upload.image, upload.image_memory, 0) != VK_SUCCESS) {
                return false;
            }
            upload.width = width;
            upload.height = height;
            return true;
        }

        bool ensure_upload(uint32_t frame, uint32_t width, uint32_t height)
        {
            CanvasUpload& upload = uploads_[frame];
            if (upload.width == width && upload.height == height && upload.staging.mapped) {
                return true;
            }
            destroy_upload(upload);
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;
            if (!create_staging(upload.staging, bytes)) {
                destroy_upload(upload);
                return false;
            }
            if (!create_canvas_image(upload, width, height)) {
                destroy_upload(upload);
                return false;
            }
            return true;
        }

        void destroy_upload(CanvasUpload& upload)
        {
            destroy_staging(upload.staging);
            if (upload.view) {
                vkDestroyImageView(device(), upload.view, nullptr);
                upload.view = VK_NULL_HANDLE;
            }
            if (upload.image) {
                vkDestroyImage(device(), upload.image, nullptr);
                upload.image = VK_NULL_HANDLE;
            }
            if (upload.image_memory) {
                vkFreeMemory(device(), upload.image_memory, nullptr);
                upload.image_memory = VK_NULL_HANDLE;
            }
            upload.width = 0;
            upload.height = 0;
        }

        void destroy_uploads()
        {
            for (CanvasUpload& upload : uploads_) {
                destroy_upload(upload);
            }
        }

        // ---- HD composite (ADR 0008, slice V2) --------------------------------

        bool create_hd_plane(HdPlane& plane, uint32_t width)
        {
            destroy_hd_plane(plane);
            plane.width = width;
            plane.bytes = static_cast<VkDeviceSize>(width) * 192 * 2 * 4;
            VkBufferCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = plane.bytes;
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device(), &info, nullptr, &plane.buffer) != VK_SUCCESS) {
                return false;
            }
            VkMemoryRequirements req {};
            vkGetBufferMemoryRequirements(device(), plane.buffer, &req);
            const uint32_t type = context_->find_memory_type(
                req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (type == UINT32_MAX) {
                return false;
            }
            VkMemoryAllocateInfo alloc {};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = type;
            if (vkAllocateMemory(device(), &alloc, nullptr, &plane.memory) != VK_SUCCESS) {
                return false;
            }
            if (vkBindBufferMemory(device(), plane.buffer, plane.memory, 0) != VK_SUCCESS) {
                return false;
            }
            if (!create_staging(plane.staging, plane.bytes)) {
                return false;
            }
            return true;
        }

        void destroy_hd_plane(HdPlane& plane)
        {
            destroy_staging(plane.staging);
            if (plane.buffer) {
                vkDestroyBuffer(device(), plane.buffer, nullptr);
                plane.buffer = VK_NULL_HANDLE;
            }
            if (plane.memory) {
                vkFreeMemory(device(), plane.memory, nullptr);
                plane.memory = VK_NULL_HANDLE;
            }
            plane.width = 0;
            plane.bytes = 0;
        }

        void destroy_hd()
        {
            if (!device()) {
                return;
            }
            destroy_hd_plane(hd_top_);
            destroy_hd_plane(hd_below_);
            destroy_hd_plane(hd_fallback_);
            if (hd_pool_ != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device(), hd_pool_, nullptr);
                hd_pool_ = VK_NULL_HANDLE;
            }
            hd_set_ = VK_NULL_HANDLE;
            if (hd_desc_layout_ != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device(), hd_desc_layout_, nullptr);
                hd_desc_layout_ = VK_NULL_HANDLE;
            }
            if (hd_pipeline_layout_ != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device(), hd_pipeline_layout_, nullptr);
                hd_pipeline_layout_ = VK_NULL_HANDLE;
            }
            if (hd_pipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device(), hd_pipeline_, nullptr);
                hd_pipeline_ = VK_NULL_HANDLE;
            }
        }

        bool ensure_hd_pipeline()
        {
            if (hd_pipeline_ != VK_NULL_HANDLE) {
                return true;
            }
            VkShaderModuleCreateInfo module_info { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            module_info.codeSize = pkmn_present_hd_spv_size;
            module_info.pCode = reinterpret_cast<const uint32_t*>(pkmn_present_hd_spv);
            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device(), &module_info, nullptr, &module) != VK_SUCCESS) {
                std::fprintf(stderr, "[render] HD composite shader module failed\n");
                return false;
            }

            std::array<VkDescriptorSetLayoutBinding, 5> bindings {};
            for (uint32_t i = 0; i < 2; ++i) {
                bindings[i].binding = i;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            for (uint32_t i = 2; i < bindings.size(); ++i) {
                bindings[i].binding = i;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo layout_info {};
            layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
            layout_info.pBindings = bindings.data();
            if (vkCreateDescriptorSetLayout(device(), &layout_info, nullptr, &hd_desc_layout_)
                != VK_SUCCESS) {
                vkDestroyShaderModule(device(), module, nullptr);
                return false;
            }

            VkPushConstantRange push {};
            push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            push.offset = 0;
            push.size = sizeof(HdPush);
            VkPipelineLayoutCreateInfo pipeline_info {};
            pipeline_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipeline_info.setLayoutCount = 1;
            pipeline_info.pSetLayouts = &hd_desc_layout_;
            pipeline_info.pushConstantRangeCount = 1;
            pipeline_info.pPushConstantRanges = &push;
            if (vkCreatePipelineLayout(device(), &pipeline_info, nullptr, &hd_pipeline_layout_)
                != VK_SUCCESS) {
                vkDestroyShaderModule(device(), module, nullptr);
                return false;
            }

            VkPipelineShaderStageCreateInfo stage {};
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = module;
            stage.pName = "main";
            VkComputePipelineCreateInfo pipeline {};
            pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipeline.stage = stage;
            pipeline.layout = hd_pipeline_layout_;
            const VkResult result = vkCreateComputePipelines(
                device(), VK_NULL_HANDLE, 1, &pipeline, nullptr, &hd_pipeline_);
            vkDestroyShaderModule(device(), module, nullptr);
            if (result != VK_SUCCESS) {
                std::fprintf(stderr, "[render] HD composite pipeline failed\n");
                return false;
            }

            VkDescriptorPoolSize sizes[2] {};
            sizes[0] = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 };
            sizes[1] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 };
            VkDescriptorPoolCreateInfo pool {};
            pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool.maxSets = 1;
            pool.poolSizeCount = 2;
            pool.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(device(), &pool, nullptr, &hd_pool_) != VK_SUCCESS) {
                return false;
            }
            VkDescriptorSetAllocateInfo alloc {};
            alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool = hd_pool_;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &hd_desc_layout_;
            return vkAllocateDescriptorSets(device(), &alloc, &hd_set_) == VK_SUCCESS;
        }

        bool present_hd(
            const HdComposite& hd, const Canvas& fallback, const PresentTarget& target) override
        {
            if (!device() || swapchain_ == VK_NULL_HANDLE || !hd.valid() || !fallback.data
                || target.empty()) {
                return false;
            }
            if (static_cast<uint64_t>(target.x) + target.width > extent_.width
                || static_cast<uint64_t>(target.y) + target.height > extent_.height) {
                return false;
            }
            if (!ensure_hd_pipeline()) {
                return false;
            }

            const uint32_t frame = frame_;
            vkWaitForFences(device(), 1, &in_flight_[frame], VK_TRUE, UINT64_MAX);

            uint32_t image_index = 0;
            const VkResult acquired = vkAcquireNextImageKHR(device(), swapchain_, UINT64_MAX,
                image_available_[frame], VK_NULL_HANDLE, &image_index);
            if (acquired != VK_SUCCESS) {
                return false;
            }
            vkResetFences(device(), 1, &in_flight_[frame]);

            // The composite fills a canvas at the target's own resolution, so
            // every hi-res sample survives to the window.
            if (!ensure_upload(frame, target.width, target.height)) {
                return false;
            }
            CanvasUpload& upload = uploads_[frame];

            // HD stack planes: width * 192 * 2 words each (colour, attr).
            const std::size_t plane_bytes = static_cast<std::size_t>(hd.width) * 192 * 2 * 4;
            if (hd_top_.width != hd.width) {
                if (!create_hd_plane(hd_top_, hd.width) || !create_hd_plane(hd_below_, hd.width)) {
                    return false;
                }
            }
            if (hd_fallback_.width != fallback.width) {
                if (!create_hd_plane(hd_fallback_, fallback.width)) {
                    return false;
                }
            }
            const VkDeviceSize fallback_bytes_local
                = static_cast<VkDeviceSize>(fallback.width) * 192 * 4;
            std::memcpy(hd_top_.staging.mapped, hd.top, plane_bytes);
            std::memcpy(hd_below_.staging.mapped, hd.below, plane_bytes);
            auto* fb_dst = static_cast<uint8_t*>(hd_fallback_.staging.mapped);
            for (uint32_t y = 0; y < 192; ++y) {
                std::memcpy(fb_dst + static_cast<std::size_t>(y) * fallback.width * 4,
                    fallback.data + static_cast<std::size_t>(y) * fallback.pitch,
                    static_cast<std::size_t>(fallback.width) * 4);
            }

            VkCommandBuffer cmd = command_buffers_[frame];
            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo begin {};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
                return false;
            }

            VkMemoryBarrier host_barrier {};
            host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            host_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            host_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                1, &host_barrier, 0, nullptr, 0, nullptr);

            // Staging -> plane buffers.
            VkBufferCopy plane_copy {};
            plane_copy.size = plane_bytes;
            vkCmdCopyBuffer(cmd, hd_top_.staging.buffer, hd_top_.buffer, 1, &plane_copy);
            vkCmdCopyBuffer(cmd, hd_below_.staging.buffer, hd_below_.buffer, 1, &plane_copy);
            plane_copy.size = fallback_bytes_local;
            vkCmdCopyBuffer(cmd, hd_fallback_.staging.buffer, hd_fallback_.buffer, 1, &plane_copy);

            // The upload image is both the composite target and the blit
            // source; it starts undefined and ends as a transfer source.
            VkImageMemoryBarrier to_general { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            to_general.srcAccessMask = 0;
            to_general.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            to_general.image = upload.image;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_general);

            VkBufferMemoryBarrier to_shader[3] {};
            for (VkBufferMemoryBarrier& barrier : to_shader) {
                barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            }
            to_shader[0].buffer = hd_top_.buffer;
            to_shader[0].size = plane_bytes;
            to_shader[1].buffer = hd_below_.buffer;
            to_shader[1].size = plane_bytes;
            to_shader[2].buffer = hd_fallback_.buffer;
            to_shader[2].size = fallback_bytes_local;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 3, to_shader, 0, nullptr);

            // Descriptor writes: the composite target, the foreign hi-res
            // view and the three planes.
            VkDescriptorImageInfo canvas_info {};
            canvas_info.imageView = upload.view;
            canvas_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo hires_info {};
            hires_info.imageView = static_cast<VkImageView>(const_cast<void*>(hd.hires_view));
            hires_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorBufferInfo buffer_info[3] {};
            buffer_info[0] = { hd_top_.buffer, 0, VK_WHOLE_SIZE };
            buffer_info[1] = { hd_below_.buffer, 0, VK_WHOLE_SIZE };
            buffer_info[2] = { hd_fallback_.buffer, 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet writes[5] {};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = hd_set_;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[0].pImageInfo = &canvas_info;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = hd_set_;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].pImageInfo = &hires_info;
            for (uint32_t i = 0; i < 3; ++i) {
                writes[2 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[2 + i].dstSet = hd_set_;
                writes[2 + i].dstBinding = 2 + i;
                writes[2 + i].descriptorCount = 1;
                writes[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[2 + i].pBufferInfo = &buffer_info[i];
            }
            vkUpdateDescriptorSets(device(), 5, writes, 0, nullptr);

            HdPush push {};
            push.canvas_width = target.width;
            push.canvas_height = target.height;
            push.hd_width = hd.width;
            push.fallback_width = fallback.width;
            push.fallback_offset = hd.fallback_offset;
            push.content_row0 = hd.content_row0;
            push.content_rows = hd.content_rows;
            push.scale3d = hd.scale;
            push.render_xpos = hd.render_xpos;
            push.priority3d = hd.priority_3d;
            push.order3d = hd.order_3d;
            push.bldcnt = hd.bldcnt;
            push.eva = hd.eva;
            push.evb = hd.evb;
            push.evy = hd.evy;
            push.master_bright = hd.master_bright;
            push.zoom = hd.zoom;
            push.pan_x = hd.pan_x;
            push.pan_y = hd.pan_y;

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hd_pipeline_);
            vkCmdPushConstants(
                cmd, hd_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (target.width + 7) / 8, (target.height + 7) / 8, 1);

            VkImageMemoryBarrier canvas_ready = to_general;
            canvas_ready.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            canvas_ready.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            canvas_ready.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            canvas_ready.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &canvas_ready);

            // From here the path is identical to present(): clear the
            // swapchain, blit the canvas into the placed rectangle, present.
            VkImageMemoryBarrier to_transfer { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_transfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            to_transfer.srcAccessMask = 0;
            to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_transfer.image = images_[image_index];
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);

            const uint8_t letterbox_b = static_cast<uint8_t>(kLetterboxBgra & 0xFFu);
            const uint8_t letterbox_g = static_cast<uint8_t>((kLetterboxBgra >> 8) & 0xFFu);
            const uint8_t letterbox_r = static_cast<uint8_t>((kLetterboxBgra >> 16) & 0xFFu);
            VkClearColorValue clear {};
            clear.float32[0] = static_cast<float>(letterbox_r) / 255.0f;
            clear.float32[1] = static_cast<float>(letterbox_g) / 255.0f;
            clear.float32[2] = static_cast<float>(letterbox_b) / 255.0f;
            clear.float32[3] = 1.0f;
            const VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(
                cmd, images_[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

            VkImageBlit blit {};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[0] = { 0, 0, 0 };
            blit.srcOffsets[1]
                = { static_cast<int32_t>(target.width), static_cast<int32_t>(target.height), 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.dstOffsets[0]
                = { static_cast<int32_t>(target.x), static_cast<int32_t>(target.y), 0 };
            blit.dstOffsets[1] = { static_cast<int32_t>(target.x + target.width),
                static_cast<int32_t>(target.y + target.height), 1 };
            vkCmdBlitImage(cmd, upload.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                images_[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                linear_filter_ ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

            VkImageMemoryBarrier to_present = to_transfer;
            to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_present.dstAccessMask = 0;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_present);

            if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
                return false;
            }

            VkSubmitInfo submit {};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &image_available_[frame];
            submit.pWaitDstStageMask = &wait_stage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &render_finished_[frame];
            if (vkQueueSubmit(queue(), 1, &submit, in_flight_[frame]) != VK_SUCCESS) {
                return false;
            }

            VkPresentInfoKHR present_info {};
            present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present_info.waitSemaphoreCount = 1;
            present_info.pWaitSemaphores = &render_finished_[frame];
            present_info.swapchainCount = 1;
            present_info.pSwapchains = &swapchain_;
            present_info.pImageIndices = &image_index;
            const VkResult presented = vkQueuePresentKHR(queue(), &present_info);
            if (presented != VK_SUCCESS) {
                return false;
            }

            frame_ = (frame_ + 1) % kFramesInFlight;
            return true;
        }

        VkSurfaceKHR surface() const { return context_ ? context_->surface() : VK_NULL_HANDLE; }
        VkPhysicalDevice physical_device() const
        {
            return context_ ? context_->physical_device() : VK_NULL_HANDLE;
        }
        VkDevice device() const { return context_ ? context_->device() : VK_NULL_HANDLE; }
        VkQueue queue() const { return context_ ? context_->queue() : VK_NULL_HANDLE; }
        uint32_t queue_family() const { return context_ ? context_->queue_family() : 0; }

        SDL_Window* window_ = nullptr;
        bool vsync_ = true;
        bool linear_filter_ = false;
        std::shared_ptr<VulkanContext> context_;

        VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
        VkExtent2D extent_ { 0, 0 };
        std::vector<VkImage> images_;

        VkCommandPool command_pool_ = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> command_buffers_;
        VkSemaphore image_available_[kFramesInFlight] {};
        VkSemaphore render_finished_[kFramesInFlight] {};
        VkFence in_flight_[kFramesInFlight] {};
        uint32_t frame_ = 0;
        CanvasUpload uploads_[kFramesInFlight];

        // HD composite state (ADR 0008, slice V2).
        HdPlane hd_top_;
        HdPlane hd_below_;
        HdPlane hd_fallback_;
        VkDescriptorSetLayout hd_desc_layout_ = VK_NULL_HANDLE;
        VkPipelineLayout hd_pipeline_layout_ = VK_NULL_HANDLE;
        VkPipeline hd_pipeline_ = VK_NULL_HANDLE;
        VkDescriptorPool hd_pool_ = VK_NULL_HANDLE;
        VkDescriptorSet hd_set_ = VK_NULL_HANDLE;
    };

} // namespace

std::unique_ptr<Presenter> create_vulkan_presenter() { return std::make_unique<VulkanPresenter>(); }

} // namespace pkmn::render

#endif // PKMN_RENDER_VULKAN
