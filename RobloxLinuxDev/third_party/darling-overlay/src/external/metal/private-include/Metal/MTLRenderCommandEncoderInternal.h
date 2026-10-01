// SPDX-FileCopyrightText: 2026 Darling Developers
// SPDX-License-Identifier: MPL-2.0

#ifndef _METAL_MTLRENDERCOMMANDENCODERINTERNAL_H_
#define _METAL_MTLRENDERCOMMANDENCODERINTERNAL_H_

#import <Metal/MTLRenderCommandEncoder.h>

#if DARLING_METAL_ENABLED
#include <indium/indium.hpp>
#endif

METAL_DECLARATIONS_BEGIN

#if DARLING_METAL_ENABLED
NS_INLINE
Indium::ClearColor MTLClearColorToIndium(MTLClearColor clearColor) {
	return Indium::ClearColor { clearColor.red, clearColor.green, clearColor.blue, clearColor.alpha };
};

NS_INLINE
Indium::Viewport MTLViewportToIndium(MTLViewport viewport) {
	return Indium::Viewport {
		viewport.originX,
		viewport.originY,
		viewport.width,
		viewport.height,
		viewport.znear,
		viewport.zfar,
	};
};

NS_INLINE
Indium::ScissorRect MTLScissorRectToIndium(MTLScissorRect rect) {
	return Indium::ScissorRect {
		rect.height,
		rect.width,
		rect.x,
		rect.y,
	};
};

@interface MTLRenderPassAttachmentDescriptor (Internal)

- (Indium::RenderPassAttachmentDescriptor)asIndiumDescriptor;

@end

@interface MTLRenderPassColorAttachmentDescriptor (Internal)

- (Indium::RenderPassColorAttachmentDescriptor)asIndiumDescriptor;

@end

@interface MTLRenderPassDepthAttachmentDescriptor (Internal)

- (Indium::RenderPassDepthAttachmentDescriptor)asIndiumDescriptor;

@end

@interface MTLRenderPassStencilAttachmentDescriptor (Internal)

- (Indium::RenderPassStencilAttachmentDescriptor)asIndiumDescriptor;

@end

@interface MTLRenderPassColorAttachmentDescriptorArray (Internal) <NSCopying>

- (std::unordered_map<size_t, Indium::RenderPassColorAttachmentDescriptor>)asIndiumDescriptors;

- (std::vector<Indium::RenderPassColorAttachmentDescriptor>)asContiguousIndiumDescriptors;

@end

@interface MTLRenderPassDescriptor (Internal)

- (Indium::RenderPassDescriptor)asIndiumDescriptor;

@end
#endif

@interface MTLRenderCommandEncoderInternal : NSObject <MTLRenderCommandEncoder>

#if DARLING_METAL_ENABLED
// Backend ownership is fixed by init; copying the shared_ptr retains it without a property lock.
@property(nonatomic, readonly) std::shared_ptr<Indium::RenderCommandEncoder> encoder;

- (instancetype)initWithEncoder: (std::shared_ptr<Indium::RenderCommandEncoder>)encoder
                         device: (id<MTLDevice>)device;
#endif

@end

METAL_DECLARATIONS_END

#endif // _METAL_MTLRENDERCOMMANDENCODERINTERNAL_H_
