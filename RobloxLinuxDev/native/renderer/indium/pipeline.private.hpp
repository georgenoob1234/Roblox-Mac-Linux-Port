#pragma once

#include <indium/library.private.hpp>
#include <indium/device.private.hpp>
#include <indium/dynamic-vk.hpp>

#include <algorithm>
#include <array>
#include <indium/dynamic-buffer.hpp>

namespace Indium {
	// Graphics descriptor sets. Set 0 holds the buffers of both stages and is a
	// push-descriptor set when the device supports it: vertex binding b stays at
	// b, fragment binding b moves to 16 + b. Everything else keeps its binding in
	// the stage's own set (1 vertex, 2 fragment). library.cpp rewrites the
	// SPIR-V decorations to match; compute keeps its single set 0.
	constexpr uint32_t pushedBuffersPerStage = 16;
	constexpr uint32_t graphicsSetCount = 3;

	inline bool isGraphicsStage(FunctionType type) {
		return type == FunctionType::Vertex || type == FunctionType::Fragment;
	}
	inline uint32_t stageSet(FunctionType type) {
		return type == FunctionType::Fragment ? 2 : 1;
	}
	// Whether a graphics function's buffers live in the pushed set 0.
	inline bool pushesBuffers(const FunctionInfo& info, const PrivateDevice& device) {
		if (!device.pushDescriptors || !isGraphicsStage(info.functionType)) return false;
		for (const auto& binding : info.bindings)
			if (binding.type == Iridium::BindingType::Buffer && binding.internalIndex >= pushedBuffersPerStage) return false;
		return true;
	}
	inline uint32_t pushedBufferBinding(const FunctionInfo& info, const Iridium::BindingInfo& binding) {
		return (info.functionType == FunctionType::Fragment ? pushedBuffersPerStage : 0) + uint32_t(binding.internalIndex);
	}
	// Compute functions push their whole set when it fits (encoders write at most 32).
	constexpr size_t maxPushedComputeBindings = 32;
	inline bool pushesComputeSet(const FunctionInfo& info, const PrivateDevice& device) {
		return device.pushDescriptors && info.functionType == FunctionType::Kernel &&
			info.bindings.size() <= std::min<size_t>(device.maxPushDescriptors, maxPushedComputeBindings);
	}

	// The set a function owns: its non-pushed resources (graphics) or all of
	// them (compute). Equal inputs give identically defined layouts, which
	// pipeline libraries linked into different pipelines rely on.
	inline VkDescriptorSetLayout createFunctionSetLayout(PrivateDevice& device, const FunctionInfo* info) {
		std::vector<VkDescriptorSetLayoutBinding> bindings;
		VkDescriptorSetLayoutCreateInfo layoutInfo {};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		if (info) {
			const bool graphics = isGraphicsStage(info->functionType);
			const bool pushedBuffers = pushesBuffers(*info, device);
			const bool pushedSet = pushesComputeSet(*info, device);
			if (pushedSet) layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
			const auto stages = functionTypeToVkShaderStageFlags(info->functionType);
			for (const auto& bindingInfo : info->bindings) {
				VkDescriptorSetLayoutBinding binding {};
				binding.binding = bindingInfo.internalIndex;
				binding.descriptorCount = 1;
				binding.stageFlags = stages;
				if (bindingInfo.type == Iridium::BindingType::Buffer) {
					if (pushedBuffers) continue;
					binding.descriptorType = graphics && dynamicBufferBinding(*info, bindingInfo, device) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				} else if (bindingInfo.type == Iridium::BindingType::Texture) {
					binding.descriptorType = (bindingInfo.textureAccessType == Iridium::TextureAccessType::Sample) ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				} else if (bindingInfo.type == Iridium::BindingType::Sampler) {
					binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
				} else continue;
				bindings.push_back(binding);
			}
		}
		layoutInfo.bindingCount = bindings.size();
		layoutInfo.pBindings = bindings.data();
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		if (DynamicVK::vkCreateDescriptorSetLayout(device.device(), &layoutInfo, nullptr, &layout) != VK_SUCCESS)
			throw std::runtime_error("Could not create descriptor set layout");
		return layout;
	}

	template<size_t count>
	struct DescriptorSetLayouts {
	private:
		INDIUM_PREVENT_COPY(DescriptorSetLayouts);
		std::array<bool, count> _owned {};

	public:
		std::array<VkDescriptorSetLayout, count> layouts;
		std::shared_ptr<PrivateDevice> privateDevice;

		DescriptorSetLayouts(std::shared_ptr<PrivateDevice> device):
			privateDevice(device)
		{
			for (size_t i = 0; i < layouts.size(); ++i) {
				layouts[i] = VK_NULL_HANDLE;
			}
			// The shared buffer set belongs to the device.
			if constexpr (count == graphicsSetCount) layouts[0] = privateDevice->bufferSetLayout;
		};

		~DescriptorSetLayouts() {
			for (size_t i = 0; i < layouts.size(); ++i) {
				if (layouts[i] == VK_NULL_HANDLE || !_owned[i]) {
					continue;
				}
				DynamicVK::vkDestroyDescriptorSetLayout(privateDevice->device(), layouts[i], nullptr);
			}
		};

		void processFunction(std::shared_ptr<PrivateFunction> function, size_t layoutIndex) {
			layouts[layoutIndex] = createFunctionSetLayout(*privateDevice, function ? &function->functionInfo() : nullptr);
			_owned[layoutIndex] = true;
		};
	};
};
