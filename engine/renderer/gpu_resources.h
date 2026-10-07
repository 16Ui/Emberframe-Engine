#pragma once

#include "core/resource_registry.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstddef>
#include <cstdint>

namespace emberframe::renderer {

// 返回的 Vulkan Handle 只借用：不能自行销毁，也不能在资源退休后继续录制新命令。
struct BufferView {
    VkBuffer buffer { VK_NULL_HANDLE };
    VkDeviceSize size { 0 };
};

struct ImageView {
    VkImage image { VK_NULL_HANDLE };
    VkImageView view { VK_NULL_HANDLE };
    VkExtent3D extent {};
    VkFormat format { VK_FORMAT_UNDEFINED };
};

struct GpuMemoryStatistics {
    std::uint32_t allocation_count { 0 };
    VkDeviceSize allocation_bytes { 0 }; // 资源实际申请量，不等于 VMA 预留块大小。
    VkDeviceSize block_bytes { 0 };
};

// VMA Allocator 必须比所有由它分配的 Buffer/Image 活得更久。
class GpuResources {
public:
    GpuResources(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device);
    ~GpuResources();

    GpuResources(const GpuResources&) = delete;
    GpuResources& operator=(const GpuResources&) = delete;

    [[nodiscard]] core::ResourceHandle create_buffer(VkDeviceSize size,
        VkBufferUsageFlags usage, VmaMemoryUsage memory_usage,
        VmaAllocationCreateFlags flags = 0);
    [[nodiscard]] core::ResourceHandle create_image(VkExtent3D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspect);
    [[nodiscard]] BufferView buffer(core::ResourceHandle handle) const noexcept;
    [[nodiscard]] ImageView image(core::ResourceHandle handle) const noexcept;

    // 退休立即使引擎 Handle 失效；实际 Vulkan/VMA 销毁要等 GPU 完成对应提交。
    [[nodiscard]] bool retire_buffer(core::ResourceHandle handle, std::uint64_t after_serial);
    [[nodiscard]] bool retire_image(core::ResourceHandle handle, std::uint64_t after_serial);
    void collect(std::uint64_t completed_serial);
    [[nodiscard]] std::size_t pending_releases() const noexcept;
    [[nodiscard]] GpuMemoryStatistics memory_statistics() const noexcept;

private:
    struct AllocatorOwner {
        VmaAllocator handle { VK_NULL_HANDLE };
        ~AllocatorOwner();
    };
    struct BufferResource {
        VmaAllocator allocator { VK_NULL_HANDLE };
        VkBuffer handle { VK_NULL_HANDLE };
        VmaAllocation allocation { VK_NULL_HANDLE };
        VkDeviceSize size { 0 };
        BufferResource() = default;
        BufferResource(const BufferResource&) = delete;
        BufferResource& operator=(const BufferResource&) = delete;
        BufferResource(BufferResource&& other) noexcept;
        BufferResource& operator=(BufferResource&& other) noexcept;
        ~BufferResource();
    };
    struct ImageResource {
        VmaAllocator allocator { VK_NULL_HANDLE };
        VkDevice device { VK_NULL_HANDLE };
        VkImage handle { VK_NULL_HANDLE };
        VkImageView view { VK_NULL_HANDLE };
        VmaAllocation allocation { VK_NULL_HANDLE };
        VkExtent3D extent {};
        VkFormat format { VK_FORMAT_UNDEFINED };
        ImageResource() = default;
        ImageResource(const ImageResource&) = delete;
        ImageResource& operator=(const ImageResource&) = delete;
        ImageResource(ImageResource&& other) noexcept;
        ImageResource& operator=(ImageResource&& other) noexcept;
        ~ImageResource();
    };

    VkDevice device_ { VK_NULL_HANDLE }; // 借用；由 VulkanDevice 最后销毁。
    AllocatorOwner allocator_; // 声明在 Registry 之前，保证最后析构。
    core::ResourceRegistry<BufferResource> buffers_;
    core::ResourceRegistry<ImageResource> images_;
    core::DeferredReleaseQueue<BufferResource> retired_buffers_;
    core::DeferredReleaseQueue<ImageResource> retired_images_;
};

} // namespace emberframe::renderer
