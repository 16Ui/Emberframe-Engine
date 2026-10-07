#include "renderer/gpu_resources.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace emberframe::renderer {
namespace {
void require_success(const VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed, VkResult=" + std::to_string(result));
    }
}
} // namespace

GpuResources::AllocatorOwner::~AllocatorOwner()
{
    if (handle != VK_NULL_HANDLE) vmaDestroyAllocator(handle);
}

GpuResources::BufferResource::BufferResource(BufferResource&& other) noexcept
    : allocator(std::exchange(other.allocator, VK_NULL_HANDLE))
    , handle(std::exchange(other.handle, VK_NULL_HANDLE))
    , allocation(std::exchange(other.allocation, VK_NULL_HANDLE))
    , size(std::exchange(other.size, 0)) {}

GpuResources::BufferResource& GpuResources::BufferResource::operator=(BufferResource&& other) noexcept
{
    if (this != &other) {
        if (handle != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, handle, allocation);
        allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
        handle = std::exchange(other.handle, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        size = std::exchange(other.size, 0);
    }
    return *this;
}

GpuResources::BufferResource::~BufferResource()
{
    if (handle != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, handle, allocation);
}

GpuResources::ImageResource::ImageResource(ImageResource&& other) noexcept
    : allocator(std::exchange(other.allocator, VK_NULL_HANDLE))
    , device(std::exchange(other.device, VK_NULL_HANDLE))
    , handle(std::exchange(other.handle, VK_NULL_HANDLE))
    , view(std::exchange(other.view, VK_NULL_HANDLE))
    , allocation(std::exchange(other.allocation, VK_NULL_HANDLE))
    , extent(other.extent), format(other.format) {}

GpuResources::ImageResource& GpuResources::ImageResource::operator=(ImageResource&& other) noexcept
{
    if (this != &other) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
        if (handle != VK_NULL_HANDLE) vmaDestroyImage(allocator, handle, allocation);
        allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
        device = std::exchange(other.device, VK_NULL_HANDLE);
        handle = std::exchange(other.handle, VK_NULL_HANDLE);
        view = std::exchange(other.view, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        extent = other.extent;
        format = other.format;
    }
    return *this;
}

GpuResources::ImageResource::~ImageResource()
{
    // ImageView 依赖 Image，所以先释放 View 再释放 VMA Image+Allocation。
    if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
    if (handle != VK_NULL_HANDLE) vmaDestroyImage(allocator, handle, allocation);
}

GpuResources::GpuResources(const VkInstance instance,
    const VkPhysicalDevice physical_device, const VkDevice device)
    : device_(device)
{
    if (instance == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
        throw std::invalid_argument("GpuResources requires Instance, Physical Device and Device");
    }
    VmaAllocatorCreateInfo info {};
    info.instance = instance;
    info.physicalDevice = physical_device;
    info.device = device;
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    require_success(vmaCreateAllocator(&info, &allocator_.handle), "vmaCreateAllocator");
}

GpuResources::~GpuResources()
{
    // 保险路径：即使调用者漏掉 collect，也不能在 GPU 仍引用资源时销毁。
    vkDeviceWaitIdle(device_);
    collect(std::numeric_limits<std::uint64_t>::max());
}

core::ResourceHandle GpuResources::create_buffer(const VkDeviceSize size,
    const VkBufferUsageFlags usage, const VmaMemoryUsage memory_usage,
    const VmaAllocationCreateFlags flags)
{
    if (size == 0 || usage == 0) throw std::invalid_argument("Buffer size and usage must be nonzero");
    VkBufferCreateInfo buffer_info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation_info {};
    allocation_info.usage = memory_usage;
    allocation_info.flags = flags;
    BufferResource resource;
    resource.allocator = allocator_.handle;
    resource.size = size;
    require_success(vmaCreateBuffer(allocator_.handle, &buffer_info, &allocation_info,
        &resource.handle, &resource.allocation, nullptr), "vmaCreateBuffer");
    return buffers_.create(std::move(resource));
}

core::ResourceHandle GpuResources::create_image(const VkExtent3D extent,
    const VkFormat format, const VkImageUsageFlags usage, const VkImageAspectFlags aspect)
{
    if (extent.width == 0 || extent.height == 0 || extent.depth == 0
        || format == VK_FORMAT_UNDEFINED || usage == 0 || aspect == 0) {
        throw std::invalid_argument("Image extent, format, usage and aspect must be valid");
    }
    VkImageCreateInfo image_info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    image_info.imageType = extent.depth == 1 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_3D;
    image_info.extent = extent;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = format;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = usage;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation_info {};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ImageResource resource;
    resource.allocator = allocator_.handle;
    resource.device = device_;
    resource.extent = extent;
    resource.format = format;
    require_success(vmaCreateImage(allocator_.handle, &image_info, &allocation_info,
        &resource.handle, &resource.allocation, nullptr), "vmaCreateImage");
    VkImageViewCreateInfo view_info { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view_info.image = resource.handle;
    view_info.viewType = extent.depth == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_3D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = aspect;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    require_success(vkCreateImageView(device_, &view_info, nullptr, &resource.view), "vkCreateImageView");
    return images_.create(std::move(resource));
}

BufferView GpuResources::buffer(const core::ResourceHandle handle) const noexcept
{
    const auto* resource = buffers_.get(handle);
    return resource ? BufferView { resource->handle, resource->size } : BufferView {};
}

ImageView GpuResources::image(const core::ResourceHandle handle) const noexcept
{
    const auto* resource = images_.get(handle);
    return resource ? ImageView { resource->handle, resource->view, resource->extent, resource->format }
        : ImageView {};
}

bool GpuResources::retire_buffer(const core::ResourceHandle handle, const std::uint64_t after_serial)
{
    auto resource = buffers_.release(handle);
    if (!resource) return false;
    try {
        retired_buffers_.retire(after_serial, std::move(*resource));
    } catch (...) {
        vkDeviceWaitIdle(device_); // 回退为保守同步，再由局部对象析构。
    }
    return true;
}

bool GpuResources::retire_image(const core::ResourceHandle handle, const std::uint64_t after_serial)
{
    auto resource = images_.release(handle);
    if (!resource) return false;
    try {
        retired_images_.retire(after_serial, std::move(*resource));
    } catch (...) {
        vkDeviceWaitIdle(device_);
    }
    return true;
}

void GpuResources::collect(const std::uint64_t completed_serial)
{
    retired_buffers_.collect(completed_serial);
    retired_images_.collect(completed_serial);
}

std::size_t GpuResources::pending_releases() const noexcept
{
    return retired_buffers_.pending_count() + retired_images_.pending_count();
}

GpuMemoryStatistics GpuResources::memory_statistics() const noexcept
{
    VmaTotalStatistics statistics {};
    vmaCalculateStatistics(allocator_.handle, &statistics);
    return { statistics.total.statistics.allocationCount,
        statistics.total.statistics.allocationBytes,
        statistics.total.statistics.blockBytes };
}

} // namespace emberframe::renderer
