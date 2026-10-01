#include "../../runtime/profiler/renderer.h"
#include <indium/compute-pipeline.private.hpp>
#include <indium/device.private.hpp>
#include <stdexcept>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <indium/dynamic-vk.hpp>

Indium::ComputePipelineState::~ComputePipelineState() {};

Indium::PrivateComputePipelineState::PrivateComputePipelineState(std::shared_ptr<PrivateDevice> device, const ComputePipelineDescriptor& descriptor):
	_privateDevice(device),
	_descriptor(descriptor),
	_descriptorSetLayouts(_privateDevice)
{
	_hostAllocator=static_cast<const VkAllocationCallbacks*>(RbxProfiler::allocator(RBX_PROF_COMPUTE_PIPELINE));
	if (_descriptor.stageInputDescriptor) {
		// TODO
		//
		// Vulkan doesn't support stage-in/vertex-buffer parameters in compute shaders,
		// so we'll have to emulate it by passing the binding info to the shader and
		// having it compute the right addresses on its own
		throw std::runtime_error("TODO: support stage-in parameters in compute shaders");
	}

	_descriptorSetLayouts.processFunction(std::static_pointer_cast<PrivateFunction>(_descriptor.computeFunction), 0);

	VkPushConstantRange dispatchRange { VK_SHADER_STAGE_COMPUTE_BIT, 0, 48 };
	VkPipelineLayoutCreateInfo layoutInfo {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = _descriptorSetLayouts.layouts.size();
	layoutInfo.pSetLayouts = _descriptorSetLayouts.layouts.data();
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &dispatchRange;

	if (DynamicVK::vkCreatePipelineLayout(_privateDevice->device(), &layoutInfo, _hostAllocator, &_layout) != VK_SUCCESS) {
		// TODO
		abort();
	}

	// determine device properties
	VkPhysicalDeviceVulkan13Properties vk13Props {};
	vk13Props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
	VkPhysicalDeviceProperties2 props {};
	props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props.pNext = &vk13Props;
	DynamicVK::vkGetPhysicalDeviceProperties2(_privateDevice->physicalDevice(), &props);

	_maxTotalThreadsPerThreadgroup = vk13Props.maxSubgroupSize;
	_threadExecutionWidth = props.properties.limits.maxComputeWorkGroupInvocations;
	_staticThreadgroupMemoryLength = props.properties.limits.maxComputeSharedMemorySize; // not entirely sure about this one, but it seems close enough
};

Indium::PrivateComputePipelineState::~PrivateComputePipelineState() {
	for (const auto& entry: _pipelines)
		DynamicVK::vkDestroyPipeline(_privateDevice->device(), entry.second, _hostAllocator);
	if (_layout) {
		DynamicVK::vkDestroyPipelineLayout(_privateDevice->device(), _layout, _hostAllocator);
	}
};

VkPipeline Indium::PrivateComputePipelineState::createPipeline(Size threadsPerThreadgroup) {
	VkPipeline pipeline = VK_NULL_HANDLE;

	auto func = std::static_pointer_cast<PrivateFunction>(_descriptor.computeFunction);
	auto funcName = func->name();

	std::array<uint32_t, 3> threadsPerThreadgroupData = { static_cast<uint32_t>(threadsPerThreadgroup.width), static_cast<uint32_t>(threadsPerThreadgroup.height), static_cast<uint32_t>(threadsPerThreadgroup.depth) };
	std::promise<VkPipeline> completion;
	std::shared_future<VkPipeline> pending;
	{
		std::lock_guard<std::mutex> lock(_pipelineMutex);
		const auto cached = _pipelines.find(threadsPerThreadgroupData);
		if (cached != _pipelines.end()) return cached->second;
		const auto existing = _preparing.find(threadsPerThreadgroupData);
		if (existing != _preparing.end()) pending = existing->second;
		else _preparing.emplace(threadsPerThreadgroupData, completion.get_future().share());
	}
	// Same specialization shares one result; independent specializations compile
	// on their requesting threads without holding the PSO's cache mutex.
	if (pending.valid()) return pending.get();
	try {

	std::array<VkSpecializationMapEntry, 3> mapEntries {};

	for (size_t i = 0; i < mapEntries.size(); ++i) {
		mapEntries[i].constantID = i;
		mapEntries[i].offset = i * sizeof(uint32_t);
		mapEntries[i].size = sizeof(uint32_t);
	}

	VkSpecializationInfo specInfo {};
	specInfo.mapEntryCount = mapEntries.size();
	specInfo.pMapEntries = mapEntries.data();
	specInfo.dataSize = threadsPerThreadgroupData.size() * sizeof(*threadsPerThreadgroupData.data());
	specInfo.pData = threadsPerThreadgroupData.data();

	VkComputePipelineCreateInfo info {};
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = func->library()->shaderModule();
	info.stage.pName = "main";
	info.stage.pSpecializationInfo = &specInfo;
	info.layout = _layout;

	RbxProfiler::Scope profile(RBX_PROF_COMPUTE_PIPELINE);
	const bool trace = std::getenv("RBX_PIPELINE_TRACE") != nullptr;
	const auto started = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	if (trace) std::fprintf(stderr, "COMPUTE begin %s %u,%u,%u\n", funcName.c_str(), threadsPerThreadgroupData[0], threadsPerThreadgroupData[1], threadsPerThreadgroupData[2]);
	if (DynamicVK::vkCreateComputePipelines(_privateDevice->device(), _privateDevice->pipelineCache, 1, &info, _hostAllocator, &pipeline) != VK_SUCCESS) {
		throw std::runtime_error("Could not create compute pipeline");
	}

	if (trace) std::fprintf(stderr, "COMPUTE done %.3fs\n", std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count());
	{
		std::lock_guard<std::mutex> lock(_pipelineMutex);
		_pipelines.emplace(threadsPerThreadgroupData, pipeline);
		_preparing.erase(threadsPerThreadgroupData);
	}
	completion.set_value(pipeline);
	return pipeline;
	} catch (...) {
		completion.set_exception(std::current_exception());
		if (pipeline) DynamicVK::vkDestroyPipeline(_privateDevice->device(), pipeline, _hostAllocator);
		std::lock_guard<std::mutex> lock(_pipelineMutex);
		_preparing.erase(threadsPerThreadgroupData);
		throw;
	}
};

const Indium::FunctionInfo& Indium::PrivateComputePipelineState::functionInfo() const {
	// Every Indium function is a PrivateFunction; runs per dispatch.
	return static_cast<PrivateFunction*>(_descriptor.computeFunction.get())->functionInfo();
};

std::shared_ptr<Indium::Device> Indium::PrivateComputePipelineState::device() {
	return _privateDevice;
};

size_t Indium::PrivateComputePipelineState::imageblockMemoryLength(Size dimensions) {
	throw std::runtime_error("TODO");
};

std::shared_ptr<Indium::FunctionHandle> Indium::PrivateComputePipelineState::functionHandle(std::shared_ptr<Function> function) {
	throw std::runtime_error("TODO");
};

std::shared_ptr<Indium::ComputePipelineState> Indium::PrivateComputePipelineState::newComputePipelineState(const std::vector<std::shared_ptr<Function>>& functions) {
	throw std::runtime_error("TODO");
};

std::shared_ptr<Indium::VisibleFunctionTable> Indium::PrivateComputePipelineState::newVisibleFunctionTable(const VisibleFunctionTableDescriptor& descriptor) {
	throw std::runtime_error("TODO");
};

std::shared_ptr<Indium::IntersectionFunctionTable> Indium::PrivateComputePipelineState::newIntersectionFunctionTable(const IntersectionFunctionTableDescriptor& descriptor) {
	throw std::runtime_error("TODO");
};
