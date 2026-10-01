#pragma once
#include <indium/device.private.hpp>
#include <indium/dynamic-vk.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <algorithm>
#include "../../../../runtime/profiler/renderer.h"

namespace Indium {
// Opt-in wall-clock spans; GPU execution duration is reported separately below.
using CommandClock = std::chrono::steady_clock;
// F9-only attribution, independent of the command/GPU debug environment flags.
// Report per-operation samples, never deltas of counters accumulated while hidden.
struct UploadProfileScope {
    unsigned stage, parent;
    bool active;
    size_t bytes;
    CommandClock::time_point start;
    explicit UploadProfileScope(unsigned id, size_t copiedBytes = 0):
        stage(id), parent(UINT32_MAX), active(RbxProfiler::enabled()), bytes(copiedBytes) {
        if (active) { parent = RbxProfiler::cpuBegin(stage); start = CommandClock::now(); }
    }
    ~UploadProfileScope() {
        if (active && RbxProfiler::enabled()) {
            const auto end = CommandClock::now();
            RbxProfiler::cpuEnd(parent);
            RbxProfiler::metric(stage, std::chrono::duration<double,std::milli>(end-start).count());
            if (stage == RBX_PROF_UPLOAD_COPY) RbxProfiler::metric(RBX_PROF_UPLOAD_COPY_BYTES, bytes);
        } else RbxProfiler::cpuEnd(parent);
    }
};
enum class CommandSpan { Commit, QueueLock, Scheduled, Completion, Wait, Readback, PresentEnqueue, PresentDispatch,
    PreSubmit, Submit, PostSubmit, PreResources, PreUploads, PreEndEncoding, PreSync,
    SubmitWaits, SubmitSignals, SubmitReads, SubmitWrites, Count };
static_assert(static_cast<unsigned>(CommandSpan::Count)==RBX_PROF_GPU, "Profiler metric order");
inline bool commandTimingEnabled() {
    static const bool enabled = std::getenv("RBX_COMMAND_TIMING") != nullptr;
    return enabled || RbxProfiler::enabled();
}
inline CommandClock::time_point commandTimingStart() {
    return commandTimingEnabled() ? CommandClock::now() : CommandClock::time_point{};
}
inline void reportCommandMetric(CommandSpan span, double value) {
    if (!commandTimingEnabled()) return;
    if (RbxProfiler::enabled()) RbxProfiler::metric(static_cast<unsigned>(span), value);
    if (!std::getenv("RBX_COMMAND_TIMING")) return;
    auto now = CommandClock::now();
    static std::mutex mutex;
    static auto epoch = now;
    static std::array<std::vector<double>,static_cast<size_t>(CommandSpan::Count)> samples;
    static const char* names[] = {"commit-wall", "queue-lock-wait", "submit-to-scheduled", "submit-to-completion-callback", "waitUntilCompleted", "readback-wait", "present-enqueue", "present-dispatch-delay",
        "commit-pre-submit", "vkQueueSubmit2", "commit-post-submit", "pre-resources", "pre-uploads", "pre-end-command", "pre-sync-setup",
        "submit-waits", "submit-signals", "submit-read-images", "submit-write-images"};
    std::scoped_lock lock(mutex);
    samples[static_cast<size_t>(span)].push_back(value);
    if (now-epoch < std::chrono::seconds(2)) return;
    const double seconds = std::chrono::duration<double>(now-epoch).count();
    for (size_t i=0;i<samples.size();++i) {
        auto& values=samples[i]; if (values.empty()) continue;
        double sum=0; for(auto value:values) sum+=value;
        std::sort(values.begin(),values.end());
        const char* unit=i>=static_cast<size_t>(CommandSpan::SubmitWaits)?"items":"ms";
        std::fprintf(stderr,"COMMAND timing phase=%s window=%.3fs count=%zu avg=%.3f%s p50=%.3f%s p99=%.3f%s max=%.3f%s\n",
            names[i],seconds,values.size(),sum/values.size(),unit,values[(values.size()-1)/2],unit,values[(values.size()*99+99)/100-1],unit,values.back(),unit);
        values.clear();
    }
    epoch=now;
}
inline void reportCommandTiming(CommandSpan span, CommandClock::time_point start) {
    if (start != CommandClock::time_point{} && commandTimingEnabled())
        reportCommandMetric(span,std::chrono::duration<double,std::milli>(CommandClock::now()-start).count());
}
inline void destroyTimestampPool(VkDevice device, VkQueryPool pool) {
    static DynamicVK::DynamicFunction<PFN_vkDestroyQueryPool> destroy("vkDestroyQueryPool");
    destroy(device, pool, nullptr);
}
inline bool gpuTimingEnabled() {
    // Explicit debug timestamps remain independent of the F7 profiler.
    return std::getenv("RBX_GPU_TIMESTAMPS") || RbxProfiler::enabled();
}
inline VkQueryPool beginGpuTiming(PrivateDevice& device, VkCommandBuffer command) {
    if (!gpuTimingEnabled() || !device.graphicsTimestampBits) return VK_NULL_HANDLE;
    VkQueryPool pool = VK_NULL_HANDLE;
    {
        std::scoped_lock lock(device.rendererCacheMutex);
        if (!device.timestampPools.empty()) { pool = device.timestampPools.back(); device.timestampPools.pop_back(); }
    }
    if (!pool) {
        static DynamicVK::DynamicFunction<PFN_vkCreateQueryPool> create("vkCreateQueryPool");
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP; info.queryCount = 2;
        if (create(device.device(), &info, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    static DynamicVK::DynamicFunction<PFN_vkCmdResetQueryPool> reset("vkCmdResetQueryPool");
    static DynamicVK::DynamicFunction<PFN_vkCmdWriteTimestamp> write("vkCmdWriteTimestamp");
    reset(command, pool, 0, 2);
    write(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
    return pool;
}
inline void endGpuTiming(VkCommandBuffer command, VkQueryPool pool) {
    if (!pool || !gpuTimingEnabled()) return;
    static DynamicVK::DynamicFunction<PFN_vkCmdWriteTimestamp> write("vkCmdWriteTimestamp");
    write(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 1);
}
inline void reportGpuTiming(PrivateDevice& device, VkQueryPool pool) {
    if (!pool || !gpuTimingEnabled()) return;
    static DynamicVK::DynamicFunction<PFN_vkGetQueryPoolResults> results("vkGetQueryPoolResults");
    uint64_t data[4]{};
    // Called from the existing completion callback. Never add WAIT_BIT or wait
    // for a query solely to collect profiling data.
    auto result = results(device.device(), pool, 0, 2, sizeof(data), data, 2 * sizeof(uint64_t),
                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS || !data[1] || !data[3]) return;
    uint64_t mask = device.graphicsTimestampBits >= 64 ? UINT64_MAX : (uint64_t(1) << device.graphicsTimestampBits) - 1;
    double ms = ((data[2] - data[0]) & mask) * device.properties().limits.timestampPeriod / 1e6;
    if (RbxProfiler::enabled()) RbxProfiler::metric(RBX_PROF_GPU, ms);
    if (!std::getenv("RBX_GPU_TIMESTAMPS")) return;
    using Clock = std::chrono::steady_clock;
    static std::mutex mutex;
    static auto epoch = Clock::now();
    static unsigned count = 0;
    static double total = 0, maximum = 0;
    std::scoped_lock lock(mutex);
    ++count; total += ms; maximum = std::max(maximum, ms);
    auto now = Clock::now();
    if (now - epoch >= std::chrono::seconds(2)) {
        std::fprintf(stderr, "GPU timestamps commands=%u avg=%.3fms max=%.3fms total=%.3fms\n", count, total / count, maximum, total);
        epoch = now; count = 0; total = maximum = 0;
    }
}
}
