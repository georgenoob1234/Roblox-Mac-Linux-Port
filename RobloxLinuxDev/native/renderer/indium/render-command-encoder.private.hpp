#pragma once

#include <indium/render-command-encoder.hpp>
#include <indium/render-pass.hpp>
#include <indium/sampler.private.hpp>
#include <indium/device.hpp>
#include <indium/command-encoder.private.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <optional>
#include <variant>
#include <vector>
#include <map>
#include <unordered_set>
#include <indium/renderer-cache.hpp>

namespace Indium {
	class PrivateCommandBuffer;
	class PrivateRenderPipelineState;
	class PrivateDevice;
	class SamplerState;

	class PrivateRenderCommandEncoder: public RenderCommandEncoder {
	private:
		// we keep a copy because we need to keep some of the resources it contains alive
		RenderPassDescriptor _descriptor;
		// the command buffer always outlives us
		std::weak_ptr<PrivateCommandBuffer> _privateCommandBuffer;
		std::shared_ptr<PrivateDevice> _privateDevice;
		std::shared_ptr<PrivateRenderPipelineState> _privatePSO;
		VkFramebuffer _framebuffer = VK_NULL_HANDLE;
		VkRenderPass _renderPass = VK_NULL_HANDLE;
		VkDescriptorPool _pool = VK_NULL_HANDLE;
		std::vector<VkDescriptorPool> _fullPools;

		std::vector<FunctionResources> _savedFunctionResources;

		std::array<FunctionResources, 2> _functionResources {};
		std::vector<std::shared_ptr<Buffer>> _keepAliveBuffers;

		void updateBindings(VkCommandBuffer commandBuffer);

	public:
		PrivateRenderCommandEncoder(std::shared_ptr<PrivateCommandBuffer> commandBuffer, const RenderPassDescriptor& descriptor);
		~PrivateRenderCommandEncoder();

		virtual void setRenderPipelineState(std::shared_ptr<RenderPipelineState> renderPipelineState) override;
		virtual void setFrontFacingWinding(Winding frontFaceWinding) override;
		virtual void setCullMode(CullMode cullMode) override;
		virtual void setDepthBias(float depthBias, float slopeScale, float clamp) override;
		virtual void setDepthClipMode(DepthClipMode depthClipMode) override;
		virtual void setViewport(const Viewport& viewport) override;
		virtual void setViewports(const Viewport* viewports, size_t count) override;
		virtual void setViewports(const std::vector<Viewport>& viewports) override;
		virtual void setScissorRect(const ScissorRect& scissorRect) override;
		virtual void setScissorRects(const ScissorRect* scissorRects, size_t count) override;
		virtual void setScissorRects(const std::vector<ScissorRect>& scissorRects) override;
		virtual void setBlendColor(float red, float green, float blue, float alpha) override;
		virtual void setDepthStencilState(std::shared_ptr<DepthStencilState> state) override;
		virtual void setTriangleFillMode(TriangleFillMode triangleFillMode) override;
		virtual void setStencilReferenceValue(uint32_t value) override;
		virtual void setStencilReferenceValue(uint32_t front, uint32_t back) override;
		virtual void setVisibilityResultMode(VisibilityResultMode mode, size_t offset) override;

		virtual void drawPrimitives(PrimitiveType primitiveType, size_t vertexStart, size_t vertexCount, size_t instanceCount, size_t baseInstance) override;
		virtual void drawPrimitives(PrimitiveType primitiveType, size_t vertexStart, size_t vertexCount, size_t instanceCount) override;
		virtual void drawPrimitives(PrimitiveType primitiveType, size_t vertexStart, size_t vertexCount) override;

		virtual void drawIndexedPrimitives(PrimitiveType primitiveType, size_t indexCount, IndexType indexType, std::shared_ptr<Buffer> indexBuffer, size_t indexBufferOffset, size_t instanceCount, int64_t baseVertex, size_t baseInstance) override;
		virtual void drawIndexedPrimitives(PrimitiveType primitiveType, size_t indexCount, IndexType indexType, std::shared_ptr<Buffer> indexBuffer, size_t indexBufferOffset, size_t instanceCount) override;
		virtual void drawIndexedPrimitives(PrimitiveType primitiveType, size_t indexCount, IndexType indexType, std::shared_ptr<Buffer> indexBuffer, size_t indexBufferOffset) override;

		virtual void setVertexBytes(const void* bytes, size_t length, size_t index) override;
		virtual void setVertexBuffer(std::shared_ptr<Buffer> buffer, size_t offset, size_t index) override;
		virtual void setVertexBuffers(const std::vector<std::shared_ptr<Buffer>>& buffers, const std::vector<size_t>& offsets, Range<size_t> range) override;
		virtual void setVertexBufferOffset(size_t offset, size_t index) override;
		virtual void setVertexSamplerState(std::shared_ptr<SamplerState> state, size_t index) override;
		virtual void setVertexSamplerState(std::shared_ptr<SamplerState> state, float lodMinClamp, float lodMaxClamp, size_t index) override;
		virtual void setVertexSamplerStates(const std::vector<std::shared_ptr<SamplerState>>& states, Range<size_t> range) override;
		virtual void setVertexSamplerStates(const std::vector<std::shared_ptr<SamplerState>>& states, const std::vector<float>& lodMinClamps, const std::vector<float>& lodMaxClamps, Range<size_t> range) override;
		virtual void setVertexTexture(std::shared_ptr<Texture> texture, size_t index) override;
		virtual void setVertexTextures(const std::vector<std::shared_ptr<Texture>>& textures, Range<size_t> range) override;

		virtual void setFragmentBytes(const void* bytes, size_t length, size_t index) override;
		virtual void setFragmentBuffer(std::shared_ptr<Buffer> buffer, size_t offset, size_t index) override;
		virtual void setFragmentBuffers(const std::vector<std::shared_ptr<Buffer>>& buffers, const std::vector<size_t>& offsets, Range<size_t> range) override;
		virtual void setFragmentBufferOffset(size_t offset, size_t index) override;
		virtual void setFragmentSamplerState(std::shared_ptr<SamplerState> state, size_t index) override;
		virtual void setFragmentSamplerState(std::shared_ptr<SamplerState> state, float lodMinClamp, float lodMaxClamp, size_t index) override;
		virtual void setFragmentSamplerStates(const std::vector<std::shared_ptr<SamplerState>>& states, Range<size_t> range) override;
		virtual void setFragmentSamplerStates(const std::vector<std::shared_ptr<SamplerState>>& states, const std::vector<float>& lodMinClamps, const std::vector<float>& lodMaxClamps, Range<size_t> range) override;
		virtual void setFragmentTexture(std::shared_ptr<Texture> texture, size_t index) override;
		virtual void setFragmentTextures(std::vector<std::shared_ptr<Texture>>& textures, Range<size_t> range) override;

		virtual void useResource(std::shared_ptr<Resource> resource, ResourceUsage usage, RenderStages stages) override;
		virtual void useResources(const std::vector<std::shared_ptr<Resource>>& resources, ResourceUsage usage, RenderStages stages) override;
		virtual void useResource(std::shared_ptr<Resource> resource, ResourceUsage usage) override;
		virtual void useResources(const std::vector<std::shared_ptr<Resource>>& resources, ResourceUsage usage) override;

		virtual void endEncoding() override;

		INDIUM_PROPERTY_OBJECT_VECTOR(Texture, r, R,eadOnlyTextures);
		INDIUM_PROPERTY_OBJECT_VECTOR(Texture, r, R,eadWriteTextures);
	private:
		bool _depthClamp = false;
		bool _boundDepthClamp = false;
		// Metal's default command buffers retain what they reference; buffers
		// with unretained references leave buffers and textures to the app.
		bool _retain = true;
		VkPipeline _boundPipeline = VK_NULL_HANDLE;
		VkPrimitiveTopology _boundTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
		std::shared_ptr<PrivateRenderPipelineState> _descriptorPSO;
		// Per stage: the set bound at 1 + stage, its key and dynamic offsets.
		std::array<VkDescriptorSet,2> _boundSets{};
		std::array<std::vector<uint64_t>,2> _descriptorKeys;
		std::array<std::vector<uint32_t>,2> _stageDynamicOffsets;
		std::array<FunctionResources,2> _boundResources{};
		enum BindingDirty { BuffersDirty = 1, TexturesDirty = 2, SamplersDirty = 4, AllBindingsDirty = 7 };
		std::array<unsigned,2> _bindingsDirty{AllBindingsDirty, AllBindingsDirty};
		std::optional<VkFrontFace> _frontFace;
		std::optional<VkCullModeFlags> _cullMode;
		std::optional<std::array<float,3>> _depthBias;
		std::optional<std::array<float,4>> _blendColor;
		std::optional<std::array<uint32_t,2>> _stencilReference;
		std::vector<VkViewport> _viewports;
		std::vector<VkRect2D> _scissors;
		std::shared_ptr<Buffer> _inlineBuffer;
		size_t _inlineOffset = 0;
		void setInlineBytes(unsigned stage, const void* bytes, size_t length, size_t index);
		// setBytes data length per stage and Metal buffer index, valid while
		// that index still binds the same inline slice.
		struct InlineRange { const Buffer* buffer = nullptr; size_t offset = 0, length = 0; };
		std::array<std::vector<InlineRange>,2> _inlineRanges;
		std::shared_ptr<DescriptorArena> _descriptorArena;
		std::shared_ptr<RenderTargetCacheEntry> _renderTarget;
		std::vector<std::shared_ptr<PrivateRenderPipelineState>> _savedPipelineStates;
		std::shared_ptr<DepthStencilState> _depthStencilState;
		std::vector<VkBuffer> _vertexBuffers;
		std::vector<VkDeviceSize> _vertexOffsets;
		VkBuffer _indexBuffer = VK_NULL_HANDLE;
		VkDeviceSize _indexOffset = 0;
		VkIndexType _indexType = VK_INDEX_TYPE_MAX_ENUM;
		// Each texture is listed once per encoder for commit's hazard tracking.
		std::array<std::unordered_set<const Texture*>,2> _trackedTextures; // read-only, read-write
		void trackTexture(const std::shared_ptr<Texture>& texture, bool write);
		// Reuse per-encoder scratch storage instead of allocating on every draw.
		std::vector<uint64_t> _keyScratch;
		std::vector<size_t> _rangeScratch;
		std::vector<std::pair<uint32_t,uint32_t>> _stageOffsetScratch;
		std::vector<VkWriteDescriptorSet> _pushWrites;
		std::vector<VkDescriptorBufferInfo> _pushBuffers;
		std::vector<VkBuffer> _vertexBufferScratch;
		std::vector<VkDeviceSize> _vertexOffsetScratch;
	};
};
