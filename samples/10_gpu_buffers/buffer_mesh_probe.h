#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace emberframe::samples {

// B10 教学对象：使用 VMA 管理 Buffer 内存，通过 Staging Upload 把 CPU Mesh
// 上传到长期存在的 Device Buffer，并使用 Vertex/Index Buffer 完成 Indexed Draw。
class BufferMeshProbe {
public:
    static constexpr std::size_t frame_overlap = 2;

    enum class DrawResult {
        presented,
        needs_swapchain_recreation,
    };

    BufferMeshProbe(
        VkInstance borrowed_instance,
        VkPhysicalDevice borrowed_physical_device,
        VkDevice borrowed_device,
        VkQueue borrowed_graphics_queue,
        VkQueue borrowed_present_queue,
        std::uint32_t graphics_queue_family,
        std::size_t swapchain_image_count,
        VkFormat swapchain_format,
        std::filesystem::path shader_directory);
    ~BufferMeshProbe();

    BufferMeshProbe(const BufferMeshProbe&) = delete;
    BufferMeshProbe& operator=(const BufferMeshProbe&) = delete;
    BufferMeshProbe(BufferMeshProbe&&) = delete;
    BufferMeshProbe& operator=(BufferMeshProbe&&) = delete;

    [[nodiscard]] DrawResult draw_frame(
        VkSwapchainKHR borrowed_swapchain,
        const std::vector<VkImage>& borrowed_swapchain_images,
        const std::vector<VkImageView>& borrowed_swapchain_image_views,
        VkExtent2D extent);

    void rebuild_swapchain_resources(
        std::size_t swapchain_image_count,
        VkFormat swapchain_format);

private:
    struct Vertex {
        float position[2];
        float color[3];
    };

    struct AllocatedBuffer {
        VkBuffer buffer { VK_NULL_HANDLE };
        VmaAllocation allocation { VK_NULL_HANDLE };
        VkDeviceSize size { 0 };
    };

    struct FrameSlot {
        VkCommandPool command_pool { VK_NULL_HANDLE };
        VkCommandBuffer command_buffer { VK_NULL_HANDLE };
        VkFence in_flight_fence { VK_NULL_HANDLE };
        VkSemaphore image_available { VK_NULL_HANDLE };
        std::uint64_t submission_count { 0 };
    };

    void create_allocator(
        VkInstance instance,
        VkPhysicalDevice physical_device);
    void create_mesh_buffers();
    [[nodiscard]] AllocatedBuffer create_buffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        const VmaAllocationCreateInfo& allocation_info,
        VmaAllocationInfo* output_allocation_info = nullptr) const;
    void upload_mesh_with_staging(
        const std::array<Vertex, 4>& vertices,
        const std::array<std::uint16_t, 6>& indices);
    void print_memory_report() const;

    void create_frame_slots();
    [[nodiscard]] std::vector<VkSemaphore> create_semaphores(std::size_t count) const;
    void create_graphics_pipeline(VkFormat color_format);
    void destroy_graphics_pipeline() noexcept;
    void record_draw_commands(
        VkCommandBuffer command_buffer,
        VkImage swapchain_image,
        VkImageView swapchain_image_view,
        VkExtent2D extent) const;
    void cleanup() noexcept;

    VkDevice device_ { VK_NULL_HANDLE };
    VkQueue graphics_queue_ { VK_NULL_HANDLE };
    VkQueue present_queue_ { VK_NULL_HANDLE };
    std::uint32_t graphics_queue_family_ { 0 };

    VmaAllocator allocator_ { VK_NULL_HANDLE };
    AllocatedBuffer vertex_buffer_;
    AllocatedBuffer index_buffer_;
    std::uint32_t index_count_ { 0 };

    std::filesystem::path shader_directory_;
    VkFormat pipeline_color_format_ { VK_FORMAT_UNDEFINED };
    VkPipelineLayout pipeline_layout_ { VK_NULL_HANDLE };
    VkPipeline graphics_pipeline_ { VK_NULL_HANDLE };

    std::array<FrameSlot, frame_overlap> frame_slots_ {};
    std::vector<VkSemaphore> render_finished_semaphores_;
    std::uint64_t frame_number_ { 0 };
};

} // namespace emberframe::samples

