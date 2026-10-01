#pragma once
#include <indium/library.private.hpp>
#include <indium/device.private.hpp>

namespace Indium {
inline bool dynamicBufferBinding(const FunctionInfo& function, const Iridium::BindingInfo& binding, const PrivateDevice& device) {
    // Split the device's aggregate limit between the two graphics stages.
    // Compute layouts keep their existing static descriptors.
    unsigned preceding = 0;
    for (const auto& other : function.bindings)
        preceding += other.type == Iridium::BindingType::Buffer && other.internalIndex < binding.internalIndex;
    return preceding < device.properties().limits.maxDescriptorSetStorageBuffersDynamic / 2;
}
}
