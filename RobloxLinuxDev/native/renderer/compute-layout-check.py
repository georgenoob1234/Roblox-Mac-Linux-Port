#!/usr/bin/env python3
"""Exercise the real descriptor-layout builder without a display or GPU."""
from pathlib import Path
import subprocess
import tempfile
import sys

here = Path(__file__).resolve().parent
encoder = (here / '../../runtime/src/indium/src/indium/compute-command-encoder.cpp').read_text()
assert 'if (pushesComputeSet(info, *_privateDevice))' in encoder
assert 'vkCmdPushDescriptorSetKHR(buf->commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE' in encoder
source = (Path(sys.argv[1]) if len(sys.argv) > 1 else here / 'indium/pipeline.private.hpp').read_text()
# Compile the real layout helpers, intercepting only their Vulkan device call.
helpers = source.split('namespace Indium {', 1)[1].split('\ttemplate<size_t count>', 1)[0]
fixture = r'''
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <vector>
#include <cassert>
#include <stdexcept>
namespace Iridium {
enum class BindingType { Buffer, Texture, Sampler };
enum class TextureAccessType { Sample, Write };
struct BindingInfo { BindingType type; size_t index, internalIndex; TextureAccessType textureAccessType; };
}
namespace Indium {
enum class FunctionType { Vertex, Fragment, Kernel };
struct FunctionInfo { FunctionType functionType; std::vector<Iridium::BindingInfo> bindings; };
struct PrivateDevice {
    bool pushDescriptors; size_t maxPushDescriptors=32;
    VkDevice device() { return VK_NULL_HANDLE; }
};
bool dynamicBufferBinding(const FunctionInfo&, const Iridium::BindingInfo&, const PrivateDevice&) { return false; }
VkShaderStageFlags functionTypeToVkShaderStageFlags(FunctionType type) {
    return type==FunctionType::Kernel ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_VERTEX_BIT;
}
VkDescriptorSetLayoutCreateFlags expectedFlags;
namespace DynamicVK {
VkResult vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo* info,
                                    const VkAllocationCallbacks*, VkDescriptorSetLayout*) {
    // Layout and encoder must agree: pushed sets when supported, pool otherwise.
    assert(info->flags == expectedFlags);
    assert(info->bindingCount == 3);
    assert(info->pBindings[0].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    assert(info->pBindings[1].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    assert(info->pBindings[2].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    assert(info->pBindings[2].binding == 480);
    return VK_SUCCESS;
}
}
'''
fixture += helpers + r'''
}
int main() {
    using namespace Indium;
    using namespace Iridium;
    FunctionInfo kernel{FunctionType::Kernel, {
        {BindingType::Buffer,0,0,TextureAccessType::Sample},
        {BindingType::Buffer,1,1,TextureAccessType::Sample},
        {BindingType::Texture,0,480,TextureAccessType::Write}}};
    for(bool push : {false,true}) {
        PrivateDevice device{push};
        expectedFlags = push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
        assert(pushesComputeSet(kernel, device) == push);
        createFunctionSetLayout(device,&kernel);
        device.maxPushDescriptors = 2; // Too small: encoder must allocate a set.
        expectedFlags = 0;
        assert(!pushesComputeSet(kernel, device));
        createFunctionSetLayout(device,&kernel);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='compute-layout-') as temporary:
    path = Path(temporary)
    (path / 'check.cpp').write_text(fixture)
    subprocess.run(['c++', '-std=c++17', '-I', str(here / '../../runtime/src/Vulkan-Headers-1.3.290/include'),
                    str(path / 'check.cpp'), '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True)
print('PASS: compute layouts match pushed bindings and pool fallback')
