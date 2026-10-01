#pragma once
#include <indium/device.private.hpp>
#include <indium/dynamic-vk.hpp>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace Indium {
// Every texture used to take its own vkAllocateMemory. The driver rounds each
// allocation up to its own granularity (2 MiB on the NVIDIA build measured
// here), so a scene with thousands of small textures paid multiples of their
// real size: 2116 live allocations held 4.2 GiB. Suballocate small images from
// shared blocks the way mesh buffers already are.
//
// The free list is guarded per block rather than by the device, because a
// texture's memory is released from a lifetime deleter that deliberately does
// not retain the device (that would form an ownership cycle with pending
// uploads). Holding the block alone is enough to return a slot.
struct ImageMemoryBlock {
	VkDevice device;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	uint32_t type;
	VkDeviceSize slotSize;
	size_t capacity;
	std::mutex mutex;
	std::vector<uint32_t> freeSlots;
	ImageMemoryBlock(VkDevice device, uint32_t type, VkDeviceSize slotSize):
		device(device), type(type), slotSize(slotSize),
		capacity(std::clamp<VkDeviceSize>(16*1024*1024/slotSize, 4, 256)) {
		freeSlots.reserve(capacity);
		for (size_t i=capacity; i; --i) freeSlots.push_back(i-1);
		VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		info.allocationSize = slotSize*capacity; info.memoryTypeIndex = type;
		if (DynamicVK::vkAllocateMemory(device, &info, nullptr, &memory) != VK_SUCCESS)
			throw std::runtime_error("Failed to allocate image memory block");
	}
	~ImageMemoryBlock() { if (memory) DynamicVK::vkFreeMemory(device, memory, nullptr); }
	// Returns SIZE_MAX when the block is full.
	VkDeviceSize take() {
		std::lock_guard lock(mutex);
		if (freeSlots.empty()) return VkDeviceSize(-1);
		auto slot = freeSlots.back(); freeSlots.pop_back();
		return VkDeviceSize(slot)*slotSize;
	}
	void give(VkDeviceSize offset) {
		std::lock_guard lock(mutex);
		freeSlots.push_back(uint32_t(offset/slotSize));
	}
};
}
