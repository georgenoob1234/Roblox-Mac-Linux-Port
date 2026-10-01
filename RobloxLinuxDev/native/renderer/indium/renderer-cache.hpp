#pragma once
#include <indium/dynamic-vk.hpp>
#include <memory>
#include <vector>
#include <unordered_map>

namespace Indium {
class PrivateRenderPipelineState;
class Texture;
struct RenderTargetCacheEntry;
struct DescriptorArena {
    struct Entry {
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        std::weak_ptr<PrivateRenderPipelineState> pso;
        std::vector<uint64_t> key;
        std::vector<std::weak_ptr<void>> resources;
        bool used = false;
        size_t hash = 0;
    };
    VkDevice device;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> fullPools;
    std::vector<Entry> entries;
    uint64_t allocations = 0, hits = 0, rewrites = 0;
    // Arenas have one recording owner and are recycled only after completion.
    // Indices survive entries growing; full keys and weak lifetimes check hits.
    std::unordered_multimap<size_t,size_t> lookup;
    std::weak_ptr<RenderTargetCacheEntry> target;
    std::unordered_map<VkDescriptorSetLayout, std::vector<size_t>> available;
    static size_t keyHash(VkDescriptorSetLayout layout, const std::vector<uint64_t>& key) {
        size_t hash = std::hash<VkDescriptorSetLayout>{}(layout);
        for (auto value : key) hash = (hash ^ value) * 1099511628211ull;
        // Buffer addresses, offsets and sizes are aligned. Mix high bits down so
        // power-of-two bucket counts do not put them all in a few long chains.
        hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ull;
        hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebull;
        return hash ^ (hash >> 31);
    }
    explicit DescriptorArena(VkDevice d): device(d) {}
    ~DescriptorArena() {
        if (pool) DynamicVK::vkDestroyDescriptorPool(device, pool, nullptr);
        for (auto p : fullPools) DynamicVK::vkDestroyDescriptorPool(device, p, nullptr);
    }
};
struct RenderTargetCacheEntry {
    VkDevice device;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    std::vector<VkImageView> views;
    std::vector<uint64_t> key;
    std::vector<std::weak_ptr<Texture>> textures;
    explicit RenderTargetCacheEntry(VkDevice d): device(d) {}
    ~RenderTargetCacheEntry() {
        if (framebuffer) DynamicVK::vkDestroyFramebuffer(device, framebuffer, nullptr);
        for (auto view : views) DynamicVK::vkDestroyImageView(device, view, nullptr);
        if (pass) DynamicVK::vkDestroyRenderPass(device, pass, nullptr);
    }
};
}
