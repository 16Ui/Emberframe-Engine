#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <vector>

struct SDL_Window;

namespace emberframe::samples {

// B6 教学对象：借用此前创建的 Window、Physical Device、Logical Device 与 Surface，
// 拥有一个 Swapchain 以及应用为每张 Swapchain Image 创建的 ImageView。
class SwapchainProbe {
public:
    SwapchainProbe(
        SDL_Window* borrowed_window,
        VkPhysicalDevice borrowed_physical_device,
        VkDevice borrowed_device,
        VkSurfaceKHR borrowed_surface,
        std::uint32_t graphics_queue_family,
        std::uint32_t present_queue_family,
        VkImageUsageFlags image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    ~SwapchainProbe();

    SwapchainProbe(const SwapchainProbe&) = delete;
    SwapchainProbe& operator=(const SwapchainProbe&) = delete;
    SwapchainProbe(SwapchainProbe&&) = delete;
    SwapchainProbe& operator=(SwapchainProbe&&) = delete;

    // 窗口尺寸变化后重建整组 Swapchain 资源。
    // 最小化时像素尺寸可能为 0，此时返回 false，等待窗口恢复后再试。
    [[nodiscard]] bool recreate();

    [[nodiscard]] VkSwapchainKHR swapchain() const noexcept;
    [[nodiscard]] VkFormat image_format() const noexcept;
    [[nodiscard]] VkExtent2D extent() const noexcept;
    [[nodiscard]] const std::vector<VkImage>& images() const noexcept;
    [[nodiscard]] const std::vector<VkImageView>& image_views() const noexcept;

private:
    struct SwapchainSupport {
        VkSurfaceCapabilitiesKHR capabilities {};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> present_modes;
    };

    [[nodiscard]] SwapchainSupport query_support() const;
    [[nodiscard]] VkSurfaceFormatKHR choose_surface_format(
        const std::vector<VkSurfaceFormatKHR>& formats) const;
    [[nodiscard]] VkPresentModeKHR choose_present_mode(
        const std::vector<VkPresentModeKHR>& present_modes) const;
    [[nodiscard]] VkExtent2D choose_extent(
        const VkSurfaceCapabilitiesKHR& capabilities) const;
    [[nodiscard]] static VkCompositeAlphaFlagBitsKHR choose_composite_alpha(
        VkCompositeAlphaFlagsKHR supported);
    [[nodiscard]] static std::vector<VkImageView> create_image_views(
        VkDevice device,
        const std::vector<VkImage>& images,
        VkFormat format);

    void create_or_replace_swapchain();
    void cleanup() noexcept;

    // 这些 Handle 的生命周期都长于本对象，本类只借用、不销毁。
    SDL_Window* window_ { nullptr };
    VkPhysicalDevice physical_device_ { VK_NULL_HANDLE };
    VkDevice device_ { VK_NULL_HANDLE };
    VkSurfaceKHR surface_ { VK_NULL_HANDLE };
    std::uint32_t graphics_queue_family_ { 0 };
    std::uint32_t present_queue_family_ { 0 };
    VkImageUsageFlags image_usage_ { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };

    // Swapchain 拥有多张 VkImage；应用只取得它们的 Handle，不能逐张销毁。
    VkSwapchainKHR swapchain_ { VK_NULL_HANDLE };
    std::vector<VkImage> images_;

    // ImageView 由应用创建并拥有，因此必须先销毁 View，再销毁 Swapchain。
    std::vector<VkImageView> image_views_;
    VkFormat image_format_ { VK_FORMAT_UNDEFINED };
    VkExtent2D extent_ {};
};

} // namespace emberframe::samples
