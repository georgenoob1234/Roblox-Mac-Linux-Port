#pragma once
// Opt-in binding counters (RBX_BIND_STATS=1). Diagnostic only: counts how
// often each draw re-sends state to the driver and how much of it repeats the
// previous push for the same stage. Prints per-second rates every 2 s.
#include <vulkan/vulkan.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace Indium::BindStats {

inline bool enabled() {
    static const bool on = std::getenv("RBX_BIND_STATS") != nullptr;
    return on;
}

enum Counter { Draws, BindCalls, PsoChanges, PipelineBinds, PushCalls, PushWrites, PushSame, PushSameLayoutChanged,
    SetLookups, SetMisses, SetBinds, VertexBinds, CounterCount };
inline const char* name(int i) {
    static const char* names[] = {"draws", "updateBindings", "pso-changes", "pipeline-binds", "push-calls", "push-writes",
        "push-identical", "push-identical-after-layout-change", "set-lookups", "set-cache-misses", "set-binds", "vertex-binds"};
    return names[i];
}
inline std::atomic<uint64_t>* counters() { static std::atomic<uint64_t> c[CounterCount]{}; return c; }

inline void maybePrint() {
    using Clock = std::chrono::steady_clock;
    static std::mutex mutex; static auto epoch = Clock::now();
    auto now = Clock::now();
    if (now - epoch < std::chrono::seconds(2)) return;
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock || now - epoch < std::chrono::seconds(2)) return;
    const double seconds = std::chrono::duration<double>(now - epoch).count();
    epoch = now;
    std::fprintf(stderr, "BIND stats window=%.3fs", seconds);
    for (int i = 0; i < CounterCount; ++i) std::fprintf(stderr, " %s=%.0f/s", name(i), counters()[i].exchange(0) / seconds);
    std::fputc('\n', stderr);
}
inline void add(Counter c, uint64_t n = 1) {
    if (!enabled()) return;
    counters()[c].fetch_add(n, std::memory_order_relaxed);
    if (c == Draws) maybePrint();
}

// Remembers the last push per stage on this thread, for this encoder.
struct LastPush {
    const void* encoder = nullptr;
    VkPipelineLayout layout[2] = {};
    std::vector<uint32_t> bindings[2];
    std::vector<VkDescriptorBufferInfo> buffers[2];
};
inline void push(const void* encoder, size_t stage, VkPipelineLayout layout, const std::vector<VkWriteDescriptorSet>& writes,
                 const std::vector<VkDescriptorBufferInfo>& buffers) {
    if (!enabled()) return;
    thread_local LastPush last;
    if (last.encoder != encoder) { last = {}; last.encoder = encoder; }
    add(PushCalls); add(PushWrites, writes.size());
    bool same = last.buffers[stage].size() == buffers.size() && last.bindings[stage].size() == writes.size();
    for (size_t i = 0; same && i < writes.size(); ++i) same = last.bindings[stage][i] == writes[i].dstBinding;
    for (size_t i = 0; same && i < buffers.size(); ++i)
        same = last.buffers[stage][i].buffer == buffers[i].buffer && last.buffers[stage][i].offset == buffers[i].offset && last.buffers[stage][i].range == buffers[i].range;
    if (same) add(last.layout[stage] == layout ? PushSame : PushSameLayoutChanged);
    last.layout[stage] = layout;
    last.bindings[stage].resize(writes.size());
    for (size_t i = 0; i < writes.size(); ++i) last.bindings[stage][i] = writes[i].dstBinding;
    last.buffers[stage] = buffers;
}

} // namespace Indium::BindStats
