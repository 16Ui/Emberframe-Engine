#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace emberframe::samples {

// B9 教学对象：保留 B8 的 Acquire/Submit/Present 同步外壳，
// 在 Command Buffer 中使用 Dynamic Rendering 和 Graphics Pipeline 绘制三角形。
class TrianglePipelineProbe {
public:
    static constexpr std::size_t frame_overlap = 2;

    enum class DrawResult {
        presented,
        needs_swapchain_recreation,
    };

    TrianglePipelineProbe(
        VkDevice borrowed_device,
        VkQueue borrowed_graphics_queue,
        VkQueue borrowed_present_queue,
        std::uint32_t graphics_queue_family,
        std::size_t swapchain_image_count,
        VkFormat swapchain_format,
        std::filesystem::path shader_directory);
    ~TrianglePipelineProbe();

    TrianglePipelineProbe(const TrianglePipelineProbe&) = delete;
    TrianglePipelineProbe& operator=(const TrianglePipelineProbe&) = delete;
    TrianglePipelineProbe(TrianglePipelineProbe&&) = delete;
    TrianglePipelineProbe& operator=(TrianglePipelineProbe&&) = delete;

    [[nodiscard]] DrawResult draw_frame(
        VkSwapchainKHR borrowed_swapchain,
        const std::vector<VkImage>& borrowed_swapchain_images,
        const std::vector<VkImageView>& borrowed_swapchain_image_views,
        VkExtent2D extent);

    // Swapchain 重建后 Image 数量和 Format 都可能变化。
    void rebuild_swapchain_resources(
        std::size_t swapchain_image_count,
        VkFormat swapchain_format);

private:
    struct FrameSlot {
        VkCommandPool command_pool { VK_NULL_HANDLE };
        VkCommandBuffer command_buffer { VK_NULL_HANDLE };
        VkFence in_flight_fence { VK_NULL_HANDLE };
        VkSemaphore image_available { VK_NULL_HANDLE };
        std::uint64_t submission_count { 0 };
    };

    void create_frame_slots();
    [[nodiscard]] std::vector<VkSemaphore> create_semaphores(std::size_t count) const;
    void create_graphics_pipeline(VkFormat color_format);
    void destroy_graphics_pipeline() noexcept;
    void record_triangle_commands(
        VkCommandBuffer command_buffer,
        VkImage swapchain_image,
        VkImageView swapchain_image_view,
        VkExtent2D extent) const;
    void cleanup() noexcept;

    VkDevice device_ { VK_NULL_HANDLE };
    VkQueue graphics_queue_ { VK_NULL_HANDLE };
    VkQueue present_queue_ { VK_NULL_HANDLE };
    std::uint32_t graphics_queue_family_ { 0 };

    std::filesystem::path shader_directory_;
    VkFormat pipeline_color_format_ { VK_FORMAT_UNDEFINED };
    VkPipelineLayout pipeline_layout_ { VK_NULL_HANDLE };
    VkPipeline graphics_pipeline_ { VK_NULL_HANDLE };

    std::array<FrameSlot, frame_overlap> frame_slots_ {};
    std::vector<VkSemaphore> render_finished_semaphores_;
    std::uint64_t frame_number_ { 0 };
};

} // namespace emberframe::samples

