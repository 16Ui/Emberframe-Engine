#include "swapchain_probe.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <algorithm>
#include <array>
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

[[nodiscard]] const char* present_mode_name(const VkPresentModeKHR mode) noexcept
{
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR:
        return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return "fifo-relaxed";
    default:
        return "other";
    }
}

[[nodiscard]] const char* format_name(const VkFormat format) noexcept
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB:
        return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_SRGB:
        return "R8G8B8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM:
        return "B8G8R8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_UNORM:
        return "R8G8B8A8_UNORM";
    default:
        return "other";
    }
}

} // namespace

SwapchainProbe::SwapchainProbe(
    SDL_Window* const borrowed_window,
    const VkPhysicalDevice borrowed_physical_device,
    const VkDevice borrowed_device,
    const VkSurfaceKHR borrowed_surface,
    const std::uint32_t graphics_queue_family,
    const std::uint32_t present_queue_family,
    const VkImageUsageFlags image_usage)
    : window_(borrowed_window)
    , physical_device_(borrowed_physical_device)
    , device_(borrowed_device)
    , surface_(borrowed_surface)
    , graphics_queue_family_(graphics_queue_family)
    , present_queue_family_(present_queue_family)
    , image_usage_(image_usage)
{
    if (window_ == nullptr
        || physical_device_ == VK_NULL_HANDLE
        || device_ == VK_NULL_HANDLE
        || surface_ == VK_NULL_HANDLE) {
        throw std::invalid_argument(
            "SwapchainProbe requires valid borrowed Window, Device and Surface handles");
    }

    // 构造中途失败时析构函数不会运行，因此显式清理已经创建的 View/Swapchain。
    try {
        create_or_replace_swapchain();
    } catch (...) {
        cleanup();
        throw;
    }
}

SwapchainProbe::~SwapchainProbe()
{
    cleanup();
}

bool SwapchainProbe::recreate()
{
    int drawable_width = 0;
    int drawable_height = 0;
    SDL_Vulkan_GetDrawableSize(window_, &drawable_width, &drawable_height);
    if (drawable_width <= 0 || drawable_height <= 0) {
        std::cout << "[B6] Drawable extent is zero; defer Swapchain recreation.\n";
        return false;
    }

    // B6 还没有多帧资源和精细同步。重建前等待整个 Device 空闲最容易验证正确性，
    // B8 再用 Fence/Semaphore 与更细粒度的 Barrier 替换这条重同步路径。
    require_success(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle(before recreate)");
    create_or_replace_swapchain();
    return true;
}

VkSwapchainKHR SwapchainProbe::swapchain() const noexcept
{
    return swapchain_;
}

VkFormat SwapchainProbe::image_format() const noexcept
{
    return image_format_;
}

VkExtent2D SwapchainProbe::extent() const noexcept
{
    return extent_;
}

const std::vector<VkImage>& SwapchainProbe::images() const noexcept
{
    return images_;
}

const std::vector<VkImageView>& SwapchainProbe::image_views() const noexcept
{
    return image_views_;
}

SwapchainProbe::SwapchainSupport SwapchainProbe::query_support() const
{
    SwapchainSupport support;
    require_success(
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physical_device_,
            surface_,
            &support.capabilities),
        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    std::uint32_t format_count = 0;
    require_success(
        vkGetPhysicalDeviceSurfaceFormatsKHR(
            physical_device_,
            surface_,
            &format_count,
            nullptr),
        "vkGetPhysicalDeviceSurfaceFormatsKHR(count)");
    support.formats.resize(format_count);
    if (format_count > 0) {
        require_success(
            vkGetPhysicalDeviceSurfaceFormatsKHR(
                physical_device_,
                surface_,
                &format_count,
                support.formats.data()),
            "vkGetPhysicalDeviceSurfaceFormatsKHR(data)");
        support.formats.resize(format_count);
    }

    std::uint32_t present_mode_count = 0;
    require_success(
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            physical_device_,
            surface_,
            &present_mode_count,
            nullptr),
        "vkGetPhysicalDeviceSurfacePresentModesKHR(count)");
    support.present_modes.resize(present_mode_count);
    if (present_mode_count > 0) {
        require_success(
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                physical_device_,
                surface_,
                &present_mode_count,
                support.present_modes.data()),
            "vkGetPhysicalDeviceSurfacePresentModesKHR(data)");
        support.present_modes.resize(present_mode_count);
    }

    if (support.formats.empty() || support.present_modes.empty()) {
        throw std::runtime_error(
            "The selected Device cannot create a usable Swapchain for this Surface");
    }
    // Surface 必须支持调用方声明的全部用途。B6/B7 默认只要求 Color Attachment，
    // B8 还会要求 Transfer Destination，以便用 vkCmdClearColorImage 写入画面。
    if ((support.capabilities.supportedUsageFlags & image_usage_) != image_usage_) {
        throw std::runtime_error(
            "Surface images do not support every requested VkImageUsageFlagBits");
    }
    return support;
}

VkSurfaceFormatKHR SwapchainProbe::choose_surface_format(
    const std::vector<VkSurfaceFormatKHR>& formats) const
{
    const auto preferred = std::find_if(
        formats.begin(),
        formats.end(),
        [](const VkSurfaceFormatKHR& candidate) {
            return candidate.format == VK_FORMAT_B8G8R8A8_SRGB
                && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });

    // 优先选择常见的 sRGB Swapchain 格式；不可用时使用 Surface 提供的首个组合。
    return preferred != formats.end() ? *preferred : formats.front();
}

VkPresentModeKHR SwapchainProbe::choose_present_mode(
    const std::vector<VkPresentModeKHR>& present_modes) const
{
    const auto mailbox = std::find(
        present_modes.begin(),
        present_modes.end(),
        VK_PRESENT_MODE_MAILBOX_KHR);

    // Mailbox 延迟低且不会撕裂；若不可用，FIFO 是 Vulkan 规范保证支持的回退项。
    return mailbox != present_modes.end()
        ? VK_PRESENT_MODE_MAILBOX_KHR
        : VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D SwapchainProbe::choose_extent(
    const VkSurfaceCapabilitiesKHR& capabilities) const
{
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    int drawable_width = 0;
    int drawable_height = 0;
    SDL_Vulkan_GetDrawableSize(window_, &drawable_width, &drawable_height);
    if (drawable_width <= 0 || drawable_height <= 0) {
        throw std::runtime_error("Cannot create a Swapchain for a zero-sized drawable");
    }

    VkExtent2D chosen {
        static_cast<std::uint32_t>(drawable_width),
        static_cast<std::uint32_t>(drawable_height),
    };
    chosen.width = std::clamp(
        chosen.width,
        capabilities.minImageExtent.width,
        capabilities.maxImageExtent.width);
    chosen.height = std::clamp(
        chosen.height,
        capabilities.minImageExtent.height,
        capabilities.maxImageExtent.height);
    return chosen;
}

VkCompositeAlphaFlagBitsKHR SwapchainProbe::choose_composite_alpha(
    const VkCompositeAlphaFlagsKHR supported)
{
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> candidates {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (const VkCompositeAlphaFlagBitsKHR candidate : candidates) {
        if ((supported & candidate) != 0) {
            return candidate;
        }
    }
    throw std::runtime_error("Surface reports no supported composite alpha mode");
}

std::vector<VkImageView> SwapchainProbe::create_image_views(
    const VkDevice device,
    const std::vector<VkImage>& images,
    const VkFormat format)
{
    std::vector<VkImageView> views;
    views.reserve(images.size());

    try {
        for (const VkImage image : images) {
            VkImageViewCreateInfo create_info {
                VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            };
            create_info.image = image;
            create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            create_info.format = format;
            create_info.components = {
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
            };
            create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            create_info.subresourceRange.baseMipLevel = 0;
            create_info.subresourceRange.levelCount = 1;
            create_info.subresourceRange.baseArrayLayer = 0;
            create_info.subresourceRange.layerCount = 1;

            VkImageView view = VK_NULL_HANDLE;
            require_success(
                vkCreateImageView(device, &create_info, nullptr, &view),
                "vkCreateImageView");
            views.push_back(view);
        }
    } catch (...) {
        for (const VkImageView view : views) {
            vkDestroyImageView(device, view, nullptr);
        }
        throw;
    }
    return views;
}

void SwapchainProbe::create_or_replace_swapchain()
{
    const SwapchainSupport support = query_support();
    const VkSurfaceFormatKHR surface_format = choose_surface_format(support.formats);
    const VkPresentModeKHR present_mode = choose_present_mode(support.present_modes);
    const VkExtent2D chosen_extent = choose_extent(support.capabilities);

    std::uint32_t image_count = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, support.capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR create_info {
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
    };
    create_info.surface = surface_;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = chosen_extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = image_usage_;

    const std::array queue_families {
        graphics_queue_family_,
        present_queue_family_,
    };
    if (graphics_queue_family_ != present_queue_family_) {
        // 两个 Queue Family 都会访问 Swapchain Image；B6 用 Concurrent 避免提前引入所有权转移。
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount =
            static_cast<std::uint32_t>(queue_families.size());
        create_info.pQueueFamilyIndices = queue_families.data();
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    create_info.preTransform = support.capabilities.currentTransform;
    create_info.compositeAlpha = choose_composite_alpha(
        support.capabilities.supportedCompositeAlpha);
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = swapchain_;

    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    require_success(
        vkCreateSwapchainKHR(device_, &create_info, nullptr, &new_swapchain),
        "vkCreateSwapchainKHR");

    std::vector<VkImage> new_images;
    std::vector<VkImageView> new_views;
    try {
        std::uint32_t actual_image_count = 0;
        require_success(
            vkGetSwapchainImagesKHR(
                device_,
                new_swapchain,
                &actual_image_count,
                nullptr),
            "vkGetSwapchainImagesKHR(count)");
        new_images.resize(actual_image_count);
        require_success(
            vkGetSwapchainImagesKHR(
                device_,
                new_swapchain,
                &actual_image_count,
                new_images.data()),
            "vkGetSwapchainImagesKHR(data)");
        new_images.resize(actual_image_count);
        new_views = create_image_views(device_, new_images, surface_format.format);
    } catch (...) {
        vkDestroySwapchainKHR(device_, new_swapchain, nullptr);
        throw;
    }

    // 新资源全部成功后才释放旧资源，避免普通 Resize 过程中先把可用状态销毁。
    for (const VkImageView view : image_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    }

    swapchain_ = new_swapchain;
    images_ = std::move(new_images);
    image_views_ = std::move(new_views);
    image_format_ = surface_format.format;
    extent_ = chosen_extent;

    std::cout << "[B6] Swapchain ready: "
              << extent_.width << 'x' << extent_.height
              << ", requested images=" << image_count
              << ", actual images=" << images_.size()
              << ", views=" << image_views_.size() << '\n'
              << "     format=" << format_name(image_format_)
              << ", present mode=" << present_mode_name(present_mode) << '\n'
              << "     queue sharing="
              << (graphics_queue_family_ == present_queue_family_
                      ? "exclusive (same family)\n"
                      : "concurrent (graphics/present families differ)\n");
}

void SwapchainProbe::cleanup() noexcept
{
    if (device_ != VK_NULL_HANDLE) {
        // B6 没有提交绘制工作，但等待空闲可确保未来扩展后仍保持安全的销毁顺序。
        vkDeviceWaitIdle(device_);
        for (const VkImageView view : image_views_) {
            vkDestroyImageView(device_, view, nullptr);
        }
        image_views_.clear();
        images_.clear();

        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
        std::cout << "[B6] ImageViews destroyed before VkSwapchainKHR.\n";
    }
}

} // namespace emberframe::samples
