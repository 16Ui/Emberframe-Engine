#include "sync_present_probe.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace emberframe::samples {
namespace {

void require_success(const VkResult result, const char* const operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(
            std::string(operation) + " failed, VkResult=" + std::to_string(result));
    }
}

[[nodiscard]] VkImageSubresourceRange full_color_range() noexcept
{
    VkImageSubresourceRange range {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;
    return range;
}

} // namespace

SyncPresentProbe::SyncPresentProbe(
    const VkDevice borrowed_device,
    const VkQueue borrowed_graphics_queue,
    const VkQueue borrowed_present_queue,
    const std::uint32_t graphics_queue_family,
    const std::size_t swapchain_image_count)
    : device_(borrowed_device)
    , graphics_queue_(borrowed_graphics_queue)
    , present_queue_(borrowed_present_queue)
    , graphics_queue_family_(graphics_queue_family)
{
    if (device_ == VK_NULL_HANDLE
        || graphics_queue_ == VK_NULL_HANDLE
        || present_queue_ == VK_NULL_HANDLE
        || swapchain_image_count == 0) {
        throw std::invalid_argument(
            "SyncPresentProbe requires valid Device/Queue handles and Swapchain images");
    }

    try {
        create_frame_slots();
        render_finished_semaphores_ = create_semaphores(swapchain_image_count);
    } catch (...) {
        cleanup();
        throw;
    }

    std::cout << "[B8] Sync resources ready: " << frame_slots_.size()
              << " Frame Slots, " << render_finished_semaphores_.size()
              << " per-image render-finished Semaphores.\n";
}

SyncPresentProbe::~SyncPresentProbe()
{
    cleanup();
}

void SyncPresentProbe::create_frame_slots()
{
    for (std::size_t index = 0; index < frame_slots_.size(); ++index) {
        FrameSlot& slot = frame_slots_[index];

        VkCommandPoolCreateInfo pool_info {
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        };
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = graphics_queue_family_;
        require_success(
            vkCreateCommandPool(device_, &pool_info, nullptr, &slot.command_pool),
            "vkCreateCommandPool");

        VkCommandBufferAllocateInfo allocate_info {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        };
        allocate_info.commandPool = slot.command_pool;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        require_success(
            vkAllocateCommandBuffers(device_, &allocate_info, &slot.command_buffer),
            "vkAllocateCommandBuffers");

        // 第一帧还没有 GPU 工作可等待，因此 Fence 必须以“已有信号”状态创建。
        VkFenceCreateInfo fence_info {
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        require_success(
            vkCreateFence(device_, &fence_info, nullptr, &slot.in_flight_fence),
            "vkCreateFence");

        VkSemaphoreCreateInfo semaphore_info {
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        require_success(
            vkCreateSemaphore(device_, &semaphore_info, nullptr, &slot.image_available),
            "vkCreateSemaphore(image available)");

        std::cout << "[B8] Frame Slot " << index
                  << " owns Command Pool/Buffer + Fence + image-available Semaphore.\n";
    }
}

std::vector<VkSemaphore> SyncPresentProbe::create_semaphores(const std::size_t count) const
{
    std::vector<VkSemaphore> semaphores;
    semaphores.reserve(count);

    VkSemaphoreCreateInfo create_info {
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    try {
        for (std::size_t index = 0; index < count; ++index) {
            VkSemaphore semaphore = VK_NULL_HANDLE;
            require_success(
                vkCreateSemaphore(device_, &create_info, nullptr, &semaphore),
                "vkCreateSemaphore(render finished)");
            semaphores.push_back(semaphore);
        }
    } catch (...) {
        for (const VkSemaphore semaphore : semaphores) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
        throw;
    }
    return semaphores;
}

void SyncPresentProbe::record_clear_commands(
    const VkCommandBuffer command_buffer,
    const VkImage swapchain_image,
    const std::uint64_t frame_number) const
{
    require_success(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo begin_info {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    require_success(
        vkBeginCommandBuffer(command_buffer, &begin_info),
        "vkBeginCommandBuffer");

    // Acquire 只表示“这张 Image 可再次使用”，不会替我们改变 Image Layout。
    // 本课会覆盖整张图，所以从 UNDEFINED 开始表示主动丢弃旧画面。
    VkImageMemoryBarrier2 to_transfer {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
    };
    to_transfer.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    to_transfer.srcAccessMask = VK_ACCESS_2_NONE;
    to_transfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = swapchain_image;
    to_transfer.subresourceRange = full_color_range();

    VkDependencyInfo before_clear {
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
    };
    before_clear.imageMemoryBarrierCount = 1;
    before_clear.pImageMemoryBarriers = &to_transfer;
    vkCmdPipelineBarrier2(command_buffer, &before_clear);

    // 用缓慢变化的颜色证明画面来自每帧重新提交的 GPU 命令，而不是静态窗口背景。
    const float phase = static_cast<float>(frame_number % 360) * 0.025F;
    VkClearColorValue clear_color {};
    clear_color.float32[0] = 0.08F + 0.06F * (std::sin(phase) + 1.0F);
    clear_color.float32[1] = 0.16F + 0.10F * (std::sin(phase + 2.1F) + 1.0F);
    clear_color.float32[2] = 0.28F + 0.14F * (std::sin(phase + 4.2F) + 1.0F);
    clear_color.float32[3] = 1.0F;
    const VkImageSubresourceRange color_range = full_color_range();
    vkCmdClearColorImage(
        command_buffer,
        swapchain_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        &clear_color,
        1,
        &color_range);

    // 清屏写入完成后，把 Image 变成呈现系统要求的布局。
    VkImageMemoryBarrier2 to_present {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
    };
    to_present.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_present.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_present.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    to_present.dstAccessMask = VK_ACCESS_2_NONE;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = swapchain_image;
    to_present.subresourceRange = color_range;

    VkDependencyInfo before_present {
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
    };
    before_present.imageMemoryBarrierCount = 1;
    before_present.pImageMemoryBarriers = &to_present;
    vkCmdPipelineBarrier2(command_buffer, &before_present);

    require_success(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");
}

SyncPresentProbe::DrawResult SyncPresentProbe::draw_frame(
    const VkSwapchainKHR borrowed_swapchain,
    const std::vector<VkImage>& borrowed_swapchain_images)
{
    if (borrowed_swapchain == VK_NULL_HANDLE
        || borrowed_swapchain_images.empty()
        || borrowed_swapchain_images.size() != render_finished_semaphores_.size()) {
        throw std::invalid_argument("B8 draw_frame received mismatched Swapchain resources");
    }

    const std::size_t slot_index =
        static_cast<std::size_t>(frame_number_ % frame_slots_.size());
    FrameSlot& slot = frame_slots_[slot_index];

    // Fence 是 CPU → GPU 完成状态的桥梁：只等待当前 Slot，不再阻塞整个 Queue。
    require_success(
        vkWaitForFences(
            device_,
            1,
            &slot.in_flight_fence,
            VK_TRUE,
            std::numeric_limits<std::uint64_t>::max()),
        "vkWaitForFences");

    std::uint32_t image_index = 0;
    const VkResult acquire_result = vkAcquireNextImageKHR(
        device_,
        borrowed_swapchain,
        std::numeric_limits<std::uint64_t>::max(),
        slot.image_available,
        VK_NULL_HANDLE,
        &image_index);

    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Acquire 失败时没有新的 Queue Submit 会给 Fence 发信号，因此绝不能提前 Reset Fence。
        return DrawResult::needs_swapchain_recreation;
    }
    if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error(
            "vkAcquireNextImageKHR failed, VkResult=" + std::to_string(acquire_result));
    }

    // 从这里开始确定会提交工作，才把 Fence 复位为“无信号”等待下一次 GPU 完成。
    require_success(vkResetFences(device_, 1, &slot.in_flight_fence), "vkResetFences");

    record_clear_commands(
        slot.command_buffer,
        borrowed_swapchain_images.at(image_index),
        frame_number_);

    VkSemaphoreSubmitInfo acquire_wait {
        VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
    };
    acquire_wait.semaphore = slot.image_available;
    // 第一条真正访问 Swapchain Image 的命令是 Transfer Clear，因此在 Transfer Stage 等待。
    acquire_wait.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;

    VkCommandBufferSubmitInfo command_buffer_info {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
    };
    command_buffer_info.commandBuffer = slot.command_buffer;

    const VkSemaphore render_finished = render_finished_semaphores_.at(image_index);
    VkSemaphoreSubmitInfo render_finished_signal {
        VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
    };
    render_finished_signal.semaphore = render_finished;
    render_finished_signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit_info {
        VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
    };
    submit_info.waitSemaphoreInfoCount = 1;
    submit_info.pWaitSemaphoreInfos = &acquire_wait;
    submit_info.commandBufferInfoCount = 1;
    submit_info.pCommandBufferInfos = &command_buffer_info;
    submit_info.signalSemaphoreInfoCount = 1;
    submit_info.pSignalSemaphoreInfos = &render_finished_signal;

    require_success(
        vkQueueSubmit2(graphics_queue_, 1, &submit_info, slot.in_flight_fence),
        "vkQueueSubmit2");

    VkPresentInfoKHR present_info {
        VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
    };
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &borrowed_swapchain;
    present_info.pImageIndices = &image_index;

    const VkResult present_result = vkQueuePresentKHR(present_queue_, &present_info);

    ++slot.submission_count;
    if (frame_number_ < 8 || frame_number_ % 120 == 0) {
        std::cout << "[B8] Frame " << frame_number_
                  << " | Slot " << slot_index
                  << " | Swapchain Image " << image_index
                  << " | Wait Fence -> Acquire -> Record/Clear -> Submit -> Present"
                  << " | slot submissions=" << slot.submission_count << '\n';
    }
    ++frame_number_;

    if (present_result == VK_ERROR_OUT_OF_DATE_KHR
        || present_result == VK_SUBOPTIMAL_KHR
        || acquire_result == VK_SUBOPTIMAL_KHR) {
        return DrawResult::needs_swapchain_recreation;
    }
    if (present_result != VK_SUCCESS) {
        throw std::runtime_error(
            "vkQueuePresentKHR failed, VkResult=" + std::to_string(present_result));
    }
    return DrawResult::presented;
}

void SyncPresentProbe::rebuild_present_semaphores(const std::size_t swapchain_image_count)
{
    if (swapchain_image_count == 0) {
        throw std::invalid_argument("Swapchain must expose at least one image");
    }

    // SwapchainProbe::recreate() 已等待 Device 空闲；这里再次保证此方法可独立安全调用。
    require_success(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle(before semaphore rebuild)");
    std::vector<VkSemaphore> replacements = create_semaphores(swapchain_image_count);

    for (const VkSemaphore semaphore : render_finished_semaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    render_finished_semaphores_ = std::move(replacements);

    std::cout << "[B8] Rebuilt " << render_finished_semaphores_.size()
              << " per-image render-finished Semaphores.\n";
}

void SyncPresentProbe::cleanup() noexcept
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    vkDeviceWaitIdle(device_);

    for (const VkSemaphore semaphore : render_finished_semaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    render_finished_semaphores_.clear();

    for (FrameSlot& slot : frame_slots_) {
        if (slot.image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, slot.image_available, nullptr);
            slot.image_available = VK_NULL_HANDLE;
        }
        if (slot.in_flight_fence != VK_NULL_HANDLE) {
            vkDestroyFence(device_, slot.in_flight_fence, nullptr);
            slot.in_flight_fence = VK_NULL_HANDLE;
        }
        if (slot.command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, slot.command_pool, nullptr);
            slot.command_pool = VK_NULL_HANDLE;
            slot.command_buffer = VK_NULL_HANDLE;
        }
    }
    std::cout << "[B8] Synchronization and command resources destroyed.\n";
}

} // namespace emberframe::samples
