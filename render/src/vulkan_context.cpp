#include "vulkan_context.h"

#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vulkan_functions.h"

namespace pkmn::render {
namespace {

    using namespace pkmn::render::vk;

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

    std::string lower_ascii(std::string value)
    {
        for (char& c : value) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return value;
    }

    int gpu_score(GpuType type)
    {
        switch (type) {
        case GpuType::Discrete:
            return 4;
        case GpuType::Integrated:
            return 2;
        case GpuType::Virtual:
            return 1;
        case GpuType::Other:
            return 1;
        case GpuType::Cpu:
            return 0;
        }
        return 0;
    }

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

} // namespace

namespace {
    VulkanContext* g_shared_context = nullptr;
}

VulkanContext* shared_context() { return g_shared_context; }

void publish_shared_context(VulkanContext* context)
{
    if (!g_shared_context) {
        g_shared_context = context;
    }
}

void withdraw_shared_context(VulkanContext* context)
{
    if (g_shared_context == context) {
        g_shared_context = nullptr;
    }
}

VulkanContext::~VulkanContext() { shutdown(); }

bool VulkanContext::init(SDL_Window* window, const Config& config, bool require_compute)
{
    if (instance_ || device_) {
        return false;
    }
    window_ = window;
    if (!window_) {
        return false;
    }
    if (!create_instance(config)) {
        shutdown();
        return false;
    }
    if (!load_instance(instance_, get_instance_proc_addr_)) {
        shutdown();
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        std::fprintf(stderr, "[render] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        shutdown();
        return false;
    }
    if (config.validation && !create_debug_messenger()) {
        std::fprintf(stderr, "[render] validation requested but unavailable; continuing\n");
    }
    if (!pick_device(config.gpu, require_compute)) {
        shutdown();
        return false;
    }
    if (!create_device()) {
        shutdown();
        return false;
    }
    if (!load_device(device_, vkGetDeviceProcAddr)) {
        shutdown();
        return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return true;
}

bool VulkanContext::wait_idle()
{
    if (!device_ || !vkDeviceWaitIdle) {
        return false;
    }
    return vkDeviceWaitIdle(device_) == VK_SUCCESS;
}

uint32_t VulkanContext::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) const
{
    VkPhysicalDeviceMemoryProperties memory {};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

void VulkanContext::shutdown()
{
    if (device_) {
        if (vkDeviceWaitIdle) {
            vkDeviceWaitIdle(device_);
        }
        if (vkDestroyDevice) {
            vkDestroyDevice(device_, nullptr);
        }
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ && instance_ && vkDestroySurfaceKHR) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (destroy_debug_messenger_ && debug_messenger_) {
        destroy_debug_messenger_(instance_, debug_messenger_, nullptr);
        debug_messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ && vkDestroyInstance) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    physical_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    queue_family_ = 0;
    want_debug_utils_ = false;
    compute_capable_ = false;
    window_ = nullptr;
}

bool VulkanContext::create_instance(const Config& config)
{
    get_instance_proc_addr_
        = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!get_instance_proc_addr_ || !load_global(get_instance_proc_addr_)) {
        std::fprintf(stderr, "[render] Vulkan loader unavailable\n");
        return false;
    }

    uint32_t ext_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!sdl_extensions) {
        std::fprintf(
            stderr, "[render] SDL_Vulkan_GetInstanceExtensions failed: %s\n", SDL_GetError());
        return false;
    }
    std::vector<const char*> extensions(sdl_extensions, sdl_extensions + ext_count);

    std::vector<const char*> layers;
    if (config.validation && has_layer(kValidationLayer)) {
        layers.push_back(kValidationLayer);
        if (std::find(extensions.begin(), extensions.end(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
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

bool VulkanContext::create_debug_messenger()
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
    return create_debug_messenger_(instance_, &info, nullptr, &debug_messenger_) == VK_SUCCESS;
}

bool VulkanContext::pick_device(const std::string& preference, bool require_compute)
{
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance_, &count, nullptr) != VK_SUCCESS || count == 0) {
        std::fprintf(stderr, "[render] no Vulkan physical devices\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance_, &count, devices.data()) != VK_SUCCESS) {
        return false;
    }

    struct DeviceCandidate {
        VkPhysicalDevice device = VK_NULL_HANDLE;
        uint32_t index = 0;
        uint32_t queue_family = 0;
        VkPhysicalDeviceProperties properties {};
        VkPhysicalDeviceMemoryProperties memory {};
        GpuType type = GpuType::Other;
        bool compute = false;
    };

    std::vector<DeviceCandidate> candidates;
    for (uint32_t index = 0; index < count; ++index) {
        const VkPhysicalDevice device = devices[index];
        if (!device_has_extension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
        for (uint32_t family = 0; family < family_count; ++family) {
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            const bool compute = (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            if (require_compute && !compute) {
                continue;
            }
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, family, surface_, &present);
            if (!present) {
                continue;
            }
            DeviceCandidate candidate;
            candidate.device = device;
            candidate.index = index;
            candidate.queue_family = family;
            candidate.compute = compute;
            vkGetPhysicalDeviceProperties(device, &candidate.properties);
            vkGetPhysicalDeviceMemoryProperties(device, &candidate.memory);
            switch (candidate.properties.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                candidate.type = GpuType::Discrete;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                candidate.type = GpuType::Integrated;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                candidate.type = GpuType::Virtual;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                candidate.type = GpuType::Cpu;
                break;
            default:
                candidate.type = GpuType::Other;
                break;
            }
            candidates.push_back(std::move(candidate));
            break;
        }
    }
    if (candidates.empty()) {
        std::fprintf(stderr, "[render] no graphics+present Vulkan queue found%s\n",
            require_compute ? " with compute support" : "");
        return false;
    }

    const std::string lowered = lower_ascii(preference.empty() ? std::string("auto") : preference);
    bool has_explicit_index = false;
    uint32_t explicit_index = 0;
    if (lowered.rfind("index:", 0) == 0) {
        has_explicit_index = true;
        explicit_index = static_cast<uint32_t>(std::strtoul(lowered.c_str() + 6, nullptr, 10));
    } else if (!lowered.empty() && std::all_of(lowered.begin(), lowered.end(), [](unsigned char c) {
                   return std::isdigit(c) != 0;
               })) {
        has_explicit_index = true;
        explicit_index = static_cast<uint32_t>(std::strtoul(lowered.c_str(), nullptr, 10));
    }

    std::vector<const DeviceCandidate*> eligible;
    for (const DeviceCandidate& candidate : candidates) {
        const std::string name = lower_ascii(candidate.properties.deviceName);
        bool matches = true;
        if (lowered.empty() || lowered == "auto") {
            matches = true;
        } else if (lowered == "discrete") {
            matches = candidate.type == GpuType::Discrete;
        } else if (lowered == "integrated") {
            matches = candidate.type == GpuType::Integrated;
        } else if (has_explicit_index) {
            matches = candidate.index == explicit_index;
        } else {
            matches = name.find(lowered) != std::string::npos;
        }
        if (matches) {
            eligible.push_back(&candidate);
        }
    }
    if (eligible.empty()) {
        std::fprintf(stderr,
            "[render] gpu preference '%s' matched no present-capable device; using auto\n",
            preference.c_str());
        for (const DeviceCandidate& candidate : candidates) {
            eligible.push_back(&candidate);
        }
    }

    const DeviceCandidate* best = nullptr;
    uint64_t best_memory = 0;
    for (const DeviceCandidate* candidate : eligible) {
        uint64_t device_local = 0;
        for (uint32_t heap = 0; heap < candidate->memory.memoryHeapCount; ++heap) {
            if ((candidate->memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                != 0) {
                device_local += candidate->memory.memoryHeaps[heap].size;
            }
        }
        if (best == nullptr || gpu_score(candidate->type) > gpu_score(best->type)
            || (gpu_score(candidate->type) == gpu_score(best->type)
                && device_local > best_memory)) {
            best = candidate;
            best_memory = device_local;
        }
    }
    if (best == nullptr) {
        return false;
    }

    physical_ = best->device;
    queue_family_ = best->queue_family;
    compute_capable_ = best->compute;
    const VkPhysicalDeviceProperties& properties = best->properties;
    std::fprintf(stderr, "[render] gpu: %s (%s, %llu MiB, Vulkan %u.%u.%u, driver %u)\n",
        properties.deviceName, to_string(best->type),
        static_cast<unsigned long long>(best_memory / (1024ull * 1024ull)),
        VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
        VK_VERSION_PATCH(properties.apiVersion), properties.driverVersion);
    return true;
}

bool VulkanContext::create_device()
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

} // namespace pkmn::render
