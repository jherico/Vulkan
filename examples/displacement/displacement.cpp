/*
* Vulkan Example - Displacement mapping with tessellation shaders
*
* Copyright (C) 2016 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vulkan/vulkan.h>
#include "vulkanexamplebase.h"
#include "VulkanTexture.hpp"
#include "VulkanModel.hpp"
#include "VulkanBuffer.hpp"

#define VERTEX_BUFFER_BIND_ID 0
#define ENABLE_VALIDATION false

class VulkanExample : public VulkanExampleBase {
private:
    struct {
        vkx::texture::Texture2D colorHeightMap;
    } textures;

public:
    bool splitScreen = true;
    bool displacement = true;

    struct {
        vk::PipelineVertexInputStateCreateInfo inputState;
        std::vector<vk::VertexInputBindingDescription> bindingDescriptions;
        std::vector<vk::VertexInputAttributeDescription> attributeDescriptions;
    } vertices;

    // Vertex layout for the models
    vkx::vertex::Layout vertexLayout{ {
        vkx::vertex::VERTEX_COMPONENT_POSITION,
        vkx::vertex::VERTEX_COMPONENT_NORMAL,
        vkx::vertex::VERTEX_COMPONENT_UV,
    } };

    struct {
        vkx::model::Model object;
    } models;

    struct {
        vks::Buffer tessControl, tessEval;
    } uniformBuffers;

    struct UBOTessControl {
        float tessLevel = 64.0f;
    } uboTessControl;

    struct UBOTessEval {
        glm::mat4 projection;
        glm::mat4 model;
        glm::vec4 lightPos = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
        float tessAlpha = 1.0f;
        float tessStrength = 0.1f;
    } uboTessEval;

    struct Pipelines {
        vk::Pipeline solid;
        vk::Pipeline wireframe = VK_NULL_HANDLE;
    } pipelines;

    vk::PipelineLayout pipelineLayout;
    vk::DescriptorSet descriptorSet;
    vk::DescriptorSetLayout descriptorSetLayout;

    VulkanExample()
        : VulkanExampleBase(ENABLE_VALIDATION) {
        zoom = -1.25f;
        rotation = glm::vec3(-20.0f, 45.0f, 0.0f);
        title = "Tessellation shader displacement";
        settings.overlay = true;
    }

    ~VulkanExample() {
        // Clean up used Vulkan resources
        // Note : Inherited destructor cleans up resources stored in base class
        vkDestroyPipeline(device, pipelines.solid, nullptr);
        if (pipelines.wireframe != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipelines.wireframe, nullptr);
        };

        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);

        uniformBuffers.tessControl.destroy();
        uniformBuffers.tessEval.destroy();
        models.object.destroy();
        textures.colorHeightMap.destroy();
    }

    // Enable physical device features required for this example
    virtual void getEnabledFeatures() {
        // Tessellation shader support is required for this example
        if (deviceFeatures.tessellationShader) {
            enabledFeatures.tessellationShader = VK_TRUE;
        } else {
            vks::tools::exitFatal("Selected GPU does not support tessellation shaders!", VK_ERROR_FEATURE_NOT_PRESENT);
        }
        // Fill mode non solid is required for wireframe display
        if (deviceFeatures.fillModeNonSolid) {
            enabledFeatures.fillModeNonSolid = VK_TRUE;
        } else {
            splitScreen = false;
        }
        // Enable texture compression features. We already fail in loadAssets()
        // below if none are supported, so just enable whatever exists.
        if (deviceFeatures.textureCompressionBC) {
            enabledFeatures.textureCompressionBC = VK_TRUE;
        }

        if (deviceFeatures.textureCompressionETC2) {
            enabledFeatures.textureCompressionETC2 = VK_TRUE;
        }

        if (deviceFeatures.textureCompressionASTC_LDR) {
            enabledFeatures.textureCompressionASTC_LDR = VK_TRUE;
        }
    }

    void loadAssets() {
        models.object.loadFromFile(context, getAssetPath() + "models/plane.obj", vertexLayout, 0.25f);
        // Textures
        if (vulkanDevice->features.textureCompressionBC) {
            textures.colorHeightMap.loadFromFile(context, getAssetPath() + "textures/stonefloor03_color_bc3_unorm.ktx", vk::Format::eBC3_UNORM_BLOCK);
        } else if (vulkanDevice->features.textureCompressionASTC_LDR) {
            textures.colorHeightMap.loadFromFile(context, getAssetPath() + "textures/stonefloor03_color_astc_8x8_unorm.ktx", vk::Format::eASTC_8x8_UNORM_BLOCK);
        } else if (vulkanDevice->features.textureCompressionETC2) {
            textures.colorHeightMap.loadFromFile(context, getAssetPath() + "textures/stonefloor03_color_etc2_unorm.ktx", vk::Format::eETC2_R8G8B8_UNORM_BLOCK);
        } else {
            vks::tools::exitFatal("Device does not support any compressed texture format!", VK_ERROR_FEATURE_NOT_PRESENT);
        }
    }

    void buildCommandBuffers() {
        vk::CommandBufferBeginInfo cmdBufInfo;

        vk::ClearValue clearValues[2];
        clearValues[0].color = defaultClearColor;
        clearValues[1].depthStencil = { 1.0f, 0 };

        vk::RenderPassBeginInfo renderPassBeginInfo;
        renderPassBeginInfo.renderPass = renderPass;
        renderPassBeginInfo.renderArea.offset.x = 0;
        renderPassBeginInfo.renderArea.offset.y = 0;
        renderPassBeginInfo.renderArea.extent.width = width;
        renderPassBeginInfo.renderArea.extent.height = height;
        renderPassBeginInfo.clearValueCount = 2;
        renderPassBeginInfo.pClearValues = clearValues;

        for (int32_t i = 0; i < drawCmdBuffers.size(); ++i) {
            // Set target frame buffer
            renderPassBeginInfo.framebuffer = frameBuffers[i];

            VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));

            vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

            vk::Viewport viewport{ (float)width, (float)height, 0.0f, 1.0f };
            vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

            vk::Rect2D scissor{ splitScreen ? width / 2 : width, height, 0, 0 };
            vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

            vkCmdSetLineWidth(drawCmdBuffers[i], 1.0f);

            vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, NULL);

            vk::DeviceSize offsets[1] = { 0 };
            vkCmdBindVertexBuffers(drawCmdBuffers[i], VERTEX_BUFFER_BIND_ID, 1, &models.object.vertices.buffer, offsets);
            vkCmdBindIndexBuffer(drawCmdBuffers[i], models.object.indices.buffer, 0, VK_INDEX_TYPE_UINT32);

            if (splitScreen) {
                vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.wireframe);
                vkCmdDrawIndexed(drawCmdBuffers[i], models.object.indexCount, 1, 0, 0, 0);
                scissor.offset.x = width / 2;
                vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);
            }

            vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.solid);
            vkCmdDrawIndexed(drawCmdBuffers[i], models.object.indexCount, 1, 0, 0, 0);

            drawUI(drawCmdBuffers[i]);

            vkCmdEndRenderPass(drawCmdBuffers[i]);

            VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
        }
    }

    void setupVertexDescriptions() {
        // Binding description
        vertices.bindingDescriptions.resize(1);
        vertices.bindingDescriptions[0] =
            vk::vertexInputBindingDescription{VERTEX_BUFFER_BIND_ID, vertexLayout.stride(), vk::VertexInputRate::eVertex};

        // Attribute descriptions
        // Describes memory layout and shader positions
        vertices.attributeDescriptions.resize(3);

        // Location 0 : Position
        vertices.attributeDescriptions[0] = vk::vertexInputAttributeDescription{VERTEX_BUFFER_BIND_ID, 0, vk::Format::eR32G32B32sFloat, 0};

        // Location 1 : Normals
        vertices.attributeDescriptions[1] =
            vk::vertexInputAttributeDescription{VERTEX_BUFFER_BIND_ID, 1, vk::Format::eR32G32B32sFloat, sizeof(float) * 3};

        // Location 2 : Texture coordinates
        vertices.attributeDescriptions[2] =
            vk::vertexInputAttributeDescription{VERTEX_BUFFER_BIND_ID, 2, vk::Format::eR32G32sFloat, sizeof(float) * 6};

        vertices.inputState;
        vertices.inputState.vertexBindingDescriptionCount = static_cast<uint32_t>(vertices.bindingDescriptions.size());
        vertices.inputState.pVertexBindingDescriptions = vertices.bindingDescriptions.data();
        vertices.inputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertices.attributeDescriptions.size());
        vertices.inputState.pVertexAttributeDescriptions = vertices.attributeDescriptions.data();
    }

    void setupDescriptorPool() {
        // Example uses two ubos and two image samplers
        std::vector<vk::DescriptorPoolSize> poolSizes = { { vk::DescriptorType::eUniformBuffer, 2 }, { vk::DescriptorType::eCombinedImageSampler, 1 } };

        vk::DescriptorPoolCreateInfo descriptorPoolInfo =
            vk::descriptorPoolCreateInfo{static_cast<uint32_t>(poolSizes.size()), poolSizes.data(), 2};

        VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));
    }

    void setupDescriptorSetLayout() {
        std::vector<vk::DescriptorSetLayoutBinding> setLayoutBindings = {
            // Binding 0 : Tessellation control shader ubo
            vk::descriptorSetLayoutBinding{vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eTESSELLATION_CONTROL, 0},
            // Binding 1 : Tessellation evaluation shader ubo
            vk::descriptorSetLayoutBinding{vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eTESSELLATION_EVALUATION, 1},
            // Binding 2 : Combined color (rgb) and height (alpha) map
            vks::initializers::descriptorSetLayoutBinding(vk::DescriptorType::eCombinedImageSampler,
                                                          vk::ShaderStageFlagBits::eTESSELLATION_EVALUATION | vk::ShaderStageFlagBits::eFragment, 2),
        };

        vk::DescriptorSetLayoutCreateInfo descriptorLayout =
            vk::descriptorSetLayoutCreateInfo{setLayoutBindings.data(), static_cast<uint32_t>(setLayoutBindings.size())};

        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayout));

        vk::PipelineLayoutCreateInfo pPipelineLayoutCreateInfo = vk::pipelineLayoutCreateInfo{&descriptorSetLayout, 1};

        VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pPipelineLayoutCreateInfo, nullptr, &pipelineLayout));
    }

    void setupDescriptorSet() {
        vk::DescriptorSetAllocateInfo allocInfo{ descriptorPool, &descriptorSetLayout, 1 };
        VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet));

        std::vector<vk::WriteDescriptorSet> writeDescriptorSets = {
            // Binding 0 : Tessellation control shader ubo
            { descriptorSet, vk::DescriptorType::eUniformBuffer, 0, &uniformBuffers.tessControl.descriptor },
            // Binding 1 : Tessellation evaluation shader ubo
            { descriptorSet, vk::DescriptorType::eUniformBuffer, 1, &uniformBuffers.tessEval.descriptor },
            // Binding 2 : Color and displacement map (alpha channel)
            { descriptorSet, vk::DescriptorType::eCombinedImageSampler, 2, &textures.colorHeightMap.descriptor },
        };
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, NULL);
    }

    void preparePipelines() {
        vk::PipelineInputAssemblyStateCreateInfo inputAssemblyState =
            vk::pipelineInputAssemblyStateCreateInfo{VK_PRIMITIVE_TOPOLOGY_PATCH_LIST, 0, VK_FALSE};

        vk::PipelineRasterizationStateCreateInfo rasterizationState =
            vk::pipelineRasterizationStateCreateInfo{VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0};

        vk::PipelineColorBlendAttachmentState blendAttachmentState = vk::pipelineColorBlendAttachmentState{0xf, VK_FALSE};

        vk::PipelineColorBlendStateCreateInfo colorBlendState = vk::pipelineColorBlendStateCreateInfo{1, &blendAttachmentState};

        vk::PipelineDepthStencilStateCreateInfo depthStencilState =
            vk::pipelineDepthStencilStateCreateInfo{VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL};

        vk::PipelineViewportStateCreateInfo viewportState = { 1, 1, 0 };

        vk::PipelineMultisampleStateCreateInfo multisampleState = vk::pipelineMultisampleStateCreateInfo{VK_SAMPLE_COUNT_1_BIT, 0};

        std::vector<vk::DynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH };
        vk::PipelineDynamicStateCreateInfo dynamicState =
            vk::pipelineDynamicStateCreateInfo{dynamicStateEnables.data(), static_cast<uint32_t>(dynamicStateEnables.size()), 0};

        vk::PipelineTessellationStateCreateInfo tessellationState = { 3 };

        // Tessellation pipeline
        // Load shaders
        std::array<vk::PipelineShaderStageCreateInfo, 4> shaderStages;
        shaderStages[0] = loadShader(getAssetPath() + "shaders/displacement/base.vert.spv", vk::ShaderStageFlagBits::eVertex);
        shaderStages[1] = loadShader(getAssetPath() + "shaders/displacement/base.frag.spv", vk::ShaderStageFlagBits::eFragment);
        shaderStages[2] = loadShader(getAssetPath() + "shaders/displacement/displacement.tesc.spv", vk::ShaderStageFlagBits::eTESSELLATION_CONTROL);
        shaderStages[3] = loadShader(getAssetPath() + "shaders/displacement/displacement.tese.spv", vk::ShaderStageFlagBits::eTESSELLATION_EVALUATION);

        vk::GraphicsPipelineCreateInfo pipelineCreateInfo = vk::pipelineCreateInfo{pipelineLayout, renderPass, 0};

        pipelineCreateInfo.pVertexInputState = &vertices.inputState;
        pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
        pipelineCreateInfo.pRasterizationState = &rasterizationState;
        pipelineCreateInfo.pColorBlendState = &colorBlendState;
        pipelineCreateInfo.pMultisampleState = &multisampleState;
        pipelineCreateInfo.pViewportState = &viewportState;
        pipelineCreateInfo.pDepthStencilState = &depthStencilState;
        pipelineCreateInfo.pDynamicState = &dynamicState;
        pipelineCreateInfo.pTessellationState = &tessellationState;
        pipelineCreateInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
        pipelineCreateInfo.pStages = shaderStages.data();
        pipelineCreateInfo.renderPass = renderPass;

        // Solid pipeline
        rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
        VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.solid));
        if (deviceFeatures.fillModeNonSolid) {
            // Wireframe pipeline
            rasterizationState.polygonMode = VK_POLYGON_MODE_LINE;
            rasterizationState.cullMode = VK_CULL_MODE_NONE;
            VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.wireframe));
        }
    }

    // Prepare and initialize uniform buffer containing shader uniforms
    void prepareUniformBuffers() {
        // Tessellation evaluation shader uniform buffer
        VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.tessEval,
                                                   sizeof(uboTessEval)));

        // Tessellation control shader uniform buffer
        VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.tessControl,
                                                   sizeof(uboTessControl)));

        // Map persistent
        VK_CHECK_RESULT(uniformBuffers.tessControl.map());
        VK_CHECK_RESULT(uniformBuffers.tessEval.map());

        updateUniformBuffers();
    }

    void updateUniformBuffers() {
        // Tessellation eval
        glm::mat4 viewMatrix = glm::mat4(1.0f);
        uboTessEval.projection = glm::perspective(glm::radians(45.0f), (float)(width) / (float)height, 0.1f, 256.0f);
        viewMatrix = glm::translate(viewMatrix, glm::vec3(0.0f, 0.0f, zoom));

        uboTessEval.model = glm::mat4(1.0f);
        uboTessEval.model = viewMatrix * glm::translate(uboTessEval.model, glm::vec3(0, 0, 0));
        uboTessEval.model = glm::rotate(uboTessEval.model, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        uboTessEval.model = glm::rotate(uboTessEval.model, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        uboTessEval.model = glm::rotate(uboTessEval.model, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));

        uboTessEval.lightPos.y = -0.5f - uboTessEval.tessStrength;

        memcpy(uniformBuffers.tessEval.mapped, &uboTessEval, sizeof(uboTessEval));

        // Tessellation control
        float savedLevel = uboTessControl.tessLevel;
        if (!displacement) {
            uboTessControl.tessLevel = 1.0f;
        }

        memcpy(uniformBuffers.tessControl.mapped, &uboTessControl, sizeof(uboTessControl));

        if (!displacement) {
            uboTessControl.tessLevel = savedLevel;
        }
    }

    void draw() {
        VulkanExampleBase::prepareFrame();

        // Command buffer to be sumitted to the queue
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &drawCmdBuffers[currentBuffer];

        // Submit to queue
        VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));

        VulkanExampleBase::submitFrame();
    }

    void prepare() {
        VulkanExampleBase::prepare();

        setupVertexDescriptions();
        prepareUniformBuffers();
        setupDescriptorSetLayout();
        preparePipelines();
        setupDescriptorPool();
        setupDescriptorSet();
        buildCommandBuffers();
        prepared = true;
    }

    virtual void render() {
        if (!prepared)
            return;
        draw();
    }

    virtual void viewChanged() {
        updateUniformBuffers();
    }

    virtual void OnUpdateUIOverlay(vks::UIOverlay* overlay) {
        if (overlay->header("Settings")) {
            if (overlay->checkBox("Tessellation displacement", &displacement)) {
                updateUniformBuffers();
            }
            if (overlay->inputFloat("Strength", &uboTessEval.tessStrength, 0.025f, 3)) {
                updateUniformBuffers();
            }
            if (overlay->inputFloat("Level", &uboTessControl.tessLevel, 0.5f, 2)) {
                updateUniformBuffers();
            }
            if (deviceFeatures.fillModeNonSolid) {
                if (overlay->checkBox("Splitscreen", &splitScreen)) {
                    buildCommandBuffers();
                    updateUniformBuffers();
                }
            }
        }
    }
};

VULKAN_EXAMPLE_MAIN()