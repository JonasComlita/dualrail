#pragma once
#ifndef VULKAN_BACKEND_H
#define VULKAN_BACKEND_H

#include <vulkan/vulkan.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <stdexcept>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace sandbox {
namespace qwen {

class VulkanBackend {
public:
    VulkanBackend() {
        initVulkan();
    }

    ~VulkanBackend() {
        cleanup();
    }

    bool isReady() const { return ready_; }

    void destroyBuffer(VkBuffer buffer, VkDeviceMemory bufferMemory) {
        if (!device_) return;
        if (buffer) vkDestroyBuffer(device_, buffer, nullptr);
        if (bufferMemory) vkFreeMemory(device_, bufferMemory, nullptr);
    }

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
        buffer = VK_NULL_HANDLE;
        bufferMemory = VK_NULL_HANDLE;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan buffer of " << size << " bytes." << std::endl;
            return;
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device_, buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        try {
            allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);
        } catch (const std::exception& e) {
            std::cerr << e.what() << std::endl;
            vkDestroyBuffer(device_, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return;
        }

        if (vkAllocateMemory(device_, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
            std::cerr << "Failed to allocate Vulkan buffer memory of "
                      << memRequirements.size << " bytes." << std::endl;
            vkDestroyBuffer(device_, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return;
        }

        if (vkBindBufferMemory(device_, buffer, bufferMemory, 0) != VK_SUCCESS) {
            std::cerr << "Failed to bind Vulkan buffer memory." << std::endl;
            vkFreeMemory(device_, bufferMemory, nullptr);
            vkDestroyBuffer(device_, buffer, nullptr);
            bufferMemory = VK_NULL_HANDLE;
            buffer = VK_NULL_HANDLE;
        }
    }

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("Failed to find suitable memory type!");
    }

    void uploadData(VkBuffer dstBuffer, const void* data, VkDeviceSize size) {
        if (!dstBuffer || !data || size == 0) return;

        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingBufferMemory = VK_NULL_HANDLE;
        createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);
        if (!stagingBuffer || !stagingBufferMemory) return;

        void* mappedData = nullptr;
        if (vkMapMemory(device_, stagingBufferMemory, 0, size, 0, &mappedData) != VK_SUCCESS || !mappedData) {
            std::cerr << "Failed to map Vulkan staging buffer." << std::endl;
            vkDestroyBuffer(device_, stagingBuffer, nullptr);
            vkFreeMemory(device_, stagingBufferMemory, nullptr);
            return;
        }
        memcpy(mappedData, data, (size_t)size);
        vkUnmapMemory(device_, stagingBufferMemory);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer_, &beginInfo);

        VkBufferCopy copyRegion{};
        copyRegion.size = size;
        vkCmdCopyBuffer(commandBuffer_, stagingBuffer, dstBuffer, 1, &copyRegion);

        vkEndCommandBuffer(commandBuffer_);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer_;

        vkQueueSubmit(computeQueue_, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(computeQueue_);

        vkDestroyBuffer(device_, stagingBuffer, nullptr);
        vkFreeMemory(device_, stagingBufferMemory, nullptr);
    }

    void readBuffer(VkDeviceMemory memory, void* outData, size_t size) {
        if (!memory || !outData || size == 0) return;

        void* mappedData = nullptr;
        if (vkMapMemory(device_, memory, 0, size, 0, &mappedData) != VK_SUCCESS || !mappedData) {
            std::cerr << "Failed to map Vulkan readback buffer." << std::endl;
            return;
        }
        memcpy(outData, mappedData, size);
        vkUnmapMemory(device_, memory);
    }

    void executeMatmulInt8(int rows, int cols, float scale, uint32_t w_offset_bytes, 
                           VkBuffer weightBuffer, VkBuffer activationBuffer, VkBuffer outputBuffer) {
        if (!ready_) return;

        vkResetCommandBuffer(commandBuffer_, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer_, &beginInfo);

        vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline_);

        // Update Descriptor Set with current buffers
        VkDescriptorBufferInfo wInfo{};
        wInfo.buffer = weightBuffer;
        wInfo.offset = 0;
        wInfo.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo xInfo{};
        xInfo.buffer = activationBuffer;
        xInfo.offset = 0;
        xInfo.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo yInfo{};
        yInfo.buffer = outputBuffer;
        yInfo.offset = 0;
        yInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[3] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet_;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &wInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSet_;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &xInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descriptorSet_;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &yInfo;

        vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);

        vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);

        // Push constants
        struct {
            int rows;
            int cols;
            float scale;
            int w_offset;
        } pushConstants;

        pushConstants.rows = rows;
        pushConstants.cols = cols;
        pushConstants.scale = scale;
        pushConstants.w_offset = w_offset_bytes; // byte offset for int8 array

        vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants), &pushConstants);

        // Dispatch
        uint32_t groupCountX = (rows + 255) / 256;
        vkCmdDispatch(commandBuffer_, groupCountX, 1, 1);

        vkEndCommandBuffer(commandBuffer_);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer_;

        vkQueueSubmit(computeQueue_, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(computeQueue_);
    }

    void executeGeGLU(int length, VkBuffer gateBuffer, VkBuffer upBuffer, VkBuffer outBuffer) {
        if (!ready_) return;

        vkResetCommandBuffer(commandBuffer_, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer_, &beginInfo);

        vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, gegluPipeline_);

        VkDescriptorBufferInfo gInfo{};
        gInfo.buffer = gateBuffer;
        gInfo.offset = 0;
        gInfo.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo uInfo{};
        uInfo.buffer = upBuffer;
        uInfo.offset = 0;
        uInfo.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo oInfo{};
        oInfo.buffer = outBuffer;
        oInfo.offset = 0;
        oInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[3] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet_;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &gInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSet_;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &uInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descriptorSet_;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &oInfo;

        vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
        vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);

        struct { int length; } pushConstants;
        pushConstants.length = length;
        vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants), &pushConstants);

        uint32_t groupCountX = (length + 255) / 256;
        vkCmdDispatch(commandBuffer_, groupCountX, 1, 1);

        vkEndCommandBuffer(commandBuffer_);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer_;

        vkQueueSubmit(computeQueue_, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(computeQueue_);
    }

private:
    bool ready_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex_ = 0;
    
    VkPipeline computePipeline_ = VK_NULL_HANDLE;
    VkPipeline gegluPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;

    void initVulkan() {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Qwen Vulkan Backend";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan instance." << std::endl;
            return;
        }

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
        if (deviceCount == 0) return;

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

        int selectedDevice = -1;
        uint32_t selectedQueueFamily = 0;
        uint64_t bestScore = 0;
        VkPhysicalDeviceProperties selectedProps{};
        VkPhysicalDeviceMemoryProperties selectedMem{};

        const char* requestedDevice = std::getenv("QWEN_VULKAN_DEVICE");
        const int requestedIndex = requestedDevice ? std::atoi(requestedDevice) : -1;

        for (uint32_t devIndex = 0; devIndex < deviceCount; ++devIndex) {
            VkPhysicalDeviceProperties props{};
            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceProperties(devices[devIndex], &props);
            vkGetPhysicalDeviceMemoryProperties(devices[devIndex], &memProps);

            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[devIndex], &queueFamilyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(devices[devIndex], &queueFamilyCount, queueFamilies.data());

            bool hasCompute = false;
            uint32_t computeFamily = 0;
            for (uint32_t i = 0; i < queueFamilyCount; ++i) {
                if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    hasCompute = true;
                    computeFamily = i;
                    break;
                }
            }
            if (!hasCompute) continue;

            uint64_t localBytes = 0;
            for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
                if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    localBytes += memProps.memoryHeaps[i].size;
            }

            const bool requested = requestedIndex == static_cast<int>(devIndex);
            const bool discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            uint64_t score = localBytes;
            if (discrete) score += (uint64_t{1} << 62);
            if (requested) score = std::numeric_limits<uint64_t>::max();

            if (selectedDevice < 0 || score > bestScore) {
                selectedDevice = static_cast<int>(devIndex);
                selectedQueueFamily = computeFamily;
                bestScore = score;
                selectedProps = props;
                selectedMem = memProps;
            }
        }

        if (selectedDevice < 0) return;

        physicalDevice_ = devices[static_cast<std::size_t>(selectedDevice)];
        queueFamilyIndex_ = selectedQueueFamily;

        uint64_t selectedLocalBytes = 0;
        for (uint32_t i = 0; i < selectedMem.memoryHeapCount; ++i) {
            if (selectedMem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                selectedLocalBytes += selectedMem.memoryHeaps[i].size;
        }
        std::cerr << "Vulkan device: " << selectedProps.deviceName
                  << " (device_local="
                  << static_cast<double>(selectedLocalBytes) / (1024.0 * 1024.0 * 1024.0)
                  << " GiB)\n";

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, queueFamilies.data());

        bool found = false;
        for (uint32_t i = 0; i < queueFamilyCount; ++i) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                queueFamilyIndex_ = i;
                found = true;
                break;
            }
        }
        if (!found) return;

        float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamilyIndex_;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;

        // Enable int8 support
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        VkPhysicalDevice8BitStorageFeatures storage8bit{};
        storage8bit.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES;
        storage8bit.storageBuffer8BitAccess = VK_TRUE;
        features2.pNext = &storage8bit;

        VkDeviceCreateInfo deviceCreateInfo{};
        deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
        deviceCreateInfo.queueCreateInfoCount = 1;
        deviceCreateInfo.pNext = &features2;

        if (vkCreateDevice(physicalDevice_, &deviceCreateInfo, nullptr, &device_) != VK_SUCCESS) {
            std::cerr << "Failed to create logical device." << std::endl;
            return;
        }

        vkGetDeviceQueue(device_, queueFamilyIndex_, 0, &computeQueue_);

        createCommandPool();
        createDescriptorSetLayout();
        if (!createComputePipeline()) {
            std::cerr << "Vulkan backend disabled: compute shaders unavailable or pipeline creation failed." << std::endl;
            cleanup();
            return;
        }
        
        ready_ = true;
    }

    void createCommandPool() {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queueFamilyIndex_;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool_;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer_);
    }

    void createDescriptorSetLayout() {
        VkDescriptorSetLayoutBinding bindings[3] = {};
        for(int i=0; i<3; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings = bindings;
        vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_);

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 3;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 1;
        vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &descriptorSetLayout_;
        vkAllocateDescriptorSets(device_, &allocInfo, &descriptorSet_);
    }

    std::vector<char> readFile(const std::string& filename) {
        std::ifstream file(filename, std::ios::ate | std::ios::binary);
        if (!file.is_open()) return {};
        size_t fileSize = (size_t)file.tellg();
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        file.close();
        return buffer;
    }

    bool createComputePipeline() {
        auto shaderCode = readFile("qwen3.627b_weights/shaders/matmul_int8.spv");
        if (shaderCode.empty()) shaderCode = readFile("build/matmul_int8.comp.spv"); // CMake output
        if (shaderCode.empty()) shaderCode = readFile("build/matmul_int8.spv");      // Legacy fallback
        if (shaderCode.empty()) {
            std::cerr << "Vulkan backend: matmul_int8 SPIR-V not found." << std::endl;
            return false;
        }

        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = shaderCode.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(shaderCode.data());

        VkShaderModule computeShaderModule;
        if (vkCreateShaderModule(device_, &createInfo, nullptr, &computeShaderModule) != VK_SUCCESS) {
            std::cerr << "Vulkan backend: failed to create matmul shader module." << std::endl;
            return false;
        }

        VkPipelineShaderStageCreateInfo shaderStageInfo{};
        shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        shaderStageInfo.module = computeShaderModule;
        shaderStageInfo.pName = "main";

        VkPushConstantRange pushConstant;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(int) * 3 + sizeof(float);
        pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstant;

        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
            std::cerr << "Vulkan backend: failed to create pipeline layout." << std::endl;
            vkDestroyShaderModule(device_, computeShaderModule, nullptr);
            return false;
        }

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = shaderStageInfo;
        pipelineInfo.layout = pipelineLayout_;

        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &computePipeline_) != VK_SUCCESS) {
            std::cerr << "Vulkan backend: failed to create matmul pipeline." << std::endl;
            vkDestroyShaderModule(device_, computeShaderModule, nullptr);
            return false;
        }
        vkDestroyShaderModule(device_, computeShaderModule, nullptr);

        // GeGLU Pipeline
        auto gegluCode = readFile("qwen3.627b_weights/shaders/geglu.spv");
        if (gegluCode.empty()) gegluCode = readFile("build/geglu.comp.spv");
        if (gegluCode.empty()) {
            std::cerr << "Vulkan backend: GeGLU SPIR-V not found." << std::endl;
            return false;
        }

        VkShaderModuleCreateInfo gCreateInfo{};
        gCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        gCreateInfo.codeSize = gegluCode.size();
        gCreateInfo.pCode = reinterpret_cast<const uint32_t*>(gegluCode.data());
        
        VkShaderModule gegluModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &gCreateInfo, nullptr, &gegluModule) != VK_SUCCESS) {
            std::cerr << "Vulkan backend: failed to create GeGLU shader module." << std::endl;
            return false;
        }
        
        VkPipelineShaderStageCreateInfo gStageInfo{};
        gStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        gStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        gStageInfo.module = gegluModule;
        gStageInfo.pName = "main";
        
        VkComputePipelineCreateInfo gPipelineInfo{};
        gPipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        gPipelineInfo.stage = gStageInfo;
        gPipelineInfo.layout = pipelineLayout_;
        
        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &gPipelineInfo, nullptr, &gegluPipeline_) != VK_SUCCESS) {
            std::cerr << "Vulkan backend: failed to create GeGLU pipeline." << std::endl;
            vkDestroyShaderModule(device_, gegluModule, nullptr);
            return false;
        }
        vkDestroyShaderModule(device_, gegluModule, nullptr);
        return true;
    }

    void cleanup() {
        if (!device_) return;
        ready_ = false;
        if (computePipeline_) vkDestroyPipeline(device_, computePipeline_, nullptr);
        if (gegluPipeline_) vkDestroyPipeline(device_, gegluPipeline_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (descriptorSetLayout_) vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        if (instance_) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
    }
};

} // namespace qwen
} // namespace sandbox

#endif
