#pragma once

#include <vulkan/vulkan.h>

struct SDL_Window;

namespace emberframe::samples {

// B4 教学对象：拥有 Instance、Debug Messenger 和 Surface，借用 SDL_Window。
// 它不创建设备、队列和 Swapchain，这些属于后续课程。
class VulkanBootstrapProbe {
public:
    explicit VulkanBootstrapProbe(SDL_Window* borrowed_window);
    ~VulkanBootstrapProbe();

    VulkanBootstrapProbe(const VulkanBootstrapProbe&) = delete;
    VulkanBootstrapProbe& operator=(const VulkanBootstrapProbe&) = delete;
    VulkanBootstrapProbe(VulkanBootstrapProbe&&) = delete;
    VulkanBootstrapProbe& operator=(VulkanBootstrapProbe&&) = delete;

    [[nodiscard]] VkInstance instance() const noexcept;
    [[nodiscard]] VkSurfaceKHR surface() const noexcept;
    [[nodiscard]] bool validation_enabled() const noexcept;

private:
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
        VkDebugUtilsMessageTypeFlagsEXT message_types,
        const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
        void* user_data);
    [[nodiscard]] static VkDebugUtilsMessengerCreateInfoEXT
        make_debug_create_info() noexcept;

    void create_instance(SDL_Window* window);
    void create_debug_messenger();
    void create_surface(SDL_Window* window);
    void print_physical_device_summary() const;
    void submit_demo_debug_message() const;
    void cleanup() noexcept;

    VkInstance instance_ { VK_NULL_HANDLE };
    VkDebugUtilsMessengerEXT debug_messenger_ { VK_NULL_HANDLE };
    VkSurfaceKHR surface_ { VK_NULL_HANDLE };
    bool validation_enabled_ { false };
    bool debug_utils_enabled_ { false };
};

} // namespace emberframe::samples
