#pragma once

#include <vector>
#include <memory>
#include <optional>
#include <array>


#include <indium/command-encoder.hpp>
#include <indium/device.private.hpp>
#include <indium/buffer.hpp>
#include <indium/sampler.private.hpp>
#include <indium/buffer.private.hpp>
#include <indium/texture.private.hpp>
#include <indium/library.private.hpp>
#include <indium/dynamic-vk.hpp>
#include <indium/dynamic-buffer.hpp>

#include <iridium/iridium.hpp>

namespace Indium {
	struct FunctionResources {
		std::vector<std::pair<std::shared_ptr<Buffer>, size_t>> buffers;
		std::vector<std::shared_ptr<Texture>> textures;
		std::vector<std::shared_ptr<SamplerState>> samplers;

		void setBytes(std::shared_ptr<Device> device, const void* bytes, size_t length, size_t index) {
			// TODO: we can make this "Private" instead
			auto buf = device->newBuffer(bytes, length, ResourceOptions::StorageModeShared);

			if (buffers.size() <= index) {
				buffers.resize(index + 1);
			}

			buffers[index] = std::make_pair(buf, 0);
		};

		void setBuffer(std::shared_ptr<Buffer> buffer, size_t offset, size_t index) {
			if (buffers.size() <= index) {
				buffers.resize(index + 1);
			}

			buffers[index] = std::make_pair(buffer, offset);
		};

		void setBufferOffset(size_t offset, size_t index) {
			buffers[index].second = offset;
		};

		void setSamplerState(std::shared_ptr<SamplerState> state, std::optional<std::pair<float, float>> lodClamps, size_t index) {
			if (samplers.size() <= index) {
				samplers.resize(index + 1);
			}

			if (lodClamps && state) {
				auto privateState = std::static_pointer_cast<PrivateSamplerState>(state);
				samplers[index] = privateState->cloneWithClamps(lodClamps->first, lodClamps->second);
			} else {
				samplers[index] = state;
			}
		};

		void setTexture(std::shared_ptr<Texture> texture, size_t index) {
			if (textures.size() <= index) {
				textures.resize(index + 1);
			}

			textures[index] = texture;
		};
	};

	static constexpr std::array<VkDescriptorPoolSize, 5> poolSizes {
		VkDescriptorPoolSize { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512 },
		VkDescriptorPoolSize { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 512 },
		VkDescriptorPoolSize { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 512 },
		VkDescriptorPoolSize { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512 },
		VkDescriptorPoolSize { VK_DESCRIPTOR_TYPE_SAMPLER, 512 },
	};

	template<size_t setCount>
	std::array<VkDescriptorSet, setCount> createDescriptorSets(std::array<VkDescriptorSetLayout, setCount>& setLayouts, VkDescriptorPool& pool, std::vector<VkDescriptorPool>& fullPools, const std::shared_ptr<PrivateDevice>& privateDevice, const std::array<std::reference_wrapper<const FunctionResources>, setCount>& funcResources, const std::array<std::reference_wrapper<const FunctionInfo>, setCount>& functionInfos, std::vector<std::shared_ptr<Buffer>>& keepAliveBuffers, std::array<VkDescriptorSet, setCount> descriptorSets = {}, bool dynamic = false, const std::vector<size_t>* ranges = nullptr, bool pushedBuffers = false) {
		if (!descriptorSets[0]) {

		VkDescriptorSetAllocateInfo setAllocateInfo {};
		setAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		setAllocateInfo.descriptorPool = pool;
		setAllocateInfo.descriptorSetCount = descriptorSets.size();
		setAllocateInfo.pSetLayouts = setLayouts.data();

		auto status = DynamicVK::vkAllocateDescriptorSets(privateDevice->device(), &setAllocateInfo, descriptorSets.data());
		if (status == VK_ERROR_OUT_OF_POOL_MEMORY || status == VK_ERROR_FRAGMENTED_POOL) {
			// Submitted draws still reference old sets: retain every pool until completion.
			fullPools.push_back(pool);
			pool = VK_NULL_HANDLE;
			VkDescriptorPoolCreateInfo info {};
			info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			info.maxSets = 64;
			info.poolSizeCount = poolSizes.size();
			info.pPoolSizes = poolSizes.data();
			status = DynamicVK::vkCreateDescriptorPool(privateDevice->device(), &info, nullptr, &pool);
			if (status == VK_SUCCESS) {
				setAllocateInfo.descriptorPool = pool;
				status = DynamicVK::vkAllocateDescriptorSets(privateDevice->device(), &setAllocateInfo, descriptorSets.data());
			}
		}
		if (status != VK_SUCCESS)
			throw std::runtime_error("Vulkan descriptor allocation failed: " + std::to_string(status));
		}

		for (size_t i = 0; i < descriptorSets.size(); ++i) {
			std::unique_ptr<VkWriteDescriptorSet[]> writeDescSet;
			std::unique_ptr<VkDescriptorBufferInfo[]> bufInfos;
			std::unique_ptr<VkDescriptorImageInfo[]> imageInfos;

			const FunctionResources& functionResources = funcResources[i];
			const FunctionInfo& funcInfo = functionInfos[i];
			// Each reflected binding emits at most one write. Keep pointers stable
			// through vkUpdateDescriptorSets without three heap allocations per update.
			std::array<VkWriteDescriptorSet, 32> localWrites;
			std::array<VkDescriptorBufferInfo, 32> localBuffers;
			std::array<VkDescriptorImageInfo, 32> localImages;
			const size_t capacity = funcInfo.bindings.size();
			if (capacity > localWrites.size()) {
				writeDescSet.reset(new VkWriteDescriptorSet[capacity]);
				bufInfos.reset(new VkDescriptorBufferInfo[capacity]);
				imageInfos.reset(new VkDescriptorImageInfo[capacity]);
			}
			auto* writes = capacity <= localWrites.size() ? localWrites.data() : writeDescSet.get();
			auto* buffers = capacity <= localBuffers.size() ? localBuffers.data() : bufInfos.get();
			auto* images = capacity <= localImages.size() ? localImages.data() : imageInfos.get();
			size_t writeCount = 0, bufferCount = 0, imageCount = 0;

			// metal2vulkan exposes each Metal buffer as a Vulkan storage-buffer descriptor.
			// Pushed buffers are not part of the set (see pipeline.private.hpp).
			for (const auto& binding : funcInfo.bindings) {
				if (binding.type != Iridium::BindingType::Buffer || pushedBuffers) continue;
				if (binding.index >= functionResources.buffers.size())
					throw std::runtime_error("Missing Metal buffer binding");
				const auto& resource = functionResources.buffers[binding.index];
				// Every Indium buffer is a PrivateBuffer.
				auto* buffer = static_cast<PrivateBuffer*>(resource.first.get());
				if (!buffer || resource.second >= buffer->length())
					throw std::runtime_error("Invalid Metal buffer binding or offset");
				keepAliveBuffers.push_back(resource.first);
				auto& info = buffers[bufferCount++]; info = {};
				info.buffer = buffer->buffer();
				bool isDynamic = dynamic && dynamicBufferBinding(funcInfo, binding, *privateDevice);
				info.offset = isDynamic ? 0 : resource.second;
				info.range = ranges && binding.index < ranges->size() && (*ranges)[binding.index] ? (*ranges)[binding.index] : buffer->length() - resource.second;
				auto& write = writes[writeCount++]; write = {};
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.dstSet = descriptorSets[i];
				write.dstBinding = binding.internalIndex;
				write.descriptorType = isDynamic ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				write.descriptorCount = 1;
				write.pBufferInfo = &info;
			}

			for (size_t j = 0; j < funcInfo.bindings.size(); ++j) {
				auto& bindingInfo = funcInfo.bindings[j];

				if (bindingInfo.type == Iridium::BindingType::Texture) {
					if (bindingInfo.index >= functionResources.textures.size()) {
						continue;
					}

					auto* privateTexture = static_cast<PrivateTexture*>(functionResources.textures[bindingInfo.index].get());

					auto& info = images[imageCount++]; info = {};
					info.imageView = privateTexture->imageView();
					info.imageLayout = privateTexture->imageLayout();

					auto& descSet = writes[writeCount++]; descSet = {};
					descSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					descSet.dstSet = descriptorSets[i];
					descSet.dstBinding = bindingInfo.internalIndex;
					descSet.dstArrayElement = 0;
					descSet.descriptorType = (bindingInfo.textureAccessType == Iridium::TextureAccessType::Sample) ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
					descSet.descriptorCount = 1;
					descSet.pImageInfo = &info;
				} else if (bindingInfo.type == Iridium::BindingType::Sampler) {
					bool embeddedSampler = false;

					if (bindingInfo.index == SIZE_MAX) {
						// this binding uses an embedded sampler
						embeddedSampler = true;
					} else if (bindingInfo.index >= functionResources.samplers.size()) {
						continue;
					}

					const auto& sampler = embeddedSampler ? funcInfo.embeddedSamplerStates[bindingInfo.embeddedSamplerIndex] : functionResources.samplers[bindingInfo.index];
					auto* privateSampler = static_cast<PrivateSamplerState*>(sampler.get());

					auto& info = images[imageCount++]; info = {};
					info.sampler = privateSampler->sampler();

					auto& descSet = writes[writeCount++]; descSet = {};
					descSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					descSet.dstSet = descriptorSets[i];
					descSet.dstBinding = bindingInfo.internalIndex;
					descSet.dstArrayElement = 0;
					descSet.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
					descSet.descriptorCount = 1;
					descSet.pImageInfo = &info;
				}
			}

			DynamicVK::vkUpdateDescriptorSets(privateDevice->device(), writeCount, writes, 0, nullptr);
		}

		return descriptorSets;
	};
};
