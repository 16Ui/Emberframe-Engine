#include "vulkan_device.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace emberframe::renderer {
namespace {

void require_success(const VkResult result, const char* const operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(
            std::string(operation) + " failed, VkResult=" + std::to_string(result));
    }
}

[[nodiscard]] std::string version_string(const std::uint32_t version)
{
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "."
        + std::to_string(VK_API_VERSION_MINOR(version)) + "."
        + std::to_string(VK_API_VERSION_PATCH(version));
}

[[nodiscard]] const char* device_type_name(const VkPhysicalDeviceType type) noexcept
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "CPU Vulkan device";
    default:
        return "other";
    }
}

[[nodiscard]] std::string queue_flags_string(const VkQueueFlags flags)
{
    std::string result;
    const auto append = [&result](const char* const name) {
        if (!result.empty()) {
            result += '|';
        }
        result += name;
    };

    if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0) {
        append("graphics");
    }
    if ((flags & VK_QUEUE_COMPUTE_BIT) != 0) {
        append("compute");
    }
    if ((flags & VK_QUEUE_TRANSFER_BIT) != 0) {
        append("transfer");
    }
    if ((flags & VK_QUEUE_SPARSE_BINDING_BIT) != 0) {
        append("sparse");
    }
    if (result.empty()) {
        result = "none";
    }
    return result;
}

[[nodiscard]] int score_device(const VkPhysicalDeviceProperties& properties)
{
    int score = 0;
    switch (properties.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        score += 1000;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        score += 500;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        score += 250;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        score += 100;
        break;
    default:
        break;
    }

    // 最大二维纹理尺寸只是教学用的稳定能力指标，不代表完整 GPU 性能。
    score += static_cast<int>(properties.limits.maxImageDimension2D / 1024U);
    return score;
}

} // namespace

bool VulkanDevice::QueueFamilySelection::complete() const noexcept
{
    return graphics.has_value() && present.has_value();
}

bool VulkanDevice::DeviceCandidate::accepted() const noexcept
{
    return rejection_reasons.empty();
}

VulkanDevice::VulkanDevice(
    const VkInstance borrowed_instance,
    const VkSurfaceKHR borrowed_surface)
{
    if (borrowed_instance == VK_NULL_HANDLE || borrowed_surface == VK_NULL_HANDLE) {
        throw std::invalid_argument(
            "VulkanDevice requires valid borrowed Instance and Surface handles");
    }

    // 构造途中如果 Device 创建后又失败，本对象尚未构造完成，析构函数不会运行。
    try {
        select_physical_device(borrowed_instance, borrowed_surface);
        create_logical_device();
    } catch (...) {
        cleanup();
        throw;
    }
}

VulkanDevice::~VulkanDevice()
{
    cleanup();
}

VkPhysicalDevice VulkanDevice::physical_device() const noexcept
{
    return physical_device_;
}

VkDevice VulkanDevice::device() const noexcept
{
    return device_;
}

VkQueue VulkanDevice::graphics_queue() const noexcept
{
    return graphics_queue_;
}

VkQueue VulkanDevice::present_queue() const noexcept
{
    return present_queue_;
}

std::uint32_t VulkanDevice::graphics_queue_family() const noexcept
{
    return graphics_queue_family_;
}

std::uint32_t VulkanDevice::present_queue_family() const noexcept
{
    return present_queue_family_;
}

VulkanDevice::QueueFamilySelection VulkanDevice::inspect_queue_families(
    const VkPhysicalDevice physical_device,
    const VkSurfaceKHR surface)
{
    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);

    std::vector<VkQueueFamilyProperties> properties(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device,
        &family_count,
        properties.data());
    properties.resize(family_count);

    QueueFamilySelection selection;
    selection.reports.reserve(properties.size());

    for (std::uint32_t index = 0; index < family_count; ++index) {
        VkBool32 present_supported = VK_FALSE;
        require_success(
            vkGetPhysicalDeviceSurfaceSupportKHR(
                physical_device,
                index,
                surface,
                &present_supported),
            "vkGetPhysicalDeviceSurfaceSupportKHR");

        const VkQueueFamilyProperties& family = properties[index];
        const bool graphics_supported =
            family.queueCount > 0
            && (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        const bool can_present = family.queueCount > 0 && present_supported == VK_TRUE;

        selection.reports.push_back({
            index,
            family.queueCount,
            family.queueFlags,
            can_present,
        });

        if (graphics_supported && !selection.graphics.has_value()) {
            selection.graphics = index;
        }
        if (can_present && !selection.present.has_value()) {
            selection.present = index;
        }
    }

    return selection;
}

bool VulkanDevice::supports_device_extension(
    const VkPhysicalDevice physical_device,
    const char* const required_extension)
{
    std::uint32_t extension_count = 0;
    require_success(
        vkEnumerateDeviceExtensionProperties(
            physical_device,
            nullptr,
            &extension_count,
            nullptr),
        "vkEnumerateDeviceExtensionProperties(count)");

    std::vector<VkExtensionProperties> extensions(extension_count);
    require_success(
        vkEnumerateDeviceExtensionProperties(
            physical_device,
            nullptr,
            &extension_count,
            extensions.data()),
        "vkEnumerateDeviceExtensionProperties(data)");
    extensions.resize(extension_count);

    return std::any_of(
        extensions.begin(),
        extensions.end(),
        [required_extension](const VkExtensionProperties& extension) {
            return std::string(extension.extensionName) == required_extension;
        });
}

VulkanDevice::DeviceCandidate VulkanDevice::inspect_candidate(
    const VkPhysicalDevice physical_device,
    const VkSurfaceKHR surface)
{
    DeviceCandidate candidate;
    candidate.handle = physical_device;
    vkGetPhysicalDeviceProperties(physical_device, &candidate.properties);
    candidate.queue_families = inspect_queue_families(physical_device, surface);
    candidate.swapchain_extension_supported = supports_device_extension(
        physical_device,
        VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    VkPhysicalDeviceVulkan13Features features13 {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceFeatures2 features2 {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
    };
    features2.pNext = &features13;
    vkGetPhysicalDeviceFeatures2(physical_device, &features2);

    candidate.dynamic_rendering_supported = features13.dynamicRendering == VK_TRUE;
    candidate.synchronization2_supported = features13.synchronization2 == VK_TRUE;

    if (candidate.properties.apiVersion < VK_API_VERSION_1_3) {
        candidate.rejection_reasons.emplace_back("Vulkan 1.3 is unavailable");
    }
    if (!candidate.queue_families.graphics.has_value()) {
        candidate.rejection_reasons.emplace_back("no Graphics Queue Family");
    }
    if (!candidate.queue_families.present.has_value()) {
        candidate.rejection_reasons.emplace_back("no Queue Family can present to this Surface");
    }
    if (!candidate.swapchain_extension_supported) {
        candidate.rejection_reasons.emplace_back("VK_KHR_swapchain is unavailable");
    }
    if (!candidate.dynamic_rendering_supported) {
        candidate.rejection_reasons.emplace_back("dynamicRendering feature is unavailable");
    }
    if (!candidate.synchronization2_supported) {
        candidate.rejection_reasons.emplace_back("synchronization2 feature is unavailable");
    }

    if (candidate.accepted()) {
        candidate.score = score_device(candidate.properties);
    }
    return candidate;
}

void VulkanDevice::print_candidate_report(const DeviceCandidate& candidate)
{
    std::cout << "[Vulkan] Candidate: " << candidate.properties.deviceName << '\n'
              << "     Type: " << device_type_name(candidate.properties.deviceType) << '\n'
              << "     Vulkan API: " << version_string(candidate.properties.apiVersion) << '\n';

    for (const QueueFamilyReport& family : candidate.queue_families.reports) {
        std::cout << "     Queue Family " << family.index
                  << ": count=" << family.queue_count
                  << ", flags=" << queue_flags_string(family.flags)
                  << ", present=" << (family.supports_present ? "yes" : "no")
                  << '\n';
    }

    std::cout << "     VK_KHR_swapchain: "
              << (candidate.swapchain_extension_supported ? "supported" : "missing") << '\n'
              << "     dynamicRendering: "
              << (candidate.dynamic_rendering_supported ? "supported" : "missing") << '\n'
              << "     synchronization2: "
              << (candidate.synchronization2_supported ? "supported" : "missing") << '\n';

    if (candidate.accepted()) {
        std::cout << "     Score: " << candidate.score << '\n'
                  << "     Result: accepted\n";
        return;
    }

    std::cout << "     Result: rejected\n";
    for (const std::string& reason : candidate.rejection_reasons) {
        std::cout << "       - " << reason << '\n';
    }
}

void VulkanDevice::select_physical_device(
    const VkInstance instance,
    const VkSurfaceKHR surface)
{
    std::uint32_t device_count = 0;
    require_success(
        vkEnumeratePhysicalDevices(instance, &device_count, nullptr),
        "vkEnumeratePhysicalDevices(count)");
    if (device_count == 0) {
        throw std::runtime_error("No Vulkan Physical Device was found");
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    require_success(
        vkEnumeratePhysicalDevices(instance, &device_count, devices.data()),
        "vkEnumeratePhysicalDevices(data)");
    devices.resize(device_count);

    std::cout << "[Vulkan] Evaluating " << devices.size() << " physical device(s).\n";

    std::optional<DeviceCandidate> best_candidate;
    for (const VkPhysicalDevice physical_device : devices) {
        DeviceCandidate candidate = inspect_candidate(physical_device, surface);
        print_candidate_report(candidate);

        if (candidate.accepted()
            && (!best_candidate.has_value() || candidate.score > best_candidate->score)) {
            best_candidate = std::move(candidate);
        }
    }

    if (!best_candidate.has_value()) {
        throw std::runtime_error("No Physical Device satisfies the Vulkan requirements");
    }

    physical_device_ = best_candidate->handle;
    graphics_queue_family_ = best_candidate->queue_families.graphics.value();
    present_queue_family_ = best_candidate->queue_families.present.value();

    std::cout << "[Vulkan] Selected device: " << best_candidate->properties.deviceName << '\n'
              << "     Graphics Queue Family: " << graphics_queue_family_ << '\n'
              << "     Present Queue Family: " << present_queue_family_ << '\n';
}

void VulkanDevice::create_logical_device()
{
    std::vector<std::uint32_t> unique_queue_families { graphics_queue_family_ };
    if (present_queue_family_ != graphics_queue_family_) {
        unique_queue_families.push_back(present_queue_family_);
    }

    const float queue_priority = 1.0F;
    std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
    queue_create_infos.reserve(unique_queue_families.size());

    for (const std::uint32_t family_index : unique_queue_families) {
        VkDeviceQueueCreateInfo queue_create_info {
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        };
        queue_create_info.queueFamilyIndex = family_index;
        queue_create_info.queueCount = 1;
        queue_create_info.pQueuePriorities = &queue_priority;
        queue_create_infos.push_back(queue_create_info);
    }

    // 只启用已经在候选检查阶段确认支持、且后续渲染主线会使用的 Feature。
    VkPhysicalDeviceVulkan13Features enabled_features13 {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    enabled_features13.dynamicRendering = VK_TRUE;
    enabled_features13.synchronization2 = VK_TRUE;

    constexpr std::array<const char*, 1> required_extensions {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    VkDeviceCreateInfo create_info {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    };
    create_info.pNext = &enabled_features13;
    create_info.queueCreateInfoCount =
        static_cast<std::uint32_t>(queue_create_infos.size());
    create_info.pQueueCreateInfos = queue_create_infos.data();
    create_info.enabledExtensionCount =
        static_cast<std::uint32_t>(required_extensions.size());
    create_info.ppEnabledExtensionNames = required_extensions.data();

    require_success(
        vkCreateDevice(physical_device_, &create_info, nullptr, &device_),
        "vkCreateDevice");

    // Queue 已经在 vkCreateDevice 时申请；这里仅按 Family/Queue Index 取回 Handle。
    vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, present_queue_family_, 0, &present_queue_);
    if (graphics_queue_ == VK_NULL_HANDLE || present_queue_ == VK_NULL_HANDLE) {
        throw std::runtime_error("vkGetDeviceQueue returned a null Queue handle");
    }

    std::cout << "[Vulkan] Logical device created.\n"
              << "[Vulkan] Graphics and Present use "
              << (graphics_queue_family_ == present_queue_family_
                      ? "the same Queue Family.\n"
                      : "different Queue Families.\n")
              << "[Vulkan] Graphics and Present Queue handles are "
              << (graphics_queue_ == present_queue_ ? "the same.\n" : "different.\n");
}

void VulkanDevice::cleanup() noexcept
{
    if (device_ != VK_NULL_HANDLE) {
        // 上层帧资源已先析构；这里再等待一次，保证独立销毁 Device 也安全。
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        graphics_queue_ = VK_NULL_HANDLE;
        present_queue_ = VK_NULL_HANDLE;
        std::cout << "[Vulkan] VkDevice destroyed; its Queue handles are now invalid.\n";
    }

    // physical_device_ 由 Instance 枚举得到，没有对应的 vkDestroyPhysicalDevice。
    physical_device_ = VK_NULL_HANDLE;
}

} // namespace emberframe::renderer

