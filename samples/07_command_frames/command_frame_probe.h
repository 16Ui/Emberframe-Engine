#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace emberframe::samples {

// B7 教学对象：每个 Frame Slot 独立拥有一个 Command Pool，Pool 再分配一个
// Primary Command Buffer。这样以后允许多帧并行时，不会重置仍被 GPU 使用的命令。
class CommandFrameProbe {
public:
    static constexpr std::size_t frame_overlap = 2;

    CommandFrameProbe(
        VkDevice borrowed_device,
        VkQueue borrowed_graphics_queue,
        std::uint32_t graphics_queue_family);
    ~CommandFrameProbe();

    CommandFrameProbe(const CommandFrameProbe&) = delete;
    CommandFrameProbe& operator=(const CommandFrameProbe&) = delete;
    CommandFrameProbe(CommandFrameProbe&&) = delete;
    CommandFrameProbe& operator=(CommandFrameProbe&&) = delete;

    // 选择一个 Frame Slot，重置并录制它的 Command Buffer，然后提交给 Graphics Queue。
    // B7 为了不提前引入 Fence，提交后立即等待 Queue 空闲；B8 会移除这条串行等待。
    void record_and_submit(std::uint64_t frame_number);

private:
    struct FrameSlot {
        VkCommandPool command_pool { VK_NULL_HANDLE };
        VkCommandBuffer command_buffer { VK_NULL_HANDLE };
        std::uint64_t submission_count { 0 };
    };

    void create_frame_slots();
    void cleanup() noexcept;

    // Device 和 Queue 都由 B5 的 DeviceQueueProbe 拥有，本类只借用。
    VkDevice device_ { VK_NULL_HANDLE };
    VkQueue graphics_queue_ { VK_NULL_HANDLE };
    std::uint32_t graphics_queue_family_ { 0 };

    // 两个 Slot 只是为后续 Frame Overlap 建立资源布局；B7 仍通过 QueueWaitIdle 串行执行。
    std::array<FrameSlot, frame_overlap> frame_slots_ {};
};

} // namespace emberframe::samples

