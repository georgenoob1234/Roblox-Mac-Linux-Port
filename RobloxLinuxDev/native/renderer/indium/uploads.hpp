#pragma once
#include <indium/device.private.hpp>
#include <array>
#include <atomic>

namespace Indium {
// All methods require PrivateDevice::queueMutex. The device owns this recorder;
// batches own only raw Vulkan resources, so pending uploads cannot cycle Device.
class UploadQueue {
    struct Batch;
    PrivateDevice& device;
    std::array<std::shared_ptr<Batch>,4> batches;
    Batch* current = nullptr;
    unsigned cursor = 0;
    Batch& begin(size_t bytes);
public:
    uint64_t copies = 0, submissions = 0, waits = 0, stagingAllocations = 0;
    // Read without queueMutex by the polling thread to skip the lock entirely
    // when nothing is staged. Only ever set true while holding queueMutex, and
    // cleared by collect() once no batch is pending, so a stale false simply
    // defers servicing to the next commit (which flushes anyway).
    std::atomic<bool> needsService{false};
    explicit UploadQueue(PrivateDevice& d): device(d) {}
    ~UploadQueue() noexcept;
    void initialize(VkImage image, VkImageSubresourceRange range, std::shared_ptr<void> lifetime);
    void copy(VkImage image, VkImageAspectFlags transitionAspect, VkBufferImageCopy region,
              const void* bytes, size_t size, std::shared_ptr<void> lifetime);
    void flush();
    void collect();
};
}
