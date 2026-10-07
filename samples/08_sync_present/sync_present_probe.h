#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace emberframe::samples {

// B8 教学对象：把“取得一张可写的 Swapchain Image → GPU 写入 → Present”串成完整一帧。
//
// 资源分成两组索引：
// 1. Frame Slot 资源按 CPU 帧号轮换：Command Pool/Buffer、Fence、imageAvailable；
// 2. renderFinished 按取得的 Swapchain Image 索引选择，确保 Present 真正消费后才复用。
class SyncPresentProbe {
public:
    static constexpr std::size_t frame_overlap = 2;

    enum class DrawResult {
        presented,
        needs_swapchain_recreation,
    };

    SyncPresentProbe(
        VkDevice borrowed_device,
        VkQueue borrowed_graphics_queue,
        VkQueue borrowed_present_queue,
        std::uint32_t graphics_queue_family,
        std::size_t swapchain_image_count);
    ~SyncPresentProbe();

    SyncPresentProbe(const SyncPresentProbe&) = delete;
    SyncPresentProbe& operator=(const SyncPresentProbe&) = delete;
    SyncPresentProbe(SyncPresentProbe&&) = delete;
    SyncPresentProbe& operator=(SyncPresentProbe&&) = delete;

    [[nodiscard]] DrawResult draw_frame(
        VkSwapchainKHR borrowed_swapchain,
        const std::vector<VkImage>& borrowed_swapchain_images);

    // Swapchain 重建后 Image 数量可能变化，因此同步资源也要按新数量重建。
    void rebuild_present_semaphores(std::size_t swapchain_image_count);

private:
    struct FrameSlot {
        VkCommandPool command_pool { VK_NULL_HANDLE };
        VkCommandBuffer command_buffer { VK_NULL_HANDLE };

        // Fence 由 Queue Submit 在 GPU 完成本 Slot 工作后置为有信号，供 CPU 等待。
        VkFence in_flight_fence { VK_NULL_HANDLE };

        // Acquire 成功后由呈现系统发信号，GPU 提交在真正写 Image 前等待它。
        VkSemaphore image_available { VK_NULL_HANDLE };
        std::uint64_t submission_count { 0 };
    };

    void create_frame_slots();
    [[nodiscard]] std::vector<VkSemaphore> create_semaphores(std::size_t count) const;
    void record_clear_commands(
        VkCommandBuffer command_buffer,
        VkImage swapchain_image,
        std::uint64_t frame_number) const;
    void cleanup() noexcept;

    // 这些 Handle 由 B5 的 DeviceQueueProbe 拥有，本对象只借用。
    VkDevice device_ { VK_NULL_HANDLE };
    VkQueue graphics_queue_ { VK_NULL_HANDLE };
    VkQueue present_queue_ { VK_NULL_HANDLE };
    std::uint32_t graphics_queue_family_ { 0 };

    std::array<FrameSlot, frame_overlap> frame_slots_ {};

    // vkQueuePresentKHR 等待的 Semaphore 按 Swapchain Image 索引，而不是 Frame Slot 索引。
    std::vector<VkSemaphore> render_finished_semaphores_;
    std::uint64_t frame_number_ { 0 };
};

} // namespace emberframe::samples
