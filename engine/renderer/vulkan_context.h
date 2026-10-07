#pragma once

#include <vulkan/vulkan.h>

struct SDL_Window;

namespace emberframe::renderer {

// 窗口借用关系：SDL_Window 必须比 Context 活得更久；本类拥有 Instance、Surface 与调试回调。
class VulkanContext {
public:
    explicit VulkanContext(SDL_Window* borrowed_window);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

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
    void cleanup() noexcept;

    VkInstance instance_ { VK_NULL_HANDLE };
    VkDebugUtilsMessengerEXT debug_messenger_ { VK_NULL_HANDLE };
    VkSurfaceKHR surface_ { VK_NULL_HANDLE };
    bool validation_enabled_ { false };
    bool debug_utils_enabled_ { false };
};

} // namespace emberframe::renderer

