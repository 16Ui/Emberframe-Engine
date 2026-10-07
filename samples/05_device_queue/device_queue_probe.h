#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace emberframe::samples {

// B5 教学对象：借用 B4 创建的 Instance 和 Surface，选择 Physical Device，
// 拥有由该设备创建的 Logical Device，并取回 Graphics/Present Queue。
class DeviceQueueProbe {
public:
    DeviceQueueProbe(VkInstance borrowed_instance, VkSurfaceKHR borrowed_surface);
    ~DeviceQueueProbe();

    DeviceQueueProbe(const DeviceQueueProbe&) = delete;
    DeviceQueueProbe& operator=(const DeviceQueueProbe&) = delete;
    DeviceQueueProbe(DeviceQueueProbe&&) = delete;
    DeviceQueueProbe& operator=(DeviceQueueProbe&&) = delete;

    [[nodiscard]] VkPhysicalDevice physical_device() const noexcept;
    [[nodiscard]] VkDevice device() const noexcept;
    [[nodiscard]] VkQueue graphics_queue() const noexcept;
    [[nodiscard]] VkQueue present_queue() const noexcept;
    [[nodiscard]] std::uint32_t graphics_queue_family() const noexcept;
    [[nodiscard]] std::uint32_t present_queue_family() const noexcept;

private:
    struct QueueFamilyReport {
        std::uint32_t index { 0 };
        std::uint32_t queue_count { 0 };
        VkQueueFlags flags { 0 };
        bool supports_present { false };
    };

    struct QueueFamilySelection {
        std::optional<std::uint32_t> graphics;
        std::optional<std::uint32_t> present;
        std::vector<QueueFamilyReport> reports;

        [[nodiscard]] bool complete() const noexcept;
    };

    struct DeviceCandidate {
        VkPhysicalDevice handle { VK_NULL_HANDLE };
        VkPhysicalDeviceProperties properties {};
        QueueFamilySelection queue_families;
        bool swapchain_extension_supported { false };
        bool dynamic_rendering_supported { false };
        bool synchronization2_supported { false };
        std::vector<std::string> rejection_reasons;
        int score { 0 };

        [[nodiscard]] bool accepted() const noexcept;
    };

    [[nodiscard]] static QueueFamilySelection inspect_queue_families(
        VkPhysicalDevice physical_device,
        VkSurfaceKHR surface);
    [[nodiscard]] static bool supports_device_extension(
        VkPhysicalDevice physical_device,
        const char* required_extension);
    [[nodiscard]] static DeviceCandidate inspect_candidate(
        VkPhysicalDevice physical_device,
        VkSurfaceKHR surface);
    static void print_candidate_report(const DeviceCandidate& candidate);

    void select_physical_device(VkInstance instance, VkSurfaceKHR surface);
    void create_logical_device();
    void cleanup() noexcept;

    // VkPhysicalDevice 由 Instance 枚举得到，不由本类销毁。
    VkPhysicalDevice physical_device_ { VK_NULL_HANDLE };

    // VkDevice 由本类创建和销毁；Queue 随 Device 存活，不单独销毁。
    VkDevice device_ { VK_NULL_HANDLE };
    VkQueue graphics_queue_ { VK_NULL_HANDLE };
    VkQueue present_queue_ { VK_NULL_HANDLE };
    std::uint32_t graphics_queue_family_ { 0 };
    std::uint32_t present_queue_family_ { 0 };
};

} // namespace emberframe::samples
