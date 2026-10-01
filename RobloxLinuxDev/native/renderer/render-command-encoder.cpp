#include "../../runtime/profiler/renderer.h"
#include <indium/render-command-encoder.private.hpp>
#include <indium/command-buffer.private.hpp>
#include <indium/render-pass.hpp>
#include <indium/command-queue.private.hpp>
#include <indium/device.private.hpp>
#include <indium/texture.private.hpp>
#include <indium/render-pipeline.private.hpp>
#include <indium/types.private.hpp>
#include <indium/buffer.private.hpp>
#include <indium/library.private.hpp>
#include <indium/sampler.private.hpp>
#include <indium/depth-stencil.private.hpp>
#include <indium/command-encoder.private.hpp>
#include <indium/pipeline.private.hpp>
#include <indium/dynamic-vk.hpp>
#include <vulkan/vulkan_core.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>

Indium::RenderCommandEncoder::~RenderCommandEncoder() {};

namespace {
// Metal's tracked attachments carry dependencies between encoders. Submission
// order alone does not make a depth prepass visible to the following pass.
// These are the render pass's former external subpass dependencies.
void memoryBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags from, VkPipelineStageFlags to) {
	VkMemoryBarrier barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	barrier.srcAccessMask = barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	Indium::DynamicVK::vkCmdPipelineBarrier(commandBuffer, from, to, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
}

Indium::PrivateRenderCommandEncoder::PrivateRenderCommandEncoder(std::shared_ptr<PrivateCommandBuffer> commandBuffer, const RenderPassDescriptor& descriptor):
	_privateCommandBuffer(commandBuffer),
	_descriptor(descriptor),
	_privateDevice(commandBuffer->privateDevice())
{
	auto buf = _privateCommandBuffer.lock();
	_retain = buf->retainsReferences();

	auto vkDevice = _privateDevice->device();
	auto vkCmdBuf = buf->commandBuffer();

	// Shadow passes have no color target. Use an actual attachment for the extent.
	const RenderPassAttachmentDescriptor* first = nullptr;
	for (const auto& color : descriptor.colorAttachments)
		if (color.texture) { first = &color; break; }
	if (!first && descriptor.depthAttachment) first = &*descriptor.depthAttachment;
	if (!first && descriptor.stencilAttachment) first = &*descriptor.stencilAttachment;
	const auto targetWidth = first ? std::max<size_t>(1, first->texture->width() >> first->level) : descriptor.renderTargetWidth;
	const auto targetHeight = first ? std::max<size_t>(1, first->texture->height() >> first->level) : descriptor.renderTargetHeight;
	if (!targetWidth || !targetHeight) throw std::runtime_error("Render pass has no extent");
	const uint32_t layers = std::max<size_t>(1, descriptor.renderTargetArrayLength);

	for (const auto& color: descriptor.colorAttachments) {
		// TODO: distinguish between read-only and read-write textures
		if (color.texture) trackTexture(color.texture, true);
	}
	const RenderPassAttachmentDescriptor* depthStencil = nullptr;
	if (descriptor.depthAttachment) depthStencil = &*descriptor.depthAttachment;
	else if (descriptor.stencilAttachment) depthStencil = &*descriptor.stencilAttachment;
	if (descriptor.depthAttachment) trackTexture(descriptor.depthAttachment->texture, true);
	if (descriptor.stencilAttachment) trackTexture(descriptor.stencilAttachment->texture, true);
	if (depthStencil && descriptor.depthAttachment && descriptor.stencilAttachment) {
		auto* texture = static_cast<PrivateTexture*>(depthStencil->texture.get());
		auto* stencil = static_cast<PrivateTexture*>(descriptor.stencilAttachment->texture.get());
		if (texture->image() != stencil->image() || depthStencil->level != descriptor.stencilAttachment->level || depthStencil->slice != descriptor.stencilAttachment->slice)
			throw std::runtime_error("Separate depth and stencil images are not supported");
	}

	// Attachment views, cached per target: the same targets recur every frame.
	std::vector<uint64_t> targetKey{targetWidth, targetHeight, layers, descriptor.colorAttachments.size(), depthStencil != nullptr};
	std::vector<std::weak_ptr<Texture>> targetTextures;
	auto keyAttachment = [&](const RenderPassAttachmentDescriptor& a) {
		targetKey.insert(targetKey.end(), {reinterpret_cast<uintptr_t>(a.texture.get()), a.level, a.slice});
		targetTextures.push_back(a.texture);
	};
	for (const auto& a : descriptor.colorAttachments) keyAttachment(a);
	if (depthStencil) keyAttachment(*depthStencil);
	{
		std::scoped_lock lock(_privateDevice->rendererCacheMutex);
		auto& cache = _privateDevice->renderTargets;
		cache.erase(std::remove_if(cache.begin(), cache.end(), [](const auto& entry) {
			return std::any_of(entry->textures.begin(), entry->textures.end(), [](const auto& texture) { return texture.expired(); });
		}), cache.end());
		for (const auto& entry : cache) if (entry->key == targetKey) { _renderTarget = entry; break; }
		if (!_renderTarget) {
			_renderTarget = std::make_shared<RenderTargetCacheEntry>(vkDevice);
			_renderTarget->key = std::move(targetKey);
			_renderTarget->textures = std::move(targetTextures);
			auto attachmentView = [&](const RenderPassAttachmentDescriptor& attachment) {
				auto* texture = static_cast<PrivateTexture*>(attachment.texture.get());
				if (attachment.level >= texture->mipmapLevelCount() || attachment.slice >= texture->vulkanArrayLength() || layers > texture->vulkanArrayLength() - attachment.slice)
					throw std::out_of_range("Render attachment mip or slice out of range");
				VkImageViewCreateInfo info {};
				info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
				info.image = texture->image();
				info.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
				info.format = pixelFormatToVkFormat(texture->pixelFormat());
				info.subresourceRange.aspectMask = pixelFormatToVkImageAspectFlags(texture->pixelFormat());
				info.subresourceRange.baseMipLevel = attachment.level;
				info.subresourceRange.baseArrayLayer = attachment.slice;
				for (auto parent = attachment.texture; parent->parentTexture(); parent = parent->parentTexture()) {
					info.subresourceRange.baseMipLevel += parent->parentRelativeLevel();
					info.subresourceRange.baseArrayLayer += parent->parentRelativeSlice();
				}
				info.subresourceRange.levelCount = 1;
				info.subresourceRange.layerCount = layers;
				VkImageView view;
				if (DynamicVK::vkCreateImageView(vkDevice, &info, nullptr, &view) != VK_SUCCESS)
					throw std::runtime_error("Could not create render attachment view");
				_renderTarget->views.push_back(view);
			};
			for (const auto& colorAttachment: descriptor.colorAttachments) attachmentView(colorAttachment);
			if (depthStencil) attachmentView(*depthStencil);
			// Eviction only drops the cache reference; in-flight encoders keep entries alive.
			if (cache.size() >= 128) cache.erase(cache.begin());
			cache.push_back(_renderTarget);
		}
	}

	{
		std::scoped_lock lock(_privateDevice->rendererCacheMutex);
		auto& arenas = _privateDevice->descriptorArenas;
		// Reuse the same pass's arena; unrelated shadow/main passes otherwise
		// accumulate each other's layouts until the size limit discards them.
		auto found = std::find_if(arenas.begin(), arenas.end(), [&](const auto& arena) {
			return !arena->target.owner_before(_renderTarget) && !_renderTarget.owner_before(arena->target);
		});
		if (found == arenas.end())
			found = std::find_if(arenas.begin(), arenas.end(), [](const auto& arena) { return arena->target.expired(); });
		if (found == arenas.end() && arenas.size() >= 160) found = arenas.begin();
		if (found != arenas.end()) {
			_descriptorArena = std::move(*found);
			arenas.erase(found);
		}
	}
	// Pressure records ~45 encoders/frame and up to 1294 sets/encoder.
	// Keep bounded reuse across three in-flight frames without discarding the large pass.
	if (_descriptorArena && _descriptorArena->entries.size() > 2048) {
		if (RbxProfiler::enabled()) RbxProfiler::metric(RBX_PROF_ARENA_OVERSIZE, 1);
		_descriptorArena.reset();
	}
	if (!_descriptorArena) {
		if (RbxProfiler::enabled()) RbxProfiler::metric(RBX_PROF_ARENA_NEW, 1);
		_descriptorArena = std::make_shared<DescriptorArena>(vkDevice);
	}
	_descriptorArena->target = _renderTarget;
	// Index free sets once per completed-arena reuse, not once per PSO switch.
	for (auto& item : _descriptorArena->available) item.second.clear();
	for (size_t i = 0; i < _descriptorArena->entries.size(); ++i) {
		auto& entry = _descriptorArena->entries[i];
		entry.used = false;
		_descriptorArena->available[entry.layout].push_back(i);
	}
	_descriptorArena->hits = _descriptorArena->allocations = _descriptorArena->rewrites = 0;
	if (!_descriptorArena->pool) {
		VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		info.poolSizeCount = poolSizes.size(); info.pPoolSizes = poolSizes.data(); info.maxSets = 64;
		if (DynamicVK::vkCreateDescriptorPool(vkDevice, &info, nullptr, &_descriptorArena->pool) != VK_SUCCESS)
			throw std::runtime_error("Could not create render descriptor pool");
	}

	// Dynamic rendering: pipelines only need matching formats, never a render pass.
	std::vector<VkRenderingAttachmentInfo> colors;
	size_t viewIndex = 0;
	for (const auto& color: descriptor.colorAttachments) {
		VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		attachment.imageView = _renderTarget->views[viewIndex++];
		attachment.imageLayout = static_cast<PrivateTexture*>(color.texture.get())->imageLayout();
		attachment.loadOp = loadActionToVkAttachmentLoadOp(color.loadAction, true);
		attachment.storeOp = storeActionToVkAttachmentStoreOp(color.storeAction, true);
		attachment.clearValue.color.float32[0] = color.clearColor.red;
		attachment.clearValue.color.float32[1] = color.clearColor.green;
		attachment.clearValue.color.float32[2] = color.clearColor.blue;
		attachment.clearValue.color.float32[3] = color.clearColor.alpha;
		colors.push_back(attachment);
	}
	VkRenderingAttachmentInfo depth {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, stencil {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
	if (depthStencil) {
		auto* texture = static_cast<PrivateTexture*>(depthStencil->texture.get());
		const auto aspects = pixelFormatToVkImageAspectFlags(texture->pixelFormat());
		depth.imageView = stencil.imageView = _renderTarget->views[viewIndex];
		depth.imageLayout = stencil.imageLayout = texture->imageLayout();
		// A combined format attaches both aspects, as the pipelines declare;
		// an aspect Metal did not attach is neither loaded nor stored.
		depth.loadOp = descriptor.depthAttachment ? loadActionToVkAttachmentLoadOp(descriptor.depthAttachment->loadAction, false) : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depth.storeOp = descriptor.depthAttachment ? storeActionToVkAttachmentStoreOp(descriptor.depthAttachment->storeAction, false) : VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depth.clearValue.depthStencil.depth = descriptor.depthAttachment ? descriptor.depthAttachment->clearDepth : 1.0;
		stencil.loadOp = descriptor.stencilAttachment ? loadActionToVkAttachmentLoadOp(descriptor.stencilAttachment->loadAction, false) : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		stencil.storeOp = descriptor.stencilAttachment ? storeActionToVkAttachmentStoreOp(descriptor.stencilAttachment->storeAction, false) : VK_ATTACHMENT_STORE_OP_DONT_CARE;
		stencil.clearValue.depthStencil.stencil = descriptor.stencilAttachment ? descriptor.stencilAttachment->clearStencil : 0;
		if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) rendering.pDepthAttachment = &depth;
		if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) rendering.pStencilAttachment = &stencil;
	}
	rendering.renderArea.extent.width = targetWidth;
	rendering.renderArea.extent.height = targetHeight;
	rendering.layerCount = layers;
	rendering.colorAttachmentCount = colors.size();
	rendering.pColorAttachments = colors.data();
	memoryBarrier(vkCmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
	DynamicVK::vkCmdBeginRendering(vkCmdBuf, &rendering);

	// set default values

	setViewport(Viewport { 0, 0, static_cast<double>(targetWidth), static_cast<double>(targetHeight), 0, 1 });
	setScissorRect(ScissorRect { targetHeight, targetWidth, 0, 0 });
	setCullMode(CullMode::None);
	setFrontFacingWinding(Winding::Clockwise);

	DynamicVK::vkCmdSetDepthCompareOp(vkCmdBuf, VK_COMPARE_OP_ALWAYS);
	DynamicVK::vkCmdSetDepthBiasEnable(vkCmdBuf, false);
	DynamicVK::vkCmdSetDepthTestEnable(vkCmdBuf, false);
	DynamicVK::vkCmdSetDepthWriteEnable(vkCmdBuf, false);
	DynamicVK::vkCmdSetDepthBoundsTestEnable(vkCmdBuf, false);

	DynamicVK::vkCmdSetStencilTestEnable(vkCmdBuf, false);

	setBlendColor(0, 0, 0, 0);
	DynamicVK::vkCmdSetRasterizerDiscardEnable(vkCmdBuf, false);
	if (_privateDevice->dynamicDepthClamp) DynamicVK::vkCmdSetDepthClampEnableEXT(vkCmdBuf, VK_FALSE);
};

Indium::PrivateRenderCommandEncoder::~PrivateRenderCommandEncoder() {
	// Existing counters, per recording owner; unchanged bound sets bypass lookup.
	if (std::getenv("RBX_PRESENT_RESOURCE_TRACE"))
		std::fprintf(stderr, "DESCRIPTORS encoder=%p lookup-hits=%llu lookup-misses=%llu allocations=%llu rewrites=%llu\n",
			static_cast<void*>(this), (unsigned long long)_descriptorArena->hits,
			(unsigned long long)(_descriptorArena->allocations + _descriptorArena->rewrites),
			(unsigned long long)_descriptorArena->allocations, (unsigned long long)_descriptorArena->rewrites);
	// Report once per completed encoder, never per descriptor or while hidden.
	if (RbxProfiler::enabled()) {
		RbxProfiler::metric(RBX_PROF_DESCRIPTOR_HITS, _descriptorArena->hits);
		RbxProfiler::metric(RBX_PROF_DESCRIPTOR_ALLOCS, _descriptorArena->allocations);
		RbxProfiler::metric(RBX_PROF_DESCRIPTOR_REWRITES, _descriptorArena->rewrites);
	}
	// The command buffer holds the encoder through completion, so its arena is
	// now exclusive and can be reused without resetting live descriptors.
	std::scoped_lock lock(_privateDevice->rendererCacheMutex);
	if (_privateDevice->descriptorArenas.size() < 160)
		_privateDevice->descriptorArenas.push_back(std::move(_descriptorArena));
	else if (RbxProfiler::enabled()) RbxProfiler::metric(RBX_PROF_ARENA_OVERFLOW, 1);
};

void Indium::PrivateRenderCommandEncoder::trackTexture(const std::shared_ptr<Texture>& texture, bool write) {
	if (texture && _trackedTextures[write].insert(texture.get()).second)
		(write ? _readWriteTextures : _readOnlyTextures).push_back(texture);
}

void Indium::PrivateRenderCommandEncoder::setRenderPipelineState(std::shared_ptr<RenderPipelineState> renderPipelineState) {
	if (_privatePSO == renderPipelineState) return;
	if (_privatePSO) _savedPipelineStates.push_back(_privatePSO);
	// Every Indium render pipeline state is a PrivateRenderPipelineState.
	_privatePSO = std::static_pointer_cast<PrivateRenderPipelineState>(renderPipelineState);
	_bindingsDirty = {AllBindingsDirty, AllBindingsDirty};
};

void Indium::PrivateRenderCommandEncoder::setFrontFacingWinding(Winding frontFaceWinding) {
	const auto value = windingToVkFrontFace(frontFaceWinding);
	if (_frontFace == value) return;
	auto buf = _privateCommandBuffer.lock();
	DynamicVK::vkCmdSetFrontFace(buf->commandBuffer(), value);
	_frontFace = value;
};

void Indium::PrivateRenderCommandEncoder::setCullMode(CullMode cullMode) {
	const auto value = cullModeToVkCullMode(cullMode);
	if (_cullMode == value) return;
	auto buf = _privateCommandBuffer.lock();
	DynamicVK::vkCmdSetCullMode(buf->commandBuffer(), value);
	_cullMode = value;
};

void Indium::PrivateRenderCommandEncoder::setDepthBias(float depthBias, float slopeScale, float clamp) {
	const std::array<float,3> value{depthBias, slopeScale, clamp};
	if (_depthBias == value) return;
	auto buf = _privateCommandBuffer.lock();
	auto vkCmdBuf = buf->commandBuffer();
	if (!_depthBias) DynamicVK::vkCmdSetDepthBiasEnable(vkCmdBuf, true);
	DynamicVK::vkCmdSetDepthBias(vkCmdBuf, depthBias, clamp, slopeScale);
	_depthBias = value;
};

void Indium::PrivateRenderCommandEncoder::setDepthClipMode(DepthClipMode depthClipMode) {
	if (depthClipMode != DepthClipMode::Clip && depthClipMode != DepthClipMode::Clamp) throw BadEnumValue();
	_depthClamp = depthClipMode == DepthClipMode::Clamp;
};

void Indium::PrivateRenderCommandEncoder::setViewport(const Viewport& viewport) {
	setViewports(&viewport, 1);
};

void Indium::PrivateRenderCommandEncoder::setViewports(const Viewport* viewports, size_t count) {
	VkViewport local[16];
	std::vector<VkViewport> heap;
	VkViewport* tmp = count <= std::size(local) ? local : (heap.resize(count), heap.data());
	for (size_t i = 0; i < count; ++i) {
		const auto& viewport = viewports[i];
		VkViewport vkViewport {};
		// note: Metal's coordinates have the viewport flipped compared to Vulkan,
		//       so we use a Vulkan 1.1 feature here and just flip the Y-axis of the viewport
		//       (with a corresponding change in the origin).
		vkViewport.x = viewport.originX;
		vkViewport.y = viewport.originY + viewport.height;
		vkViewport.width = viewport.width;
		vkViewport.height = -viewport.height;
		// the documentation for setViewport and setViewports states that znear and zfar must be between
		// 0 and 1 (inclusive). however, one of Apple's examples ("Creating and Sampling Textures")
		// uses a znear of -1. i haven't tested what the actual behavior is in this case, but i'm assuming
		// that the values are simply clamped to [0, 1]. the sample in question seems to work identically
		// with clamping or without clamping (which is only possible with VK_EXT_depth_range_unrestricted enabled).
		vkViewport.minDepth = std::clamp(viewport.znear, 0., 1.);
		vkViewport.maxDepth = std::clamp(viewport.zfar, 0., 1.);
		tmp[i] = vkViewport;
	}
	if (!_viewports.empty() && _viewports.size() == count && std::equal(tmp, tmp + count, _viewports.begin(), [](const auto& a, const auto& b) {
		return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height && a.minDepth == b.minDepth && a.maxDepth == b.maxDepth;
	})) return;
	auto buf = _privateCommandBuffer.lock();
	DynamicVK::vkCmdSetViewportWithCount(buf->commandBuffer(), count, tmp);
	_viewports.assign(tmp, tmp + count);
};

void Indium::PrivateRenderCommandEncoder::setViewports(const std::vector<Viewport>& viewports) {
	setViewports(viewports.data(), viewports.size());
};

void Indium::PrivateRenderCommandEncoder::setScissorRect(const ScissorRect& scissorRect) {
	setScissorRects(&scissorRect, 1);
};

void Indium::PrivateRenderCommandEncoder::setScissorRects(const ScissorRect* scissorRects, size_t count) {
	VkRect2D local[16];
	std::vector<VkRect2D> heap;
	VkRect2D* tmp = count <= std::size(local) ? local : (heap.resize(count), heap.data());
	for (size_t i = 0; i < count; ++i) {
		const auto& scissorRect = scissorRects[i];
		VkRect2D vkRect {};
		vkRect.offset.x = scissorRect.x;
		vkRect.offset.y = scissorRect.y;
		vkRect.extent.width = scissorRect.width;
		vkRect.extent.height = scissorRect.height;
		tmp[i] = vkRect;
	}
	if (!_scissors.empty() && _scissors.size() == count && std::equal(tmp, tmp + count, _scissors.begin(), [](const auto& a, const auto& b) {
		return a.offset.x == b.offset.x && a.offset.y == b.offset.y && a.extent.width == b.extent.width && a.extent.height == b.extent.height;
	})) return;
	auto buf = _privateCommandBuffer.lock();
	DynamicVK::vkCmdSetScissorWithCount(buf->commandBuffer(), count, tmp);
	_scissors.assign(tmp, tmp + count);
};

void Indium::PrivateRenderCommandEncoder::setScissorRects(const std::vector<ScissorRect>& scissorRects) {
	setScissorRects(scissorRects.data(), scissorRects.size());
};

void Indium::PrivateRenderCommandEncoder::setBlendColor(float red, float green, float blue, float alpha) {
	const std::array<float,4> value{red, green, blue, alpha};
	if (_blendColor == value) return;
	auto buf = _privateCommandBuffer.lock();
	DynamicVK::vkCmdSetBlendConstants(buf->commandBuffer(), value.data());
	_blendColor = value;
};

void Indium::PrivateRenderCommandEncoder::updateBindings(VkCommandBuffer commandBuffer) {
	RbxProfiler::Scope profile(RBX_PROF_BINDINGS);
	if (!_bindingsDirty[0] && !_bindingsDirty[1]) return;

	const bool psoChanged = _descriptorPSO != _privatePSO;
	const auto& limits = _privateDevice->properties().limits;
	const FunctionInfo* infos[] = {&_privatePSO->vertexFunctionInfo(), &_privatePSO->fragmentFunctionInfo()};
	for (size_t stage = 0; stage < 2; ++stage) {
		if (!_bindingsDirty[stage]) continue;
		const auto& info = *infos[stage];
		const auto& current = _functionResources[stage];
		auto& previous = _boundResources[stage];
		const bool pushed = pushesBuffers(info, *_privateDevice);
		// Pushed buffers and the texture/sampler set are independent. A UI
		// transform change must not rebuild the unchanged atlas descriptor key.
		const bool updateSet = !pushed || psoChanged || (_bindingsDirty[stage] & (TexturesDirty | SamplersDirty));
		auto layout = _privatePSO->descriptorSetLayouts().layouts[1 + stage];
		auto& key = _keyScratch; key.clear();
		auto& ranges = _rangeScratch; ranges.assign(current.buffers.size(),0);
		auto& stageOffsets = _stageOffsetScratch; stageOffsets.clear();
		auto& pushWrites = _pushWrites; pushWrites.clear();
		auto& pushBuffers = _pushBuffers; pushBuffers.clear();
		bool ownSet = false;
		for (const auto& binding : info.bindings) {
			if (binding.type == Iridium::BindingType::Buffer) {
				if (pushed && !(_bindingsDirty[stage] & BuffersDirty)) continue;
				if (binding.index >= current.buffers.size()) throw std::runtime_error("Missing Metal buffer binding");
				const auto& resource = current.buffers[binding.index];
				const auto length = resource.first ? resource.first->length() : 0;
				if (resource.second >= length) throw std::out_of_range("Invalid Metal buffer offset");
				auto size = length - resource.second;
				const auto& inlines = _inlineRanges[stage];
				if (binding.index < inlines.size() && inlines[binding.index].buffer == resource.first.get() && inlines[binding.index].offset == resource.second)
					size = inlines[binding.index].length;
				const bool dynamic = !pushed && dynamicBufferBinding(info, binding, *_privateDevice);
				if (resource.second % limits.minStorageBufferOffsetAlignment || size > limits.maxStorageBufferRange ||
				    (dynamic && resource.second > UINT32_MAX)) throw std::out_of_range("Unsupported Metal storage buffer range/alignment");
				if (pushed) {
					// Every Indium buffer is a PrivateBuffer.
					pushBuffers.push_back({static_cast<PrivateBuffer*>(resource.first.get())->buffer(), resource.second, size});
					VkWriteDescriptorSet write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
					write.dstBinding = pushedBufferBinding(info, binding);
					write.descriptorCount = 1;
					write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
					pushWrites.push_back(write);
					continue;
				}
				ranges[binding.index] = size;
				key.insert(key.end(), {binding.internalIndex, reinterpret_cast<uintptr_t>(resource.first.get()), dynamic ? 0 : resource.second, size});
				if (dynamic) stageOffsets.emplace_back(binding.internalIndex, resource.second);
			} else if (binding.type == Iridium::BindingType::Texture) {
				if (!updateSet) continue;
				if (binding.index >= current.textures.size() || !current.textures[binding.index]) throw std::runtime_error("Missing Metal texture binding");
				key.insert(key.end(), {binding.internalIndex, reinterpret_cast<uintptr_t>(current.textures[binding.index].get())});
			} else if (binding.type == Iridium::BindingType::Sampler) {
				if (!updateSet) continue;
				const auto& sampler = binding.index == SIZE_MAX ? info.embeddedSamplerStates.at(binding.embeddedSamplerIndex) : current.samplers.at(binding.index);
				if (!sampler) throw std::runtime_error("Missing Metal sampler binding");
				key.insert(key.end(), {binding.internalIndex, reinterpret_cast<uintptr_t>(sampler.get())});
			} else continue;
			ownSet = true;
		}
		if (!pushWrites.empty()) {
			for (size_t i = 0; i < pushWrites.size(); ++i) pushWrites[i].pBufferInfo = &pushBuffers[i];
			DynamicVK::vkCmdPushDescriptorSetKHR(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _privatePSO->pipelineLayout(), 0, pushWrites.size(), pushWrites.data());
		}
		if (ownSet) {
			// Vulkan consumes offsets in ascending binding order, independently of reflection order.
			std::sort(stageOffsets.begin(), stageOffsets.end());
			auto& offsets = _stageDynamicOffsets[stage];
			bool bind = psoChanged || stageOffsets.size() != offsets.size();
			for (size_t i = 0; !bind && i < offsets.size(); ++i) bind = offsets[i] != stageOffsets[i].second;
			offsets.clear();
			for (const auto& offset : stageOffsets) offsets.push_back(offset.second);
			if (!_boundSets[stage] || psoChanged || key != _descriptorKeys[stage]) {
				DescriptorArena::Entry* selected = nullptr;
				size_t hash = DescriptorArena::keyHash(layout,key);
				// Control blocks identify the pipeline state without locking its weak reference.
				auto samePSO = [&](const DescriptorArena::Entry& entry) {
					return !entry.pso.owner_before(_privatePSO) && !_privatePSO.owner_before(entry.pso);
				};
				auto candidates = _descriptorArena->lookup.equal_range(hash);
				for (auto it = candidates.first; it != candidates.second; ++it) {
					auto& entry = _descriptorArena->entries[it->second];
					if (entry.layout == layout && samePSO(entry) && entry.key == key &&
						std::none_of(entry.resources.begin(), entry.resources.end(), [](const auto& resource) { return resource.expired(); })) {
						selected = &entry; ++_descriptorArena->hits; break;
					}
				}
				if (!selected) {
					// Exact cache hits may already have consumed an indexed free set.
					auto& available = _descriptorArena->available[layout];
					while (!available.empty()) {
						auto index = available.back(); available.pop_back();
						auto& entry = _descriptorArena->entries[index];
						if (!entry.used && samePSO(entry)) { selected = &entry; break; }
					}
					if (!selected) { _descriptorArena->entries.emplace_back(); selected = &_descriptorArena->entries.back(); }
					std::array<VkDescriptorSetLayout,1> layouts{layout};
					if (selected->set) ++_descriptorArena->rewrites; else ++_descriptorArena->allocations;
					// The resource tables below retain every binding through completion,
					// so the writer keeps nothing extra.
					std::vector<std::shared_ptr<Buffer>> unused;
					auto sets = createDescriptorSets<1>(layouts, _descriptorArena->pool, _descriptorArena->fullPools, _privateDevice,
						{current}, {info}, unused, {selected->set}, true, &ranges, pushed);
					size_t index = selected - _descriptorArena->entries.data();
					if (selected->set) {
						auto old = _descriptorArena->lookup.equal_range(selected->hash);
						for (auto it = old.first; it != old.second; ++it)
							if (it->second == index) { _descriptorArena->lookup.erase(it); break; }
					}
					selected->set = sets[0]; selected->layout = layout; selected->pso = _privatePSO;
					selected->key = key;
					size_t resourceIndex = 0;
					auto rememberResource = [&](const auto& resource) {
						if (resourceIndex == selected->resources.size()) selected->resources.emplace_back(resource);
						else {
							auto& old = selected->resources[resourceIndex];
							// Range-only rewrites keep their owners. Comparing control blocks
							// avoids releasing/reacquiring every unchanged weak reference.
							if (old.owner_before(resource) || resource.owner_before(old)) old = resource;
						}
						++resourceIndex;
					};
					for (const auto& binding : info.bindings) {
						if (binding.type == Iridium::BindingType::Buffer) { if (!pushed) rememberResource(current.buffers[binding.index].first); }
						else if (binding.type == Iridium::BindingType::Texture) rememberResource(current.textures[binding.index]);
						else if (binding.type == Iridium::BindingType::Sampler) rememberResource(binding.index == SIZE_MAX ? info.embeddedSamplerStates.at(binding.embeddedSamplerIndex) : current.samplers[binding.index]);
					}
					selected->resources.resize(resourceIndex);
					selected->hash = hash; _descriptorArena->lookup.emplace(hash,index);
				}
				selected->used = true;
				bind |= _boundSets[stage] != selected->set;
				_boundSets[stage] = selected->set; _descriptorKeys[stage] = key;
			}
			if (bind)
				DynamicVK::vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _privatePSO->pipelineLayout(),
					1 + stage, 1, &_boundSets[stage], offsets.size(), offsets.data());
		}
		bool sameBuffers = current.buffers.size() == previous.buffers.size() &&
			std::equal(current.buffers.begin(), current.buffers.end(), previous.buffers.begin(), [](const auto& a, const auto& b) { return a.first == b.first; });
		const bool texturesChanged = current.textures != previous.textures;
		const bool resourcesChanged = !sameBuffers || texturesChanged || current.samplers != previous.samplers;
		if (resourcesChanged) {
			// Resource tables only grow. Retain replaced slots through completion
			// (buffers and textures only when the command buffer retains them),
			// and reuse the bound tables instead of allocating a snapshot per draw.
			if (_savedFunctionResources.empty()) _savedFunctionResources.emplace_back();
			auto& saved = _savedFunctionResources.back();
			auto retainChanges = [](auto& old, const auto& next, auto& saved, bool retain) {
				for (size_t i = 0; i < old.size(); ++i)
					if (old[i] != next[i]) {
						if (retain) saved.push_back(std::move(old[i]));
						old[i] = next[i];
					}
				// Unchanged slots already own their resources; copy only the new tail.
				old.insert(old.end(), next.begin() + old.size(), next.end());
			};
			retainChanges(previous.buffers, current.buffers, saved.buffers, _retain);
			retainChanges(previous.textures, current.textures, saved.textures, _retain);
			retainChanges(previous.samplers, current.samplers, saved.samplers, true);
		}
		// Buffer/sampler changes do not change the already tracked texture accesses.
		if (psoChanged || texturesChanged) {
			for (const auto& binding : info.bindings) if (binding.type == Iridium::BindingType::Texture)
				trackTexture(current.textures[binding.index], binding.textureAccessType != Iridium::TextureAccessType::Sample);
		}
	}
	_descriptorPSO = _privatePSO;

	const auto& vertexInputBindings = _privatePSO->vertexInputBindings();
	if ((_bindingsDirty[0] & BuffersDirty) && vertexInputBindings.size() > 0) {
		auto& buffers = _vertexBufferScratch;
		auto& offsets = _vertexOffsetScratch;

		buffers.resize(vertexInputBindings.size());
		offsets.resize(vertexInputBindings.size());

		for (size_t vulkanIndex = 0; vulkanIndex < vertexInputBindings.size(); ++vulkanIndex) {
			const auto& metalIndex = vertexInputBindings[vulkanIndex];

			if (metalIndex >= _functionResources[0].buffers.size()) {
				// technically, this requires the `nullDescriptor` feature, but we should never run into this case anyways.
				buffers[vulkanIndex] = VK_NULL_HANDLE;
				offsets[vulkanIndex] = 0;
			} else {
				// The resource tables retain this buffer through command completion.
				const auto& [buffer, offset] = _functionResources[0].buffers[metalIndex];
				buffers[vulkanIndex] = static_cast<PrivateBuffer*>(buffer.get())->buffer();
				offsets[vulkanIndex] = offset;
			}
		}

		if (buffers != _vertexBuffers || offsets != _vertexOffsets) {
			DynamicVK::vkCmdBindVertexBuffers(commandBuffer, 0, vertexInputBindings.size(), buffers.data(), offsets.data());
			_vertexBuffers.swap(buffers); _vertexOffsets.swap(offsets);
		}
	}
	_bindingsDirty = {0, 0};
};

void Indium::PrivateRenderCommandEncoder::drawPrimitives(PrimitiveType primitiveType, size_t vertexStart, size_t vertexCount, size_t instanceCount, size_t baseInstance) {
	RbxProfiler::Scope profile(RBX_PROF_DRAW);
	auto buf = _privateCommandBuffer.lock();
	auto commandBuffer = buf->commandBuffer();

	// Each PSO synchronizes its own variants; ready lookup is lock-free.
	VkPipeline pipeline = _privatePSO->pipelineFor(primitiveType, _depthClamp);
	if (_boundPipeline!=pipeline) {
		DynamicVK::vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		_boundPipeline=pipeline;
	}
	auto topology=primitiveTypeToVkPrimitiveTopology(primitiveType);
	if (_boundTopology!=topology) {
		DynamicVK::vkCmdSetPrimitiveTopology(commandBuffer,topology);_boundTopology=topology;
	}
	if (_privateDevice->dynamicDepthClamp && _boundDepthClamp != _depthClamp) {
		DynamicVK::vkCmdSetDepthClampEnableEXT(commandBuffer, _depthClamp);
		_boundDepthClamp = _depthClamp;
	}

	updateBindings(commandBuffer);

	DynamicVK::vkCmdDraw(commandBuffer, vertexCount, instanceCount, vertexStart, baseInstance);

	// updateBindings retains each distinct resource state through GPU completion.
};

void Indium::PrivateRenderCommandEncoder::drawPrimitives(PrimitiveType primitiveType, size_t vertexStart, size_t vertexCount, size_t instanceCount) {
	drawPrimitives(primitiveType, vertexStart, vertexCount, instanceCount, 0);
};

void Indium::PrivateRenderCommandEncoder::drawPrimitives(PrimitiveType primitiveType, size_t vertexStart, size_t vertexCount) {
	drawPrimitives(primitiveType, vertexStart, vertexCount, 1);
};

// Metal copies setBytes data at encoding time. Append aligned slices so later
// calls never overwrite bytes referenced by an earlier draw. Draw resource
// snapshots already retain every used buffer until command completion.
void Indium::PrivateRenderCommandEncoder::setInlineBytes(unsigned stage, const void* bytes, size_t length, size_t index) {
	if (!bytes || !length) throw std::invalid_argument("Empty Metal inline data");
	const auto& limits = _privateDevice->properties().limits;
	const size_t alignment = std::max<size_t>(16, std::max(limits.minStorageBufferOffsetAlignment, limits.minUniformBufferOffsetAlignment));
	const size_t offset = (_inlineOffset + alignment - 1) & ~(alignment - 1);
	if (!_inlineBuffer || offset > _inlineBuffer->length() || length > _inlineBuffer->length() - offset) {
		_inlineBuffer = _privateDevice->newBuffer(std::max<size_t>(65536, length), ResourceOptions::StorageModeShared);
		// Internal: retained through completion whatever the app retains.
		_keepAliveBuffers.push_back(_inlineBuffer);
		_inlineOffset = 0;
	} else _inlineOffset = offset;
	std::memcpy(static_cast<char*>(_inlineBuffer->contents()) + _inlineOffset, bytes, length);
	_functionResources[stage].setBuffer(_inlineBuffer, _inlineOffset, index);
	_bindingsDirty[stage] |= BuffersDirty;
	auto& inlines = _inlineRanges[stage];
	if (inlines.size() <= index) inlines.resize(index + 1);
	inlines[index] = {_inlineBuffer.get(), _inlineOffset, length};
	_inlineOffset += length;
};

void Indium::PrivateRenderCommandEncoder::setVertexBytes(const void* bytes, size_t length, size_t index) {
	setInlineBytes(0, bytes, length, index);
};

void Indium::PrivateRenderCommandEncoder::endEncoding() {
	auto buf = _privateCommandBuffer.lock();
	DynamicVK::vkCmdEndRendering(buf->commandBuffer());
	memoryBarrier(buf->commandBuffer(), VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
};

void Indium::PrivateRenderCommandEncoder::setVertexBuffer(std::shared_ptr<Buffer> buffer, size_t offset, size_t index) {
	const auto& resources = _functionResources[0].buffers;
	if (index < resources.size() && resources[index].first == buffer && resources[index].second == offset) return;
	_functionResources[0].setBuffer(buffer, offset, index);
	_bindingsDirty[0] |= BuffersDirty;
};

void Indium::PrivateRenderCommandEncoder::setVertexBuffers(const std::vector<std::shared_ptr<Buffer>>& buffers, const std::vector<size_t>& offsets, Range<size_t> range) {
	for (size_t i = 0; i < range.length; ++i) {
		setVertexBuffer(buffers[i], offsets[i], range.start + i);
	}
};

void Indium::PrivateRenderCommandEncoder::setVertexBufferOffset(size_t offset, size_t index) {
	const auto& resources = _functionResources[0].buffers;
	if (index < resources.size() && resources[index].second == offset) return;
	_functionResources[0].setBufferOffset(offset, index);
	_bindingsDirty[0] |= BuffersDirty;
};

void Indium::PrivateRenderCommandEncoder::setVertexSamplerState(std::shared_ptr<SamplerState> state, size_t index) {
	const auto& resources = _functionResources[0].samplers;
	if (index < resources.size() && resources[index] == state) return;
	_functionResources[0].setSamplerState(state, std::nullopt, index);
	_bindingsDirty[0] |= SamplersDirty;
};

void Indium::PrivateRenderCommandEncoder::setVertexSamplerState(std::shared_ptr<SamplerState> state, float lodMinClamp, float lodMaxClamp, size_t index) {
	_functionResources[0].setSamplerState(state, std::make_pair(lodMinClamp, lodMaxClamp), index);
	_bindingsDirty[0] |= SamplersDirty;
};

void Indium::PrivateRenderCommandEncoder::setVertexSamplerStates(const std::vector<std::shared_ptr<SamplerState>>& states, Range<size_t> range) {
	for (size_t i = 0; i < range.length; ++i) {
		setVertexSamplerState(states[i], range.start + i);
	}
};

void Indium::PrivateRenderCommandEncoder::setVertexSamplerStates(const std::vector<std::shared_ptr<SamplerState>>& states, const std::vector<float>& lodMinClamps, const std::vector<float>& lodMaxClamps, Range<size_t> range) {
	for (size_t i = 0; i < range.length; ++i) {
		setVertexSamplerState(states[i], lodMinClamps[i], lodMaxClamps[i], range.start + i);
	}
};

void Indium::PrivateRenderCommandEncoder::setVertexTexture(std::shared_ptr<Texture> texture, size_t index) {
	const auto& resources = _functionResources[0].textures;
	if (index < resources.size() && resources[index] == texture) return;
	_functionResources[0].setTexture(texture, index);
	_bindingsDirty[0] |= TexturesDirty;
};

void Indium::PrivateRenderCommandEncoder::setVertexTextures(const std::vector<std::shared_ptr<Texture>>& textures, Range<size_t> range) {
	for (size_t i = 0; i < range.length; ++i) {
		setVertexTexture(textures[i], range.start + i);
	}
};

void Indium::PrivateRenderCommandEncoder::setFragmentBytes(const void* bytes, size_t length, size_t index) {
	setInlineBytes(1, bytes, length, index);
};

void Indium::PrivateRenderCommandEncoder::setFragmentBuffer(std::shared_ptr<Buffer> buffer, size_t offset, size_t index) {
	const auto& resources = _functionResources[1].buffers;
	if (index < resources.size() && resources[index].first == buffer && resources[index].second == offset) return;
	_functionResources[1].setBuffer(buffer, offset, index);
	_bindingsDirty[1] |= BuffersDirty;
};

void Indium::PrivateRenderCommandEncoder::setFragmentBuffers(const std::vector<std::shared_ptr<Buffer>>& buffers, const std::vector<size_t>& offsets, Range<size_t> range) {
	for (size_t i = 0; i < range.length; ++i) {
		setFragmentBuffer(buffers[i], offsets[i], range.start + i);
	}
};

void Indium::PrivateRenderCommandEncoder::setFragmentBufferOffset(size_t offset, size_t index) {
	const auto& resources = _functionResources[1].buffers;
	if (index < resources.size() && resources[index].second == offset) return;
	_functionResources[1].setBufferOffset(offset, index);
	_bindingsDirty[1] |= BuffersDirty;
};

void Indium::PrivateRenderCommandEncoder::setFragmentSamplerState(std::shared_ptr<SamplerState> state, size_t index) {
	const auto& resources = _functionResources[1].samplers;
	if (index < resources.size() && resources[index] == state) return;
	_functionResources[1].setSamplerState(state, std::nullopt, index);
	_bindingsDirty[1] |= SamplersDirty;
};

void Indium::PrivateRenderCommandEncoder::setFragmentSamplerState(std::shared_ptr<SamplerState> state, float lodMinClamp, float lodMaxClamp, size_t index) {
	_functionResources[1].setSamplerState(state, std::make_pair(lodMinClamp, lodMaxClamp), index);
	_bindingsDirty[1] |= SamplersDirty;
};

void Indium::PrivateRenderCommandEncoder::setFragmentSamplerStates(const std::vector<std::shared_ptr<SamplerState>>& states, Range<size_t> range) {
	for (size_t i = 0; i < range.length; ++i) {
		setFragmentSamplerState(states[i], range.start + i);
	}
};

void Indium::PrivateRenderCommandEncoder::setFragmentSamplerStates(const std::vector<std::shared_ptr<SamplerState>>& states, const std::vector<float>& lodMinClamps, const std::vector<float>& lodMaxClamps, Range<size_t> range) {
	for (size_t i = 0; i < range.length; ++i) {
		setFragmentSamplerState(states[i], lodMinClamps[i], lodMaxClamps[i], range.start + i);
	}
};

void Indium::PrivateRenderCommandEncoder::setFragmentTexture(std::shared_ptr<Texture> texture, size_t index) {
	const auto& resources = _functionResources[1].textures;
	if (index < resources.size() && resources[index] == texture) return;
	_functionResources[1].setTexture(texture, index);
	_bindingsDirty[1] |= TexturesDirty;
};

void Indium::PrivateRenderCommandEncoder::setFragmentTextures(std::vector<std::shared_ptr<Texture>>& textures, Range<size_t> range) {
	for (size_t i = 0; i < range.length; ++i) {
		setFragmentTexture(textures[i], range.start + i);
	}
};

void Indium::PrivateRenderCommandEncoder::drawIndexedPrimitives(PrimitiveType primitiveType, size_t indexCount, IndexType indexType, std::shared_ptr<Buffer> indexBuffer, size_t indexBufferOffset, size_t instanceCount, int64_t baseVertex, size_t baseInstance) {
	RbxProfiler::Scope profile(RBX_PROF_DRAW);
	auto buf = _privateCommandBuffer.lock();
	auto commandBuffer = buf->commandBuffer();

	// Each PSO synchronizes its own variants; ready lookup is lock-free.
	VkPipeline pipeline = _privatePSO->pipelineFor(primitiveType, _depthClamp);
	if (_boundPipeline!=pipeline) {
		DynamicVK::vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		_boundPipeline=pipeline;
	}
	auto topology=primitiveTypeToVkPrimitiveTopology(primitiveType);
	if (_boundTopology!=topology) {
		DynamicVK::vkCmdSetPrimitiveTopology(commandBuffer,topology);_boundTopology=topology;
	}
	if (_privateDevice->dynamicDepthClamp && _boundDepthClamp != _depthClamp) {
		DynamicVK::vkCmdSetDepthClampEnableEXT(commandBuffer, _depthClamp);
		_boundDepthClamp = _depthClamp;
	}

	updateBindings(commandBuffer);

	// Every Indium buffer is a PrivateBuffer.
	auto* privateIndexBuffer = static_cast<PrivateBuffer*>(indexBuffer.get());

	auto vkType = indexTypeToVkIndexType(indexType);
	// A retained index buffer's Vulkan handle cannot be recycled while later
	// draws reuse it; unretained command buffers leave its lifetime to the app.
	if (_retain && _indexBuffer != privateIndexBuffer->buffer()) _keepAliveBuffers.push_back(indexBuffer);
	if (_indexBuffer != privateIndexBuffer->buffer() || _indexOffset != indexBufferOffset || _indexType != vkType) {
		DynamicVK::vkCmdBindIndexBuffer(commandBuffer, privateIndexBuffer->buffer(), indexBufferOffset, vkType);
		_indexBuffer = privateIndexBuffer->buffer(); _indexOffset = indexBufferOffset; _indexType = vkType;
	}
	DynamicVK::vkCmdDrawIndexed(commandBuffer, indexCount, instanceCount, 0, baseVertex, baseInstance);

	// updateBindings retains each distinct resource state through GPU completion.
};

void Indium::PrivateRenderCommandEncoder::drawIndexedPrimitives(PrimitiveType primitiveType, size_t indexCount, IndexType indexType, std::shared_ptr<Buffer> indexBuffer, size_t indexBufferOffset, size_t instanceCount) {
	drawIndexedPrimitives(primitiveType, indexCount, indexType, indexBuffer, indexBufferOffset, instanceCount, 0, 0);
};

void Indium::PrivateRenderCommandEncoder::drawIndexedPrimitives(PrimitiveType primitiveType, size_t indexCount, IndexType indexType, std::shared_ptr<Buffer> indexBuffer, size_t indexBufferOffset) {
	drawIndexedPrimitives(primitiveType, indexCount, indexType, indexBuffer, indexBufferOffset, 1);
};

void Indium::PrivateRenderCommandEncoder::setDepthStencilState(std::shared_ptr<DepthStencilState> state) {
	// State objects are immutable: the same one sets the same dynamic state.
	if (state && state == _depthStencilState) return;
	_depthStencilState = state;
	auto buf = _privateCommandBuffer.lock();
	auto* privateState = static_cast<PrivateDepthStencilState*>(state.get());
	const DepthStencilDescriptor defaultState{};
	const auto& desc = privateState ? privateState->descriptor() : defaultState;

	DynamicVK::vkCmdSetDepthWriteEnable(buf->commandBuffer(), desc.depthWriteEnabled ? VK_TRUE : VK_FALSE);
	DynamicVK::vkCmdSetDepthCompareOp(buf->commandBuffer(), compareFunctionToVkCompareOp(desc.depthCompareFunction));
	DynamicVK::vkCmdSetDepthTestEnable(buf->commandBuffer(), VK_TRUE);

	DynamicVK::vkCmdSetStencilTestEnable(buf->commandBuffer(), (desc.frontFaceStencil || desc.backFaceStencil) ? VK_TRUE : VK_FALSE);

	if (desc.frontFaceStencil || desc.backFaceStencil) {
		if (desc.frontFaceStencil) {
			DynamicVK::vkCmdSetStencilCompareMask(buf->commandBuffer(), VK_STENCIL_FACE_FRONT_BIT, desc.frontFaceStencil->readMask);
			DynamicVK::vkCmdSetStencilWriteMask(buf->commandBuffer(), VK_STENCIL_FACE_FRONT_BIT, desc.frontFaceStencil->writeMask);

			DynamicVK::vkCmdSetStencilOp(
				buf->commandBuffer(),
				VK_STENCIL_FACE_FRONT_BIT,
				stencilOperationToVkStencilOp(desc.frontFaceStencil->stencilFailureOperation),
				stencilOperationToVkStencilOp(desc.frontFaceStencil->depthStencilPassOperation),
				stencilOperationToVkStencilOp(desc.frontFaceStencil->depthFailureOperation),
				compareFunctionToVkCompareOp(desc.frontFaceStencil->stencilCompareFunction)
			);
		} else {
			DynamicVK::vkCmdSetStencilOp(buf->commandBuffer(), VK_STENCIL_FACE_FRONT_BIT, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
		}
		if (desc.backFaceStencil) {
			DynamicVK::vkCmdSetStencilCompareMask(buf->commandBuffer(), VK_STENCIL_FACE_BACK_BIT, desc.backFaceStencil->readMask);
			DynamicVK::vkCmdSetStencilWriteMask(buf->commandBuffer(), VK_STENCIL_FACE_BACK_BIT, desc.backFaceStencil->writeMask);

			DynamicVK::vkCmdSetStencilOp(
				buf->commandBuffer(),
				VK_STENCIL_FACE_BACK_BIT,
				stencilOperationToVkStencilOp(desc.backFaceStencil->stencilFailureOperation),
				stencilOperationToVkStencilOp(desc.backFaceStencil->depthStencilPassOperation),
				stencilOperationToVkStencilOp(desc.backFaceStencil->depthFailureOperation),
				compareFunctionToVkCompareOp(desc.backFaceStencil->stencilCompareFunction)
			);
		} else {
			DynamicVK::vkCmdSetStencilOp(buf->commandBuffer(), VK_STENCIL_FACE_BACK_BIT, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
		}
	}
};

void Indium::PrivateRenderCommandEncoder::setTriangleFillMode(TriangleFillMode triangleFillMode) {
	if (triangleFillMode != TriangleFillMode::Fill) {
		// we might have to just create duplicate pipelines for each possible fill mode;
		// there's only 2 at the moment, but this has to multiplied by the number of pipelines required
		// for other combinations. for example, we currently have to create a pipeline for each topology class
		// and there's 3 of those, so we would need 6 pipelines total. yikes.
		throw std::runtime_error("TODO: support changing fill mode");
	}
};

void Indium::PrivateRenderCommandEncoder::setStencilReferenceValue(uint32_t value) {
	const std::array<uint32_t,2> values{value, value};
	if (_stencilReference == values) return;
	auto buf = _privateCommandBuffer.lock();
	DynamicVK::vkCmdSetStencilReference(buf->commandBuffer(), VK_STENCIL_FACE_FRONT_AND_BACK, value);
	_stencilReference = values;
};

void Indium::PrivateRenderCommandEncoder::setStencilReferenceValue(uint32_t front, uint32_t back) {
	if (front == back) return setStencilReferenceValue(front);
	const std::array<uint32_t,2> values{front, back};
	if (_stencilReference == values) return;
	auto buf = _privateCommandBuffer.lock();
	if (!_stencilReference || (*_stencilReference)[0] != front)
		DynamicVK::vkCmdSetStencilReference(buf->commandBuffer(), VK_STENCIL_FACE_FRONT_BIT, front);
	if (!_stencilReference || (*_stencilReference)[1] != back)
		DynamicVK::vkCmdSetStencilReference(buf->commandBuffer(), VK_STENCIL_FACE_BACK_BIT, back);
	_stencilReference = values;
};

void Indium::PrivateRenderCommandEncoder::setVisibilityResultMode(VisibilityResultMode mode, size_t offset) {
	auto buf = _privateCommandBuffer.lock();

	// TODO: this can be implemented using Vulkan's occlusion queries
	throw std::runtime_error("TODO: support visibility results");
};

void Indium::PrivateRenderCommandEncoder::useResource(std::shared_ptr<Resource> resource, ResourceUsage usage, RenderStages stages) {
	useResources({ resource }, usage, stages);
};

void Indium::PrivateRenderCommandEncoder::useResources(const std::vector<std::shared_ptr<Resource>>& resources, ResourceUsage usage, RenderStages stages) {
	// TODO: image layout transitions. maybe.
	//       right now, we always keep images in the layout described by their imageLayout() method.
	//       we only briefly transition them away for an operation and then transition them back.
	//       however, these transitions are probably unnecessary in most cases, so we could optimize
	//       performance by getting rid of them.

	auto buf = _privateCommandBuffer.lock();

	std::vector<VkBufferMemoryBarrier> bufferBarriers;
	std::vector<VkImageMemoryBarrier> imageBarriers;

	// TODO: relax this mask, maybe; it depends on what Metal does here.
	VkAccessFlags source = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	VkAccessFlags dest = VK_ACCESS_NONE;

	if (!!(usage & (ResourceUsage::Read | ResourceUsage::Sample))) {
		dest |= VK_ACCESS_SHADER_READ_BIT;
	}

	if (!!(usage & ResourceUsage::Write)) {
		dest |= VK_ACCESS_SHADER_WRITE_BIT;
	}

	for (const auto& resource: resources) {
		if (auto buffer = std::dynamic_pointer_cast<PrivateBuffer>(resource)) {
			if (_retain) _keepAliveBuffers.push_back(buffer);
			VkBufferMemoryBarrier barrier {};

			barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			barrier.srcAccessMask = source;
			barrier.dstAccessMask = dest;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer = buffer->buffer();
			barrier.offset = 0;
			barrier.size = VK_WHOLE_SIZE;

			bufferBarriers.push_back(barrier);
		} else if (auto texture = std::dynamic_pointer_cast<PrivateTexture>(resource)) {
			trackTexture(texture, !!(usage & ResourceUsage::Write));
			VkImageMemoryBarrier barrier {};

			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.srcAccessMask = source;
			barrier.dstAccessMask = dest;
			barrier.oldLayout = texture->imageLayout();
			barrier.newLayout = barrier.oldLayout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = texture->image();
			barrier.subresourceRange.aspectMask = pixelFormatToVkImageAspectFlags(texture->pixelFormat());
			barrier.subresourceRange.baseMipLevel = 0;
			barrier.subresourceRange.levelCount = texture->mipmapLevelCount();
			barrier.subresourceRange.baseArrayLayer = 0;
			barrier.subresourceRange.layerCount = texture->vulkanArrayLength();

			imageBarriers.push_back(barrier);
		} else {
			throw std::runtime_error("Unsupported resource");
		}
	}

	// TODO: relax this, maybe, depending on what Metal does.
	VkPipelineStageFlags srcStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkPipelineStageFlags dstStages = VK_PIPELINE_STAGE_NONE;

	if (!!(stages & RenderStages::Vertex)) {
		dstStages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
	}

	if (!!(stages & RenderStages::Fragment)) {
		dstStages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}

	if (!!(stages & RenderStages::Tile)) {
		// XXX: not sure about this one
		dstStages |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT;
	}

	if (!!(stages & RenderStages::Object)) {
		// ???
	}

	if (!!(stages & RenderStages::Mesh)) {
		// not yet defined on all platforms as 'EXT' instead of 'NV'
		//dstStages |= VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT;
		dstStages |= VK_PIPELINE_STAGE_MESH_SHADER_BIT_NV;
	}

	DynamicVK::vkCmdPipelineBarrier(buf->commandBuffer(), srcStages, dstStages, 0, 0, nullptr, bufferBarriers.size(), bufferBarriers.data(), imageBarriers.size(), imageBarriers.data());
};

void Indium::PrivateRenderCommandEncoder::useResource(std::shared_ptr<Resource> resource, ResourceUsage usage) {
	useResources({ resource }, usage);
};

void Indium::PrivateRenderCommandEncoder::useResources(const std::vector<std::shared_ptr<Resource>>& resources, ResourceUsage usage) {
	// TODO: check what Metal does in this case. this is just an educated guess
	useResources(resources, usage, RenderStages::Vertex | RenderStages::Fragment | RenderStages::Tile | RenderStages::Object | RenderStages::Mesh);
};
