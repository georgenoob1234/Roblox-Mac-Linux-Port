#pragma once
#include <indium/blit-command-encoder.private.hpp>
#include <indium/resource.hpp>
#include "../../../runtime/profiler/renderer.h"

namespace Indium {
// The upstream encoder records raw handles without retaining them. Keep the
// resources and expose texture accesses to the common submission dependency path.
class TrackedBlit final : public PrivateBlitCommandEncoder {
public:
    using PrivateBlitCommandEncoder::PrivateBlitCommandEncoder;
    std::vector<std::shared_ptr<Resource>> resources;
    std::vector<std::shared_ptr<Texture>> reads, writes;
    uint64_t bufferCopyBytes=0;
    void copy(std::shared_ptr<Buffer> src, size_t so, std::shared_ptr<Buffer> dst, size_t off, size_t size) override {
        RbxProfiler::DiagnosticScope copy(RBX_DIAG_BUFFER_BLIT,size,dst.get(),reinterpret_cast<uintptr_t>(src.get()));
        if(copy.active())bufferCopyBytes+=size;
        resources.insert(resources.end(), {src, dst});
        PrivateBlitCommandEncoder::copy(src, so, dst, off, size);
    }
    void copy(std::shared_ptr<Buffer> src, size_t off, size_t row, size_t image, Size size, std::shared_ptr<Texture> dst, size_t slice, size_t level, Origin origin, BlitOption options) override {
        resources.push_back(src); writes.push_back(dst);
        PrivateBlitCommandEncoder::copy(src, off, row, image, size, dst, slice, level, origin, options);
    }
    void copy(std::shared_ptr<Texture> src, size_t slice, size_t level, Origin origin, Size size, std::shared_ptr<Buffer> dst, size_t off, size_t row, size_t image, BlitOption options) override {
        reads.push_back(src); resources.push_back(dst);
        PrivateBlitCommandEncoder::copy(src, slice, level, origin, size, dst, off, row, image, options);
    }
    void copy(std::shared_ptr<Texture> src, std::shared_ptr<Texture> dst) override {
        reads.push_back(src); writes.push_back(dst);
        PrivateBlitCommandEncoder::copy(src, dst);
    }
    void copy(std::shared_ptr<Texture> src, size_t ss, size_t sl, std::shared_ptr<Texture> dst, size_t ds, size_t dl, size_t slices, size_t levels) override {
        reads.push_back(src); writes.push_back(dst);
        PrivateBlitCommandEncoder::copy(src, ss, sl, dst, ds, dl, slices, levels);
    }
    void copy(std::shared_ptr<Texture> src, size_t ss, size_t sl, Origin so, Size size, std::shared_ptr<Texture> dst, size_t ds, size_t dl, Origin dest) override {
        reads.push_back(src); writes.push_back(dst);
        PrivateBlitCommandEncoder::copy(src, ss, sl, so, size, dst, ds, dl, dest);
    }
    void fillBuffer(std::shared_ptr<Buffer> buffer, Range<size_t> range, uint8_t value) override {
        resources.push_back(buffer);
        PrivateBlitCommandEncoder::fillBuffer(buffer, range, value);
    }
    void generateMipmapsForTexture(std::shared_ptr<Texture> texture) override {
        writes.push_back(texture);
        PrivateBlitCommandEncoder::generateMipmapsForTexture(texture);
    }
};
}
