// Compile through compute-sync-check.py: it supplies the production barrier helper.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <vulkan/vulkan.h>
namespace Indium::DynamicVK {
using ::vkCmdPipelineBarrier;
}
#include "compute-sync-production.inc"
static void check(VkResult value) {
    if (value != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan failure %d\n", value);
        std::abort();
    }
}
struct Buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *data;
};
static Buffer buffer(VkDevice device, const VkPhysicalDeviceMemoryProperties &properties, VkDeviceSize size) {
    Buffer out{};
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = size;
    create.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    check(vkCreateBuffer(device, &create, nullptr, &out.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    uint32_t type = 0;
    constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (type < properties.memoryTypeCount &&
           (!(req.memoryTypeBits & (1u << type)) ||
            (properties.memoryTypes[type].propertyFlags & flags) != flags))
        ++type;
    assert(type < properties.memoryTypeCount);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    check(vkAllocateMemory(device, &alloc, nullptr, &out.memory));
    check(vkBindBufferMemory(device, out.buffer, out.memory, 0));
    check(vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.data));
    return out;
}

static VkPipeline pipeline(VkDevice device, VkPipelineLayout layout, const std::string &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    assert(file);
    auto size = file.tellg();
    assert(size > 0 && size % 4 == 0);
    std::vector<uint32_t> code(size / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(code.data()), size);
    assert(file);
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = size;
    moduleInfo.pCode = code.data();
    VkShaderModule module;
    check(vkCreateShaderModule(device, &moduleInfo, nullptr, &module));
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.layout = layout;
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    VkPipeline result;
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &result));
    vkDestroyShaderModule(device, module, nullptr);
    return result;
}
int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    const bool synchronize = argc == 2;
    const std::string directory = argv[1];
    constexpr uint32_t n = 65536;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "compute-sync-check";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    VkInstance instance;
    check(vkCreateInstance(&instanceInfo, nullptr, &instance));
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    assert(count);
    std::vector<VkPhysicalDevice> physical(count);
    check(vkEnumeratePhysicalDevices(instance, &count, physical.data()));
    auto gpu = physical.front();
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(gpu, &properties);
    std::printf("GPU %s driver=%u\n", properties.deviceName, properties.driverVersion);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, queues.data());
    unsigned family = 0;
    while (!(queues.at(family).queueFlags & VK_QUEUE_COMPUTE_BIT))
        ++family;
    float priority = 1;
    VkDeviceQueueCreateInfo qinfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qinfo.queueFamilyIndex = family;
    qinfo.queueCount = 1;
    qinfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &qinfo;
    VkDevice device;
    check(vkCreateDevice(gpu, &deviceInfo, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    auto input = buffer(device, memory, n * 128);
    auto middle = buffer(device, memory, n * 112);
    auto output = buffer(device, memory, n * 112);
    auto data = static_cast<uint32_t *>(input.data);
    std::fill(data, data + n * 32, 0);
    for (unsigned i = 0; i < n; ++i) {
        data[i * 32] = n - 1 - i;
        for (unsigned j = 0; j < 28; ++j)
            data[i * 32 + 4 + j] = (n - i) * 100 + j;
    }
    const std::array<uint32_t, 2> bindingIDs{0, 1};
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (unsigned i = 0; i < 2; ++i) {
        bindings[i].binding = bindingIDs[i];
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings.data();
    VkDescriptorSetLayout setLayout;
    check(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout));
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    VkPipelineLayout layout;
    check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout));
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool pool;
    check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool));
    VkDescriptorSetLayout layouts[]{setLayout, setLayout};
    VkDescriptorSetAllocateInfo setAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAlloc.descriptorPool = pool;
    setAlloc.descriptorSetCount = 2;
    setAlloc.pSetLayouts = layouts;
    VkDescriptorSet sets[2];
    check(vkAllocateDescriptorSets(device, &setAlloc, sets));
    VkDescriptorBufferInfo infos[] = {{middle.buffer, 0, n * 112},
                                      {input.buffer, 0, n * 128},
                                      {output.buffer, 0, n * 112},
                                      {middle.buffer, 0, n * 112}};
    VkWriteDescriptorSet writes[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = sets[i / 2];
        writes[i].dstBinding = bindingIDs[i % 2];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
    VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.queueFamilyIndex = family;
    VkCommandPool commandPool;
    check(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool));
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer command;
    check(vkAllocateCommandBuffers(device, &commandInfo, &command));
    const VkPipeline pipelines[]{pipeline(device, layout, directory + "/update.spv"),
                                 pipeline(device, layout, directory + "/copy.spv")};
    for (unsigned trial = 0; trial < 2; ++trial) {
        std::memset(middle.data, 0, n * 112);
        std::memset(output.data, 0, n * 112);
        check(vkResetCommandPool(device, commandPool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(command, &begin));
        for (unsigned stage = 0; stage < 2; ++stage) {
            if (synchronize)
                computeMemoryBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[stage]);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[stage], 0,
                                    nullptr);
            vkCmdDispatch(command, n / 64, 1, 1);
        }
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &barrier, 0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(command));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        check(vkQueueWaitIdle(queue));
        auto results = static_cast<const uint32_t *>(output.data),
             mid = static_cast<const uint32_t *>(middle.data);
        unsigned corrupt = 0, partial = 0, midBad = 0;
        for (unsigned i = 0; i < n; ++i) {
            unsigned good = 0;
            for (unsigned j = 0; j < 28; ++j) {
                const uint32_t expected = (i + 1) * 100 + j;
                good += results[i * 28 + j] == expected;
                midBad += mid[i * 28 + j] != expected;
            }
            corrupt += good != 28;
            partial += good && good != 28;
        }
        printf("trial=%u barrier=%u corrupt_records=%u partial_records=%u source_bad_words=%u\n", trial,
               synchronize, corrupt, partial, midBad);
        fflush(stdout);
        if (synchronize && (corrupt || midBad))
            return 1;
    }
    for (auto pipeline : pipelines)
        vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    for (auto b : {input, middle, output}) {
        vkUnmapMemory(device, b.memory);
        vkDestroyBuffer(device, b.buffer, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
    }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
}
