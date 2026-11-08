#pragma once
// Opt-in per-render-pass GPU timing (RBX_GPU_PASS_TIMING=1). Diagnostic only.
//
// State lives in a side table keyed by VkCommandBuffer so no Indium class
// layout changes (prebuilt Darling code depends on the existing offsets).
// When the variable is unset every entry point returns after one static bool.
//
// Each timed pass gets two BOTTOM_OF_PIPE timestamps: "begin" lands once all
// earlier work in the command buffer (including the barrier in front of the
// pass) has finished, "end" once the pass itself has finished. The gap between
// one pass's end and the next pass's begin is barrier/idle/non-render work
// (blits, compute) inside the same command buffer.
#include <indium/gpu-timing.hpp>
#include <indium/barrier-merge.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Indium::PassTiming {

inline bool enabled() {
    static const bool on = std::getenv("RBX_GPU_PASS_TIMING") != nullptr;
    return on;
}

constexpr uint32_t kQueries = 256; // passes + encoder boundaries per command buffer

struct Pass { std::string key; uint32_t draws = 0; uint32_t query = UINT32_MAX; };
struct CommandState {
    VkQueryPool pool = VK_NULL_HANDLE;
    uint32_t used = 0;
    int open = -1;
    uint32_t untimed = 0;
    std::vector<Pass> passes;
    // Encoder boundaries: kind ('R' render, 'B' blit, 'C' compute, 'E' end) and query.
    std::vector<std::pair<char, uint32_t>> segments;
};
enum SegmentKind { SegRender, SegBlit, SegCompute, SegKinds };
struct Stat { uint64_t count = 0; double total = 0, maximum = 0; uint64_t draws = 0; };

struct Registry {
    std::mutex mutex;
    std::unordered_map<VkCommandBuffer, CommandState> commands;
    std::vector<VkQueryPool> freePools;
    // Aggregation window
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    std::unordered_map<std::string, Stat> stats;
    uint64_t commandBuffers = 0, timedCommandBuffers = 0, untimedPasses = 0, unavailable = 0;
    double gapTotal = 0, passTotal = 0;
    double segmentTotal[SegKinds] = {}; uint64_t segmentCount[SegKinds] = {};
    double renderSegmentPassTotal = 0; // pass time inside fully timed render segments
};
inline Registry& registry() { static Registry* r = new Registry; return *r; }

inline const char* formatName(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: return "R8";
    case VK_FORMAT_R8G8_UNORM: return "RG8";
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case VK_FORMAT_B8G8R8A8_SRGB: return "BGRA8_SRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "RGB10A2";
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "BGR10A2";
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "RG11B10F";
    case VK_FORMAT_R16_SFLOAT: return "R16F";
    case VK_FORMAT_R16G16_SFLOAT: return "RG16F";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
    case VK_FORMAT_R32_SFLOAT: return "R32F";
    case VK_FORMAT_R32G32_SFLOAT: return "RG32F";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "RGBA32F";
    case VK_FORMAT_D16_UNORM: return "D16";
    case VK_FORMAT_D32_SFLOAT: return "D32";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32S8";
    case VK_FORMAT_S8_UINT: return "S8";
    default: return nullptr;
    }
}
inline std::string formatLabel(VkFormat f) {
    if (auto name = formatName(f)) return name;
    return "fmt" + std::to_string(static_cast<int>(f));
}

inline VkQueryPool takePool(PrivateDevice& device, Registry& r) {
    if (!r.freePools.empty()) { auto pool = r.freePools.back(); r.freePools.pop_back(); return pool; }
    static DynamicVK::DynamicFunction<PFN_vkCreateQueryPool> create("vkCreateQueryPool");
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP; info.queryCount = kQueries;
    VkQueryPool pool = VK_NULL_HANDLE;
    if (create(device.device(), &info, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pool;
}
inline void releasePool(Registry& r, VkQueryPool pool) { if (pool) r.freePools.push_back(pool); }

// After vkBeginCommandBuffer, outside any render pass.
inline void beginCommand(PrivateDevice& device, VkCommandBuffer command) {
    if (!enabled() || !device.graphicsTimestampBits || !command) return;
    auto& r = registry();
    std::scoped_lock lock(r.mutex);
    auto& state = r.commands[command];
    // A recycled handle whose previous owner never completed (never committed).
    if (!state.pool) state.pool = takePool(device, r);
    state.used = 0; state.open = -1; state.untimed = 0; state.passes.clear(); state.segments.clear();
    if (!state.pool) return;
    static DynamicVK::DynamicFunction<PFN_vkCmdResetQueryPool> reset("vkCmdResetQueryPool");
    reset(command, state.pool, 0, kQueries);
}

inline void writeTimestamp(VkCommandBuffer command, VkQueryPool pool, uint32_t query) {
    static DynamicVK::DynamicFunction<PFN_vkCmdWriteTimestamp> write("vkCmdWriteTimestamp");
    write(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, query);
}

// Before an encoder records anything ('R','B','C') and at commit ('E').
inline void boundary(VkCommandBuffer command, char kind) {
    if (!enabled()) return;
    auto& r = registry();
    std::scoped_lock lock(r.mutex);
    auto found = r.commands.find(command);
    if (found == r.commands.end() || !found->second.pool) return;
    auto& state = found->second;
    if (state.used + 1 > kQueries) return;
    const uint32_t query = state.used++;
    writeTimestamp(command, state.pool, query);
    state.segments.emplace_back(kind, query);
}

// Outside dynamic rendering, after the pass's leading barrier.
inline void passBegin(VkCommandBuffer command, std::string key) {
    if (!enabled()) return;
    auto& r = registry();
    std::scoped_lock lock(r.mutex);
    auto found = r.commands.find(command);
    if (found == r.commands.end() || !found->second.pool) return;
    auto& state = found->second;
    Pass pass; pass.key = std::move(key);
    if (state.used + 2 <= kQueries) {
        pass.query = state.used; state.used += 2;
        writeTimestamp(command, state.pool, pass.query);
    } else ++state.untimed;
    state.passes.push_back(std::move(pass));
    state.open = static_cast<int>(state.passes.size()) - 1;
}

inline void draw(VkCommandBuffer command) {
    if (!enabled()) return;
    auto& r = registry();
    std::scoped_lock lock(r.mutex);
    auto found = r.commands.find(command);
    if (found == r.commands.end() || found->second.open < 0) return;
    ++found->second.passes[found->second.open].draws;
}

// After vkCmdEndRendering, before the trailing barrier.
inline void passEnd(VkCommandBuffer command) {
    if (!enabled()) return;
    auto& r = registry();
    std::scoped_lock lock(r.mutex);
    auto found = r.commands.find(command);
    if (found == r.commands.end() || found->second.open < 0) return;
    auto& state = found->second;
    const auto& pass = state.passes[state.open];
    if (pass.query != UINT32_MAX) writeTimestamp(command, state.pool, pass.query + 1);
    state.open = -1;
}

inline void printWindow(Registry& r, double seconds) {
    std::vector<std::pair<std::string, Stat>> rows(r.stats.begin(), r.stats.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.total > b.second.total; });
    std::fprintf(stderr, "GPU segments window=%.3fs render=%.3fms(n=%llu, of which passes %.3fms) blit=%.3fms(n=%llu) compute=%.3fms(n=%llu) barriers-skipped=%llu barriers-kept=%llu\n",
        seconds, r.segmentTotal[SegRender], (unsigned long long)r.segmentCount[SegRender], r.renderSegmentPassTotal,
        r.segmentTotal[SegBlit], (unsigned long long)r.segmentCount[SegBlit], r.segmentTotal[SegCompute], (unsigned long long)r.segmentCount[SegCompute],
        (unsigned long long)BarrierMerge::skippedCount(), (unsigned long long)BarrierMerge::keptCount());
    for (int i = 0; i < SegKinds; ++i) { r.segmentTotal[i] = 0; r.segmentCount[i] = 0; }
    r.renderSegmentPassTotal = 0;
    std::fprintf(stderr, "GPU passes window=%.3fs cmdbufs=%llu timed-cmdbufs=%llu pass-total=%.3fms between-passes=%.3fms untimed-passes=%llu unavailable=%llu kinds=%zu\n",
        seconds, (unsigned long long)r.commandBuffers, (unsigned long long)r.timedCommandBuffers, r.passTotal, r.gapTotal,
        (unsigned long long)r.untimedPasses, (unsigned long long)r.unavailable, rows.size());
    size_t shown = 0;
    for (const auto& [key, s] : rows) {
        if (++shown > 15) break;
        std::fprintf(stderr, "GPU pass total=%.3fms count=%llu avg=%.3fms max=%.3fms draws/pass=%.1f %s\n",
            s.total, (unsigned long long)s.count, s.total / s.count, s.maximum, double(s.draws) / s.count, key.c_str());
    }
    r.stats.clear();
    r.commandBuffers = r.timedCommandBuffers = r.untimedPasses = r.unavailable = 0;
    r.gapTotal = r.passTotal = 0;
}

// From the completion callback, before the command pool is recycled.
// Never waits: results are complete once the command buffer has completed.
inline void complete(PrivateDevice& device, VkCommandBuffer command) {
    if (!enabled() || !command) return;
    auto& r = registry();
    std::scoped_lock lock(r.mutex);
    auto found = r.commands.find(command);
    if (found == r.commands.end()) return;
    CommandState state = std::move(found->second);
    r.commands.erase(found);
    ++r.commandBuffers;
    r.untimedPasses += state.untimed;
    if (state.pool && state.used) {
        static DynamicVK::DynamicFunction<PFN_vkGetQueryPoolResults> results("vkGetQueryPoolResults");
        std::vector<uint64_t> data(state.used * 2);
        auto result = results(device.device(), state.pool, 0, state.used, data.size() * sizeof(uint64_t), data.data(),
            2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result == VK_SUCCESS || result == VK_NOT_READY) {
            const uint64_t mask = device.graphicsTimestampBits >= 64 ? UINT64_MAX : (uint64_t(1) << device.graphicsTimestampBits) - 1;
            const double period = device.properties().limits.timestampPeriod / 1e6;
            bool timed = false;
            uint64_t previousEnd = 0; bool havePrevious = false;
            for (const auto& pass : state.passes) {
                if (pass.query == UINT32_MAX) continue;
                const uint64_t begin = data[pass.query * 2], beginOk = data[pass.query * 2 + 1];
                const uint64_t end = data[(pass.query + 1) * 2], endOk = data[(pass.query + 1) * 2 + 1];
                if (!beginOk || !endOk) { ++r.unavailable; havePrevious = false; continue; }
                const double ms = ((end - begin) & mask) * period;
                auto& s = r.stats[pass.key];
                ++s.count; s.total += ms; s.maximum = std::max(s.maximum, ms); s.draws += pass.draws;
                r.passTotal += ms;
                if (havePrevious) r.gapTotal += ((begin - previousEnd) & mask) * period;
                previousEnd = end; havePrevious = true; timed = true;
            }
            if (timed) ++r.timedCommandBuffers;
            // Segment i runs from its boundary to the next one. Render segments
            // include the pass's leading/trailing barriers; the pass part is
            // subtracted using the pass that starts inside the segment.
            size_t passCursor = 0;
            for (size_t i = 0; i + 1 < state.segments.size(); ++i) {
                const auto [kind, query] = state.segments[i];
                const uint32_t next = state.segments[i + 1].second;
                if (!data[query * 2 + 1] || !data[next * 2 + 1]) continue;
                const double ms = ((data[next * 2] - data[query * 2]) & mask) * period;
                const int k = kind == 'R' ? SegRender : kind == 'B' ? SegBlit : kind == 'C' ? SegCompute : -1;
                if (k < 0) continue;
                r.segmentTotal[k] += ms; ++r.segmentCount[k];
                if (k == SegRender) {
                    // Passes are recorded in encoder order; match the next timed pass.
                    while (passCursor < state.passes.size() && state.passes[passCursor].query == UINT32_MAX) ++passCursor;
                    if (passCursor < state.passes.size()) {
                        const auto& pass = state.passes[passCursor++];
                        if (pass.query > query && pass.query < next && data[pass.query * 2 + 1] && data[(pass.query + 1) * 2 + 1])
                            r.renderSegmentPassTotal += ((data[(pass.query + 1) * 2] - data[pass.query * 2]) & mask) * period;
                    }
                }
            }
        }
    }
    releasePool(r, state.pool);
    const auto now = std::chrono::steady_clock::now();
    if (now - r.epoch >= std::chrono::seconds(2)) {
        printWindow(r, std::chrono::duration<double>(now - r.epoch).count());
        r.epoch = now;
    }
}

} // namespace Indium::PassTiming
