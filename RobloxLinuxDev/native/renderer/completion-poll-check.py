#!/usr/bin/env python3
"""Two properties of the completion thread's poll that were only ever shown in game.

1. The GPU-completion wait is capped well above 1 ms. NVIDIA's driver does not block for a
   timeout of 1 ms or less, it spins until the timeout expires: with a 1 ms cap the thread
   burned about half a core for a whole session (3c754cc).
2. Buffer blocks that have been idle for a minute are freed, a few at a time, outside the
   pool lock. Without that 1.5 GiB of mapped driver memory stayed resident after one game's
   join until the client exited (30878b4). The production function is compiled against a
   small stand-in for the device and run.

usage: completion-poll-check.py [DIRECTORY_WITH_device.cpp_AND_buffer.cpp]
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

here = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parent
device = (here / 'device.cpp').read_text()
buffer = (here / 'buffer.cpp').read_text()


def function(signature, source):
    start = source.index(signature)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


poll = function('void Indium::PrivateDevice::pollEvents(uint64_t timeoutNanoseconds)', device)
waits = re.findall(r'vkWaitSemaphores\(_device, &info, ([^;]*)\);', poll)
assert len(waits) == 1, f'pollEvents should wait in one place, found {waits}'
cap = re.search(r'std::min<uint64_t>\(timeoutNanoseconds, (\w+)\)', waits[0])
assert cap, f'the completion wait is not capped by a named constant: {waits[0]}'
value = re.search(r'constexpr uint64_t ' + cap.group(1) + r' = (\d+);', device)
assert value or cap.group(1).isdigit(), f'{cap.group(1)} is not a constant in device.cpp'
nanoseconds = int(value.group(1) if value else cap.group(1))
assert 2_000_000 <= nanoseconds <= 16_000_000, \
    f'completion wait cap is {nanoseconds / 1e6:g} ms: 1 ms or less makes the NVIDIA driver spin, more than a frame stalls a missed wait'

assert 'trimIdleBufferMemory(*this);' in poll, 'pollEvents no longer trims idle buffer blocks'
release = function('Indium::PrivateBuffer::~PrivateBuffer()', buffer)
assert '_memoryBlock->idleSince = std::chrono::steady_clock::now();' in release, \
    'a single-slot block is not stamped when it becomes idle'
fixture = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
namespace Indium {
struct PrivateDevice;
static PrivateDevice* pool;
static int freed, freedUnderLock;
struct BufferMemoryBlock {
    size_t capacity = 1;
    std::vector<uint32_t> freeSlots;
    std::chrono::steady_clock::time_point idleSince;
    ~BufferMemoryBlock();
};
struct PrivateDevice {
    std::mutex bufferMemoryMutex;
    std::vector<std::shared_ptr<BufferMemoryBlock>> bufferMemoryBlocks;
};
// The real destructor calls the driver (unmap and free); that must not happen under the pool lock.
BufferMemoryBlock::~BufferMemoryBlock() {
    ++freed;
    if (pool->bufferMemoryMutex.try_lock()) pool->bufferMemoryMutex.unlock(); else ++freedUnderLock;
}
void trimIdleBufferMemory(PrivateDevice& device);
}
''' + function('void Indium::trimIdleBufferMemory(PrivateDevice& device)', buffer) + r'''
using namespace Indium;
using namespace std::chrono;
static std::shared_ptr<BufferMemoryBlock> block(size_t capacity, size_t freeSlots, seconds idleFor) {
    auto result = std::make_shared<BufferMemoryBlock>();
    result->capacity = capacity;
    result->freeSlots.assign(freeSlots, 0);
    result->idleSince = steady_clock::now() - idleFor;
    return result;
}
int main() {
    PrivateDevice device; pool = &device;
    auto& blocks = device.bufferMemoryBlocks;
    for (int i = 0; i < 10; ++i) blocks.push_back(block(1, 1, seconds(61)));  // stale
    for (int i = 0; i < 3; ++i) blocks.push_back(block(1, 1, seconds(30)));   // idle, but not for long
    for (int i = 0; i < 2; ++i) blocks.push_back(block(1, 0, seconds(600)));  // in use
    blocks.push_back(block(8, 8, seconds(600)));                              // a shared block of small slots
    const auto pause = [] { std::this_thread::sleep_for(milliseconds(260)); };

    device.bufferMemoryMutex.lock();        // somebody is creating a buffer: do not wait, do not free
    trimIdleBufferMemory(device);
    device.bufferMemoryMutex.unlock();
    assert(freed == 0 && blocks.size() == 16);
    pause();
    trimIdleBufferMemory(device); assert(freed == 4);            // four per round
    trimIdleBufferMemory(device); assert(freed == 4);            // and one round per quarter second
    pause(); trimIdleBufferMemory(device); assert(freed == 8);
    pause(); trimIdleBufferMemory(device); assert(freed == 10);
    pause(); trimIdleBufferMemory(device); assert(freed == 10);  // fresh, used and shared blocks stay
    assert(blocks.size() == 6 && freedUnderLock == 0);
    size_t fresh = 0, used = 0, shared = 0;
    for (auto& kept : blocks) { fresh += kept->capacity == 1 && !kept->freeSlots.empty(); used += kept->freeSlots.empty(); shared += kept->capacity == 8; }
    assert(fresh == 3 && used == 2 && shared == 1);
    std::puts("PASS completion poll: wait cap above the driver's spin threshold; idle buffer blocks freed after a minute, four per round, outside the pool lock");
}
'''
with tempfile.TemporaryDirectory(prefix='completion-poll-') as temporary:
    path = Path(temporary)
    (path / 'check.cpp').write_text(fixture)
    subprocess.run(['c++', '-std=c++17', '-O2', '-pthread', str(path / 'check.cpp'), '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True)
