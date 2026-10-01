#include <Metal/MTLDeviceInternal.h>
#include <QuartzCore/CAMetalLayer.h>
#include <indium/device.private.hpp>
#include <indium/dynamic-vk.hpp>
#include <objc/runtime.h>
#include "memory-type.h"

namespace {
struct DrawableMemory {
    VkDevice device;
    VkPhysicalDeviceMemoryProperties memory;
    bool hostVisible;
};
thread_local const DrawableMemory* creating = nullptr;
PFN_vkGetImageMemoryRequirements originalRequirements;
void (*originalRecreate)(id, SEL);

void requirements(VkDevice device, VkImage image, VkMemoryRequirements* result) {
    originalRequirements(device, image, result);
    if (!creating || creating->device != device) return;
    // Cocotron's prebuilt constructor picks the first compatible type. Restrict
    // its choice to the preferred supported heap, only while creating drawables.
    auto type = drawable_memory_type(creating->memory, result->memoryTypeBits, creating->hostVisible);
    if (type != UINT32_MAX) result->memoryTypeBits = 1u << type;
}

void recreate(id self, SEL selector) {
    auto device = std::dynamic_pointer_cast<Indium::PrivateDevice>([(MTLDeviceInternal*)[(CAMetalLayer*)self device] device]);
    DrawableMemory context{device ? device->device() : VK_NULL_HANDLE,
        device ? device->memoryProperties() : VkPhysicalDeviceMemoryProperties{}, ![(CAMetalLayer*)self framebufferOnly]};
    struct Restore {
        const DrawableMemory* previous = creating;
        ~Restore() { creating = previous; }
    } restore;
    creating = device ? &context : nullptr;
    originalRecreate(self, selector);
}
}

extern "C" int trackb_install_drawable_memory() {
    if (originalRecreate) return 1;
    Method method = class_getInstanceMethod(objc_getClass("CAMetalLayerInternal"), sel_registerName("recreateDrawables"));
    auto& query = Indium::DynamicVK::vkGetImageMemoryRequirements;
    if (!method || !query.resolve()) return 0;
    originalRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(query.pointer);
    query.pointer = reinterpret_cast<void*>(&requirements);
    originalRecreate = reinterpret_cast<decltype(originalRecreate)>(method_setImplementation(method, reinterpret_cast<IMP>(&recreate)));
    return 1;
}
