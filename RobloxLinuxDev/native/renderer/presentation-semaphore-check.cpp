#include <cassert>
#include <memory>
namespace Indium {
struct PrivateDevice;
struct BinarySemaphore { std::shared_ptr<PrivateDevice> device; };
struct PrivateDevice : std::enable_shared_from_this<PrivateDevice> {
    std::shared_ptr<BinarySemaphore> getWrappedBinarySemaphore(bool exportable) {
        assert(exportable);
        return std::make_shared<BinarySemaphore>(BinarySemaphore{shared_from_this()});
    }
};
}
#include "presentation-semaphore.hpp"
int main() {
    using namespace PresentationSemaphores;
    auto device = std::make_shared<Indium::PrivateDevice>();
    auto semaphore = acquire(device);
    auto entry = remember(semaphore);
    auto identity = semaphore.get();
    semaphore.reset();
    assert(acquire(device).get() != identity); // Pending GL wait cannot be reused.
    consumed(entry);
    auto command = entry->semaphore;
    assert(acquire(device).get() != identity); // Vulkan user still owns it.
    command.reset();
    auto otherDevice = std::make_shared<Indium::PrivateDevice>();
    assert(acquire(otherDevice).get() != identity);
    semaphore = acquire(device);
    assert(semaphore.get() == identity);
    semaphore.reset();
    assert(acquire(device).get() != identity); // Each new signal needs a new GL wait.
    consumed(entry);
    assert(acquire(device).get() == identity);
    std::weak_ptr<Indium::PrivateDevice> lifetime = device;
    entry.reset(); device.reset();
    assert(lifetime.expired()); // Registry must not pin the device or its payloads.
    acquire(otherDevice);
    assert(entries.empty());
}
