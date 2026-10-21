// SPDX-License-Identifier: GPL-2.0-or-later
// Tests that need a Vulkan device skip on hosts without a driver (build containers). Vulkan::Instance
// asserts there, so the loader is asked first; any other Vulkan failure still fails the test.
#pragma once
#include <cstdio>
#include "video_core/renderer_vulkan/vk_common.h"

// Prints the skip and returns true when there is no loader, no driver or no device to create.
inline bool SkipWithoutVulkanDriver() {
    const auto missing = [](VkResult result) {
        return result == VK_ERROR_INCOMPATIBLE_DRIVER || result == VK_ERROR_INITIALIZATION_FAILED;
    };
    static vk::detail::DynamicLoader loader;
    const auto get_proc = loader.success()
        ? loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr")
        : nullptr;
    bool skip = !get_proc;
    if (!skip) {
        vk::detail::DispatchLoaderDynamic d;
        d.init(get_proc);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance_info.pApplicationInfo = &app;
        VkInstance instance{};
        const VkResult created = d.vkCreateInstance(&instance_info, nullptr, &instance);
        skip = missing(created);
        if (created == VK_SUCCESS) {
            d.init(vk::Instance{instance});
            VkPhysicalDevice physical{};
            uint32_t count = 1;
            const VkResult listed = d.vkEnumeratePhysicalDevices(instance, &count, &physical);
            skip = missing(listed) || ((listed == VK_SUCCESS || listed == VK_INCOMPLETE) && !count);
            if (!skip && count) {
                const float priority = 1;
                VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                queue_info.queueCount = 1;
                queue_info.pQueuePriorities = &priority;
                VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                device_info.queueCreateInfoCount = 1;
                device_info.pQueueCreateInfos = &queue_info;
                VkDevice device{};
                const VkResult device_created =
                    d.vkCreateDevice(physical, &device_info, nullptr, &device);
                skip = missing(device_created);
                if (device_created == VK_SUCCESS) {
                    d.vkDestroyDevice(device, nullptr);
                }
            }
            d.vkDestroyInstance(instance, nullptr);
        }
    }
    if (skip) {
        std::puts("SKIP: no Vulkan driver");
    }
    return skip;
}
