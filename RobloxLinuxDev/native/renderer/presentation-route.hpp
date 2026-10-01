#pragma once
#include <cstdint>

enum class PresentationRoute { GL, Vulkan, Drop };

// The semaphore belongs to this frame's GL export, unlike the device flag,
// which can change after submission when an earlier frame starts fallback.
constexpr PresentationRoute presentationRoute(bool hasGLExport, bool vulkanActive) {
    if (hasGLExport) return PresentationRoute::GL;
    return vulkanActive ? PresentationRoute::Vulkan : PresentationRoute::Drop;
}

constexpr bool presentationSerialIsStale(uint64_t serial, uint64_t newest) {
    return serial <= newest;
}
