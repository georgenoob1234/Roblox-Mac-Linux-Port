#pragma once
#include <memory>
#include <mutex>
#include <vector>

// GL layers own imported payloads. The weak registry neither pins a layer nor
// creates a Device ownership cycle. No prebuilt Indium object layout changes.
namespace PresentationSemaphores {
struct Entry {
    std::shared_ptr<Indium::BinarySemaphore> semaphore;
    bool consumed = false; // Protected by mutex; set only after the GL fence.
};
inline std::mutex mutex;
inline std::vector<std::weak_ptr<Entry>> entries;

inline std::shared_ptr<Entry> remember(std::shared_ptr<Indium::BinarySemaphore> semaphore) {
    auto entry = std::make_shared<Entry>();
    entry->semaphore = std::move(semaphore);
    std::scoped_lock lock(mutex);
    entries.emplace_back(entry);
    return entry;
}
inline void consumed(const std::shared_ptr<Entry>& entry) {
    std::scoped_lock lock(mutex);
    entry->consumed = true;
}
inline std::shared_ptr<Indium::BinarySemaphore> acquire(const std::shared_ptr<Indium::PrivateDevice>& device) {
    std::scoped_lock lock(mutex);
    for (auto it = entries.begin(); it != entries.end();) {
        auto entry = it->lock();
        if (!entry) { it = entries.erase(it); continue; }
        ++it;
        // Both APIs must have finished: GL consumed its binary wait, and no
        // Vulkan command, texture, or queued drawable still references it.
        if (entry->consumed && entry->semaphore.use_count() == 1 && entry->semaphore->device == device) {
            entry->consumed = false;
            return entry->semaphore;
        }
    }
    return device->getWrappedBinarySemaphore(true);
}
}
