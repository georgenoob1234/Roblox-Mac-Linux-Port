// SPDX-FileCopyrightText: 2026 Darling Developers
// SPDX-License-Identifier: MPL-2.0

#ifndef _METAL_MTLBUFFERINTERNAL_H_
#define _METAL_MTLBUFFERINTERNAL_H_

#import <Metal/MTLBuffer.h>

#if DARLING_METAL_ENABLED
#include <indium/indium.hpp>
#endif

@interface MTLBufferInternal : NSObject <MTLBuffer>

#if DARLING_METAL_ENABLED
// Backend ownership is fixed by init; copying the shared_ptr retains it without a property lock.
@property(nonatomic, readonly) std::shared_ptr<Indium::Buffer> buffer;

- (instancetype)initWithBuffer: (std::shared_ptr<Indium::Buffer>)buffer
                        device: (id<MTLDevice>)device
               resourceOptions: (MTLResourceOptions)options;
#endif

@end

#endif // _METAL_MTLBUFFERINTERNAL_H_
