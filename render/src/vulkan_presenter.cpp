#if defined(PKMN_RENDER_VULKAN)

#include "vulkan_presenter.h"

#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "vulkan_context.h"
#include "vulkan_functions.h"

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
        VkDeviceMemory image_memory = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
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
                VK_FILTER_NEAREST);

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
            info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(device(), &info, nullptr, &upload.image) != VK_SUCCESS) {
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
    };

} // namespace

std::unique_ptr<Presenter> create_vulkan_presenter() { return std::make_unique<VulkanPresenter>(); }

} // namespace pkmn::render

#endif // PKMN_RENDER_VULKAN
