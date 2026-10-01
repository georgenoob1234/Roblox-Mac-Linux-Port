#pragma once
#include <indium/texture.private.hpp>
#include <indium/device.private.hpp>
#include <indium/command-buffer.private.hpp>
#include <indium/command-queue.hpp>
#include <indium/blit-command-encoder.hpp>
#include <indium/buffer.hpp>
#include <cstring>

namespace Indium {
inline void readTextureBytes(std::shared_ptr<Texture> texture, void* bytes, size_t rowBytes, size_t imageBytes,
                             Region region, size_t level, size_t slice) {
    auto image = std::dynamic_pointer_cast<PrivateTexture>(texture);
    if (!bytes || !image || level >= image->mipmapLevelCount() || slice >= image->vulkanArrayLength())
        throw std::out_of_range("Invalid texture readback destination, mip or slice");
    if (!region.size.width || !region.size.height || !region.size.depth) return;
    auto format = texture->pixelFormat();
    for (auto parent = texture; parent->parentTexture(); parent = parent->parentTexture()) {
        level += parent->parentRelativeLevel(); slice += parent->parentRelativeSlice(); texture = parent->parentTexture();
    }
    auto fits = [](size_t origin, size_t extent, size_t size) { return origin < size && extent <= size - origin; };
    if (!fits(region.origin.x, region.size.width, std::max<size_t>(1, texture->width() >> level)) ||
        !fits(region.origin.y, region.size.height, std::max<size_t>(1, texture->height() >> level)) ||
        !fits(region.origin.z, region.size.depth, std::max<size_t>(1, texture->depth() >> level)))
        throw std::out_of_range("Texture readback outside mip extent");
    auto layout = textureCopyLayout(format, region.size.width, region.size.height, region.size.depth, rowBytes, imageBytes);
    auto tightRow = textureCopyLayout(format, region.size.width, 1, 1, 0, 0).dataSize;
    auto rows = textureCopyLayout(format, region.size.width, region.size.height, 1, 0, 0).dataSize / tightRow;
    auto rowStride = rowBytes ? rowBytes : tightRow;
    auto imageStride = imageBytes ? imageBytes : rowStride * rows;
    auto device = std::dynamic_pointer_cast<PrivateDevice>(texture->device());
    auto staging = device->newBuffer(layout.dataSize, ResourceOptions::StorageModeShared);
    auto command = std::dynamic_pointer_cast<PrivateCommandBuffer>(device->newCommandQueue()->commandBuffer());
    auto blit = command->blitCommandEncoder();
    blit->copy(texture, slice, level, region.origin, region.size, staging, 0, rowBytes, imageBytes);
    blit->endEncoding();
    command->commit(); // flushes earlier replaceRegion batches under queueMutex
    command->waitForGPU(); // valid even when called from a completion handler
    for (size_t z = 0; z < region.size.depth; ++z) for (size_t y = 0; y < rows; ++y) {
        auto offset = z * imageStride + y * rowStride;
        std::memcpy(static_cast<char*>(bytes) + offset, static_cast<char*>(staging->contents()) + offset, tightRow);
    }
}
}
