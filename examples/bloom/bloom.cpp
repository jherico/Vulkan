/*
* Vulkan Example - Implements a separable two-pass fullscreen blur (also known as bloom)
*
* Copyright (C) Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#include <vulkanExampleBase.hpp>
#include <vertex.hpp>
#include <model.hpp>
// Offscreen frame buffer properties
#define FB_DIM 256
#define FB_COLOR_FORMAT vk::Format::eR8G8B8A8Unorm

class VulkanExample : public vkx::ExampleBase {
public:
    bool bloom{ true };

    struct Textures {
        vks::texture::TextureCubeMap cubemap;
    } textures;

    // Vertex layout for the models
    vkx::vertex::Layout vertexLayout{ {
        vkx::vertex::VERTEX_COMPONENT_POSITION,
        vkx::vertex::VERTEX_COMPONENT_UV,
        vkx::vertex::VERTEX_COMPONENT_COLOR,
        vkx::vertex::VERTEX_COMPONENT_NORMAL,
    } };

    struct {
        vkx::model::Model ufo;
        vkx::model::Model ufoGlow;
        vkx::model::Model skyBox;
    } models;

    struct {
        vks::Buffer scene;
        vks::Buffer skyBox;
        vks::Buffer blurParams;
    } uniformBuffers;

    struct UBO {
        glm::mat4 projection;
        glm::mat4 view;
        glm::mat4 model;
    };

    struct UBOBlurParams {
        float blurScale = 1.0f;
        float blurStrength = 1.5f;
    };

    struct {
        UBO scene, skyBox;
        UBOBlurParams blurParams;
    } ubos;

    struct {
        vk::Pipeline blurVert;
        vk::Pipeline blurHorz;
        vk::Pipeline glowPass;
        vk::Pipeline phongPass;
        vk::Pipeline skyBox;
    } pipelines;

    struct PipelineLayouts {
        vk::PipelineLayout blur;
        vk::PipelineLayout scene;
    } pipelineLayouts;

    struct DescriptorSets {
        vk::DescriptorSet blurVert;
        vk::DescriptorSet blurHorz;
        vk::DescriptorSet scene;
        vk::DescriptorSet skyBox;
    } descriptorSets;

    struct DescriptorSetLayouts {
        vk::DescriptorSetLayout blur;
        vk::DescriptorSetLayout scene;
    } descriptorSetLayouts;

    // Framebuffer for offscreen rendering
    struct FrameBufferAttachment {
        vk::Image image;
        vk::DeviceMemory mem;
        vk::ImageView view;
    };
    struct FrameBuffer {
        vk::Framebuffer framebuffer;
        FrameBufferAttachment color, depth;
        vk::DescriptorImageInfo descriptor;
    };
    struct OffscreenPass {
        int32_t width, height;
        vk::RenderPass renderPass;
        vk::Sampler sampler;
        std::array<FrameBuffer, 2> framebuffers;
    } offscreenPass;

    VulkanExample() {
        title = "Bloom (offscreen rendering)";
        timerSpeed *= 0.5f;
        settings.overlay = true;
        camera.type = Camera::CameraType::lookat;
        camera.setPosition(glm::vec3(0.0f, 0.0f, -10.25f));
        camera.setRotation(glm::vec3(7.5f, -343.0f, 0.0f));
        camera.setPerspective(45.0f, (float)width / (float)height, 0.1f, 256.0f);
    }

    ~VulkanExample() {
        // Clean up used Vulkan resources
        // Note : Inherited destructor cleans up resources stored in base class

        vkDestroySampler(device, offscreenPass.sampler, nullptr);

        // Frame buffer
        for (auto& framebuffer : offscreenPass.framebuffers) {
            // Attachments
            vkDestroyImageView(device, framebuffer.color.view, nullptr);
            vkDestroyImage(device, framebuffer.color.image, nullptr);
            vkFreeMemory(device, framebuffer.color.mem, nullptr);
            vkDestroyImageView(device, framebuffer.depth.view, nullptr);
            vkDestroyImage(device, framebuffer.depth.image, nullptr);
            vkFreeMemory(device, framebuffer.depth.mem, nullptr);

            vkDestroyFramebuffer(device, framebuffer.framebuffer, nullptr);
        }
        vkDestroyRenderPass(device, offscreenPass.renderPass, nullptr);

        vkDestroyPipeline(device, pipelines.blurHorz, nullptr);
        vkDestroyPipeline(device, pipelines.blurVert, nullptr);
        vkDestroyPipeline(device, pipelines.phongPass, nullptr);
        vkDestroyPipeline(device, pipelines.glowPass, nullptr);
        vkDestroyPipeline(device, pipelines.skyBox, nullptr);

        vkDestroyPipelineLayout(device, pipelineLayouts.blur, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayouts.scene, nullptr);

        vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.blur, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.scene, nullptr);

        // Models
        models.ufo.destroy();
        models.ufoGlow.destroy();
        models.skyBox.destroy();

        // Uniform buffers
        uniformBuffers.scene.destroy();
        uniformBuffers.skyBox.destroy();
        uniformBuffers.blurParams.destroy();

        textures.cubemap.destroy();
    }

    // Setup the offscreen framebuffer for rendering the mirrored scene
    // The color attachment of this framebuffer will then be sampled from
    void prepareOffscreenFramebuffer(FrameBuffer* frameBuf, vk::Format colorFormat, vk::Format depthFormat) {
        // Color attachment
        vk::ImageCreateInfo image;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = colorFormat;
        image.extent.width = FB_DIM;
        image.extent.height = FB_DIM;
        image.extent.depth = 1;
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        // We will sample directly from the color attachment
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        vk::MemoryAllocateInfo memAlloc;
        vk::MemoryRequirements memReqs;

        vk::ImageViewCreateInfo colorImageView;
        colorImageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
        colorImageView.format = colorFormat;
        colorImageView.flags = 0;
        colorImageView.subresourceRange = {};
        colorImageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        colorImageView.subresourceRange.baseMipLevel = 0;
        colorImageView.subresourceRange.levelCount = 1;
        colorImageView.subresourceRange.baseArrayLayer = 0;
        colorImageView.subresourceRange.layerCount = 1;

        VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &frameBuf->color.image));
        vkGetImageMemoryRequirements(device, frameBuf->color.image, &memReqs);
        memAlloc.allocationSize = memReqs.size;
        memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &frameBuf->color.mem));
        VK_CHECK_RESULT(vkBindImageMemory(device, frameBuf->color.image, frameBuf->color.mem, 0));

        colorImageView.image = frameBuf->color.image;
        VK_CHECK_RESULT(vkCreateImageView(device, &colorImageView, nullptr, &frameBuf->color.view));

        // Depth stencil attachment
        image.format = depthFormat;
        image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        vk::ImageViewCreateInfo depthStencilView;
        depthStencilView.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depthStencilView.format = depthFormat;
        depthStencilView.flags = 0;
        depthStencilView.subresourceRange = {};
        depthStencilView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        depthStencilView.subresourceRange.baseMipLevel = 0;
        depthStencilView.subresourceRange.levelCount = 1;
        depthStencilView.subresourceRange.baseArrayLayer = 0;
        depthStencilView.subresourceRange.layerCount = 1;

        VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &frameBuf->depth.image));
        vkGetImageMemoryRequirements(device, frameBuf->depth.image, &memReqs);
        memAlloc.allocationSize = memReqs.size;
        memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &frameBuf->depth.mem));
        VK_CHECK_RESULT(vkBindImageMemory(device, frameBuf->depth.image, frameBuf->depth.mem, 0));

        depthStencilView.image = frameBuf->depth.image;
        VK_CHECK_RESULT(vkCreateImageView(device, &depthStencilView, nullptr, &frameBuf->depth.view));

        vk::ImageView attachments[2];
        attachments[0] = frameBuf->color.view;
        attachments[1] = frameBuf->depth.view;

        vk::FramebufferCreateInfo fbufCreateInfo;
        fbufCreateInfo.renderPass = offscreenPass.renderPass;
        fbufCreateInfo.attachmentCount = 2;
        fbufCreateInfo.pAttachments = attachments;
        fbufCreateInfo.width = FB_DIM;
        fbufCreateInfo.height = FB_DIM;
        fbufCreateInfo.layers = 1;

        VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &frameBuf->framebuffer));

        // Fill a descriptor for later use in a descriptor set
        frameBuf->descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        frameBuf->descriptor.imageView = frameBuf->color.view;
        frameBuf->descriptor.sampler = offscreenPass.sampler;
    }

    // Prepare the offscreen framebuffers used for the vertical- and horizontal blur
    void prepareOffscreen() {
        offscreenPass.width = FB_DIM;
        offscreenPass.height = FB_DIM;

        // Find a suitable depth format
        vk::Format fbDepthFormat;
        vk::Bool32 validDepthFormat = vks::tools::getSupportedDepthFormat(physicalDevice, &fbDepthFormat);
        assert(validDepthFormat);

        // Create a separate render pass for the offscreen rendering as it may differ from the one used for scene rendering

        std::array<vk::AttachmentDescription, 2> attchmentDescriptions = {};
        // Color attachment
        attchmentDescriptions[0].format = FB_COLOR_FORMAT;
        attchmentDescriptions[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attchmentDescriptions[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attchmentDescriptions[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attchmentDescriptions[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attchmentDescriptions[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attchmentDescriptions[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attchmentDescriptions[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // Depth attachment
        attchmentDescriptions[1].format = fbDepthFormat;
        attchmentDescriptions[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attchmentDescriptions[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attchmentDescriptions[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attchmentDescriptions[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attchmentDescriptions[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attchmentDescriptions[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attchmentDescriptions[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        vk::AttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        vk::AttachmentReference depthReference = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

        vk::SubpassDescription subpassDescription = {};
        subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDescription.colorAttachmentCount = 1;
        subpassDescription.pColorAttachments = &colorReference;
        subpassDescription.pDepthStencilAttachment = &depthReference;

        // Use subpass dependencies for layout transitions
        std::array<vk::SubpassDependency, 2> dependencies;

        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        // Create the actual renderpass
        vk::RenderPassCreateInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attchmentDescriptions.size());
        renderPassInfo.pAttachments = attchmentDescriptions.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpassDescription;
        renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
        renderPassInfo.pDependencies = dependencies.data();

        VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassInfo, nullptr, &offscreenPass.renderPass));

        // Create sampler to sample from the color attachments
        vk::SamplerCreateInfo sampler;
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = sampler.addressModeU;
        sampler.addressModeW = sampler.addressModeU;
        sampler.mipLodBias = 0.0f;
        sampler.maxAnisotropy = 1.0f;
        sampler.minLod = 0.0f;
        sampler.maxLod = 1.0f;
        sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        VK_CHECK_RESULT(vkCreateSampler(device, &sampler, nullptr, &offscreenPass.sampler));

        // Create two frame buffers
        prepareOffscreenFramebuffer(&offscreenPass.framebuffers[0], FB_COLOR_FORMAT, fbDepthFormat);
        prepareOffscreenFramebuffer(&offscreenPass.framebuffers[1], FB_COLOR_FORMAT, fbDepthFormat);
    }

    void buildCommandBuffers() {
        vk::CommandBufferBeginInfo cmdBufInfo;

        vk::ClearValue clearValues[2];
        vk::Viewport viewport;
        vk::Rect2D scissor;
        vk::DeviceSize offsets[1] = { 0 };

        /*
			The blur method used in this example is multi pass and renders the vertical blur first and then the horizontal one
			While it's possible to blur in one pass, this method is widely used as it requires far less samples to generate the blur
		*/

        for (int32_t i = 0; i < drawCmdBuffers.size(); ++i) {
            VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));

            if (bloom) {
                clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
                clearValues[1].depthStencil = { 1.0f, 0 };

                vk::RenderPassBeginInfo renderPassBeginInfo;
                renderPassBeginInfo.renderPass = offscreenPass.renderPass;
                renderPassBeginInfo.framebuffer = offscreenPass.framebuffers[0].framebuffer;
                renderPassBeginInfo.renderArea.extent.width = offscreenPass.width;
                renderPassBeginInfo.renderArea.extent.height = offscreenPass.height;
                renderPassBeginInfo.clearValueCount = 2;
                renderPassBeginInfo.pClearValues = clearValues;

                viewport{ (float)offscreenPass.width, (float)offscreenPass.height, 0.0f, 1.0f };
                vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

                scissor{ offscreenPass.width, offscreenPass.height, 0, 0 };
                vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

                /*
					First render pass: Render glow parts of the model (separate mesh) to an offscreen frame buffer
				*/

                vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

                vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.scene, 0, 1, &descriptorSets.scene, 0, NULL);
                vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.glowPass);

                vk::DeviceSize offsets[1] = { 0 };
                vkCmdBindVertexBuffers(drawCmdBuffers[i], 0, 1, &models.ufoGlow.vertices.buffer, offsets);
                vkCmdBindIndexBuffer(drawCmdBuffers[i], models.ufoGlow.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(drawCmdBuffers[i], models.ufoGlow.indexCount, 1, 0, 0, 0);

                vkCmdEndRenderPass(drawCmdBuffers[i]);

                /*
					Second render pass: Vertical blur

					Render contents of the first pass into a second framebuffer and apply a vertical blur
					This is the first blur pass, the horizontal blur is applied when rendering on top of the scene
				*/

                renderPassBeginInfo.framebuffer = offscreenPass.framebuffers[1].framebuffer;

                vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

                vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.blur, 0, 1, &descriptorSets.blurVert, 0, NULL);
                vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.blurVert);
                vkCmdDraw(drawCmdBuffers[i], 3, 1, 0, 0);

                vkCmdEndRenderPass(drawCmdBuffers[i]);
            }

            /*
				Note: Explicit synchronization is not required between the render pass, as this is done implicit via sub pass dependencies
			*/

            /*
				Third render pass: Scene rendering with applied vertical blur

				Renders the scene and the (vertically blurred) contents of the second framebuffer and apply a horizontal blur

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

                vk::Viewport viewport{ (float)width, (float)height, 0.0f, 1.0f };
                vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

                vk::Rect2D scissor{ width, height, 0, 0 };
                vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

                vk::DeviceSize offsets[1] = { 0 };

                // Skybox
                vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.scene, 0, 1, &descriptorSets.skyBox, 0, NULL);
                vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.skyBox);

                vkCmdBindVertexBuffers(drawCmdBuffers[i], 0, 1, &models.skyBox.vertices.buffer, offsets);
                vkCmdBindIndexBuffer(drawCmdBuffers[i], models.skyBox.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(drawCmdBuffers[i], models.skyBox.indexCount, 1, 0, 0, 0);

                // 3D scene
                vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.scene, 0, 1, &descriptorSets.scene, 0, NULL);
                vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.phongPass);

                vkCmdBindVertexBuffers(drawCmdBuffers[i], 0, 1, &models.ufo.vertices.buffer, offsets);
                vkCmdBindIndexBuffer(drawCmdBuffers[i], models.ufo.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(drawCmdBuffers[i], models.ufo.indexCount, 1, 0, 0, 0);

                if (bloom) {
                    vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.blur, 0, 1, &descriptorSets.blurHorz, 0, NULL);
                    vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.blurHorz);
                    vkCmdDraw(drawCmdBuffers[i], 3, 1, 0, 0);
                }

                drawUI(drawCmdBuffers[i]);

                vkCmdEndRenderPass(drawCmdBuffers[i]);
            }

            VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
        }
    }

    void loadAssets() {
        models.ufo.loadFromFile(context, getAssetPath() + "models/retroufo.dae", vertexLayout, 0.05f);
        models.ufoGlow.loadFromFile(context, getAssetPath() + "models/retroufo_glow.dae", vertexLayout, 0.05f);
        models.skyBox.loadFromFile(context, getAssetPath() + "models/cube.obj", vertexLayout, 1.0f);
        textures.cubemap.loadFromFile(context, getAssetPath() + "textures/cubemap_space.ktx", vk::Format::eR8G8B8A8Unorm);
    }

    void setupDescriptorPool() {
        std::vector<vk::DescriptorPoolSize> poolSizes = { { vk::DescriptorType::eUniformBuffer, 8 }, { vk::DescriptorType::eCombinedImageSampler, 6 } };

        vk::DescriptorPoolCreateInfo descriptorPoolInfo{ poolSizes.size(), poolSizes.data(), 5 };

        VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));
    }

    void setupDescriptorSetLayout() {
        std::vector<vk::DescriptorSetLayoutBinding> setLayoutBindings;
        vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
        vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;

        // Fullscreen blur
        setLayoutBindings = {
            { 0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment },  // Binding 0: Fragment shader uniform buffer
            vks::initializers::descriptorSetLayoutBinding(vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment,
                                                          1)  // Binding 1: Fragment shader image sampler
        };
        descriptorSetLayoutCreateInfo = { setLayoutBindings.data(), static_cast<uint32_t>(setLayoutBindings.size()) };
        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCreateInfo, nullptr, &descriptorSetLayouts.blur));
        pipelineLayoutCreateInfo{ &descriptorSetLayouts.blur, 1 };
        VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayouts.blur));

        // Scene rendering
        setLayoutBindings = {
            { 0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex },           // Binding 0 : Vertex shader uniform buffer
            { 1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment },  // Binding 1 : Fragment shader image sampler
            { 2, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment },         // Binding 2 : Framgnet shader image sampler
        };

        descriptorSetLayoutCreateInfo{ setLayoutBindings.data(), setLayoutBindings.size() };
        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCreateInfo, nullptr, &descriptorSetLayouts.scene));
        pipelineLayoutCreateInfo{ &descriptorSetLayouts.scene, 1 };
        VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayouts.scene));
    }

    void setupDescriptorSet() {
        vk::DescriptorSetAllocateInfo descriptorSetAllocInfo;
        std::vector<vk::WriteDescriptorSet> writeDescriptorSets;

        // Full screen blur
        // Vertical
        descriptorSetAllocInfo{ descriptorPool, &descriptorSetLayouts.blur, 1 };
        VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorSetAllocInfo, &descriptorSets.blurVert));
        writeDescriptorSets = {
            vks::initializers::writeDescriptorSet(descriptorSets.blurVert, vk::DescriptorType::eUniformBuffer, 0,
                                                  &uniformBuffers.blurParams.descriptor),  // Binding 0: Fragment shader uniform buffer
            vks::initializers::writeDescriptorSet(descriptorSets.blurVert, vk::DescriptorType::eCombinedImageSampler, 1,
                                                  &offscreenPass.framebuffers[0].descriptor),  // Binding 1: Fragment shader texture sampler
        };
        vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);
        // Horizontal
        descriptorSetAllocInfo{ descriptorPool, &descriptorSetLayouts.blur, 1 };
        VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorSetAllocInfo, &descriptorSets.blurHorz));
        writeDescriptorSets = {
            vks::initializers::writeDescriptorSet(descriptorSets.blurHorz, vk::DescriptorType::eUniformBuffer, 0,
                                                  &uniformBuffers.blurParams.descriptor),  // Binding 0: Fragment shader uniform buffer
            vks::initializers::writeDescriptorSet(descriptorSets.blurHorz, vk::DescriptorType::eCombinedImageSampler, 1,
                                                  &offscreenPass.framebuffers[1].descriptor),  // Binding 1: Fragment shader texture sampler
        };
        vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);

        // Scene rendering
        descriptorSetAllocInfo{ descriptorPool, &descriptorSetLayouts.scene, 1 };
        VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorSetAllocInfo, &descriptorSets.scene));
        writeDescriptorSets = {
            vks::initializers::writeDescriptorSet(descriptorSets.scene, vk::DescriptorType::eUniformBuffer, 0,
                                                  &uniformBuffers.scene.descriptor)  // Binding 0: Vertex shader uniform buffer
        };
        vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);

        // Skybox
        VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorSetAllocInfo, &descriptorSets.skyBox));
        writeDescriptorSets = {
            vks::initializers::writeDescriptorSet(descriptorSets.skyBox, vk::DescriptorType::eUniformBuffer, 0,
                                                  &uniformBuffers.skyBox.descriptor),  // Binding 0: Vertex shader uniform buffer
            vks::initializers::writeDescriptorSet(descriptorSets.skyBox, vk::DescriptorType::eCombinedImageSampler, 1,
                                                  &textures.cubemap.descriptor),  // Binding 1: Fragment shader texture sampler
        };
        vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);
    }

    void preparePipelines() {
        vk::PipelineInputAssemblyStateCreateInfo inputAssemblyStateCI = { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE };
        vk::PipelineRasterizationStateCreateInfo rasterizationStateCI = { VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE, 0 };
        vk::PipelineColorBlendAttachmentState blendAttachmentState{ 0xf, VK_FALSE };
        vk::PipelineColorBlendStateCreateInfo colorBlendStateCI{ 1, &blendAttachmentState };
        vk::PipelineDepthStencilStateCreateInfo depthStencilStateCI = { VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL };
        vk::PipelineViewportStateCreateInfo viewportStateCI{ 1, 1, 0 };
        vk::PipelineMultisampleStateCreateInfo multisampleStateCI{ VK_SAMPLE_COUNT_1_BIT, 0 };
        std::vector<vk::DynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        vk::PipelineDynamicStateCreateInfo dynamicStateCI = { dynamicStateEnables.data(), dynamicStateEnables.size(), 0 };

        std::array<vk::PipelineShaderStageCreateInfo, 2> shaderStages;

        vk::GraphicsPipelineCreateInfo pipelineCI{ pipelineLayouts.blur, renderPass, 0 };
        pipelineCI.pInputAssemblyState = &inputAssemblyStateCI;
        pipelineCI.pRasterizationState = &rasterizationStateCI;
        pipelineCI.pColorBlendState = &colorBlendStateCI;
        pipelineCI.pMultisampleState = &multisampleStateCI;
        pipelineCI.pViewportState = &viewportStateCI;
        pipelineCI.pDepthStencilState = &depthStencilStateCI;
        pipelineCI.pDynamicState = &dynamicStateCI;
        pipelineCI.stageCount = shaderStages.size();
        pipelineCI.pStages = shaderStages.data();

        // Blur pipelines
        shaderStages[0] = loadShader(getAssetPath() + "shaders/bloom/gaussblur.vert.spv", vk::ShaderStageFlagBits::eVertex);
        shaderStages[1] = loadShader(getAssetPath() + "shaders/bloom/gaussblur.frag.spv", vk::ShaderStageFlagBits::eFragment);
        // Empty vertex input state
        vk::PipelineVertexInputStateCreateInfo emptyInputState;
        pipelineCI.pVertexInputState = &emptyInputState;
        pipelineCI.layout = pipelineLayouts.blur;
        // Additive blending
        blendAttachmentState.colorWriteMask = 0xF;
        blendAttachmentState.blendEnable = VK_TRUE;
        blendAttachmentState.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachmentState.alphaBlendOp = VK_BLEND_OP_ADD;
        blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;

        // Use specialization constants to select between horizontal and vertical blur
        uint32_t blurdirection = 0;
        vk::SpecializationMapEntry specializationMapEntry{ 0, 0, sizeof(uint32_t) };
        vk::SpecializationInfo specializationInfo{ 1, &specializationMapEntry, sizeof(uint32_t), &blurdirection };
        shaderStages[1].pSpecializationInfo = &specializationInfo;
        // Vertical blur pipeline
        pipelineCI.renderPass = offscreenPass.renderPass;
        VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.blurVert));
        // Horizontal blur pipeline
        blurdirection = 1;
        pipelineCI.renderPass = renderPass;
        VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.blurHorz));

        // Phong pass (3D model)
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
        pipelineCI.layout = pipelineLayouts.scene;
        shaderStages[0] = loadShader(getAssetPath() + "shaders/bloom/phongpass.vert.spv", vk::ShaderStageFlagBits::eVertex);
        shaderStages[1] = loadShader(getAssetPath() + "shaders/bloom/phongpass.frag.spv", vk::ShaderStageFlagBits::eFragment);
        blendAttachmentState.blendEnable = VK_FALSE;
        depthStencilStateCI.depthWriteEnable = VK_TRUE;
        rasterizationStateCI.cullMode = VK_CULL_MODE_BACK_BIT;
        pipelineCI.renderPass = renderPass;
        VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.phongPass));

        // Color only pass (offscreen blur base)
        shaderStages[0] = loadShader(getAssetPath() + "shaders/bloom/colorpass.vert.spv", vk::ShaderStageFlagBits::eVertex);
        shaderStages[1] = loadShader(getAssetPath() + "shaders/bloom/colorpass.frag.spv", vk::ShaderStageFlagBits::eFragment);
        pipelineCI.renderPass = offscreenPass.renderPass;
        VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.glowPass));

        // Skybox (cubemap)
        shaderStages[0] = loadShader(getAssetPath() + "shaders/bloom/skybox.vert.spv", vk::ShaderStageFlagBits::eVertex);
        shaderStages[1] = loadShader(getAssetPath() + "shaders/bloom/skybox.frag.spv", vk::ShaderStageFlagBits::eFragment);
        depthStencilStateCI.depthWriteEnable = VK_FALSE;
        rasterizationStateCI.cullMode = VK_CULL_MODE_FRONT_BIT;
        pipelineCI.renderPass = renderPass;
        VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.skyBox));
    }

    // Prepare and initialize uniform buffer containing shader uniforms
    void prepareUniformBuffers() {
        // Phong and color pass vertex shader uniform buffer
        VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.scene,
                                                   sizeof(ubos.scene)));

        // Blur parameters uniform buffers
        VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.blurParams,
                                                   sizeof(ubos.blurParams)));

        // Skybox
        VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.skyBox,
                                                   sizeof(ubos.skyBox)));

        // Map persistent
        VK_CHECK_RESULT(uniformBuffers.scene.map());
        VK_CHECK_RESULT(uniformBuffers.blurParams.map());
        VK_CHECK_RESULT(uniformBuffers.skyBox.map());

        // Intialize uniform buffers
        updateUniformBuffersScene();
        updateUniformBuffersBlur();
    }

    // Update uniform buffers for rendering the 3D scene
    void updateUniformBuffersScene() {
        // UFO
        ubos.scene.projection = camera.matrices.perspective;
        ubos.scene.view = camera.matrices.view;

        ubos.scene.model =
            glm::translate(glm::mat4(1.0f), glm::vec3(sin(glm::radians(timer * 360.0f)) * 0.25f, -1.0f, cos(glm::radians(timer * 360.0f)) * 0.25f) + cameraPos);
        ubos.scene.model = glm::rotate(ubos.scene.model, -sinf(glm::radians(timer * 360.0f)) * 0.15f, glm::vec3(1.0f, 0.0f, 0.0f));
        ubos.scene.model = glm::rotate(ubos.scene.model, glm::radians(timer * 360.0f), glm::vec3(0.0f, 1.0f, 0.0f));

        memcpy(uniformBuffers.scene.mapped, &ubos.scene, sizeof(ubos.scene));

        // Skybox
        ubos.skyBox.projection = glm::perspective(glm::radians(45.0f), (float)width / (float)height, 0.1f, 256.0f);
        ubos.skyBox.view = glm::mat4(glm::mat3(camera.matrices.view));
        ubos.skyBox.model = glm::mat4(1.0f);

        memcpy(uniformBuffers.skyBox.mapped, &ubos.skyBox, sizeof(ubos.skyBox));
    }

    // Update blur pass parameter uniform buffer
    void updateUniformBuffersBlur() {
        memcpy(uniformBuffers.blurParams.mapped, &ubos.blurParams, sizeof(ubos.blurParams));
    }

    void draw() {
        VulkanExampleBase::prepareFrame();
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &drawCmdBuffers[currentBuffer];
        VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
        VulkanExampleBase::submitFrame();
    }

    void prepare() {
        VulkanExampleBase::prepare();

        prepareUniformBuffers();
        prepareOffscreen();
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
        if (!paused || camera.updated) {
            updateUniformBuffersScene();
        }
    }

    virtual void OnUpdateUIOverlay(vks::UIOverlay* overlay) {
        if (overlay->header("Settings")) {
            if (overlay->checkBox("Bloom", &bloom)) {
                buildCommandBuffers();
            }
            if (overlay->inputFloat("Scale", &ubos.blurParams.blurScale, 0.1f, 2)) {
                updateUniformBuffersBlur();
            }
        }
    }
};

VULKAN_EXAMPLE_MAIN()
