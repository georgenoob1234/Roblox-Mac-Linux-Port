#!/usr/bin/env python3
"""Check production compute dependencies; --gpu also exercises two dependent kernels."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--gpu', action='store_true', help='requires Vulkan and glslangValidator')
parser.add_argument('--negative-control', action='store_true', help='also report unsynchronized GPU results')
parser.add_argument('--source', type=Path, default=here / '../../runtime/src/indium/src/indium/compute-command-encoder.cpp')
args = parser.parse_args()
if args.negative_control and not args.gpu:
    parser.error('--negative-control requires --gpu')
source = args.source.read_text()


def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


helper = function('static void computeMemoryBarrier(')
dispatch = function('void Indium::PrivateComputeCommandEncoder::dispatchThreads(')
finish = function('void Indium::PrivateComputeCommandEncoder::endEncoding(')
constructor = function('Indium::PrivateComputeCommandEncoder::PrivateComputeCommandEncoder(')
# Cover the actual call sites as well as the compiled helper: removing either
# the serial dependency or an encoder boundary must fail the ordinary build.
serial = re.search(r'if\s*\(_descriptor.dispatchType\s*==\s*DispatchType::Serial\)\s*'
                   r'computeMemoryBarrier\(buf->commandBuffer\(\),\s*'
                   r'VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,\s*VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT\);', dispatch)
assert serial and serial.end() < dispatch.index('vkCmdDispatch(')
assert 'VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT' in finish
assert 'computeMemoryBarrier(' in finish
assert re.search(r'if\s*\(_descriptor.dispatchType\s*==\s*DispatchType::Concurrent\)\s*'
                 r'computeMemoryBarrier\(commandBuffer->commandBuffer\(\),\s*'
                 r'VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,\s*VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT\);', constructor)

recorder = r'''
#include <vulkan/vulkan.h>
#include <cassert>
static unsigned calls;
namespace Indium::DynamicVK {
void vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags source, VkPipelineStageFlags destination,
                          VkDependencyFlags, uint32_t count, const VkMemoryBarrier* barriers,
                          uint32_t bufferCount, const VkBufferMemoryBarrier*,
                          uint32_t imageCount, const VkImageMemoryBarrier*) {
    assert(source && destination && count == 1 && !bufferCount && !imageCount);
    assert(barriers[0].sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    assert(barriers[0].srcAccessMask & (VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT));
    assert(barriers[0].dstAccessMask & (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT));
    ++calls;
}
}
'''
recorder += helper + r'''
int main() {
    computeMemoryBarrier(VK_NULL_HANDLE, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    computeMemoryBarrier(VK_NULL_HANDLE, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    assert(calls == 2);
}
'''
include = here / '../../runtime/src/Vulkan-Headers-1.3.290/include'
with tempfile.TemporaryDirectory(prefix='compute-sync-') as temporary:
    root = Path(temporary)
    (root / 'check.cpp').write_text(recorder)
    subprocess.run(['c++', '-std=c++17', '-I', str(include), str(root / 'check.cpp'),
                    '-o', str(root / 'check')], check=True)
    subprocess.run([str(root / 'check')], check=True)
    print('PASS: production serial dispatch and encoder boundaries preserve compute dependencies', flush=True)
    if args.gpu:
        # Original kernels, deliberately using 112-byte records and a reverse
        # update order so a subsequent buffer-growth copy needs synchronization.
        common = '#version 450\nlayout(local_size_x=64) in;\n'
        common += 'layout(set=0,binding=0,std430) buffer Output { uint dst[]; };\n'
        for name in ['update', 'copy']:
            shader = common + f'layout(set=0,binding=1,std430) readonly buffer Input {{ uint src[]; }};\n'
            shader += 'void main() { uint i=gl_GlobalInvocationID.x;\n'
            if name == 'update':
                shader += 'uint d=src[i*32];\n'
                shader += '\n'.join(f'dst[d*28+{j}]=src[i*32+{j+4}];' for j in range(28))
            else:
                shader += '\n'.join(f'dst[i*28+{j}]=src[i*28+{j}];' for j in range(28))
            shader += '\n}\n'
            (root / f'{name}.comp').write_text(shader)
            subprocess.run(['glslangValidator', '-V', str(root / f'{name}.comp'),
                            '-o', str(root / f'{name}.spv')], check=True, stdout=subprocess.DEVNULL)
        (root / 'compute-sync-production.inc').write_text(helper)
        subprocess.run(['c++', '-std=c++17', '-O2', '-I', str(include), '-I', str(root),
                        str(here / 'compute-sync-check.cpp'), '-lvulkan', '-o', str(root / 'gpu-check')], check=True)
        subprocess.run([str(root / 'gpu-check'), str(root)], check=True)
        if args.negative_control:
            # Undefined results are diagnostic only; a driver may serialize anyway.
            subprocess.run([str(root / 'gpu-check'), str(root), 'omit-barrier'], check=True)
