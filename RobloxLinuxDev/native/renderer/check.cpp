#include "memory-type.h"
#include <cassert>
int main() {
    VkPhysicalDeviceMemoryProperties m{};m.memoryTypeCount=5;
    // RTX 5090 order: system, device, uncached host, cached host, visible device.
    m.memoryTypes[0].propertyFlags=0;m.memoryTypes[1].propertyFlags=1;
    m.memoryTypes[2].propertyFlags=6;m.memoryTypes[3].propertyFlags=14;
    m.memoryTypes[4].propertyFlags=7;
    assert(buffer_memory_type(m,31,false,false,false)==1);
    assert(buffer_memory_type(m,31,true,false,false)==3);
    assert(buffer_memory_type(m,31,false,true,false)==3);
    assert(buffer_memory_type(m,31,true,false,true)==2);
    assert(buffer_memory_type(m,31,false,true,true)==2);
    assert(buffer_memory_type(m,4,true,false,false)==2); // cached is a preference
    assert(buffer_memory_type(m,1,false,false,false)==0); // device-local fallback
    assert(buffer_memory_type(m,3,true,false,false)==UINT32_MAX);
    assert(drawable_memory_type(m,31,false)==1);
    assert(drawable_memory_type(m,31,true)==4);
    assert(drawable_memory_type(m,13,false)==0); // no local compatible heap
    assert(drawable_memory_type(m,13,true)==2); // retain host fallback
    assert(drawable_memory_type(m,3,true)==UINT32_MAX);
    assert(drawable_memory_type(m,0,false)==UINT32_MAX);
    m.memoryTypes[3].propertyFlags=10; // visible + cached, but not coherent
    assert(buffer_memory_type(m,31,true,false,false)==2);
    assert(buffer_memory_type(m,31,false,true,false)==3);
    assert(buffer_memory_type(m,0,false,false,false)==UINT32_MAX);
}
