/*
* Vulkan Example - Shadow mapping for directional light sources
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
#include "VulkanBuffer.hpp"
#include "VulkanModel.hpp"

#define ENABLE_VALIDATION false

// 16 bits of depth is enough for such a small scene
#define DEPTH_FORMAT vk::Format::eD16Unorm

// Shadowmap properties
#if defined(__ANDROID__)
#define SHADOWMAP_DIM 1024
#else
#define SHADOWMAP_DIM 2048
#endif
#define SHADOWMAP_FILTER VK_FILTER_LINEAR

// Offscreen frame buffer properties
#define FB_COLOR_FORMAT vk::Format::eR8G8B8A8Unorm

class VulkanExample : public VulkanExampleBase {
public:
    bool displayShadowMap = false;
    bool filterPCF = true;

    // Keep depth range as small as possible
    // for better shadow map precision
    float zNear = 1.0f;
    float zFar = 96.0f;

    // Depth bias (and slope) are used to avoid shadowing artefacts
    // Constant depth bias factor (always applied)
    float depthBiasConstant = 1.25f;
    // Slope depth bias factor, applied depending on polygon's slope
    float depthBiasSlope = 1.75f;

    glm::vec3 lightPos = glm::vec3();
    float lightFOV = 45.0f;

    // Vertex layout for the models
    vkx::vertex::Layout vertexLayout{ {
        vkx::vertex::VERTEX_COMPONENT_POSITION,
        vkx::vertex::VERTEX_COMPONENT_UV,
        vkx::vertex::VERTEX_COMPONENT_COLOR,
        vkx::vertex::VERTEX_COMPONENT_NORMAL,
    } };

    struct {
        vkx::model::Model quad;
    } models;

    std::vector<vkx::model::Model> scenes;
    std::vector<std::string> sceneNames;
    int32_t sceneIndex = 0;

    struct {
        vks::Buffer scene;
        vks::Buffer offscreen;
        vks::Buffer debug;
    } uniformBuffers;

    struct {
        glm::mat4 projection;
        glm::mat4 model;
    } uboVSquad;

    struct {
        glm::mat4 projection;
        glm::mat4 view;
        glm::mat4 model;
        glm::mat4 depthBiasMVP;
        glm::vec3 lightPos;
    } uboVSscene;

    struct {
        glm::mat4 depthMVP;
    } uboOffscreenVS;

    struct {
        vk::Pipeline quad;
        vk::Pipeline offscreen;
        vk::Pipeline sceneShadow;
        vk::Pipeline sceneShadowPCF;
    } pipelines;

    struct {
        vk::PipelineLayout quad;
        vk::PipelineLayout offscreen;
    } pipelineLayouts;

    struct {
        vk::DescriptorSet offscreen;
        vk::DescriptorSet scene;
    } descriptorSets;

    vk::DescriptorSet descriptorSet;
    vk::DescriptorSetLayout descriptorSetLayout;

    // Framebuffer for offscreen rendering
    struct FrameBufferAttachment {
        vk::Image image;
        vk::DeviceMemory mem;
        vk::ImageView view;
    };
    struct OffscreenPass {
        int32_t width, height;
        vk::Framebuffer frameBuffer;
        FrameBufferAttachment depth;
        vk::RenderPass renderPass;
        vk::Sampler depthSampler;
        vk::DescriptorImageInfo descriptor;
    } offscreenPass;

    VulkanExample()
        : VulkanExampleBase(ENABLE_VALIDATION) {
        zoom = -20.0f;
        rotation = { -15.0f, -390.0f, 0.0f };
        title = "Projected shadow mapping";
        timerSpeed *= 0.5f;
        settings.overlay = true;
    }

    ~VulkanExample() {
        // Clean up used Vulkan resources
        // Note : Inherited destructor cleans up resources stored in base class

        // Frame buffer
        vkDestroySampler(device, offscreenPass.depthSampler, nullptr);

        // Depth attachment
        vkDestroyImageView(device, offscreenPass.depth.view, nullptr);
        vkDestroyImage(device, offscreenPass.depth.image, nullptr);
        vkFreeMemory(device, offscreenPass.depth.mem, nullptr);

        vkDestroyFramebuffer(device, offscreenPass.frameBuffer, nullptr);

        vkDestroyRenderPass(device, offscreenPass.renderPass, nullptr);

        vkDestroyPipeline(device, pipelines.quad, nullptr);
        vkDestroyPipeline(device, pipelines.offscreen, nullptr);
        vkDestroyPipeline(device, pipelines.sceneShadow, nullptr);
        vkDestroyPipeline(device, pipelines.sceneShadowPCF, nullptr);

        vkDestroyPipelineLayout(device, pipelineLayouts.quad, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayouts.offscreen, nullptr);

        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);

        // Meshes
        for (auto scene : scenes) {
            scene.destroy();
        }
        models.quad.destroy();

        // Uniform buffers
        uniformBuffers.offscreen.destroy();
        uniformBuffers.scene.destroy();
        uniformBuffers.debug.destroy();
    }

    // Set up a separate render pass for the offscreen frame buffer
    // This is necessary as the offscreen frame buffer attachments use formats different to those from the example render pass
    void prepareOffscreenRenderpass() {
        vk::AttachmentDescription attachmentDescription{};
        attachmentDescription.format = DEPTH_FORMAT;
        attachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT;
        attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;    // Clear depth at beginning of the render pass
        attachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_STORE;  // We will read from depth, so it's important to store the depth attachment results
        attachmentDescription.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachmentDescription.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // We don't care about initial layout of the attachment
        attachmentDescription.finalLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;  // Attachment will be transitioned to shader read at render pass end

        vk::AttachmentReference depthReference = {};
        depthReference.attachment = 0;
        depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;  // Attachment will be used as depth/stencil during render pass

        vk::SubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 0;                   // No color attachments
        subpass.pDepthStencilAttachment = &depthReference;  // Reference to our depth attachment

        // Use subpass dependencies for layout transitions
        std::array<vk::SubpassDependency, 2> dependencies;

        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        vk::RenderPassCreateInfo renderPassCreateInfo;
        renderPassCreateInfo.attachmentCount = 1;
        renderPassCreateInfo.pAttachments = &attachmentDescription;
        renderPassCreateInfo.subpassCount = 1;
        renderPassCreateInfo.pSubpasses = &subpass;
        renderPassCreateInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
        renderPassCreateInfo.pDependencies = dependencies.data();

        VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassCreateInfo, nullptr, &offscreenPass.renderPass));
    }

    // Setup the offscreen framebuffer for rendering the scene from light's point-of-view to
    // The depth attachment of this framebuffer will then be used to sample from in the fragment shader of the shadowing pass
    void prepareOffscreenFramebuffer() {
        offscreenPass.width = SHADOWMAP_DIM;
        offscreenPass.height = SHADOWMAP_DIM;

        vk::Format fbColorFormat = FB_COLOR_FORMAT;

        // For shadow mapping we only need a depth attachment
        vk::ImageCreateInfo image;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.extent.width = offscreenPass.width;
        image.extent.height = offscreenPass.height;
        image.extent.depth = 1;
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.format = DEPTH_FORMAT;  // Depth stencil attachment
        image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;  // We will sample directly from the depth attachment for the shadow mapping
        VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &offscreenPass.depth.image));

        vk::MemoryAllocateInfo memAlloc;
        vk::MemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, offscreenPass.depth.image, &memReqs);
        memAlloc.allocationSize = memReqs.size;
        memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &offscreenPass.depth.mem));
        VK_CHECK_RESULT(vkBindImageMemory(device, offscreenPass.depth.image, offscreenPass.depth.mem, 0));

        vk::ImageViewCreateInfo depthStencilView;
        depthStencilView.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depthStencilView.format = DEPTH_FORMAT;
        depthStencilView.subresourceRange = {};
        depthStencilView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthStencilView.subresourceRange.baseMipLevel = 0;
        depthStencilView.subresourceRange.levelCount = 1;
        depthStencilView.subresourceRange.baseArrayLayer = 0;
        depthStencilView.subresourceRange.layerCount = 1;
        depthStencilView.image = offscreenPass.depth.image;
        VK_CHECK_RESULT(vkCreateImageView(device, &depthStencilView, nullptr, &offscreenPass.depth.view));

        // Create sampler to sample from to depth attachment
        // Used to sample in the fragment shader for shadowed rendering
        vk::SamplerCreateInfo sampler;
        sampler.magFilter = SHADOWMAP_FILTER;
        sampler.minFilter = SHADOWMAP_FILTER;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = sampler.addressModeU;
        sampler.addressModeW = sampler.addressModeU;
        sampler.mipLodBias = 0.0f;
        sampler.maxAnisotropy = 1.0f;
        sampler.minLod = 0.0f;
        sampler.maxLod = 1.0f;
        sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        VK_CHECK_RESULT(vkCreateSampler(device, &sampler, nullptr, &offscreenPass.depthSampler));

        prepareOffscreenRenderpass();

        // Create frame buffer
        vk::FramebufferCreateInfo fbufCreateInfo;
        fbufCreateInfo.renderPass = offscreenPass.renderPass;
        fbufCreateInfo.attachmentCount = 1;
        fbufCreateInfo.pAttachments = &offscreenPass.depth.view;
        fbufCreateInfo.width = offscreenPass.width;
        fbufCreateInfo.height = offscreenPass.height;
        fbufCreateInfo.layers = 1;

        VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &offscreenPass.frameBuffer));
    }

    void buildCommandBuffers() {
        vk::CommandBufferBeginInfo cmdBufInfo;

        vk::ClearValue clearValues[2];
        vk::Viewport viewport;
        vk::Rect2D scissor;
        vk::DeviceSize offsets[1] = { 0 };

        for (int32_t i = 0; i < drawCmdBuffers.size(); ++i) {
            VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));

            /*
				First render pass: Generate shadow map by rendering the scene from light's POV
			*/
            {
                clearValues[0].depthStencil = { 1.0f, 0 };

                vk::RenderPassBeginInfo renderPassBeginInfo;
                renderPassBeginInfo.renderPass = offscreenPass.renderPass;
                renderPassBeginInfo.framebuffer = offscreenPass.frameBuffer;
                renderPassBeginInfo.renderArea.extent.width = offscreenPass.width;
                renderPassBeginInfo.renderArea.extent.height = offscreenPass.height;
                renderPassBeginInfo.clearValueCount = 1;
                renderPassBeginInfo.pClearValues = clearValues;

                vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

                viewport{ (float)offscreenPass.width, (float)offscreenPass.height, 0.0f, 1.0f };
                vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

                scissor{ offscreenPass.width, offscreenPass.height, 0, 0 };
                vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

                // Set depth bias (aka "Polygon offset")
                // Required to avoid shadow mapping artefacts
                vkCmdSetDepthBias(drawCmdBuffers[i], depthBiasConstant, 0.0f, depthBiasSlope);

                vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.offscreen);
                vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.offscreen, 0, 1, &descriptorSets.offscreen, 0,
                                        NULL);

                vkCmdBindVertexBuffers(drawCmdBuffers[i], 0, 1, &scenes[sceneIndex].vertices.buffer, offsets);
                vkCmdBindIndexBuffer(drawCmdBuffers[i], scenes[sceneIndex].indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(drawCmdBuffers[i], scenes[sceneIndex].indexCount, 1, 0, 0, 0);

                vkCmdEndRenderPass(drawCmdBuffers[i]);
            }

            /*
				Note: Explicit synchronization is not required between the render pass, as this is done implicit via sub pass dependencies
			*/

            /*
				Second pass: Scene rendering with applied shadow map
			*/

            {
                clearValues[0].color = defaultClearColor;
                clearValues[1].depthStencil = { 1.0f, 0 };

                vk::RenderPassBeginInfo renderPassBeginInfo;
                renderPassBeginInfo.renderPass = renderPass;
                renderPassBeginInfo.framebuffer = frameBuffers[i];
                renderPassBeginInfo.renderArea.extent.width = width;
                renderPassBeginInfo.renderArea.extent.height = height;
                renderPassBeginInfo.clearValueCount = 2;
                renderPassBeginInfo.pClearValues = clearValues;

                vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

                viewport{ (float)width, (float)height, 0.0f, 1.0f };
                vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

                scissor{ width, height, 0, 0 };
                vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

                // Visualize shadow map
                if (displayShadowMap) {
                    vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.quad, 0, 1, &descriptorSet, 0, NULL);
                    vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.quad);
                    vkCmdBindVertexBuffers(drawCmdBuffers[i], 0, 1, &models.quad.vertices.buffer, offsets);
                    vkCmdBindIndexBuffer(drawCmdBuffers[i], models.quad.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(drawCmdBuffers[i], models.quad.indexCount, 1, 0, 0, 0);
                }

                // 3D scene
                vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.quad, 0, 1, &descriptorSets.scene, 0, NULL);
                vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, (filterPCF) ? pipelines.sceneShadowPCF : pipelines.sceneShadow);

                vkCmdBindVertexBuffers(drawCmdBuffers[i], 0, 1, &scenes[sceneIndex].vertices.buffer, offsets);
                vkCmdBindIndexBuffer(drawCmdBuffers[i], scenes[sceneIndex].indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(drawCmdBuffers[i], scenes[sceneIndex].indexCount, 1, 0, 0, 0);

                drawUI(drawCmdBuffers[i]);

                vkCmdEndRenderPass(drawCmdBuffers[i]);
            }

            VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
        }
    }

    void loadAssets() {
        scenes.resize(2);
        scenes[0].loadFromFile(context, getAssetPath() + "models/vulkanscene_shadow.dae", vertexLayout, 4.0f);
        scenes[1].loadFromFile(context, getAssetPath() + "models/samplescene.dae", vertexLayout, 0.25f);
        sceneNames = { "Vulkan scene", "Teapots and pillars" };
    }

    void generateQuad() {
        // Setup vertices for a single uv-mapped quad
        struct Vertex {
            float pos[3];
            float uv[2];
            float col[3];
            float normal[3];
        };

#define QUAD_COLOR_NORMAL { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 1.0f }
        std::vector<Vertex> vertexBuffer = { { { 1.0f, 1.0f, 0.0f }, { 1.0f, 1.0f }, QUAD_COLOR_NORMAL },
                                             { { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f }, QUAD_COLOR_NORMAL },
                                             { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f }, QUAD_COLOR_NORMAL },
                                             { { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }, QUAD_COLOR_NORMAL } };
#undef QUAD_COLOR_NORMAL

        VK_CHECK_RESULT(
            vulkanDevice->createBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       vertexBuffer.size() * sizeof(Vertex), &models.quad.vertices.buffer, &models.quad.vertices.memory, vertexBuffer.data()));

        // Setup indices
        std::vector<uint32_t> indexBuffer = { 0, 1, 2, 2, 3, 0 };
        models.quad.indexCount = indexBuffer.size();

        VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                   indexBuffer.size() * sizeof(uint32_t), &models.quad.indices.buffer, &models.quad.indices.memory,
                                                   indexBuffer.data()));

        models.quad.device = device;
    }

    void setupDescriptorPool() {
        // Example uses three ubos and two image samplers
        std::vector<vk::DescriptorPoolSize> poolSizes = { { vk::DescriptorType::eUniformBuffer, 6 }, { vk::DescriptorType::eCombinedImageSampler, 4 } };

        vk::DescriptorPoolCreateInfo descriptorPoolInfo = vk::descriptorPoolCreateInfo{poolSizes.size(), poolSizes.data(), 3};

        VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));
    }

    void setupDescriptorSetLayout() {
        // Textured quad pipeline layout
        std::vector<vk::DescriptorSetLayoutBinding> setLayoutBindings = { // Binding 0 : Vertex shader uniform buffer
                                                                          { vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eVertex, 0 },
                                                                          // Binding 1 : Fragment shader image sampler
                                                                          { vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment, 1 }
        };

        vk::DescriptorSetLayoutCreateInfo descriptorLayout =
            vk::descriptorSetLayoutCreateInfo{setLayoutBindings.data(), setLayoutBindings.size()};
        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayout));

        vk::PipelineLayoutCreateInfo pPipelineLayoutCreateInfo = vk::pipelineLayoutCreateInfo{&descriptorSetLayout, 1};
        VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pPipelineLayoutCreateInfo, nullptr, &pipelineLayouts.quad));

        // Offscreen pipeline layout
        VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pPipelineLayoutCreateInfo, nullptr, &pipelineLayouts.offscreen));
    }

    void setupDescriptorSets() {
        std::vector<vk::WriteDescriptorSet> writeDescriptorSets;

        // Textured quad descriptor set
        vk::DescriptorSetAllocateInfo allocInfo = vk::descriptorSetAllocateInfo{descriptorPool, &descriptorSetLayout, 1};

        VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet));

        // Image descriptor for the shadow map attachment
        vk::DescriptorImageInfo texDescriptor =
            vk::descriptorImageInfo{offscreenPass.depthSampler, offscreenPass.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};

        writeDescriptorSets = { // Binding 0 : Vertex shader uniform buffer
                                { descriptorSet, vk::DescriptorType::eUniformBuffer, 0, &uniformBuffers.debug.descriptor },
                                // Binding 1 : Fragment shader texture sampler
                                { descriptorSet, vk::DescriptorType::eCombinedImageSampler, 1, &texDescriptor }
        };
        vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);

        // Offscreen
        VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSets.offscreen));

        writeDescriptorSets = {
            // Binding 0 : Vertex shader uniform buffer
            { descriptorSets.offscreen, vk::DescriptorType::eUniformBuffer, 0, &uniformBuffers.offscreen.descriptor },
        };
        vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);

        // 3D scene
        VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSets.scene));

        // Image descriptor for the shadow map attachment
        texDescriptor.sampler = offscreenPass.depthSampler;
        texDescriptor.imageView = offscreenPass.depth.view;

        writeDescriptorSets = { // Binding 0 : Vertex shader uniform buffer
                                { descriptorSets.scene, vk::DescriptorType::eUniformBuffer, 0, &uniformBuffers.scene.descriptor },
                                // Binding 1 : Fragment shader shadow sampler
                                { descriptorSets.scene, vk::DescriptorType::eCombinedImageSampler, 1, &texDescriptor }
        };
        vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);
    }

    void preparePipelines() {
        vk::PipelineInputAssemblyStateCreateInfo inputAssemblyStateCI{ VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE };
        vk::PipelineRasterizationStateCreateInfo rasterizationStateCI{ VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE, 0 };
        vk::PipelineColorBlendAttachmentState blendAttachmentState{ 0xf, VK_FALSE };
        vk::PipelineColorBlendStateCreateInfo colorBlendStateCI{ 1, &blendAttachmentState };
        vk::PipelineDepthStencilStateCreateInfo depthStencilStateCI{ VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL };
        vk::PipelineViewportStateCreateInfo viewportStateCI{ 1, 1, 0 };
        vk::PipelineMultisampleStateCreateInfo multisampleStateCI{ VK_SAMPLE_COUNT_1_BIT, 0 };
        std::vector<vk::DynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        vk::PipelineDynamicStateCreateInfo dynamicStateCI{ dynamicStateEnables.data(), dynamicStateEnables.size(), 0 };
        std::array<vk::PipelineShaderStageCreateInfo, 2> shaderStages;

        vk::GraphicsPipelineCreateInfo pipelineCI{ pipelineLayouts.quad, renderPass, 0 };

        pipelineCI.pInputAssemblyState = &inputAssemblyStateCI;
        pipelineCI.pRasterizationState = &rasterizationStateCI;
        pipelineCI.pColorBlendState = &colorBlendStateCI;
        pipelineCI.pMultisampleState = &multisampleStateCI;
        pipelineCI.pViewportState = &viewportStateCI;
        pipelineCI.pDepthStencilState = &depthStencilStateCI;
        pipelineCI.pDynamicState = &dynamicStateCI;
        pipelineCI.stageCount = shaderStages.size();
        pipelineCI.pStages = shaderStages.data();

        // Shadow mapping debug quad display
        rasterizationStateCI.cullMode = VK_CULL_MODE_NONE;
        shaderStages[0] = loadShader(getAssetPath() + "shaders/shadowmapping/quad.vert.spv", vk::ShaderStageFlagBits::eVertex);
        shaderStages[1] = loadShader(getAssetPath() + "shaders/shadowmapping/quad.frag.spv", vk::ShaderStageFlagBits::eFragment);
        // Empty vertex input state
        vk::PipelineVertexInputStateCreateInfo emptyInputState;
        pipelineCI.pVertexInputState = &emptyInputState;
        VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.quad));

        // Vertex bindings and attributes
        std::vector<vk::VertexInputBindingDescription> vertexInputBindings = {
            { 0, vertexLayout.stride(), vk::VertexInputRate::eVertex },
        };
        std::vector<vk::VertexInputAttributeDescription> vertexInputAttributes = {
            { 0, 0, vk::Format::eR32G32B32sFloat, 0 },                  // Position
            { 0, 1, vk::Format::eR32G32sFloat, sizeof(float) * 3 },     // Texture coordinates
            { 0, 2, vk::Format::eR32G32B32sFloat, sizeof(float) * 5 },  // Color
            { 0, 3, vk::Format::eR32G32B32sFloat, sizeof(float) * 8 },  // Normal
        };
        vk::PipelineVertexInputStateCreateInfo vertexInputState;
        vertexInputState.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexInputBindings.size());
        vertexInputState.pVertexBindingDescriptions = vertexInputBindings.data();
        vertexInputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputAttributes.size());
        vertexInputState.pVertexAttributeDescriptions = vertexInputAttributes.data();
        pipelineCI.pVertexInputState = &vertexInputState;

        // Scene rendering with shadows applied
        rasterizationStateCI.cullMode = VK_CULL_MODE_BACK_BIT;
        shaderStages[0] = loadShader(getAssetPath() + "shaders/shadowmapping/scene.vert.spv", vk::ShaderStageFlagBits::eVertex);
        shaderStages[1] = loadShader(getAssetPath() + "shaders/shadowmapping/scene.frag.spv", vk::ShaderStageFlagBits::eFragment);
        // Use specialization constants to select between horizontal and vertical blur
        uint32_t enablePCF = 0;
        vk::SpecializationMapEntry specializationMapEntry{ 0, 0, sizeof(uint32_t) };
        vk::SpecializationInfo specializationInfo{ 1, &specializationMapEntry, sizeof(uint32_t), &enablePCF };
        shaderStages[1].pSpecializationInfo = &specializationInfo;
        // No filtering
        VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.sceneShadow));
        // PCF filtering
        enablePCF = 1;
        VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.sceneShadowPCF));

        // Offscreen pipeline (vertex shader only)
        shaderStages[0] = loadShader(getAssetPath() + "shaders/shadowmapping/offscreen.vert.spv", vk::ShaderStageFlagBits::eVertex);
        pipelineCI.stageCount = 1;
        // No blend attachment states (no color attachments used)
        colorBlendStateCI.attachmentCount = 0;
        // Cull front faces
        depthStencilStateCI.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        // Enable depth bias
        rasterizationStateCI.depthBiasEnable = VK_TRUE;
        // Add depth bias to dynamic state, so we can change it at runtime
        dynamicStateEnables.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
        dynamicStateCI = vk::pipelineDynamicStateCreateInfo{dynamicStateEnables.data(), dynamicStateEnables.size(), 0};

        pipelineCI.layout = pipelineLayouts.offscreen;
        pipelineCI.renderPass = offscreenPass.renderPass;
        VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.offscreen));
    }

    // Prepare and initialize uniform buffer containing shader uniforms
    void prepareUniformBuffers() {
        // Debug quad vertex shader uniform buffer block
        VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.debug,
                                                   sizeof(uboVSscene)));

        // Offscreen vertex shader uniform buffer block
        VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.offscreen,
                                                   sizeof(uboOffscreenVS)));

        // Scene vertex shader uniform buffer block
        VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.scene,
                                                   sizeof(uboVSscene)));

        // Map persistent
        VK_CHECK_RESULT(uniformBuffers.debug.map());
        VK_CHECK_RESULT(uniformBuffers.offscreen.map());
        VK_CHECK_RESULT(uniformBuffers.scene.map());

        updateLight();
        updateUniformBufferOffscreen();
        updateUniformBuffers();
    }

    void updateLight() {
        // Animate the light source
        lightPos.x = cos(glm::radians(timer * 360.0f)) * 40.0f;
        lightPos.y = -50.0f + sin(glm::radians(timer * 360.0f)) * 20.0f;
        lightPos.z = 25.0f + sin(glm::radians(timer * 360.0f)) * 5.0f;
    }

    void updateUniformBuffers() {
        // Shadow map debug quad
        float AR = (float)height / (float)width;

        uboVSquad.projection = glm::ortho(2.5f / AR, 0.0f, 0.0f, 2.5f, -1.0f, 1.0f);
        uboVSquad.model = glm::mat4(1.0f);

        memcpy(uniformBuffers.debug.mapped, &uboVSquad, sizeof(uboVSquad));

        // 3D scene
        uboVSscene.projection = glm::perspective(glm::radians(45.0f), (float)width / (float)height, zNear, zFar);

        uboVSscene.view = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, zoom));
        uboVSscene.view = glm::rotate(uboVSscene.view, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        uboVSscene.view = glm::rotate(uboVSscene.view, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        uboVSscene.view = glm::rotate(uboVSscene.view, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));

        uboVSscene.model = glm::mat4(1.0f);

        uboVSscene.lightPos = lightPos;

        uboVSscene.depthBiasMVP = uboOffscreenVS.depthMVP;

        memcpy(uniformBuffers.scene.mapped, &uboVSscene, sizeof(uboVSscene));
    }

    void updateUniformBufferOffscreen() {
        // Matrix from light's point of view
        glm::mat4 depthProjectionMatrix = glm::perspective(glm::radians(lightFOV), 1.0f, zNear, zFar);
        glm::mat4 depthViewMatrix = glm::lookAt(lightPos, glm::vec3(0.0f), glm::vec3(0, 1, 0));
        glm::mat4 depthModelMatrix = glm::mat4(1.0f);

        uboOffscreenVS.depthMVP = depthProjectionMatrix * depthViewMatrix * depthModelMatrix;

        memcpy(uniformBuffers.offscreen.mapped, &uboOffscreenVS, sizeof(uboOffscreenVS));
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

        generateQuad();
        prepareOffscreenFramebuffer();
        prepareUniformBuffers();
        setupDescriptorSetLayout();
        preparePipelines();
        setupDescriptorPool();
        setupDescriptorSets();
        buildCommandBuffers();
        prepared = true;
    }

    virtual void render() {
        if (!prepared)
            return;
        draw();
        if (!paused || camera.updated) {
            updateLight();
            updateUniformBufferOffscreen();
            updateUniformBuffers();
        }
    }

    virtual void OnUpdateUIOverlay(vks::UIOverlay* overlay) {
        if (overlay->header("Settings")) {
            if (overlay->comboBox("Scenes", &sceneIndex, sceneNames)) {
                buildCommandBuffers();
            }
            if (overlay->checkBox("Display shadow render target", &displayShadowMap)) {
                buildCommandBuffers();
            }
            if (overlay->checkBox("PCF filtering", &filterPCF)) {
                buildCommandBuffers();
            }
        }
    }
};

VULKAN_EXAMPLE_MAIN()
