#if defined(PKMN_RENDER_VULKAN)

#include "vulkan_presenter.h"

#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "vulkan_functions.h"

namespace pkmn::render {
namespace {

    using namespace pkmn::render::vk;

    constexpr uint32_t kFramesInFlight = 2;
    constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

    VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
    {
        const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error"
            : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)             ? "warn"
                                                                                       : "info";
        std::fprintf(stderr, "[render][vulkan %s] %s\n", level,
            data && data->pMessage ? data->pMessage : "(no message)");
        return VK_FALSE;
    }

    struct StagingBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
    };

    bool has_layer(const char* name)
    {
        uint32_t count = 0;
        if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
            return false;
        }
        std::vector<VkLayerProperties> layers(count);
        if (count && vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
            return false;
        }
        for (const VkLayerProperties& layer : layers) {
            if (std::strcmp(layer.layerName, name) == 0) {
                return true;
            }
        }
        return false;
    }

    bool device_has_extension(VkPhysicalDevice device, const char* name)
    {
        uint32_t count = 0;
        if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
            return false;
        }
        std::vector<VkExtensionProperties> extensions(count);
        if (count
            && vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data())
                != VK_SUCCESS) {
            return false;
        }
        for (const VkExtensionProperties& ext : extensions) {
            if (std::strcmp(ext.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    }

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
            if (!create_instance(config)) {
                return false;
            }
            if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
                std::fprintf(
                    stderr, "[render] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
                return false;
            }
            if (!load_instance(instance_, get_instance_proc_addr_)) {
                return false;
            }
            if (config.validation && !create_debug_messenger()) {
                std::fprintf(stderr, "[render] validation requested but unavailable; continuing\n");
            }
            if (!pick_device()) {
                return false;
            }
            if (!create_device()) {
                return false;
            }
            if (!load_device(device_, vkGetDeviceProcAddr)) {
                return false;
            }
            vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
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
            if (device_) {
                vkDeviceWaitIdle(device_);
            }
            destroy_swapchain();
            destroy_staging();
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                if (image_available_[i]) {
                    vkDestroySemaphore(device_, image_available_[i], nullptr);
                    image_available_[i] = VK_NULL_HANDLE;
                }
                if (render_finished_[i]) {
                    vkDestroySemaphore(device_, render_finished_[i], nullptr);
                    render_finished_[i] = VK_NULL_HANDLE;
                }
                if (in_flight_[i]) {
                    vkDestroyFence(device_, in_flight_[i], nullptr);
                    in_flight_[i] = VK_NULL_HANDLE;
                }
            }
            if (command_pool_) {
                vkDestroyCommandPool(device_, command_pool_, nullptr);
                command_pool_ = VK_NULL_HANDLE;
            }
            command_buffers_.clear();
            if (device_) {
                vkDestroyDevice(device_, nullptr);
                device_ = VK_NULL_HANDLE;
            }
            if (surface_ && instance_) {
                vkDestroySurfaceKHR(instance_, surface_, nullptr);
                surface_ = VK_NULL_HANDLE;
            }
            if (destroy_debug_messenger_ && debug_messenger_) {
                destroy_debug_messenger_(instance_, debug_messenger_, nullptr);
                debug_messenger_ = VK_NULL_HANDLE;
            }
            if (instance_) {
                vkDestroyInstance(instance_, nullptr);
                instance_ = VK_NULL_HANDLE;
            }
        }

        bool resize(uint32_t width, uint32_t height) override
        {
            if (!device_ || width == 0 || height == 0) {
                return false;
            }
            vkDeviceWaitIdle(device_);
            destroy_swapchain();
            return create_swapchain(width, height);
        }

        uint32_t width() const override { return extent_.width; }
        uint32_t height() const override { return extent_.height; }
        BackendKind kind() const override { return BackendKind::Vulkan; }

        bool present(const Canvas& canvas) override
        {
            if (!device_ || swapchain_ == VK_NULL_HANDLE) {
                return false;
            }
            if (!canvas.data || canvas.width != extent_.width || canvas.height != extent_.height) {
                // The caller must resize() to the current extent first; report a
                // recoverable failure rather than guessing a scale.
                return false;
            }

            const uint32_t frame = frame_;
            vkWaitForFences(device_, 1, &in_flight_[frame], VK_TRUE, UINT64_MAX);

            uint32_t image_index = 0;
            const VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                image_available_[frame], VK_NULL_HANDLE, &image_index);
            if (acquired == VK_ERROR_OUT_OF_DATE_KHR || acquired == VK_SUBOPTIMAL_KHR) {
                return false;
            }
            if (acquired != VK_SUCCESS) {
                std::fprintf(stderr, "[render] vkAcquireNextImageKHR failed (%d)\n", acquired);
                return false;
            }

            vkResetFences(device_, 1, &in_flight_[frame]);

            // Upload the canvas into the mapped staging buffer (row pitch may
            // exceed width*4).
            const std::size_t row_bytes = static_cast<std::size_t>(canvas.width) * 4;
            const std::size_t dst_pitch = static_cast<std::size_t>(extent_.width) * 4;
            auto* dst = static_cast<uint8_t*>(staging_.mapped);
            for (uint32_t y = 0; y < canvas.height; ++y) {
                std::memcpy(dst + y * dst_pitch,
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

            // Present source -> transfer destination. We overwrite every texel, so
            // the previous contents are declared undefined rather than preserved.
            VkImageMemoryBarrier to_transfer {};
            to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_transfer.image = images_[image_index];
            to_transfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            to_transfer.srcAccessMask = 0;
            to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);

            VkBufferImageCopy region {};
            region.bufferOffset = 0;
            region.bufferRowLength = 0; // tightly packed rows in the staging buffer
            region.bufferImageHeight = 0;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageOffset = { 0, 0, 0 };
            region.imageExtent = { canvas.width, canvas.height, 1 };
            vkCmdCopyBufferToImage(cmd, staging_.buffer, images_[image_index],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

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
            if (vkQueueSubmit(queue_, 1, &submit, in_flight_[frame]) != VK_SUCCESS) {
                return false;
            }

            VkPresentInfoKHR present_info {};
            present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present_info.waitSemaphoreCount = 1;
            present_info.pWaitSemaphores = &render_finished_[frame];
            present_info.swapchainCount = 1;
            present_info.pSwapchains = &swapchain_;
            present_info.pImageIndices = &image_index;
            const VkResult presented = vkQueuePresentKHR(queue_, &present_info);
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
        bool create_instance(const Config& config)
        {
            get_instance_proc_addr_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                SDL_Vulkan_GetVkGetInstanceProcAddr());
            if (!get_instance_proc_addr_ || !load_global(get_instance_proc_addr_)) {
                std::fprintf(stderr, "[render] Vulkan loader unavailable\n");
                return false;
            }

            uint32_t ext_count = 0;
            const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&ext_count);
            if (!sdl_extensions) {
                std::fprintf(stderr, "[render] SDL_Vulkan_GetInstanceExtensions failed: %s\n",
                    SDL_GetError());
                return false;
            }
            std::vector<const char*> extensions(sdl_extensions, sdl_extensions + ext_count);

            std::vector<const char*> layers;
            if (config.validation && has_layer(kValidationLayer)) {
                layers.push_back(kValidationLayer);
                if (std::find(
                        extensions.begin(), extensions.end(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
                    == extensions.end()) {
                    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                }
            }

            VkApplicationInfo app {};
            app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            app.pApplicationName = "pkmng5pc";
            app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
            app.pEngineName = "pkmng5pc-render";
            app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
            app.apiVersion = VK_API_VERSION_1_1;

            VkInstanceCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            info.pApplicationInfo = &app;
            info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            info.ppEnabledExtensionNames = extensions.data();
            info.enabledLayerCount = static_cast<uint32_t>(layers.size());
            info.ppEnabledLayerNames = layers.data();

            const VkResult result = vkCreateInstance(&info, nullptr, &instance_);
            if (result != VK_SUCCESS) {
                std::fprintf(stderr, "[render] vkCreateInstance failed (%d)\n", result);
                return false;
            }
            want_debug_utils_ = !layers.empty();
            return true;
        }

        bool create_debug_messenger()
        {
            if (!want_debug_utils_) {
                return false;
            }
            create_debug_messenger_ = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                get_instance_proc_addr_(instance_, "vkCreateDebugUtilsMessengerEXT"));
            destroy_debug_messenger_ = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                get_instance_proc_addr_(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (!create_debug_messenger_ || !destroy_debug_messenger_) {
                return false;
            }
            VkDebugUtilsMessengerCreateInfoEXT info {};
            info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = debug_callback;
            return create_debug_messenger_(instance_, &info, nullptr, &debug_messenger_)
                == VK_SUCCESS;
        }

        bool pick_device()
        {
            uint32_t count = 0;
            if (vkEnumeratePhysicalDevices(instance_, &count, nullptr) != VK_SUCCESS
                || count == 0) {
                std::fprintf(stderr, "[render] no Vulkan physical devices\n");
                return false;
            }
            std::vector<VkPhysicalDevice> devices(count);
            if (vkEnumeratePhysicalDevices(instance_, &count, devices.data()) != VK_SUCCESS) {
                return false;
            }
            for (VkPhysicalDevice device : devices) {
                if (!device_has_extension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                    continue;
                }
                uint32_t family_count = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
                std::vector<VkQueueFamilyProperties> families(family_count);
                vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
                for (uint32_t i = 0; i < family_count; ++i) {
                    if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                        continue;
                    }
                    VkBool32 present = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &present);
                    if (!present) {
                        continue;
                    }
                    physical_ = device;
                    queue_family_ = i;
                    return true;
                }
            }
            std::fprintf(stderr, "[render] no graphics+present Vulkan queue found\n");
            return false;
        }

        bool create_device()
        {
            const float priority = 1.0f;
            VkDeviceQueueCreateInfo queue_info {};
            queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_info.queueFamilyIndex = queue_family_;
            queue_info.queueCount = 1;
            queue_info.pQueuePriorities = &priority;

            const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
            VkDeviceCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            info.queueCreateInfoCount = 1;
            info.pQueueCreateInfos = &queue_info;
            info.enabledExtensionCount = 1;
            info.ppEnabledExtensionNames = extensions;

            const VkResult result = vkCreateDevice(physical_, &info, nullptr, &device_);
            if (result != VK_SUCCESS) {
                std::fprintf(stderr, "[render] vkCreateDevice failed (%d)\n", result);
                return false;
            }
            return true;
        }

        bool create_command_resources()
        {
            VkCommandPoolCreateInfo pool {};
            pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = queue_family_;
            if (vkCreateCommandPool(device_, &pool, nullptr, &command_pool_) != VK_SUCCESS) {
                return false;
            }
            command_buffers_.resize(kFramesInFlight, VK_NULL_HANDLE);
            VkCommandBufferAllocateInfo alloc {};
            alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            alloc.commandPool = command_pool_;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = kFramesInFlight;
            return vkAllocateCommandBuffers(device_, &alloc, command_buffers_.data()) == VK_SUCCESS;
        }

        bool create_sync()
        {
            VkSemaphoreCreateInfo sem {};
            sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkFenceCreateInfo fence {};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                if (vkCreateSemaphore(device_, &sem, nullptr, &image_available_[i]) != VK_SUCCESS
                    || vkCreateSemaphore(device_, &sem, nullptr, &render_finished_[i]) != VK_SUCCESS
                    || vkCreateFence(device_, &fence, nullptr, &in_flight_[i]) != VK_SUCCESS) {
                    return false;
                }
            }
            return true;
        }

        VkFormat choose_format() const
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data());
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
            vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &count, nullptr);
            std::vector<VkPresentModeKHR> modes(count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &count, modes.data());
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
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps)
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
            info.surface = surface_;
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

            if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS) {
                std::fprintf(stderr, "[render] vkCreateSwapchainKHR failed\n");
                return false;
            }
            uint32_t count = 0;
            vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
            images_.resize(count);
            vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data());
            extent_ = extent;
            if (!create_staging(static_cast<VkDeviceSize>(extent.width) * extent.height * 4)) {
                return false;
            }
            return true;
        }

        void destroy_swapchain()
        {
            if (swapchain_) {
                vkDestroySwapchainKHR(device_, swapchain_, nullptr);
                swapchain_ = VK_NULL_HANDLE;
            }
            images_.clear();
            destroy_staging();
            extent_ = { 0, 0 };
        }

        uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) const
        {
            VkPhysicalDeviceMemoryProperties memory {};
            vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
            for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
                if ((type_bits & (1u << i))
                    && (memory.memoryTypes[i].propertyFlags & props) == props) {
                    return i;
                }
            }
            return UINT32_MAX;
        }

        bool create_staging(VkDeviceSize size)
        {
            if (size == 0) {
                return false;
            }
            VkBufferCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = size;
            info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device_, &info, nullptr, &staging_.buffer) != VK_SUCCESS) {
                return false;
            }
            VkMemoryRequirements req {};
            vkGetBufferMemoryRequirements(device_, staging_.buffer, &req);
            const uint32_t type = find_memory_type(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (type == UINT32_MAX) {
                return false;
            }
            VkMemoryAllocateInfo alloc {};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = type;
            if (vkAllocateMemory(device_, &alloc, nullptr, &staging_.memory) != VK_SUCCESS) {
                return false;
            }
            vkBindBufferMemory(device_, staging_.buffer, staging_.memory, 0);
            if (vkMapMemory(device_, staging_.memory, 0, size, 0, &staging_.mapped) != VK_SUCCESS) {
                return false;
            }
            staging_.size = size;
            return true;
        }

        void destroy_staging()
        {
            if (!device_) {
                return;
            }
            if (staging_.mapped) {
                vkUnmapMemory(device_, staging_.memory);
                staging_.mapped = nullptr;
            }
            if (staging_.buffer) {
                vkDestroyBuffer(device_, staging_.buffer, nullptr);
                staging_.buffer = VK_NULL_HANDLE;
            }
            if (staging_.memory) {
                vkFreeMemory(device_, staging_.memory, nullptr);
                staging_.memory = VK_NULL_HANDLE;
            }
            staging_.size = 0;
        }

        SDL_Window* window_ = nullptr;
        bool vsync_ = true;
        bool want_debug_utils_ = false;

        VkInstance instance_ = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
        VkSurfaceKHR surface_ = VK_NULL_HANDLE;
        VkPhysicalDevice physical_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueue queue_ = VK_NULL_HANDLE;
        uint32_t queue_family_ = 0;

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
        StagingBuffer staging_;

        PFN_vkGetInstanceProcAddr get_instance_proc_addr_ = nullptr;
        PFN_vkCreateDebugUtilsMessengerEXT create_debug_messenger_ = nullptr;
        PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_messenger_ = nullptr;
    };

} // namespace

std::unique_ptr<Presenter> create_vulkan_presenter() { return std::make_unique<VulkanPresenter>(); }

} // namespace pkmn::render

#endif // PKMN_RENDER_VULKAN
