#include "../../runtime/profiler/renderer.h"
#include <indium/library.private.hpp>
#include <indium/device.private.hpp>
#include <indium/sampler.hpp>
#include <indium/dynamic-vk.hpp>
#include <indium/pipeline.private.hpp>
#include <indium/graphics-libraries.hpp>
#include <cstring>
#include <unordered_map>

// libmetalgaps constructs PrivateLibrary using the original public build's
// private header. Keep that object layout intact across the dylib boundary.
#ifdef __APPLE__
static_assert(sizeof(Indium::PrivateLibrary)==88, "PrivateLibrary ABI changed; rebuild every allocating bridge");
#endif
namespace {
// Moves a graphics module's descriptor decorations to the set scheme in
// pipeline.private.hpp: pushed buffers to set 0 (fragment ones at 16 + b),
// everything else to the stage's own set. False for an unexpected module.
bool remapDescriptorSets(std::vector<uint32_t>& words, const Indium::FunctionInfo& info, const Indium::PrivateDevice& device) {
	if (words.size() < 5 || words[0] != 0x07230203) return false;
	struct Target { size_t set = 0, binding = 0; }; // word indices of the literals
	std::unordered_map<uint32_t, Target> targets;
	unsigned entryPoints = 0;
	for (size_t i = 5; i < words.size();) {
		const uint32_t opcode = words[i] & 0xffff, count = words[i] >> 16;
		if (!count || i + count > words.size()) return false;
		if (opcode == 15) ++entryPoints; // OpEntryPoint
		// Decoration groups (OpDecorationGroup, OpGroupDecorate, OpGroupMemberDecorate)
		// could carry descriptor decorations out of reach; no translator emits them.
		if (opcode == 73 || opcode == 74 || opcode == 75) return false;
		if (opcode == 71 && count == 4) { // OpDecorate %id DescriptorSet|Binding literal
			if (words[i + 2] == 34) targets[words[i + 1]].set = i + 3;
			if (words[i + 2] == 33) targets[words[i + 1]].binding = i + 3;
		}
		i += count;
	}
	if (entryPoints != 1) return false;
	const bool pushed = Indium::pushesBuffers(info, device);
	for (const auto& [id, target] : targets) {
		if (!target.set || !target.binding) continue;
		const Iridium::BindingInfo* binding = nullptr;
		for (const auto& candidate : info.bindings)
			if (candidate.internalIndex == words[target.binding] && candidate.type != Iridium::BindingType::VertexInput) { binding = &candidate; break; }
		if (!binding) continue; // never bound by Indium
		if (binding->type == Iridium::BindingType::Buffer && pushed) {
			words[target.set] = 0;
			words[target.binding] = Indium::pushedBufferBinding(info, *binding);
		} else words[target.set] = Indium::stageSet(info.functionType);
	}
	return true;
}
}

static const VkAllocationCallbacks *shaderAllocator() {
	// One immutable callback set keeps create/destroy paired without a field
	// in objects allocated by another dylib (also stable if profiling is absent).
	static auto *callbacks=static_cast<const VkAllocationCallbacks*>(RbxProfiler::allocator(RBX_PROF_SHADER));
	return callbacks;
}

Indium::Function::~Function() {};
Indium::Library::~Library() {};

std::shared_ptr<Indium::Device> Indium::PrivateFunction::device() {
	return _privateDevice;
};

std::shared_ptr<Indium::Device> Indium::PrivateLibrary::device() {
	return _privateDevice;
};

Indium::PrivateFunction::PrivateFunction(std::shared_ptr<PrivateLibrary> library, const std::string& name, const FunctionInfo& functionInfo):
	_library(library),
	_privateDevice(library->privateDevice()),
	_functionInfo(functionInfo)
{
	_name = name;
};

Indium::PrivateLibrary::PrivateLibrary(std::shared_ptr<PrivateDevice> device, const char* data, size_t dataLength, std::unordered_map<std::string, FunctionInfo> functionInfos):
	_privateDevice(device),
	_functionInfos(functionInfos)
{
	const auto *hostAllocator=shaderAllocator();
	// Translated modules hold one function each. A graphics function's
	// descriptors move to the graphics set scheme before the module exists.
	std::vector<uint32_t> code(dataLength / 4);
	std::memcpy(code.data(), data, code.size() * sizeof(uint32_t));
	const FunctionInfo* graphics = nullptr;
	for (const auto& entry : _functionInfos) if (isGraphicsStage(entry.second.functionType)) graphics = &entry.second;
	if (graphics && (_functionInfos.size() != 1 || !remapDescriptorSets(code, *graphics, *_privateDevice)))
		throw std::runtime_error("Unsupported graphics shader module layout");
	VkShaderModuleCreateInfo createInfo {};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = code.size() * sizeof(uint32_t);
	createInfo.pCode = code.data();

	if (DynamicVK::vkCreateShaderModule(_privateDevice->device(), &createInfo, hostAllocator, &_shaderModule) != VK_SUCCESS) {
		// TODO
		abort();
	}
	// Build its pipeline library in the background, before any pipeline asks.
	if (graphics && _privateDevice->pipelineLibraries) _privateDevice->graphicsLibraries->prepare(_shaderModule, *graphics);

	// create sampler states for embedded samplers
	for (auto& [name, funcInfo]: _functionInfos) {
		for (const auto& embeddedSampler: funcInfo.embeddedSamplers) {
			SamplerDescriptor descriptor {};

			auto translateAddressMode = [](Iridium::EmbeddedSampler::AddressMode irAddrMode) {
				switch (irAddrMode) {
					case Iridium::EmbeddedSampler::AddressMode::ClampToZero:
						return SamplerAddressMode::ClampToZero;
					case Iridium::EmbeddedSampler::AddressMode::ClampToEdge:
						return SamplerAddressMode::ClampToEdge;
					case Iridium::EmbeddedSampler::AddressMode::Repeat:
						return SamplerAddressMode::Repeat;
					case Iridium::EmbeddedSampler::AddressMode::MirrorRepeat:
						return SamplerAddressMode::MirrorRepeat;
					case Iridium::EmbeddedSampler::AddressMode::ClampToBorderColor:
						return SamplerAddressMode::ClampToBorderColor;
					default:
						return SamplerAddressMode::ClampToEdge;
				}
			};

			auto translateFilter = [](Iridium::EmbeddedSampler::Filter irFilter) {
				switch (irFilter) {
					case Iridium::EmbeddedSampler::Filter::Nearest:
						return SamplerMinMagFilter::Nearest;
					case Iridium::EmbeddedSampler::Filter::Linear:
						return SamplerMinMagFilter::Linear;
					default:
						return SamplerMinMagFilter::Nearest;
				}
			};

			auto translateMipFilter = [](Iridium::EmbeddedSampler::MipFilter irMipFilter) {
				switch (irMipFilter) {
					case Iridium::EmbeddedSampler::MipFilter::None:
						return SamplerMipFilter::NotMipmapped;
					case Iridium::EmbeddedSampler::MipFilter::Nearest:
						return SamplerMipFilter::Nearest;
					case Iridium::EmbeddedSampler::MipFilter::Linear:
						return SamplerMipFilter::Linear;
					default:
						return SamplerMipFilter::NotMipmapped;
				}
			};

			auto translateBorderColor = [](Iridium::EmbeddedSampler::BorderColor irBorderColor) {
				switch (irBorderColor) {
					case Iridium::EmbeddedSampler::BorderColor::TransparentBlack:
						return SamplerBorderColor::TransparentBlack;
					case Iridium::EmbeddedSampler::BorderColor::OpaqueBlack:
						return SamplerBorderColor::OpaqueBlack;
					case Iridium::EmbeddedSampler::BorderColor::OpaqueWhite:
						return SamplerBorderColor::OpaqueWhite;
					default:
						return SamplerBorderColor::TransparentBlack;
				}
			};

			auto translateCompareFunction = [](Iridium::EmbeddedSampler::CompareFunction irCompareFunction) {
				switch (irCompareFunction) {
					case Iridium::EmbeddedSampler::CompareFunction::None:
						return CompareFunction::Never;
					case Iridium::EmbeddedSampler::CompareFunction::Less:
						return CompareFunction::Less;
					case Iridium::EmbeddedSampler::CompareFunction::LessEqual:
						return CompareFunction::LessEqual;
					case Iridium::EmbeddedSampler::CompareFunction::Greater:
						return CompareFunction::Greater;
					case Iridium::EmbeddedSampler::CompareFunction::GreaterEqual:
						return CompareFunction::GreaterEqual;
					case Iridium::EmbeddedSampler::CompareFunction::Equal:
						return CompareFunction::Equal;
					case Iridium::EmbeddedSampler::CompareFunction::NotEqual:
						return CompareFunction::NotEqual;
					case Iridium::EmbeddedSampler::CompareFunction::Always:
						return CompareFunction::Always;
					case Iridium::EmbeddedSampler::CompareFunction::Never:
						return CompareFunction::Never;
					default:
						return CompareFunction::Never;
				}
			};

			descriptor.minFilter = translateFilter(embeddedSampler.minificationFilter);
			descriptor.magFilter = translateFilter(embeddedSampler.magnificationFilter);
			descriptor.mipFilter = translateMipFilter(embeddedSampler.mipmapFilter);
			descriptor.maxAnisotropy = embeddedSampler.anisotropyLevel;
			descriptor.sAddressMode = translateAddressMode(embeddedSampler.widthAddressMode);
			descriptor.tAddressMode = translateAddressMode(embeddedSampler.heightAddressMode);
			descriptor.rAddressMode = translateAddressMode(embeddedSampler.depthAddressMode);
			descriptor.borderColor = translateBorderColor(embeddedSampler.borderColor);
			descriptor.normalizedCoordinates = embeddedSampler.usesNormalizedCoordinates;
			descriptor.lodMinClamp = embeddedSampler.lodMin;
			descriptor.lodMaxClamp = embeddedSampler.lodMax;
			descriptor.supportArgumentBuffers = false;
			descriptor.compareFunction = translateCompareFunction(embeddedSampler.compareFunction);

			funcInfo.embeddedSamplerStates.push_back(_privateDevice->newSamplerState(descriptor));
		}
	}
};

Indium::PrivateLibrary::~PrivateLibrary() {
	// A library build may still be using the module.
	if (_privateDevice->graphicsLibraries) _privateDevice->graphicsLibraries->release(_shaderModule);
	DynamicVK::vkDestroyShaderModule(_privateDevice->device(), _shaderModule, shaderAllocator());
};

std::shared_ptr<Indium::Function> Indium::PrivateLibrary::newFunction(const std::string& name) {
	// Libraries are immutable after construction; missing names must not insert
	// into the map while other compiler threads read its function metadata.
	const auto& functions = _functionInfos;
	const auto found = functions.find(name);
	if (found == functions.end()) return nullptr;
	return std::make_shared<PrivateFunction>(shared_from_this(), name, found->second);
};
