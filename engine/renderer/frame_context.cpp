#include "frame_context.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace emberframe::renderer {
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

FrameContext::FrameContext(
    const VkPhysicalDevice borrowed_physical_device,
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
    if (borrowed_physical_device == VK_NULL_HANDLE
        || device_ == VK_NULL_HANDLE
        || graphics_queue_ == VK_NULL_HANDLE
        || present_queue_ == VK_NULL_HANDLE
        || swapchain_image_count == 0) {
        throw std::invalid_argument(
            "FrameContext requires valid Device/Queue handles and Swapchain images");
    }

    try {
        create_frame_slots();
        render_finished_semaphores_ = create_semaphores(swapchain_image_count);

        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(borrowed_physical_device, &properties);
        timestamp_period_ns_ = properties.limits.timestampPeriod;

        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(borrowed_physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(borrowed_physical_device, &family_count, families.data());
        if (graphics_queue_family_ >= family_count) {
            throw std::runtime_error("Graphics queue family index is out of range");
        }
        timestamp_valid_bits_ = families[graphics_queue_family_].timestampValidBits;
        if (timestamp_valid_bits_ != 0) {
            VkQueryPoolCreateInfo query_info { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query_info.queryCount = static_cast<std::uint32_t>(2 * frame_overlap);
            require_success(
                vkCreateQueryPool(device_, &query_info, nullptr, &timestamp_pool_),
                "vkCreateQueryPool(timestamp)");
        }
    } catch (...) {
        cleanup();
        throw;
    }

    std::cout << "[Vulkan] Sync resources ready: " << frame_slots_.size()
              << " Frame Slots, " << render_finished_semaphores_.size()
              << " per-image render-finished Semaphores.\n";
}

FrameContext::~FrameContext()
{
    cleanup();
}

void FrameContext::create_frame_slots()
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

        std::cout << "[Vulkan] Frame Slot " << index
                  << " owns Command Pool/Buffer + Fence + image-available Semaphore.\n";
    }
}

std::vector<VkSemaphore> FrameContext::create_semaphores(const std::size_t count) const
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

void FrameContext::record_clear_commands(
    const VkCommandBuffer command_buffer,
    const VkImage swapchain_image,
    const std::uint64_t frame_number,
    const std::uint32_t query_base) const
{
    require_success(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo begin_info {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    require_success(
        vkBeginCommandBuffer(command_buffer, &begin_info),
        "vkBeginCommandBuffer");

    if (timestamp_pool_ != VK_NULL_HANDLE) {
        // Query 随 Frame Slot 复用；先重置本 Slot 的两个时间戳，再量 GPU 命令段。
        vkCmdResetQueryPool(command_buffer, timestamp_pool_, query_base, 2);
        vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            timestamp_pool_, query_base);
    }

    // Acquire 只表示“这张 Image 可再次使用”，不会替我们改变 Image Layout。
    // 本帧会覆盖整张图，因此从 UNDEFINED 开始表示主动丢弃旧画面。
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

    if (timestamp_pool_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            timestamp_pool_, query_base + 1);
    }

    require_success(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");
}

void FrameContext::record_triangle_commands(
    const VkCommandBuffer command_buffer,
    const VkImage swapchain_image,
    const VkImageView image_view,
    const VkExtent2D extent,
    const TrianglePipeline& pipeline,
    const TriangleMaterial& material,
    const std::uint32_t query_base) const
{
    require_success(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    require_success(vkBeginCommandBuffer(command_buffer, &begin), "vkBeginCommandBuffer");

    if (timestamp_pool_ != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(command_buffer, timestamp_pool_, query_base, 2);
        vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            timestamp_pool_, query_base);
    }

    // 当前帧覆盖整张图。先让 Swapchain Image 进入颜色输出布局。
    VkImageMemoryBarrier2 to_color { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    to_color.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    to_color.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_color.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.image = swapchain_image;
    to_color.subresourceRange = full_color_range();
    VkDependencyInfo before_render { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    before_render.imageMemoryBarrierCount = 1;
    before_render.pImageMemoryBarriers = &to_color;
    vkCmdPipelineBarrier2(command_buffer, &before_render);

    // Attachment 是本次 Rendering 的输出槽位：指向 ImageView，并说明开始清屏、结束保存。
    VkRenderingAttachmentInfo color { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView = image_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = { { 0.025F, 0.035F, 0.065F, 1.0F } };
    VkRenderingInfo rendering { VK_STRUCTURE_TYPE_RENDERING_INFO };
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(command_buffer, &rendering);

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline());
    const VkViewport viewport { 0.0F, 0.0F,
        static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0F, 1.0F };
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    const VkRect2D scissor { { 0, 0 }, extent };
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdPushConstants(command_buffer, pipeline.layout(), VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(material), &material);
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
    vkCmdEndRendering(command_buffer);

    VkImageMemoryBarrier2 to_present { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    to_present.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_present.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_present.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    to_present.dstAccessMask = VK_ACCESS_2_NONE;
    to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = swapchain_image;
    to_present.subresourceRange = full_color_range();
    VkDependencyInfo before_present { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    before_present.imageMemoryBarrierCount = 1;
    before_present.pImageMemoryBarriers = &to_present;
    vkCmdPipelineBarrier2(command_buffer, &before_present);

    if (timestamp_pool_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            timestamp_pool_, query_base + 1);
    }
    require_success(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");
}

FrameContext::DrawResult FrameContext::draw_frame(
    const VkSwapchainKHR borrowed_swapchain,
    const std::vector<VkImage>& borrowed_swapchain_images,
    const std::vector<VkImageView>& borrowed_image_views,
    const VkExtent2D extent,
    const TrianglePipeline* const pipeline,
    const TriangleMaterial& material)
{
    if (borrowed_swapchain == VK_NULL_HANDLE
        || borrowed_swapchain_images.empty()
        || borrowed_swapchain_images.size() != render_finished_semaphores_.size()
        || (pipeline != nullptr && (borrowed_swapchain_images.size() != borrowed_image_views.size()
            || extent.width == 0 || extent.height == 0))) {
        throw std::invalid_argument("Vulkan draw_frame received mismatched Swapchain resources");
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
    read_gpu_sample(slot_index);
    mark_slot_completed(slot_index);

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

    if (pipeline != nullptr) {
        record_triangle_commands(slot.command_buffer, borrowed_swapchain_images.at(image_index),
            borrowed_image_views.at(image_index), extent, *pipeline, material,
            static_cast<std::uint32_t>(slot_index * 2));
    } else {
        record_clear_commands(slot.command_buffer, borrowed_swapchain_images.at(image_index),
            frame_number_, static_cast<std::uint32_t>(slot_index * 2));
    }

    VkSemaphoreSubmitInfo acquire_wait {
        VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
    };
    acquire_wait.semaphore = slot.image_available;
    // 三角形路径首个图像访问是颜色输出；清屏路径则是 Transfer Clear。
    acquire_wait.stageMask = pipeline != nullptr
        ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
        : VK_PIPELINE_STAGE_2_TRANSFER_BIT;

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
    slot.submission_serial = ++last_submitted_serial_;
    if (pipeline != nullptr) {
        ++draw_call_count_;
    }

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
    slot.timestamp_pending = timestamp_pool_ != VK_NULL_HANDLE;
    if (frame_number_ < 8 || frame_number_ % 120 == 0) {
        std::cout << "[Vulkan] Frame " << frame_number_
                  << " | Slot " << slot_index
                  << " | Swapchain Image " << image_index
                  << (pipeline != nullptr ? " | Draw triangle" : " | Clear image")
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

void FrameContext::rebuild_present_semaphores(const std::size_t swapchain_image_count)
{
    if (swapchain_image_count == 0) {
        throw std::invalid_argument("Swapchain must expose at least one image");
    }

    // Swapchain 重建前必须让所有旧 Image 的 GPU/Present 工作退出。
    require_success(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle(before semaphore rebuild)");
    for (std::size_t i = 0; i < frame_slots_.size(); ++i) {
        read_gpu_sample(i);
        mark_slot_completed(i);
    }
    std::vector<VkSemaphore> replacements = create_semaphores(swapchain_image_count);

    for (const VkSemaphore semaphore : render_finished_semaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    render_finished_semaphores_ = std::move(replacements);

    std::cout << "[Vulkan] Rebuilt " << render_finished_semaphores_.size()
              << " per-image render-finished Semaphores.\n";
}

void FrameContext::mark_slot_completed(const std::size_t slot_index)
{
    FrameSlot& slot = frame_slots_[slot_index];
    if (slot.submission_serial == 0) {
        return;
    }
    completed_out_of_order_.insert(slot.submission_serial);
    slot.submission_serial = 0;
    // 两个 Slot 可以反序完成；只有连续前沿才能证明所有更早提交都已结束。
    while (completed_out_of_order_.erase(completed_serial_ + 1) != 0) {
        ++completed_serial_;
    }
}

void FrameContext::read_gpu_sample(const std::size_t slot_index)
{
    FrameSlot& slot = frame_slots_[slot_index];
    if (!slot.timestamp_pending || timestamp_pool_ == VK_NULL_HANDLE) {
        return;
    }

    std::uint64_t ticks[2] {};
    require_success(vkGetQueryPoolResults(device_, timestamp_pool_,
        static_cast<std::uint32_t>(slot_index * 2), 2,
        sizeof(ticks), ticks, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT),
        "vkGetQueryPoolResults(timestamp)");

    // 有些队列只保留时间戳的低 N 位；差值按该位宽回绕计算。
    const std::uint64_t mask = timestamp_valid_bits_ == 64
        ? std::numeric_limits<std::uint64_t>::max()
        : (std::uint64_t { 1 } << timestamp_valid_bits_) - 1;
    const std::uint64_t elapsed_ticks = (ticks[1] - ticks[0]) & mask;
    gpu_samples_ms_.push_back(
        static_cast<double>(elapsed_ticks) * timestamp_period_ns_ / 1'000'000.0);
    slot.timestamp_pending = false;
}

void FrameContext::collect_gpu_samples()
{
    require_success(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle(collect timestamps)");
    for (std::size_t i = 0; i < frame_slots_.size(); ++i) {
        read_gpu_sample(i);
        mark_slot_completed(i);
    }
}

void FrameContext::reset_gpu_samples()
{
    collect_gpu_samples();
    gpu_samples_ms_.clear();
}

bool FrameContext::gpu_timestamps_supported() const noexcept
{
    return timestamp_pool_ != VK_NULL_HANDLE;
}

const std::vector<double>& FrameContext::gpu_samples_ms() const noexcept
{
    return gpu_samples_ms_;
}

std::uint64_t FrameContext::last_submitted_serial() const noexcept
{
    return last_submitted_serial_;
}

std::uint64_t FrameContext::completed_serial() const noexcept
{
    return completed_serial_;
}

std::uint64_t FrameContext::draw_call_count() const noexcept
{
    return draw_call_count_;
}

void FrameContext::cleanup() noexcept
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    vkDeviceWaitIdle(device_);

    if (timestamp_pool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, timestamp_pool_, nullptr);
        timestamp_pool_ = VK_NULL_HANDLE;
    }

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
    std::cout << "[Vulkan] Synchronization and command resources destroyed.\n";
}

} // namespace emberframe::renderer

