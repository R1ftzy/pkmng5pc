#include "pkmn/render/backend.h"

#if defined(PKMN_RENDER_VULKAN)
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <vector>

#include "vulkan_functions.h"
#endif

namespace pkmn::render {

const char* to_string(GpuType type)
{
    switch (type) {
    case GpuType::Other:
        return "other";
    case GpuType::Integrated:
        return "integrated";
    case GpuType::Discrete:
        return "discrete";
    case GpuType::Virtual:
        return "virtual";
    case GpuType::Cpu:
        return "cpu";
    }
    return "unknown";
}

#if defined(PKMN_RENDER_VULKAN)

namespace {

    using namespace pkmn::render::vk;

    GpuType gpu_type(VkPhysicalDeviceType type)
    {
        switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return GpuType::Integrated;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return GpuType::Discrete;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return GpuType::Virtual;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return GpuType::Cpu;
        default:
            return GpuType::Other;
        }
    }

} // namespace

std::vector<GpuInfo> enumerate_gpus()
{
    std::vector<GpuInfo> result;
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return result;
    }
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        return result;
    }
    const auto get_instance_proc_addr
        = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!get_instance_proc_addr || !load_global(get_instance_proc_addr)) {
        return result;
    }

    VkApplicationInfo app {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "pkmng5pc";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "pkmng5pc-render";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
        return result;
    }

    // The enumeration instance enables no surface extensions, so only the
    // device-independent entry points are usable here. Do not call
    // load_instance(), which requires the surface functions.
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        get_instance_proc_addr(instance, "vkEnumeratePhysicalDevices"));
    const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        get_instance_proc_addr(instance, "vkGetPhysicalDeviceProperties"));
    const auto get_memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        get_instance_proc_addr(instance, "vkGetPhysicalDeviceMemoryProperties"));
    const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
        get_instance_proc_addr(instance, "vkDestroyInstance"));
    if (!enumerate || !get_properties || !get_memory || !destroy_instance) {
        if (destroy_instance) {
            destroy_instance(instance, nullptr);
        }
        return result;
    }

    uint32_t count = 0;
    if (enumerate(instance, &count, nullptr) == VK_SUCCESS && count > 0) {
        std::vector<VkPhysicalDevice> devices(count);
        if (enumerate(instance, &count, devices.data()) == VK_SUCCESS) {
            result.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                VkPhysicalDeviceProperties properties {};
                get_properties(devices[i], &properties);

                VkPhysicalDeviceMemoryProperties memory {};
                get_memory(devices[i], &memory);

                uint64_t device_local = 0;
                for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
                    if ((memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                        device_local += memory.memoryHeaps[heap].size;
                    }
                }

                GpuInfo gpu;
                gpu.index = i;
                gpu.name = properties.deviceName;
                gpu.type = gpu_type(properties.deviceType);
                gpu.api_version = properties.apiVersion;
                gpu.driver_version = properties.driverVersion;
                gpu.memory_bytes = device_local;
                result.push_back(std::move(gpu));
            }
        }
    }

    destroy_instance(instance, nullptr);
    return result;
}

#else

std::vector<GpuInfo> enumerate_gpus() { return {}; }

#endif

} // namespace pkmn::render
