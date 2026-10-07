#pragma once

#include "renderer/triangle_pipeline.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace emberframe::renderer {

// 正式帧资源：取得可写的 Swapchain Image，录制命令，提交 GPU，再交给 Present Queue。
//
// 资源分成两组索引：
// 1. Frame Slot 资源按 CPU 帧号轮换：Command Pool/Buffer、Fence、imageAvailable；
// 2. renderFinished 按取得的 Swapchain Image 索引选择，确保 Present 真正消费后才复用。
class FrameContext {
public:
    static constexpr std::size_t frame_overlap = 2;

    enum class DrawResult {
        presented,
        needs_swapchain_recreation,
    };

    FrameContext(
        VkPhysicalDevice borrowed_physical_device,
        VkDevice borrowed_device,
        VkQueue borrowed_graphics_queue,
        VkQueue borrowed_present_queue,
        std::uint32_t graphics_queue_family,
        std::size_t swapchain_image_count);
    ~FrameContext();

    FrameContext(const FrameContext&) = delete;
    FrameContext& operator=(const FrameContext&) = delete;
    FrameContext(FrameContext&&) = delete;
    FrameContext& operator=(FrameContext&&) = delete;

    [[nodiscard]] DrawResult draw_frame(
        VkSwapchainKHR borrowed_swapchain,
        const std::vector<VkImage>& borrowed_swapchain_images,
        const std::vector<VkImageView>& borrowed_image_views,
        VkExtent2D extent,
        const TrianglePipeline* pipeline,
        const TriangleMaterial& material);

    // Swapchain 重建后 Image 数量可能变化，因此同步资源也要按新数量重建。
    void rebuild_present_semaphores(std::size_t swapchain_image_count);
    void collect_gpu_samples();
    void reset_gpu_samples();
    [[nodiscard]] bool gpu_timestamps_supported() const noexcept;
    [[nodiscard]] const std::vector<double>& gpu_samples_ms() const noexcept;
    [[nodiscard]] std::uint64_t last_submitted_serial() const noexcept;
    [[nodiscard]] std::uint64_t completed_serial() const noexcept;
    [[nodiscard]] std::uint64_t draw_call_count() const noexcept;

private:
    struct FrameSlot {
        VkCommandPool command_pool { VK_NULL_HANDLE };
        VkCommandBuffer command_buffer { VK_NULL_HANDLE };

        // Fence 由 Queue Submit 在 GPU 完成本 Slot 工作后置为有信号，供 CPU 等待。
        VkFence in_flight_fence { VK_NULL_HANDLE };

        // Acquire 成功后由呈现系统发信号，GPU 提交在真正写 Image 前等待它。
        VkSemaphore image_available { VK_NULL_HANDLE };
        std::uint64_t submission_count { 0 };
        bool timestamp_pending { false };
        std::uint64_t submission_serial { 0 };
    };

    void create_frame_slots();
    [[nodiscard]] std::vector<VkSemaphore> create_semaphores(std::size_t count) const;
    void record_clear_commands(
        VkCommandBuffer command_buffer,
        VkImage swapchain_image,
        std::uint64_t frame_number,
        std::uint32_t query_base) const;
    void record_triangle_commands(
        VkCommandBuffer command_buffer,
        VkImage swapchain_image,
        VkImageView image_view,
        VkExtent2D extent,
        const TrianglePipeline& pipeline,
        const TriangleMaterial& material,
        std::uint32_t query_base) const;
    void read_gpu_sample(std::size_t slot_index);
    void mark_slot_completed(std::size_t slot_index);
    void cleanup() noexcept;

    // Queue 与 Device 由 VulkanDevice 拥有，本对象只借用。
    VkDevice device_ { VK_NULL_HANDLE };
    VkQueue graphics_queue_ { VK_NULL_HANDLE };
    VkQueue present_queue_ { VK_NULL_HANDLE };
    std::uint32_t graphics_queue_family_ { 0 };

    std::array<FrameSlot, frame_overlap> frame_slots_ {};
    VkQueryPool timestamp_pool_ { VK_NULL_HANDLE };
    double timestamp_period_ns_ { 0.0 };
    std::uint32_t timestamp_valid_bits_ { 0 };
    std::vector<double> gpu_samples_ms_;

    // vkQueuePresentKHR 等待的 Semaphore 按 Swapchain Image 索引，而不是 Frame Slot 索引。
    std::vector<VkSemaphore> render_finished_semaphores_;
    std::uint64_t frame_number_ { 0 };
    std::uint64_t draw_call_count_ { 0 };
    std::uint64_t last_submitted_serial_ { 0 };
    std::uint64_t completed_serial_ { 0 };
    std::set<std::uint64_t> completed_out_of_order_;
};

} // namespace emberframe::renderer

