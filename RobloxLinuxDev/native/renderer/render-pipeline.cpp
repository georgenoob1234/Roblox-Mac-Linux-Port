#include "../../runtime/profiler/renderer.h"
#include <indium/render-pipeline.private.hpp>
#include <indium/device.private.hpp>
#include <indium/library.private.hpp>
#include <indium/types.private.hpp>
#include <indium/dynamic-vk.hpp>
#include <indium/pipeline-workers.hpp>
#include <indium/graphics-libraries.hpp>

#include <stdexcept>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <unordered_map>

Indium::RenderPipelineState::~RenderPipelineState() {};

namespace {
using namespace Indium;
// Dynamic state, grouped by the pipeline library subset it belongs to.
constexpr VkDynamicState vertexInputDynamic[] = { VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY };
constexpr VkDynamicState preRasterDynamic[] = {
	VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_CULL_MODE,
	VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
	VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
	VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT, // last: only with dynamicDepthClamp
};
constexpr VkDynamicState fragmentDynamic[] = {
	VK_DYNAMIC_STATE_DEPTH_BOUNDS, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
	VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
	VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
	VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
};
constexpr VkDynamicState outputDynamic[] = { VK_DYNAMIC_STATE_BLEND_CONSTANTS };

uint32_t preRasterDynamicCount(const PrivateDevice& device) {
	return std::size(preRasterDynamic) - (device.dynamicDepthClamp ? 0 : 1);
}
bool traceEnabled() {
	static const bool value = std::getenv("RBX_PIPELINE_TRACE") != nullptr;
	return value;
}
double secondsSince(std::chrono::steady_clock::time_point start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

size_t pipelineIndex(PrimitiveType primitive) {
	switch (primitive) {
		case PrimitiveType::Point: return 0;
		case PrimitiveType::Line: case PrimitiveType::LineStrip: return 1;
		case PrimitiveType::Triangle: case PrimitiveType::TriangleStrip: return 2;
		default: throw BadEnumValue();
	}
}
constexpr PrimitiveType topologyPrimitive[] = { PrimitiveType::Point, PrimitiveType::Line, PrimitiveType::Triangle };

// Fixed-function state shared by monolithic pipelines and pipeline libraries.
struct FixedState {
	VkPipelineViewportStateCreateInfo viewport {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	VkPipelineRasterizationStateCreateInfo rasterization {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	VkPipelineMultisampleStateCreateInfo multisample {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	VkPipelineDepthStencilStateCreateInfo depthStencil {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	explicit FixedState(bool clamp) {
		rasterization.depthClampEnable = clamp;
		rasterization.polygonMode = VK_POLYGON_MODE_FILL;
		rasterization.lineWidth = 1.0f;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		multisample.minSampleShading = 1.0f;
	}
};

struct VertexInput {
	std::vector<VkVertexInputBindingDescription> bindings;
	std::vector<VkVertexInputAttributeDescription> attributes;
	VkPipelineVertexInputStateCreateInfo state {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	explicit VertexInput(const std::optional<VertexDescriptor>& descriptor) {
		if (descriptor) {
			// Compact holes between binding indices; the encoder binds the same order.
			std::unordered_map<size_t, uint32_t> metalToVulkan;
			for (const auto& [index, layout]: descriptor->layouts) {
				if (layout.stepRate != 1) throw std::runtime_error("TODO: support step rates other than 1");
				if (layout.stepFunction != VertexStepFunction::PerVertex && layout.stepFunction != VertexStepFunction::PerInstance)
					throw std::runtime_error("TODO: support step functions other than per-vertex and per-instance");
				const uint32_t vulkanIndex = bindings.size();
				metalToVulkan[index] = vulkanIndex;
				bindings.push_back({vulkanIndex, uint32_t(layout.stride), layout.stepFunction == VertexStepFunction::PerVertex ? VK_VERTEX_INPUT_RATE_VERTEX : VK_VERTEX_INPUT_RATE_INSTANCE});
			}
			for (const auto& [index, attribute]: descriptor->attributes)
				attributes.push_back({uint32_t(index), metalToVulkan[attribute.bufferIndex], vertexFormatToVkFormat(attribute.format), uint32_t(attribute.offset)});
		}
		state.vertexBindingDescriptionCount = bindings.size();
		state.pVertexBindingDescriptions = bindings.data();
		state.vertexAttributeDescriptionCount = attributes.size();
		state.pVertexAttributeDescriptions = attributes.data();
	}
};

struct ColorBlend {
	std::vector<VkPipelineColorBlendAttachmentState> attachments;
	VkPipelineColorBlendStateCreateInfo state {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	explicit ColorBlend(const std::vector<RenderPipelineColorAttachmentDescriptor>& colors) {
		for (const auto& color : colors) {
			VkPipelineColorBlendAttachmentState blend {};
			blend.blendEnable = color.blendingEnabled;
			blend.srcColorBlendFactor = blendFactorToVkBlendFactor(color.sourceRGBBlendFactor);
			blend.dstColorBlendFactor = blendFactorToVkBlendFactor(color.destinationRGBBlendFactor);
			blend.colorBlendOp = blendOperationToVkBlendOp(color.rgbBlendOperation);
			blend.srcAlphaBlendFactor = blendFactorToVkBlendFactor(color.sourceAlphaBlendFactor);
			blend.dstAlphaBlendFactor = blendFactorToVkBlendFactor(color.destinationAlphaBlendFactor);
			blend.alphaBlendOp = blendOperationToVkBlendOp(color.alphaBlendOperation);
			blend.colorWriteMask = colorWriteMaskToVkColorComponentFlags(color.writeMask);
			attachments.push_back(blend);
		}
		state.attachmentCount = attachments.size();
		state.pAttachments = attachments.data();
	}
};
}

//
// shared shader libraries
//

Indium::ShaderLibrary::~ShaderLibrary() {
	if (pipeline) DynamicVK::vkDestroyPipeline(device, pipeline, nullptr);
}

std::shared_ptr<Indium::ShaderLibrary> Indium::GraphicsLibraries::entry(VkShaderModule module, FunctionType stage) {
	std::lock_guard lock(_mutex);
	auto& library = _libraries[{module, int(stage)}];
	if (!library) library = std::make_shared<ShaderLibrary>();
	return library;
}

void Indium::GraphicsLibraries::compile(ShaderLibrary& library, VkShaderModule module, const FunctionInfo* info, FunctionType stage) {
	RbxProfiler::Scope profile(RBX_PROF_GRAPHICS_PIPELINE);
	const auto started = std::chrono::steady_clock::now();
	library.device = _device.device();
	const bool vertex = stage == FunctionType::Vertex;
	// Only this stage's own set; the other stage's set stays undefined.
	VkDescriptorSetLayout own = createFunctionSetLayout(_device, info);
	std::array<VkDescriptorSetLayout, graphicsSetCount> sets { _device.bufferSetLayout, VK_NULL_HANDLE, VK_NULL_HANDLE };
	sets[stageSet(stage)] = own;
	VkPipelineLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layoutInfo.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
	layoutInfo.setLayoutCount = sets.size();
	layoutInfo.pSetLayouts = sets.data();
	VkPipelineLayout layout = VK_NULL_HANDLE;
	if (DynamicVK::vkCreatePipelineLayout(library.device, &layoutInfo, nullptr, &layout) == VK_SUCCESS) {
		VkGraphicsPipelineLibraryCreateInfoEXT part {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
		part.flags = vertex ? VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT : VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
		VkPipelineRenderingCreateInfo rendering {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		rendering.pNext = &part;
		VkPipelineShaderStageCreateInfo shader {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
		shader.stage = vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
		shader.module = module;
		shader.pName = "main";
		FixedState fixed(false);
		VkPipelineDynamicStateCreateInfo dynamic {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		VkGraphicsPipelineCreateInfo create {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		create.pNext = &rendering;
		create.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
		create.stageCount = module ? 1 : 0;
		create.pStages = &shader;
		create.layout = layout;
		create.pDynamicState = &dynamic;
		if (vertex) {
			create.pViewportState = &fixed.viewport;
			create.pRasterizationState = &fixed.rasterization;
			dynamic.dynamicStateCount = preRasterDynamicCount(_device);
			dynamic.pDynamicStates = preRasterDynamic;
		} else {
			create.pMultisampleState = &fixed.multisample;
			create.pDepthStencilState = &fixed.depthStencil;
			dynamic.dynamicStateCount = std::size(fragmentDynamic);
			dynamic.pDynamicStates = fragmentDynamic;
		}
		if (DynamicVK::vkCreateGraphicsPipelines(library.device, _device.pipelineCache, 1, &create, nullptr, &library.pipeline) != VK_SUCCESS)
			library.pipeline = VK_NULL_HANDLE;
		DynamicVK::vkDestroyPipelineLayout(library.device, layout, nullptr);
	}
	DynamicVK::vkDestroyDescriptorSetLayout(library.device, own, nullptr);
	if (traceEnabled())
		std::fprintf(stderr, "PIPELINE library %s %.3fs%s\n", vertex ? "vertex" : "fragment", secondsSince(started), library.pipeline ? "" : " FAILED");
}

std::shared_ptr<Indium::ShaderLibrary> Indium::GraphicsLibraries::get(VkShaderModule module, const FunctionInfo& info) {
	auto library = entry(module, info.functionType);
	std::call_once(library->once, [&] { compile(*library, module, &info, info.functionType); });
	return library;
}

std::shared_ptr<Indium::ShaderLibrary> Indium::GraphicsLibraries::emptyFragment() {
	std::shared_ptr<ShaderLibrary> library;
	{
		std::lock_guard lock(_mutex);
		if (!_emptyFragment) _emptyFragment = std::make_shared<ShaderLibrary>();
		library = _emptyFragment;
	}
	std::call_once(library->once, [&] { compile(*library, VK_NULL_HANDLE, nullptr, FunctionType::Fragment); });
	return library;
}

void Indium::GraphicsLibraries::prepare(VkShaderModule module, const FunctionInfo& info) {
	if (!isGraphicsStage(info.functionType) || !PipelineWorkers::count() || std::getenv("RBX_NO_LIBRARY_PRECOMPILE")) return;
	std::weak_ptr<ShaderLibrary> weak = entry(module, info.functionType);
	FunctionInfo copy;
	copy.functionType = info.functionType;
	copy.bindings = info.bindings;
	PipelineWorkers::shared().submitBackground([self = std::weak_ptr<GraphicsLibraries>(shared_from_this()), weak, module, copy] {
		auto libraries = self.lock();
		if (!libraries) return;
		{
			std::lock_guard lock(libraries->_mutex);
			if (libraries->_closed) return;
			++libraries->_running;
		}
		// An expired entry was released with its module; release() also
		// cancels the build, so a running compile never outlives the module.
		if (auto library = weak.lock()) {
			try { std::call_once(library->once, [&] { libraries->compile(*library, module, &copy, copy.functionType); }); }
			catch (...) {}
		}
		libraries->_device.savePipelineCache();
		{
			std::lock_guard lock(libraries->_mutex);
			--libraries->_running;
		}
		libraries->_idle.notify_all();
	});
}

void Indium::GraphicsLibraries::release(VkShaderModule module) {
	std::vector<std::shared_ptr<ShaderLibrary>> removed;
	{
		std::lock_guard lock(_mutex);
		for (auto stage : {FunctionType::Vertex, FunctionType::Fragment}) {
			auto found = _libraries.find({module, int(stage)});
			if (found == _libraries.end()) continue;
			removed.push_back(std::move(found->second));
			_libraries.erase(found);
		}
	}
	// Cancels a queued build, or waits for a running one to finish.
	for (auto& library : removed) std::call_once(library->once, [] {});
}

void Indium::GraphicsLibraries::close() {
	std::unique_lock lock(_mutex);
	_closed = true;
	_idle.wait(lock, [&] { return _running == 0; });
	_libraries.clear();
	_emptyFragment.reset();
}

//
// pipeline state
//

std::shared_ptr<Indium::Device> Indium::PrivateRenderPipelineState::device() {
	return _privateDevice;
};

Indium::PrivateRenderPipelineState::PrivateRenderPipelineState(std::shared_ptr<PrivateDevice> device, const RenderPipelineDescriptor& descriptor):
	_privateDevice(device),
	_descriptorSetLayouts(_privateDevice)
{
	_hostAllocator=static_cast<const VkAllocationCallbacks*>(RbxProfiler::allocator(RBX_PROF_GRAPHICS_PIPELINE));
	for (auto& ready : _ready) ready.store(VK_NULL_HANDLE, std::memory_order_relaxed);
	_colorAttachments = descriptor.colorAttachments;
	// Accessing an unused Metal attachment creates a descriptor with Invalid format.
	while (!_colorAttachments.empty() && _colorAttachments.back().pixelFormat == PixelFormat::Invalid)
		_colorAttachments.pop_back();
	_vertexDescriptor = descriptor.vertexDescriptor;
	_primitiveTopology = descriptor.inputPrimitiveTopology;
	_depthFormat = descriptor.depthAttachmentPixelFormat;
	_stencilFormat = descriptor.stencilAttachmentPixelFormat;

	// Every Indium function is a PrivateFunction.
	_vertexFunction = std::static_pointer_cast<PrivateFunction>(descriptor.vertexFunction);
	_fragmentFunction = std::static_pointer_cast<PrivateFunction>(descriptor.fragmentFunction);

	_descriptorSetLayouts.processFunction(_vertexFunction, 1);
	_descriptorSetLayouts.processFunction(_fragmentFunction, 2);
	// Published once with the PSO; compiling another topology must not resize a
	// vector that a concurrent render encoder is already reading.
	if (_vertexDescriptor)
		for (const auto& entry : _vertexDescriptor->layouts) _vertexInputBindings.push_back(entry.first);

	VkPipelineLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	// Linked and optimized pipelines share this layout, so descriptors bound for
	// one stay valid when the other replaces it mid-encoder.
	if (_privateDevice->pipelineLibraries) layoutInfo.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
	layoutInfo.setLayoutCount = _descriptorSetLayouts.layouts.size();
	layoutInfo.pSetLayouts = _descriptorSetLayouts.layouts.data();
	if (DynamicVK::vkCreatePipelineLayout(_privateDevice->device(), &layoutInfo, _hostAllocator, &_pipelineLayout) != VK_SUCCESS)
		throw std::runtime_error("Could not create render pipeline layout");
	preparePipeline(descriptor);
};

Indium::PrivateRenderPipelineState::~PrivateRenderPipelineState() {
	// Queued tasks own no naked resource past these waits. They may be
	// compiling a PSO that was released without ever being drawn.
	if (_preparation.valid()) _preparation.wait();
	std::vector<std::shared_future<void>> compiles;
	{
		std::lock_guard lock(_pipelineMutex);
		compiles = _backgroundCompiles;
	}
	for (auto& compile : compiles) compile.wait();
	const auto device = _privateDevice->device();
	for (auto& variants : { _pipelines, _clampedPipelines, _linkedPipelines, _vertexInputLibraries })
		for (auto pipeline : variants) DynamicVK::vkDestroyPipeline(device, pipeline, _hostAllocator);
	DynamicVK::vkDestroyPipeline(device, _outputLibrary, _hostAllocator);
	if (_pipelineLayout) {
		DynamicVK::vkDestroyPipelineLayout(device, _pipelineLayout, _hostAllocator);
		_pipelineLayout = VK_NULL_HANDLE;
	}
};

void Indium::PrivateRenderPipelineState::fillRenderingInfo(VkPipelineRenderingCreateInfo& info, std::vector<VkFormat>& formats) const {
	formats.clear();
	for (const auto& color : _colorAttachments)
		formats.push_back(color.pixelFormat == PixelFormat::Invalid ? VK_FORMAT_UNDEFINED : pixelFormatToVkFormat(color.pixelFormat));
	info = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	info.colorAttachmentCount = formats.size();
	info.pColorAttachmentFormats = formats.data();
	// Metal attaches one depth/stencil texture; both aspects of a combined
	// format are attached, exactly as the render encoder begins rendering.
	const auto depthStencil = _depthFormat != PixelFormat::Invalid ? _depthFormat : _stencilFormat;
	if (depthStencil != PixelFormat::Invalid) {
		const auto aspects = pixelFormatToVkImageAspectFlags(depthStencil);
		if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) info.depthAttachmentFormat = pixelFormatToVkFormat(depthStencil);
		if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) info.stencilAttachmentFormat = pixelFormatToVkFormat(depthStencil);
	}
}

void Indium::PrivateRenderPipelineState::preparePipeline(const RenderPipelineDescriptor& descriptor) {
	if (!PipelineWorkers::count()) return;
	// Triangle is the normal Metal draw topology; Point/Line and depth clamp
	// remain demand-compiled. Do not compile a point variant without PointSize.
	if (_primitiveTopology != PrimitiveTopologyClass::Unspecified && _primitiveTopology != PrimitiveTopologyClass::Triangle) return;
	if (descriptor.rasterSampleCount > 1) return; // encoder currently supports single sample only
	for (const auto& color : _colorAttachments) if (color.pixelFormat == PixelFormat::Invalid) return;
	const auto depth = descriptor.depthAttachmentPixelFormat;
	const auto stencil = descriptor.stencilAttachmentPixelFormat;
	if (depth != PixelFormat::Invalid && stencil != PixelFormat::Invalid && depth != stencil) return;
	_preparation = PipelineWorkers::shared().submit(std::packaged_task<void()>([this] {
		// A pipeline cached by an earlier session needs no compile at all.
		if (compilePipeline(PrimitiveType::Triangle, false, VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT)) return;
		// Usable within about a millisecond when its shaders are already built;
		// the optimized pipeline below replaces it for later draws.
		if (_privateDevice->pipelineLibraries) linkPipeline(2);
		compilePipeline(PrimitiveType::Triangle, false);
		_privateDevice->savePipelineCache();
	}));
}

VkPipeline Indium::PrivateRenderPipelineState::pipelineFor(PrimitiveType primitive, bool depthClamp) {
	const auto topology = pipelineIndex(primitive);
	const bool clamp = depthClamp && !_privateDevice->dynamicDepthClamp;
	const auto index = topology + (clamp ? 3 : 0);
	if (auto ready = _ready[index].load(std::memory_order_acquire)) {
		if (index < 3 && (_deferredOptimization.load(std::memory_order_relaxed) & (1u << index))) optimizeLater(index);
		return ready;
	}
	RbxProfiler::Scope wait(RBX_PROF_PIPELINE_WAIT);
	if (!clamp) {
		if (auto cached = compilePipeline(primitive, false, VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT)) return cached;
		if (_privateDevice->pipelineLibraries) {
			if (auto linked = linkPipeline(topology)) { optimizeLater(topology); return linked; }
		}
	}
	if (index == 2 && _preparation.valid()) {
		_preparation.get();
		if (auto ready = _ready[index].load(std::memory_order_acquire)) return ready;
	}
	return compilePipeline(primitive, clamp);
}

void Indium::PrivateRenderPipelineState::optimizeLater(size_t topology) {
	{
		std::lock_guard lock(_pipelineMutex);
		// The preparation task already optimizes the triangle variant.
		if (_optimizing[topology] || !PipelineWorkers::count() || (topology == 2 && _preparation.valid())) return;
		_optimizing[topology] = true;
	}
	// A full queue skips optional optimization; keep the linked pipeline usable.
	const auto primitive = topologyPrimitive[topology];
	auto compile = PipelineWorkers::shared().submit(std::packaged_task<void()>([this, primitive] {
		compilePipeline(primitive, false);
		_privateDevice->savePipelineCache();
	}));
	std::lock_guard lock(_pipelineMutex);
	if (compile.valid()) {
		_backgroundCompiles.push_back(compile.share());
		_deferredOptimization.fetch_and(~(1u << topology), std::memory_order_relaxed);
	} else {
		_optimizing[topology] = false;
		// A linked pipeline is already usable; retry optional optimization on
		// its next use without locking the normal ready-pipeline path.
		_deferredOptimization.fetch_or(1u << topology, std::memory_order_relaxed);
	}
}

VkPipeline Indium::PrivateRenderPipelineState::linkPipeline(size_t topology) {
	auto& libraries = *_privateDevice->graphicsLibraries;
	// Shared shader libraries are built outside this pipeline's lock.
	auto vertex = libraries.get(_vertexFunction->library()->shaderModule(), _vertexFunction->functionInfo());
	auto fragment = _fragmentFunction ? libraries.get(_fragmentFunction->library()->shaderModule(), _fragmentFunction->functionInfo()) : libraries.emptyFragment();
	if (!vertex->pipeline || !fragment->pipeline) return VK_NULL_HANDLE;

	std::lock_guard lock(_pipelineMutex);
	if (_linkedPipelines[topology]) return _ready[topology].load(std::memory_order_acquire);
	const auto started = std::chrono::steady_clock::now();
	const auto device = _privateDevice->device();
	_vertexLibrary = vertex;
	_fragmentLibrary = fragment;
	VkGraphicsPipelineLibraryCreateInfoEXT part {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
	if (!_vertexInputLibraries[topology]) {
		VertexInput input(_vertexDescriptor);
		VkPipelineInputAssemblyStateCreateInfo assembly {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		assembly.topology = primitiveTypeToVkPrimitiveTopology(topologyPrimitive[topology]);
		VkPipelineDynamicStateCreateInfo dynamic {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dynamic.dynamicStateCount = std::size(vertexInputDynamic);
		dynamic.pDynamicStates = vertexInputDynamic;
		part.flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
		VkGraphicsPipelineCreateInfo create {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		create.pNext = &part;
		create.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
		create.pVertexInputState = &input.state;
		create.pInputAssemblyState = &assembly;
		create.pDynamicState = &dynamic;
		if (DynamicVK::vkCreateGraphicsPipelines(device, _privateDevice->pipelineCache, 1, &create, _hostAllocator, &_vertexInputLibraries[topology]) != VK_SUCCESS)
			_vertexInputLibraries[topology] = VK_NULL_HANDLE;
	}
	if (!_outputLibrary) {
		std::vector<VkFormat> formats;
		VkPipelineRenderingCreateInfo rendering;
		fillRenderingInfo(rendering, formats);
		part.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
		rendering.pNext = &part;
		ColorBlend blend(_colorAttachments);
		FixedState fixed(false);
		VkPipelineDynamicStateCreateInfo dynamic {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dynamic.dynamicStateCount = std::size(outputDynamic);
		dynamic.pDynamicStates = outputDynamic;
		VkGraphicsPipelineCreateInfo create {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		create.pNext = &rendering;
		create.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
		create.pColorBlendState = &blend.state;
		create.pMultisampleState = &fixed.multisample;
		create.pDynamicState = &dynamic;
		if (DynamicVK::vkCreateGraphicsPipelines(device, _privateDevice->pipelineCache, 1, &create, _hostAllocator, &_outputLibrary) != VK_SUCCESS)
			_outputLibrary = VK_NULL_HANDLE;
	}
	if (!_vertexInputLibraries[topology] || !_outputLibrary) return VK_NULL_HANDLE;
	const VkPipeline parts[] = { _vertexInputLibraries[topology], vertex->pipeline, fragment->pipeline, _outputLibrary };
	VkPipelineLibraryCreateInfoKHR link {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
	link.libraryCount = std::size(parts);
	link.pLibraries = parts;
	VkGraphicsPipelineCreateInfo create {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	create.pNext = &link;
	create.layout = _pipelineLayout;
	VkPipeline linked = VK_NULL_HANDLE;
	if (DynamicVK::vkCreateGraphicsPipelines(device, _privateDevice->pipelineCache, 1, &create, _hostAllocator, &linked) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	_linkedPipelines[topology] = linked;
	// Never replace an optimized pipeline that is already published.
	VkPipeline expected = VK_NULL_HANDLE;
	_ready[topology].compare_exchange_strong(expected, linked, std::memory_order_acq_rel);
	if (traceEnabled())
		std::fprintf(stderr, "PIPELINE linked %.3fs %s / %s\n", secondsSince(started), _vertexFunction->name().c_str(),
			_fragmentFunction ? _fragmentFunction->name().c_str() : "<depth-only>");
	return _ready[topology].load(std::memory_order_acquire);
}

VkPipeline Indium::PrivateRenderPipelineState::compilePipeline(PrimitiveType primitive, bool depthClamp, VkPipelineCreateFlags flags) {
	const auto index = pipelineIndex(primitive);
	{
		std::lock_guard lock(_pipelineMutex);
		auto& pipelines = depthClamp ? _clampedPipelines : _pipelines;
		// Compile only topologies actually drawn; triangle shaders need not write PointSize.
		if (pipelines[index]) return pipelines[index];
	}

	const bool probe = flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
	auto vertexFunctionName = _vertexFunction->name();
	auto fragmentFunctionName = _fragmentFunction ? _fragmentFunction->name() : "<depth-only>";
	std::vector<VkPipelineShaderStageCreateInfo> stages;

	VkPipelineShaderStageCreateInfo shaderStage {};
	shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStage.module = _vertexFunction->library()->shaderModule();
	shaderStage.pName = "main";
	stages.push_back(shaderStage);
	if (_fragmentFunction) {
		shaderStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		shaderStage.module = _fragmentFunction->library()->shaderModule();
		stages.push_back(shaderStage);
	}

	VertexInput input(_vertexDescriptor);
	VkPipelineInputAssemblyStateCreateInfo inputAssemblyState {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	inputAssemblyState.topology = primitiveTypeToVkPrimitiveTopology(primitive);
	VkPipelineTessellationStateCreateInfo tesselationState {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
	FixedState fixed(depthClamp);
	ColorBlend blend(_colorAttachments);

	std::vector<VkDynamicState> dynamicStates(std::begin(vertexInputDynamic), std::end(vertexInputDynamic));
	dynamicStates.insert(dynamicStates.end(), preRasterDynamic, preRasterDynamic + preRasterDynamicCount(*_privateDevice));
	dynamicStates.insert(dynamicStates.end(), std::begin(fragmentDynamic), std::end(fragmentDynamic));
	dynamicStates.insert(dynamicStates.end(), std::begin(outputDynamic), std::end(outputDynamic));
	VkPipelineDynamicStateCreateInfo dynamicState {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dynamicState.dynamicStateCount = dynamicStates.size();
	dynamicState.pDynamicStates = dynamicStates.data();

	std::vector<VkFormat> formats;
	VkPipelineRenderingCreateInfo rendering;
	fillRenderingInfo(rendering, formats);

	VkGraphicsPipelineCreateInfo pipelineCreateInfo {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pipelineCreateInfo.pNext = &rendering;
	pipelineCreateInfo.flags = flags;
	pipelineCreateInfo.stageCount = stages.size();
	pipelineCreateInfo.pStages = stages.data();
	pipelineCreateInfo.pVertexInputState = &input.state;
	pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
	pipelineCreateInfo.pTessellationState = &tesselationState;
	pipelineCreateInfo.pViewportState = &fixed.viewport;
	pipelineCreateInfo.pRasterizationState = &fixed.rasterization;
	pipelineCreateInfo.pMultisampleState = &fixed.multisample;
	pipelineCreateInfo.pDepthStencilState = &fixed.depthStencil;
	pipelineCreateInfo.pColorBlendState = &blend.state;
	pipelineCreateInfo.pDynamicState = &dynamicState;
	pipelineCreateInfo.layout = _pipelineLayout;

	std::optional<RbxProfiler::Scope> profile;
	if (!probe) profile.emplace(RBX_PROF_GRAPHICS_PIPELINE);
	const auto started = std::chrono::steady_clock::now();
	if (traceEnabled() && !probe) std::fprintf(stderr, "PIPELINE begin %s / %s\n", vertexFunctionName.c_str(), fragmentFunctionName.c_str());
	VkPipeline compiled = VK_NULL_HANDLE;
	const auto result = DynamicVK::vkCreateGraphicsPipelines(_privateDevice->device(), _privateDevice->pipelineCache, 1, &pipelineCreateInfo, _hostAllocator, &compiled);
	if (result != VK_SUCCESS) {
		if (compiled) DynamicVK::vkDestroyPipeline(_privateDevice->device(), compiled, _hostAllocator);
		// VK_PIPELINE_COMPILE_REQUIRED: not cached yet, and the caller asked not to compile.
		if (probe) return VK_NULL_HANDLE;
		throw std::runtime_error("Could not create render pipeline");
	}
	if (traceEnabled())
		std::fprintf(stderr, "PIPELINE %s %.3fs %s / %s\n", probe ? "cached" : "done", secondsSince(started), vertexFunctionName.c_str(), fragmentFunctionName.c_str());
	std::lock_guard lock(_pipelineMutex);
	auto& pipelines = depthClamp ? _clampedPipelines : _pipelines;
	if (pipelines[index]) {
		// Another thread published the same variant first.
		DynamicVK::vkDestroyPipeline(_privateDevice->device(), compiled, _hostAllocator);
		return pipelines[index];
	}
	pipelines[index] = compiled;
	_ready[index + (depthClamp ? 3 : 0)].store(compiled, std::memory_order_release);
	return compiled;
};

const Indium::FunctionInfo& Indium::PrivateRenderPipelineState::vertexFunctionInfo() {
	return _vertexFunction->functionInfo();
};

const Indium::FunctionInfo& Indium::PrivateRenderPipelineState::fragmentFunctionInfo() {
	static const FunctionInfo empty{};
	return _fragmentFunction ? _fragmentFunction->functionInfo() : empty;
};
