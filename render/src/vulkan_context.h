#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

#include "pkmn/render/backend.h"

struct SDL_Window;

namespace pkmn::render {

// Instance, physical/logical device, queue and memory helpers shared by the
// presenter and the DS render sinks. The presenter owns the swapchain; this
// class owns only the device context. Private to render/src.
class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // Creates an instance, a surface for `window`, and a graphics+present
    // device. `require_compute` makes the queue choice fail when no
    // graphics+present+compute family exists.
    bool init(SDL_Window* window, const Config& config, bool require_compute = false);

    bool wait_idle();

    VkInstance instance() const { return instance_; }
    VkSurfaceKHR surface() const { return surface_; }
    VkPhysicalDevice physical_device() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    uint32_t queue_family() const { return queue_family_; }
    bool compute_capable() const { return compute_capable_; }

    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) const;

private:
    void shutdown();
    bool create_instance(const Config& config);
    bool create_debug_messenger();
    bool pick_device(const std::string& preference, bool require_compute);
    bool create_device();

    SDL_Window* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    bool want_debug_utils_ = false;
    bool compute_capable_ = false;

    PFN_vkGetInstanceProcAddr get_instance_proc_addr_ = nullptr;
    PFN_vkCreateDebugUtilsMessengerEXT create_debug_messenger_ = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_messenger_ = nullptr;
};

// Process-wide host context. The presenter that owns the top-screen swapchain
// publishes its context here; the native DS render sinks attach to it instead
// of creating a second instance/device on the same thread (ADR 0007 decision
// 2). Returns null when no presenter is live.
VulkanContext* shared_context();
void publish_shared_context(VulkanContext* context);
void withdraw_shared_context(VulkanContext* context);

} // namespace pkmn::render
