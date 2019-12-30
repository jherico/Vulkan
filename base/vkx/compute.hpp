#pragma once

#include <khrpp/vks/context.hpp>

namespace vkx {

// Resources for the compute part of the example
struct Compute {
    vk::Device device;
    vk::Queue queue;
    vk::CommandPool commandPool;

    struct Semaphores {
        vk::Semaphore ready;
        vk::Semaphore complete;
    } semaphores;

    virtual void prepare(const vks::Context& context) {
        device = context.device;
        // Create a compute capable device queue
        queue = context.device.getQueue(context.queueFamilyIndices.compute, 0);
        semaphores.ready = device.createSemaphore({});
        semaphores.complete = device.createSemaphore({});
        // Separate command pool as queue family for compute may be different than graphics
        commandPool = device.createCommandPool({ vk::CommandPoolCreateFlagBits::eResetCommandBuffer, context.queueFamilyIndices.compute });
    }

    virtual void destroy() {
        device.destroy(semaphores.complete);
        device.destroy(semaphores.ready);
        device.destroy(commandPool);
    }

    void submit(const vk::ArrayProxy<const vk::CommandBuffer>& commandBuffers) {
        static const std::vector<vk::PipelineStageFlags> waitStages{ vk::PipelineStageFlagBits::eComputeShader };
        // Submit compute commands
        vk::SubmitInfo computeSubmitInfo;
        computeSubmitInfo.commandBufferCount = commandBuffers.size();
        computeSubmitInfo.pCommandBuffers = commandBuffers.data();
        computeSubmitInfo.waitSemaphoreCount = 1;
        computeSubmitInfo.pWaitSemaphores = &semaphores.ready;
        computeSubmitInfo.pWaitDstStageMask = waitStages.data();
        computeSubmitInfo.signalSemaphoreCount = 1;
        computeSubmitInfo.pSignalSemaphores = &semaphores.complete;
        queue.submit(computeSubmitInfo, {});
    }
};

}  // namespace vkx
