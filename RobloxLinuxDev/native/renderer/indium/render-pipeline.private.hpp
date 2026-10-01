#pragma once

#include <indium/render-pipeline.hpp>
#include <indium/pipeline.private.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <optional>
#include <atomic>
#include <future>
#include <mutex>

namespace Indium {
	class PrivateDevice;
	class PrivateFunction;
	struct FunctionInfo;
	struct ShaderLibrary;

	class PrivateRenderPipelineState: public RenderPipelineState {
		private:
			std::vector<RenderPipelineColorAttachmentDescriptor> _colorAttachments;
			PrimitiveTopologyClass _primitiveTopology;
			std::shared_ptr<PrivateFunction> _vertexFunction;
			std::shared_ptr<PrivateFunction> _fragmentFunction;
			std::optional<VertexDescriptor> _vertexDescriptor;

		public:
			PrivateRenderPipelineState(std::shared_ptr<PrivateDevice> device, const RenderPipelineDescriptor& descriptor);
			~PrivateRenderPipelineState();

			virtual std::shared_ptr<Device> device() override;

			// Safe for concurrent encoders; ready lookups are lock-free. Pipelines
			// use dynamic rendering, so any pass with matching formats may use them.
			VkPipeline pipelineFor(PrimitiveType primitive, bool depthClamp = false);

			const FunctionInfo& vertexFunctionInfo();
			const FunctionInfo& fragmentFunctionInfo();

			INDIUM_PROPERTY_READONLY_OBJECT(PrivateDevice, p, P,rivateDevice);

			// one for each topology class
			using PipelineArray = std::array<VkPipeline, 3>;
			INDIUM_PROPERTY(PipelineArray, p, P,ipelines) {};
			INDIUM_PROPERTY(VkPipelineLayout, p, P,ipelineLayout) = VK_NULL_HANDLE;

			// Sets 0 (device buffer set), 1 (vertex) and 2 (fragment); see pipeline.private.hpp.
			INDIUM_PROPERTY_REF(DescriptorSetLayouts<graphicsSetCount>, d, D,escriptorSetLayouts);

			INDIUM_PROPERTY_READONLY_REF(std::vector<size_t>, v,V,ertexInputBindings);
		private:
			PipelineArray _clampedPipelines {};
			std::array<std::atomic<VkPipeline>, 6> _ready {};
			std::mutex _pipelineMutex;
			std::shared_future<void> _preparation;
			const VkAllocationCallbacks *_hostAllocator = nullptr;
			// Optimized (monolithic) compile. With flags containing
			// FAIL_ON_PIPELINE_COMPILE_REQUIRED it returns null unless cached.
			VkPipeline compilePipeline(PrimitiveType primitive, bool clamp, VkPipelineCreateFlags flags = 0);
			void preparePipeline(const RenderPipelineDescriptor& descriptor);
			// Fast-linked pipeline from shared shader libraries, or null.
			VkPipeline linkPipeline(size_t topology);
			void optimizeLater(size_t topology);
			void fillRenderingInfo(VkPipelineRenderingCreateInfo& info, std::vector<VkFormat>& formats) const;
			PixelFormat _depthFormat = PixelFormat::Invalid, _stencilFormat = PixelFormat::Invalid;
			PipelineArray _linkedPipelines {};
			PipelineArray _vertexInputLibraries {};
			VkPipeline _outputLibrary = VK_NULL_HANDLE;
			std::shared_ptr<ShaderLibrary> _vertexLibrary, _fragmentLibrary;
			std::array<bool, 3> _optimizing {};
			std::vector<std::shared_future<void>> _backgroundCompiles;
			std::atomic<unsigned> _deferredOptimization {0};
	};
};
