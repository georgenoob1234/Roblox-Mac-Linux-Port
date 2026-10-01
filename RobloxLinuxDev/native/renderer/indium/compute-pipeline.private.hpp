#pragma once

#include <indium/compute-pipeline.hpp>
#include <indium/pipeline.private.hpp>
#include <map>
#include <mutex>
#include <future>

namespace Indium {
	class PrivateDevice;

	class PrivateComputePipelineState: public ComputePipelineState {
	private:
		std::shared_ptr<PrivateDevice> _privateDevice;
		ComputePipelineDescriptor _descriptor;

	public:
		PrivateComputePipelineState(std::shared_ptr<PrivateDevice> device, const ComputePipelineDescriptor& descriptor);
		~PrivateComputePipelineState();

		// Specialization depends on local size, not on dispatch count or grid size.
		VkPipeline createPipeline(Size threadsPerThreadgroup);

		const FunctionInfo& functionInfo() const;

		virtual std::shared_ptr<Device> device() override;
		virtual size_t imageblockMemoryLength(Size dimensions) override;
		virtual std::shared_ptr<FunctionHandle> functionHandle(std::shared_ptr<Function> function) override;
		virtual std::shared_ptr<ComputePipelineState> newComputePipelineState(const std::vector<std::shared_ptr<Function>>& functions) override;
		virtual std::shared_ptr<VisibleFunctionTable> newVisibleFunctionTable(const VisibleFunctionTableDescriptor& descriptor) override;
		virtual std::shared_ptr<IntersectionFunctionTable> newIntersectionFunctionTable(const IntersectionFunctionTableDescriptor& descriptor) override;

		INDIUM_PROPERTY_REF(DescriptorSetLayouts<1>, d,D,escriptorSetLayouts);
		INDIUM_PROPERTY_READONLY(VkPipelineLayout, l,L,ayout) = VK_NULL_HANDLE;
	private:
		// Append state to preserve prebuilt Darling's inline accessor offsets.
		std::mutex _pipelineMutex;
		std::map<std::array<uint32_t, 3>, VkPipeline> _pipelines;
		std::map<std::array<uint32_t, 3>, std::shared_future<VkPipeline>> _preparing;
		const VkAllocationCallbacks *_hostAllocator = nullptr;
	};
};
