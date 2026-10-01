// Shared renderer override: memory-type selection and mapped-range cache maintenance.
#include <indium/buffer.private.hpp>
#include <indium/device.private.hpp>
#include <indium/dynamic-vk.hpp>

#include <cstring>
#include <algorithm>
#include "memory-type.h"
#include "../../runtime/profiler/renderer.h"

namespace Indium {
// Mesh streaming creates thousands of small buffers. Bind them to aligned slots
// instead of growing the driver's device-memory handle list for every mesh.
struct BufferMemoryBlock {
	VkDevice device;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void* mapped = nullptr;
	uint32_t type;
	VkDeviceSize slotSize;
	std::vector<uint32_t> freeSlots;
	size_t capacity;
	BufferMemoryBlock(VkDevice device, uint32_t type, VkDeviceSize slotSize, bool visible):
		device(device), type(type), slotSize(slotSize),
		capacity(slotSize > 1024*1024 ? 1 : std::clamp<VkDeviceSize>(4*1024*1024/slotSize, 4, 256)) {
		freeSlots.reserve(capacity);
		for (size_t i=capacity; i; --i) freeSlots.push_back(i-1);
		VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
		flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		info.pNext = &flags; info.allocationSize = slotSize*capacity; info.memoryTypeIndex = type;
		RbxProfiler::DiagnosticScope allocate(RBX_DIAG_BUFFER_ALLOCATE,info.allocationSize,this,1);
		if (DynamicVK::vkAllocateMemory(device, &info, nullptr, &memory) != VK_SUCCESS)
			throw std::runtime_error("Failed to allocate buffer memory block");
		allocate.finish();
		if (visible) {
		RbxProfiler::DiagnosticScope map(RBX_DIAG_BUFFER_MAP,info.allocationSize,this);
		if (DynamicVK::vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
			DynamicVK::vkFreeMemory(device, memory, nullptr);
			throw std::runtime_error("Failed to map buffer memory block");
		}
		}
	}
	~BufferMemoryBlock() {
		if (mapped) DynamicVK::vkUnmapMemory(device, memory);
		DynamicVK::vkFreeMemory(device, memory, nullptr);
	}
};
}

Indium::Buffer::~Buffer() {};

Indium::PrivateBuffer::PrivateBuffer(std::shared_ptr<PrivateDevice> device, size_t length, ResourceOptions options):
	_privateDevice(device),
	_length(length)
{
	RbxProfiler::DiagnosticScope create(RBX_DIAG_BUFFER_CREATE,length,this,static_cast<size_t>(options));
	_storageMode = static_cast<StorageMode>((static_cast<size_t>(options) >> 4) & 0xf);

	VkBufferCreateInfo info {};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = length;
	info.usage =
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
		VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
		VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
		;

	// TODO: maybe make this CONCURRENT instead? we already know all the queue families that can access it;
	//       that info is available in the PrivateDevice instance.
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	{
	RbxProfiler::DiagnosticScope vkCreate(RBX_DIAG_BUFFER_VK_CREATE,length,this);
	if (DynamicVK::vkCreateBuffer(_privateDevice->device(), &info, nullptr, &_buffer) != VK_SUCCESS) {
		// TODO
		abort();
	}
	}

	try {
		static DynamicVK::DynamicFunction<PFN_vkGetBufferMemoryRequirements2> getRequirements("vkGetBufferMemoryRequirements2");
		VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
		VkMemoryRequirements2 requirements2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
		requirements2.pNext = &dedicated;
		VkBufferMemoryRequirementsInfo2 request{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
		request.buffer = _buffer;
		getRequirements(_privateDevice->device(), &request, &requirements2);
		const auto& requirements = requirements2.memoryRequirements;

		const auto targetIndex = buffer_memory_type(_privateDevice->memoryProperties(),
			requirements.memoryTypeBits, _storageMode == StorageMode::Shared,
			_storageMode == StorageMode::Managed, (static_cast<size_t>(options) & 1) != 0);
		if (targetIndex == UINT32_MAX)
			throw std::runtime_error("No suitable memory region found for buffer with requested storage mode");
		_hostCoherent = (_privateDevice->memoryProperties().memoryTypes[targetIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
		const bool visible = (_privateDevice->memoryProperties().memoryTypes[targetIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
		_allocationSize = requirements.size;
		// ponytail: power-of-two slots (256-byte minimum) trade padding for cheap reuse.
		// Recycle large shared staging allocations at their exact aligned size,
		// without rounding each 12 MiB upload into a four-slot 64 MiB slab.
		// Dedicated and noncoherent allocations retain their separate paths.
		const auto required = std::max(requirements.size, requirements.alignment);
		if (!dedicated.requiresDedicatedAllocation && (!visible || _hostCoherent) &&
			(required <= 1024*1024 || (_storageMode == StorageMode::Shared && required <= 16*1024*1024))) {
			VkDeviceSize slotSize = 256;
			if (required <= 1024*1024) {
				while (slotSize < required) slotSize *= 2;
			} else slotSize = (requirements.size + requirements.alignment - 1) / requirements.alignment * requirements.alignment;
			RbxProfiler::DiagnosticScope poolLock(RBX_DIAG_BUFFER_POOL_LOCK,length,this);
			std::lock_guard lock(_privateDevice->bufferMemoryMutex);
			poolLock.finish();
			for (auto& block : _privateDevice->bufferMemoryBlocks) {
				if (block->type == targetIndex && block->slotSize == slotSize && !block->freeSlots.empty()) {
					_memoryBlock = block; break;
				}
			}
			if (!_memoryBlock) {
				_memoryBlock = std::make_shared<BufferMemoryBlock>(_privateDevice->device(), targetIndex, slotSize, visible);
				_privateDevice->bufferMemoryBlocks.push_back(_memoryBlock);
			}
			_memoryOffset = _memoryBlock->freeSlots.back()*slotSize;
			_memoryBlock->freeSlots.pop_back();
			_memory = _memoryBlock->memory;
		} else {

			VkMemoryAllocateInfo allocateInfo {};
			allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocateInfo.allocationSize = requirements.size;
			allocateInfo.memoryTypeIndex = targetIndex;

			VkMemoryAllocateFlagsInfo allocateFlags {};
			allocateFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
			allocateFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

			allocateInfo.pNext = &allocateFlags;
			VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
			if (dedicated.requiresDedicatedAllocation) {
				dedicatedInfo.buffer = _buffer;
				allocateFlags.pNext = &dedicatedInfo;
			}

			RbxProfiler::DiagnosticScope allocate(RBX_DIAG_BUFFER_ALLOCATE,allocateInfo.allocationSize,this,0);
			if (DynamicVK::vkAllocateMemory(_privateDevice->device(), &allocateInfo, nullptr, &_memory) != VK_SUCCESS)
				abort();
		}

		RbxProfiler::DiagnosticScope bind(RBX_DIAG_BUFFER_BIND,length,this);
		if (DynamicVK::vkBindBufferMemory(_privateDevice->device(), _buffer, _memory, _memoryOffset) != VK_SUCCESS)
			abort();
	} catch (...) {
		DynamicVK::vkDestroyBuffer(_privateDevice->device(), _buffer, nullptr);
		throw;
	}
};

Indium::PrivateBuffer::PrivateBuffer(std::shared_ptr<PrivateDevice> device, const void* pointer, size_t length, ResourceOptions options):
	PrivateBuffer(device, length, options)
{
	auto ptr = contents();

	if (!ptr) {
		// TODO: support non-host-visible memory
		abort();
	}

	{
		RbxProfiler::DiagnosticScope copy(RBX_DIAG_BUFFER_COPY,length,this);
		memcpy(ptr, pointer, length);
	}

	if (_storageMode == StorageMode::Managed && !_hostCoherent) {
		RbxProfiler::DiagnosticScope flush(RBX_DIAG_BUFFER_FLUSH,length,this);
		VkMappedMemoryRange range {};
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = _memory;
		range.size = VK_WHOLE_SIZE;
		range.offset = 0;
		if (DynamicVK::vkFlushMappedMemoryRanges(_privateDevice->device(), 1, &range) != VK_SUCCESS) {
			// TODO
			abort();
		}
	}
};

Indium::PrivateBuffer::~PrivateBuffer() {
	RbxProfiler::WallScope timing(RBX_PROF_BUFFER_DESTROY);
	if (_mapped && !_memoryBlock) {
		DynamicVK::vkUnmapMemory(_privateDevice->device(), _memory);
	}

	DynamicVK::vkDestroyBuffer(_privateDevice->device(), _buffer, nullptr);
	if (_memoryBlock) {
		std::lock_guard lock(_privateDevice->bufferMemoryMutex);
		_memoryBlock->freeSlots.push_back(_memoryOffset/_memoryBlock->slotSize);
		if (_memoryBlock->freeSlots.size() == _memoryBlock->capacity) {
			auto& blocks = _privateDevice->bufferMemoryBlocks;
			if (_memoryBlock->capacity == 1) {
				// ponytail: scan the bounded idle cache; add a byte counter only if
				// scanning becomes measurable. Evict old idle sizes as streaming changes.
				const auto& memory = _privateDevice->memoryProperties();
				const auto heap = memory.memoryTypes[_memoryBlock->type].heapIndex;
				const auto limit = std::min<VkDeviceSize>(memory.memoryHeaps[heap].size / 16, 2ULL*1024*1024*1024);
				VkDeviceSize idle = 0;
				for (const auto& block : blocks)
					if (block->capacity == 1 && !block->freeSlots.empty()) idle += block->slotSize;
				while (idle > limit) {
					auto old = std::find_if(blocks.begin(), blocks.end(), [](const auto& block) {
						return block->capacity == 1 && !block->freeSlots.empty();
					});
					idle -= (*old)->slotSize;
					blocks.erase(old);
				}
			} else {
				// Retain one empty block per memory type and size class, not the peak load.
				for (auto& block : blocks) {
					if (block != _memoryBlock && block->type == _memoryBlock->type &&
						block->slotSize == _memoryBlock->slotSize && block->freeSlots.size() == block->capacity) {
						blocks.erase(std::find(blocks.begin(), blocks.end(), _memoryBlock)); break;
					}
				}
			}
		}
	} else DynamicVK::vkFreeMemory(_privateDevice->device(), _memory, nullptr);
};

std::shared_ptr<Indium::Device> Indium::PrivateBuffer::device() {
	return _privateDevice;
};

size_t Indium::PrivateBuffer::length() const {
	return _length;
};

void* Indium::PrivateBuffer::contents() {
	if (_storageMode != StorageMode::Managed && _storageMode != StorageMode::Shared) {
		return nullptr;
	}

	if (!_mapped) {
		if (_memoryBlock) {
			_mapped = static_cast<char*>(_memoryBlock->mapped) + _memoryOffset;
			return _mapped;
		}
		RbxProfiler::DiagnosticScope map(RBX_DIAG_BUFFER_MAP,_allocationSize,this);
		if (DynamicVK::vkMapMemory(_privateDevice->device(), _memory, 0, VK_WHOLE_SIZE, 0, &_mapped) != VK_SUCCESS) {
			// TODO
			abort();
		}
	}

	return _mapped;
};

void Indium::PrivateBuffer::didModifyRange(Range<size_t> range) {
	if (range.start > _length || range.length > _length - range.start)
		throw std::out_of_range("Modified buffer range exceeds allocation");
	if (!range.length) return;
	if (!_mapped) throw std::runtime_error("Modified buffer is not mapped");
	// Coherent allocations need no host-cache maintenance. Submission/sync stays unchanged.
	if (_hostCoherent) return;
	RbxProfiler::DiagnosticScope flush(RBX_DIAG_BUFFER_FLUSH,range.length,this);
	const auto atom = _privateDevice->properties().limits.nonCoherentAtomSize;
	const auto start = range.start - range.start % atom;
	const auto end = range.start + range.length;
	const auto padding = (atom - end % atom) % atom;
	VkMappedMemoryRange vulkanRange {};
	vulkanRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	vulkanRange.memory = _memory;
	vulkanRange.size = end + std::min<VkDeviceSize>(padding, _allocationSize - end) - start;
	vulkanRange.offset = start;
	if (DynamicVK::vkFlushMappedMemoryRanges(_privateDevice->device(), 1, &vulkanRange) != VK_SUCCESS) {
		// TODO
		abort();
	}
};

uint64_t Indium::PrivateBuffer::gpuAddress() {
	VkBufferDeviceAddressInfo info {};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	info.buffer = _buffer;
	return DynamicVK::vkGetBufferDeviceAddress(_privateDevice->device(), &info);
};
