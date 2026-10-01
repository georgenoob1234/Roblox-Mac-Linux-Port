#include <indium/uploads.hpp>
#include <indium/dynamic-vk.hpp>
#include <indium/gpu-timing.hpp>
#include <cstring>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include "memory-type.h"

using namespace Indium;
namespace {
void checked(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan texture upload failed: " + std::to_string(result));
}
}
struct UploadQueue::Batch {
    VkDevice device;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    size_t capacity = 0, used = 0, operations = 0;
    bool pending = false;
    std::vector<std::shared_ptr<void>> resources;
    explicit Batch(VkDevice d): device(d) {}
    void freeStaging() {
        if (mapped) DynamicVK::vkUnmapMemory(device, memory);
        if (staging) DynamicVK::vkDestroyBuffer(device, staging, nullptr);
        if (memory) DynamicVK::vkFreeMemory(device, memory, nullptr);
        staging = VK_NULL_HANDLE; memory = VK_NULL_HANDLE; mapped = nullptr; capacity = 0;
    }
    ~Batch() {
        freeStaging();
        if (fence) DynamicVK::vkDestroyFence(device, fence, nullptr);
        if (pool) DynamicVK::vkDestroyCommandPool(device, pool, nullptr);
    }
};

UploadQueue& PrivateDevice::uploadQueue() {
    if (!uploads) uploads = std::make_shared<UploadQueue>(*this);
    return *uploads;
}

UploadQueue::Batch& UploadQueue::begin(size_t bytes) {
    const size_t alignment = std::max<VkDeviceSize>(16, device.properties().limits.optimalBufferCopyOffsetAlignment);
    if (current) {
        const auto offset = (current->used + alignment - 1) & ~(alignment - 1);
        if (offset <= current->capacity && bytes <= current->capacity - offset && current->operations < 256) {
            current->used = offset;
            return *current;
        }
        flush();
    }
    auto& slot = batches[cursor++ % batches.size()];
    if (!slot) {
        slot = std::make_shared<Batch>(device.device());
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = *device.graphicsQueueFamilyIndex();
        checked(DynamicVK::vkCreateCommandPool(device.device(), &pool, nullptr, &slot->pool));
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = slot->pool; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 1;
        checked(DynamicVK::vkAllocateCommandBuffers(device.device(), &alloc, &slot->command));
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        checked(DynamicVK::vkCreateFence(device.device(), &fence, nullptr, &slot->fence));
    }
    auto& batch = *slot;
    if (batch.pending) {
        static DynamicVK::DynamicFunction<PFN_vkGetFenceStatus> status("vkGetFenceStatus");
        auto result = status(device.device(), batch.fence);
        if (result == VK_NOT_READY) {
            // ponytail: four batches bound staging memory; wait only under upload
            // backpressure. Increase the ring only if measured stalls justify it.
            ++waits;
            UploadProfileScope timing(RBX_PROF_UPLOAD_FENCE_WAIT);
            checked(DynamicVK::vkWaitForFences(device.device(), 1, &batch.fence, VK_TRUE, UINT64_MAX));
        } else checked(result);
        batch.pending = false;
        batch.resources.clear();
    }
    if (!batch.staging || bytes > batch.capacity) {
        UploadProfileScope timing(RBX_PROF_UPLOAD_ALLOC);
        batch.freeStaging();
        const size_t capacity = std::max<size_t>(4 * 1024 * 1024, bytes);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = capacity; info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        checked(DynamicVK::vkCreateBuffer(device.device(), &info, nullptr, &batch.staging));
        VkMemoryRequirements req{};
        DynamicVK::vkGetBufferMemoryRequirements(device.device(), batch.staging, &req);
        auto index = buffer_memory_type(device.memoryProperties(), req.memoryTypeBits, true, false, false);
        if (index == UINT32_MAX) throw std::runtime_error("No coherent texture staging memory");
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size; alloc.memoryTypeIndex = index;
        checked(DynamicVK::vkAllocateMemory(device.device(), &alloc, nullptr, &batch.memory));
        checked(DynamicVK::vkBindBufferMemory(device.device(), batch.staging, batch.memory, 0));
        checked(DynamicVK::vkMapMemory(device.device(), batch.memory, 0, VK_WHOLE_SIZE, 0, &batch.mapped));
        batch.capacity = capacity;
        ++stagingAllocations;
    }
    checked(DynamicVK::vkResetCommandPool(device.device(), batch.pool, 0));
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checked(DynamicVK::vkBeginCommandBuffer(batch.command, &info));
    batch.used = batch.operations = 0;
    current = &batch;
    return batch;
}

void UploadQueue::initialize(VkImage image, VkImageSubresourceRange range, std::shared_ptr<void> lifetime) {
    auto& batch = begin(0);
    batch.resources.push_back(std::move(lifetime));
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = range;
    DynamicVK::vkCmdPipelineBarrier(batch.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   0, 0, nullptr, 0, nullptr, 1, &barrier);
    ++batch.operations;
    needsService.store(true, std::memory_order_relaxed);
}

void UploadQueue::copy(VkImage image, VkImageAspectFlags aspect, VkBufferImageCopy region,
                       const void* bytes, size_t size, std::shared_ptr<void> lifetime) {
    auto& batch = begin(size);
    batch.resources.push_back(std::move(lifetime));
    {
        UploadProfileScope timing(RBX_PROF_UPLOAD_COPY, size);
        std::memcpy(static_cast<char*>(batch.mapped) + batch.used, bytes, size);
    }
    region.bufferOffset = batch.used;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, region.imageSubresource.mipLevel, 1, region.imageSubresource.baseArrayLayer, region.imageSubresource.layerCount};
    // These scopes include earlier and later submissions on the same graphics
    // queue: uploads wait for prior sampling/readback and publish to future use.
    DynamicVK::vkCmdPipelineBarrier(batch.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, 0, nullptr, 0, nullptr, 1, &barrier);
    DynamicVK::vkCmdCopyBufferToImage(batch.command, batch.staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    DynamicVK::vkCmdPipelineBarrier(batch.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   0, 0, nullptr, 0, nullptr, 1, &barrier);
    batch.used += size; ++batch.operations; ++copies;
    needsService.store(true, std::memory_order_relaxed);
}

void UploadQueue::flush() {
    if (!current) return;
    checked(DynamicVK::vkEndCommandBuffer(current->command));
    static DynamicVK::DynamicFunction<PFN_vkResetFences> reset("vkResetFences");
    checked(reset(device.device(), 1, &current->fence));
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 1; info.pCommandBuffers = &current->command;
    {
        UploadProfileScope timing(RBX_PROF_UPLOAD_SUBMIT);
        checked(DynamicVK::vkQueueSubmit(device.graphicsQueue(), 1, &info, current->fence));
    }
    current->pending = true; current = nullptr; ++submissions;
    needsService.store(true, std::memory_order_relaxed);
}

void UploadQueue::collect() {
    static DynamicVK::DynamicFunction<PFN_vkGetFenceStatus> status("vkGetFenceStatus");
    for (auto& batch : batches) if (batch && batch->pending) {
        auto result = status(device.device(), batch->fence);
        if (result == VK_NOT_READY) continue;
        checked(result);
        batch->pending = false; batch->resources.clear();
        if (batch->capacity > 4 * 1024 * 1024) batch->freeStaging();
    }
    bool outstanding = current != nullptr;
    for (auto& batch : batches) if (batch && batch->pending) outstanding = true;
    if (!outstanding) needsService.store(false, std::memory_order_relaxed);
}

UploadQueue::~UploadQueue() noexcept {
    // No texture/consumer can still own Device here. Discard unsubmitted work;
    // submitted batches must finish before their raw image/staging owners die.
    try {
        for (auto& batch : batches) if (batch && batch->pending) {
            UploadProfileScope timing(RBX_PROF_UPLOAD_FENCE_WAIT);
            if (DynamicVK::vkWaitForFences(device.device(), 1, &batch->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
                std::abort();
        }
    } catch (...) {
        std::fputs("Texture upload teardown failed\n", stderr);
        std::abort();
    }
}
