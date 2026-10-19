#include "vulkan_functions.h"

#include <cstdio>

namespace pkmn::render::vk {

#define PKMN_VK_DEFINE(name) PFN_##name name = nullptr;
PKMN_VK_GLOBAL_FUNCTIONS(PKMN_VK_DEFINE)
PKMN_VK_INSTANCE_FUNCTIONS(PKMN_VK_DEFINE)
PKMN_VK_DEVICE_FUNCTIONS(PKMN_VK_DEFINE)
#undef PKMN_VK_DEFINE

bool load_global(PFN_vkGetInstanceProcAddr get_instance_proc_addr)
{
    if (!get_instance_proc_addr) {
        return false;
    }
#define PKMN_VK_LOAD_GLOBAL(name)                                                                  \
    name = reinterpret_cast<PFN_##name>(get_instance_proc_addr(VK_NULL_HANDLE, #name));            \
    if (!name) {                                                                                   \
        std::fprintf(stderr, "[render] missing Vulkan entry point %s\n", #name);                   \
        return false;                                                                              \
    }
    PKMN_VK_GLOBAL_FUNCTIONS(PKMN_VK_LOAD_GLOBAL)
#undef PKMN_VK_LOAD_GLOBAL
    return true;
}

bool load_instance(VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr)
{
    if (!instance || !get_instance_proc_addr) {
        return false;
    }
#define PKMN_VK_LOAD_INSTANCE(name)                                                                \
    name = reinterpret_cast<PFN_##name>(get_instance_proc_addr(instance, #name));                  \
    if (!name) {                                                                                   \
        std::fprintf(stderr, "[render] missing Vulkan entry point %s\n", #name);                   \
        return false;                                                                              \
    }
    PKMN_VK_INSTANCE_FUNCTIONS(PKMN_VK_LOAD_INSTANCE)
#undef PKMN_VK_LOAD_INSTANCE
    return true;
}

bool load_device(VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc_addr)
{
    if (!device || !get_device_proc_addr) {
        return false;
    }
#define PKMN_VK_LOAD_DEVICE(name)                                                                  \
    name = reinterpret_cast<PFN_##name>(get_device_proc_addr(device, #name));                      \
    if (!name) {                                                                                   \
        std::fprintf(stderr, "[render] missing Vulkan entry point %s\n", #name);                   \
        return false;                                                                              \
    }
    PKMN_VK_DEVICE_FUNCTIONS(PKMN_VK_LOAD_DEVICE)
#undef PKMN_VK_LOAD_DEVICE
    return true;
}

} // namespace pkmn::render::vk
