#pragma once

// Runtime-resolved Vulkan entry points.
//
// The build only needs the Khronos headers; the loader (`vulkan-1.dll` /
// `libvulkan.so.1`) ships with the GPU driver, so nothing links against a
// Vulkan import library and no SDK is required. Every entry point is fetched
// through `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`, which SDL3 hands us
// for the platform.

#include <vulkan/vulkan.h>

namespace pkmn::render::vk {

// X-macro lists keep the declarations and the loader table in step.
#define PKMN_VK_GLOBAL_FUNCTIONS(X)                                                                \
    X(vkCreateInstance)                                                                            \
    X(vkEnumerateInstanceExtensionProperties)                                                      \
    X(vkEnumerateInstanceLayerProperties)

#define PKMN_VK_INSTANCE_FUNCTIONS(X)                                                              \
    X(vkDestroyInstance)                                                                           \
    X(vkEnumeratePhysicalDevices)                                                                  \
    X(vkEnumerateDeviceExtensionProperties)                                                        \
    X(vkGetPhysicalDeviceProperties)                                                               \
    X(vkGetPhysicalDeviceMemoryProperties)                                                         \
    X(vkGetPhysicalDeviceFormatProperties)                                                         \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                    \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                        \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                   \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                        \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                                   \
    X(vkCreateDevice)                                                                              \
    X(vkGetDeviceProcAddr)                                                                         \
    X(vkDestroySurfaceKHR)

#define PKMN_VK_DEVICE_FUNCTIONS(X)                                                                \
    X(vkDestroyDevice)                                                                             \
    X(vkGetDeviceQueue)                                                                            \
    X(vkDeviceWaitIdle)                                                                            \
    X(vkQueueWaitIdle)                                                                             \
    X(vkQueueSubmit)                                                                               \
    X(vkQueuePresentKHR)                                                                           \
    X(vkAcquireNextImageKHR)                                                                       \
    X(vkCreateSwapchainKHR)                                                                        \
    X(vkDestroySwapchainKHR)                                                                       \
    X(vkGetSwapchainImagesKHR)                                                                     \
    X(vkCreateSemaphore)                                                                           \
    X(vkDestroySemaphore)                                                                          \
    X(vkCreateFence)                                                                               \
    X(vkDestroyFence)                                                                              \
    X(vkWaitForFences)                                                                             \
    X(vkResetFences)                                                                               \
    X(vkCreateCommandPool)                                                                         \
    X(vkDestroyCommandPool)                                                                        \
    X(vkAllocateCommandBuffers)                                                                    \
    X(vkFreeCommandBuffers)                                                                        \
    X(vkBeginCommandBuffer)                                                                        \
    X(vkEndCommandBuffer)                                                                          \
    X(vkResetCommandBuffer)                                                                        \
    X(vkCmdPipelineBarrier)                                                                        \
    X(vkCmdCopyBufferToImage)                                                                      \
    X(vkCmdCopyImageToBuffer)                                                                      \
    X(vkCmdBlitImage)                                                                              \
    X(vkCmdClearColorImage)                                                                        \
    X(vkCmdBindPipeline)                                                                           \
    X(vkCmdBindDescriptorSets)                                                                     \
    X(vkCmdPushConstants)                                                                          \
    X(vkCmdDispatch)                                                                               \
    X(vkCmdDispatchIndirect)                                                                       \
    X(vkCreateBuffer)                                                                              \
    X(vkDestroyBuffer)                                                                             \
    X(vkGetBufferMemoryRequirements)                                                               \
    X(vkCreateImage)                                                                               \
    X(vkDestroyImage)                                                                              \
    X(vkGetImageMemoryRequirements)                                                                \
    X(vkBindImageMemory)                                                                           \
    X(vkCreateImageView)                                                                           \
    X(vkDestroyImageView)                                                                          \
    X(vkCreateSampler)                                                                             \
    X(vkDestroySampler)                                                                            \
    X(vkCreateDescriptorSetLayout)                                                                 \
    X(vkDestroyDescriptorSetLayout)                                                                \
    X(vkCreatePipelineLayout)                                                                      \
    X(vkDestroyPipelineLayout)                                                                     \
    X(vkCreateDescriptorPool)                                                                      \
    X(vkDestroyDescriptorPool)                                                                     \
    X(vkResetDescriptorPool)                                                                       \
    X(vkAllocateDescriptorSets)                                                                    \
    X(vkUpdateDescriptorSets)                                                                      \
    X(vkCreateShaderModule)                                                                        \
    X(vkDestroyShaderModule)                                                                       \
    X(vkCreateComputePipelines)                                                                    \
    X(vkDestroyPipeline)                                                                           \
    X(vkAllocateMemory)                                                                            \
    X(vkFreeMemory)                                                                                \
    X(vkMapMemory)                                                                                 \
    X(vkUnmapMemory)                                                                               \
    X(vkBindBufferMemory)

#define PKMN_VK_DECLARE(name) extern PFN_##name name;
PKMN_VK_GLOBAL_FUNCTIONS(PKMN_VK_DECLARE)
PKMN_VK_INSTANCE_FUNCTIONS(PKMN_VK_DECLARE)
PKMN_VK_DEVICE_FUNCTIONS(PKMN_VK_DECLARE)
#undef PKMN_VK_DECLARE

bool load_global(PFN_vkGetInstanceProcAddr get_instance_proc_addr);
bool load_instance(VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr);
bool load_device(VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc_addr);

} // namespace pkmn::render::vk
