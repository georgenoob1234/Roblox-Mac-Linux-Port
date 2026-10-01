#pragma once
#include <vulkan/vulkan.h>
#include <stdint.h>

static uint32_t buffer_memory_type(const VkPhysicalDeviceMemoryProperties& memory,
        uint32_t bits, bool shared, bool managed, bool write_combined) {
    VkMemoryPropertyFlags required = (shared || managed) ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0;
    if (shared) required |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryPropertyFlags preferred = (shared || managed)
        ? (write_combined ? 0 : VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t first = UINT32_MAX;
    for (uint32_t i=0; i<memory.memoryTypeCount; ++i) {
        auto flags=memory.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (flags & required)!=required) continue;
        if (first==UINT32_MAX) first=i;
        if ((flags & preferred)==preferred) return i;
    }
    return first;
}

// Unlike mapped buffers, drawable images favor video memory even when the
// caller also requires host visibility. Preserve a compatible fallback.
static uint32_t drawable_memory_type(const VkPhysicalDeviceMemoryProperties& memory,
        uint32_t bits, bool host_visible) {
    auto required = host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0;
    uint32_t first = UINT32_MAX;
    for (uint32_t i=0; i<memory.memoryTypeCount; ++i) {
        auto flags=memory.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (flags & required)!=required) continue;
        if (first==UINT32_MAX) first=i;
        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) return i;
    }
    return first;
}
