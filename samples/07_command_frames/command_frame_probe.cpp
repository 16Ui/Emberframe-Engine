#include "command_frame_probe.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace emberframe::samples {
namespace {

void require_success(const VkResult result, const char* const operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(
            std::string(operation) + " failed, VkResult=" + std::to_string(result));
    }
}

} // namespace

CommandFrameProbe::CommandFrameProbe(
    const VkDevice borrowed_device,
    const VkQueue borrowed_graphics_queue,
    const std::uint32_t graphics_queue_family)
    : device_(borrowed_device)
    , graphics_queue_(borrowed_graphics_queue)
    , graphics_queue_family_(graphics_queue_family)
{
    if (device_ == VK_NULL_HANDLE || graphics_queue_ == VK_NULL_HANDLE) {
        throw std::invalid_argument(
            "CommandFrameProbe requires valid borrowed Device and Graphics Queue handles");
    }

    // 构造中途失败时析构函数不会执行，因此清理已经成功创建的 Pool。
    try {
        create_frame_slots();
    } catch (...) {
        cleanup();
        throw;
    }
}

CommandFrameProbe::~CommandFrameProbe()
{
    cleanup();
}

void CommandFrameProbe::create_frame_slots()
{
    for (std::size_t index = 0; index < frame_slots_.size(); ++index) {
        FrameSlot& slot = frame_slots_[index];

        VkCommandPoolCreateInfo pool_info {
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        };
        // 允许单独把 Command Buffer 从 Executable 状态重置回 Initial 状态。
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        // Pool 创建时绑定一个 Queue Family；从中分配的命令只能提交给兼容的 Queue。
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

        std::cout << "[B7] Frame Slot " << index
                  << " owns one Command Pool and one Primary Command Buffer.\n";
    }
}

void CommandFrameProbe::record_and_submit(const std::uint64_t frame_number)
{
    const std::size_t slot_index =
        static_cast<std::size_t>(frame_number % frame_slots_.size());
    FrameSlot& slot = frame_slots_[slot_index];

    // 只有 GPU 不再执行这个 Buffer 时才能重置。B7 上一帧末尾 QueueWaitIdle，
    // 所以此处安全；B8 会改为只等待当前 Slot 自己的 Fence。
    require_success(
        vkResetCommandBuffer(slot.command_buffer, 0),
        "vkResetCommandBuffer");

    VkCommandBufferBeginInfo begin_info {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    // 本次录制只提交一次，下一次使用前会重置并重新录制。
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    require_success(
        vkBeginCommandBuffer(slot.command_buffer, &begin_info),
        "vkBeginCommandBuffer");

    // B7 只建立“录制容器 → 提交”的基础闭环，因此当前 Command Buffer 可以为空。
    // B8 会录制 Image Layout Barrier，B9 再录制 BeginRendering 和 Draw Call。

    require_success(
        vkEndCommandBuffer(slot.command_buffer),
        "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo command_buffer_info {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
    };
    command_buffer_info.commandBuffer = slot.command_buffer;

    VkSubmitInfo2 submit_info {
        VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
    };
    submit_info.commandBufferInfoCount = 1;
    submit_info.pCommandBufferInfos = &command_buffer_info;

    // Queue Submit 才把已经录好的命令交给 GPU；录制本身只发生在 CPU 端。
    require_success(
        vkQueueSubmit2(graphics_queue_, 1, &submit_info, VK_NULL_HANDLE),
        "vkQueueSubmit2");

    // B7 没有 Fence，不能知道何时可以安全复用这个 Slot，只能等待整个 Queue 空闲。
    require_success(
        vkQueueWaitIdle(graphics_queue_),
        "vkQueueWaitIdle");

    ++slot.submission_count;
    if (frame_number < 6 || frame_number % 120 == 0) {
        std::cout << "[B7] Frame " << frame_number
                  << " -> Slot " << slot_index
                  << " | Reset -> Begin/Record -> End -> Submit -> Queue Idle"
                  << " | slot submissions=" << slot.submission_count << '\n';
    }
}

void CommandFrameProbe::cleanup() noexcept
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    // 销毁 Pool 会自动释放由它分配的 Command Buffer；不需要再逐个 Free。
    vkDeviceWaitIdle(device_);
    for (FrameSlot& slot : frame_slots_) {
        if (slot.command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, slot.command_pool, nullptr);
            slot.command_pool = VK_NULL_HANDLE;
            slot.command_buffer = VK_NULL_HANDLE;
        }
    }
    std::cout << "[B7] Command Pools destroyed; their Command Buffers were freed with them.\n";
}

} // namespace emberframe::samples

