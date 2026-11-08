#pragma once
// Experimental (RBX_MERGE_BARRIERS=1): skip a render pass's leading full memory
// barrier when the last thing recorded in the same command buffer was already a
// full barrier (the command-buffer start barrier or the previous pass's end
// barrier) and nothing was recorded in between.
//
// Why this is equivalent: the earlier barrier's destination scope contains the
// graphics stages of everything recorded after it, so it already chains every
// prior write to the new pass with MEMORY_READ|WRITE visibility. Any encoder
// that records commands in between (blit, compute) clears the flag, so a real
// dependency on that work still gets its barrier.
//
// State lives in a side table keyed by VkCommandBuffer (no class layout change).
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace Indium::BarrierMerge {

inline bool enabled() {
    static const bool on = [] {
        const bool value = std::getenv("RBX_MERGE_BARRIERS") != nullptr;
        if (value) std::fprintf(stderr, "Indium: RBX_MERGE_BARRIERS set: redundant render-pass barriers are skipped (experimental)\n");
        return value;
    }();
    return on;
}

struct Registry {
    std::mutex mutex;
    std::unordered_map<VkCommandBuffer, bool> lastWasFullBarrier;
    std::atomic<uint64_t> skipped{0}, kept{0};
};
inline Registry& registry() { static Registry* r = new Registry; return *r; }

// A full ALL_COMMANDS<->graphics memory barrier was just recorded.
inline void markFull(VkCommandBuffer command) {
    if (!enabled() || !command) return;
    auto& r = registry();
    std::scoped_lock lock(r.mutex);
    r.lastWasFullBarrier[command] = true;
}

// Something else (blit/compute encoder) is about to record commands.
inline void clear(VkCommandBuffer command) {
    if (!enabled() || !command) return;
    auto& r = registry();
    std::scoped_lock lock(r.mutex);
    auto found = r.lastWasFullBarrier.find(command);
    if (found != r.lastWasFullBarrier.end()) found->second = false;
}

// True when the caller may skip its leading barrier. Always resets the flag:
// the pass about to begin records work of its own.
inline bool consume(VkCommandBuffer command) {
    if (!enabled() || !command) return false;
    auto& r = registry();
    bool skip = false;
    {
        std::scoped_lock lock(r.mutex);
        auto found = r.lastWasFullBarrier.find(command);
        if (found != r.lastWasFullBarrier.end()) { skip = found->second; found->second = false; }
    }
    (skip ? r.skipped : r.kept).fetch_add(1, std::memory_order_relaxed);
    return skip;
}

// Recording finished (commit): drop the entry so recycled handles start clean.
inline void forget(VkCommandBuffer command) {
    if (!enabled() || !command) return;
    auto& r = registry();
    std::scoped_lock lock(r.mutex);
    r.lastWasFullBarrier.erase(command);
}

inline uint64_t skippedCount() { return registry().skipped.load(std::memory_order_relaxed); }
inline uint64_t keptCount() { return registry().kept.load(std::memory_order_relaxed); }

} // namespace Indium::BarrierMerge
