#include "vulkan_context.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace emberframe::renderer {
namespace {

constexpr const char* validation_layer_name = "VK_LAYER_KHRONOS_validation";

[[nodiscard]] std::string version_string(const std::uint32_t version)
{
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "."
        + std::to_string(VK_API_VERSION_MINOR(version)) + "."
        + std::to_string(VK_API_VERSION_PATCH(version));
}

void require_success(const VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(
            std::string(operation) + " failed, VkResult=" + std::to_string(result));
    }
}

[[nodiscard]] std::vector<VkExtensionProperties> enumerate_instance_extensions()
{
    std::uint32_t count = 0;
    require_success(
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
        "vkEnumerateInstanceExtensionProperties(count)");

    std::vector<VkExtensionProperties> extensions(count);
    require_success(
        vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()),
        "vkEnumerateInstanceExtensionProperties(data)");
    extensions.resize(count);
    return extensions;
}

[[nodiscard]] std::vector<VkLayerProperties> enumerate_instance_layers()
{
    std::uint32_t count = 0;
    require_success(
        vkEnumerateInstanceLayerProperties(&count, nullptr),
        "vkEnumerateInstanceLayerProperties(count)");

    std::vector<VkLayerProperties> layers(count);
    require_success(
        vkEnumerateInstanceLayerProperties(&count, layers.data()),
        "vkEnumerateInstanceLayerProperties(data)");
    layers.resize(count);
    return layers;
}

[[nodiscard]] bool has_extension(
    const std::vector<VkExtensionProperties>& available,
    const char* name)
{
    return std::any_of(
        available.begin(),
        available.end(),
        [name](const VkExtensionProperties& extension) {
            return std::string(extension.extensionName) == name;
        });
}

[[nodiscard]] bool has_layer(
    const std::vector<VkLayerProperties>& available,
    const char* name)
{
    return std::any_of(
        available.begin(),
        available.end(),
        [name](const VkLayerProperties& layer) {
            return std::string(layer.layerName) == name;
        });
}

[[nodiscard]] const char* severity_name(
    const VkDebugUtilsMessageSeverityFlagBitsEXT severity) noexcept
{
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        return "error";
    }
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        return "warning";
    }
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0) {
        return "info";
    }
    return "verbose";
}

[[nodiscard]] std::string message_type_name(
    const VkDebugUtilsMessageTypeFlagsEXT types)
{
    std::string result;
    if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) != 0) {
        result += "general";
    }
    if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0) {
        if (!result.empty()) {
            result += '|';
        }
        result += "validation";
    }
    if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0) {
        if (!result.empty()) {
            result += '|';
        }
        result += "performance";
    }
    return result;
}

} // namespace

VulkanContext::VulkanContext(SDL_Window* const borrowed_window)
{
    if (borrowed_window == nullptr) {
        throw std::invalid_argument("VulkanContext requires a valid SDL_Window");
    }

    // 构造中途失败时析构函数不会运行，因此这里显式回滚已创建的 Vulkan 对象。
    try {
        create_instance(borrowed_window);
        create_debug_messenger();
        create_surface(borrowed_window);
    } catch (...) {
        cleanup();
        throw;
    }
}

VulkanContext::~VulkanContext()
{
    cleanup();
}

VkInstance VulkanContext::instance() const noexcept
{
    return instance_;
}

VkSurfaceKHR VulkanContext::surface() const noexcept
{
    return surface_;
}

bool VulkanContext::validation_enabled() const noexcept
{
    return validation_enabled_;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::debug_callback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
    const VkDebugUtilsMessageTypeFlagsEXT message_types,
    const VkDebugUtilsMessengerCallbackDataEXT* const callback_data,
    void*)
{
    const char* message_id = callback_data != nullptr
            && callback_data->pMessageIdName != nullptr
        ? callback_data->pMessageIdName
        : "unnamed";
    const char* message = callback_data != nullptr
            && callback_data->pMessage != nullptr
        ? callback_data->pMessage
        : "no message";

    std::cerr << "[Vulkan][" << severity_name(message_severity) << "]["
              << message_type_name(message_types) << "][" << message_id << "] "
              << message << '\n';

    // VK_FALSE 表示“只报告，不阻止 Vulkan 调用继续执行”。
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT
VulkanContext::make_debug_create_info() noexcept
{
    VkDebugUtilsMessengerCreateInfoEXT create_info {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
    };
    create_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    create_info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    create_info.pfnUserCallback = &VulkanContext::debug_callback;
    return create_info;
}

void VulkanContext::create_instance(SDL_Window* const window)
{
    std::uint32_t loader_version = VK_API_VERSION_1_0;
    require_success(vkEnumerateInstanceVersion(&loader_version), "vkEnumerateInstanceVersion");

    std::cout << "[Vulkan] Vulkan loader maximum API version: "
              << version_string(loader_version) << '\n';
    if (loader_version < VK_API_VERSION_1_3) {
        throw std::runtime_error("Vulkan requires a Vulkan 1.3 capable loader");
    }

    const std::vector<VkExtensionProperties> available_extensions =
        enumerate_instance_extensions();
    const std::vector<VkLayerProperties> available_layers =
        enumerate_instance_layers();

    unsigned int sdl_extension_count = 0;
    if (SDL_Vulkan_GetInstanceExtensions(window, &sdl_extension_count, nullptr) != SDL_TRUE) {
        throw std::runtime_error(
            std::string("SDL could not query Vulkan instance extensions: ") + SDL_GetError());
    }

    std::vector<const char*> enabled_extensions(sdl_extension_count);
    if (SDL_Vulkan_GetInstanceExtensions(
            window,
            &sdl_extension_count,
            enabled_extensions.data()) != SDL_TRUE) {
        throw std::runtime_error(
            std::string("SDL could not provide Vulkan instance extensions: ") + SDL_GetError());
    }

    std::cout << "[Vulkan] SDL requires " << enabled_extensions.size()
              << " instance extension(s):\n";
    for (const char* const extension : enabled_extensions) {
        const bool available = has_extension(available_extensions, extension);
        std::cout << "      " << extension
                  << (available ? " [available]" : " [missing]") << '\n';
        if (!available) {
            throw std::runtime_error(
                std::string("Required SDL Vulkan extension is unavailable: ") + extension);
        }
    }

    debug_utils_enabled_ = has_extension(
        available_extensions,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debug_utils_enabled_) {
        enabled_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    validation_enabled_ = has_layer(available_layers, validation_layer_name);
    const std::vector<const char*> enabled_layers = validation_enabled_
        ? std::vector<const char*> { validation_layer_name }
        : std::vector<const char*> {};

    std::cout << "[Vulkan] VK_EXT_debug_utils: "
              << (debug_utils_enabled_ ? "enabled" : "unavailable") << '\n';
    std::cout << "[Vulkan] VK_LAYER_KHRONOS_validation: "
              << (validation_enabled_ ? "enabled" : "unavailable") << '\n';

    VkApplicationInfo application_info { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    application_info.pApplicationName = "EmberFrame Engine";
    application_info.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application_info.pEngineName = "EmberFrame";
    application_info.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application_info.apiVersion = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT debug_create_info = make_debug_create_info();

    VkInstanceCreateInfo create_info { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    create_info.pApplicationInfo = &application_info;
    create_info.enabledExtensionCount =
        static_cast<std::uint32_t>(enabled_extensions.size());
    create_info.ppEnabledExtensionNames = enabled_extensions.data();
    create_info.enabledLayerCount = static_cast<std::uint32_t>(enabled_layers.size());
    create_info.ppEnabledLayerNames = enabled_layers.data();

    // 把回调创建信息挂进 pNext，可以收到 vkCreateInstance 期间产生的消息。
    if (debug_utils_enabled_) {
        create_info.pNext = &debug_create_info;
    }

    require_success(vkCreateInstance(&create_info, nullptr, &instance_), "vkCreateInstance");
    std::cout << "[Vulkan] VkInstance created for Vulkan 1.3.\n";
}

void VulkanContext::create_debug_messenger()
{
    if (!debug_utils_enabled_) {
        return;
    }

    const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (create_messenger == nullptr) {
        throw std::runtime_error("vkCreateDebugUtilsMessengerEXT was not loaded");
    }

    VkDebugUtilsMessengerCreateInfoEXT create_info = make_debug_create_info();
    require_success(
        create_messenger(instance_, &create_info, nullptr, &debug_messenger_),
        "vkCreateDebugUtilsMessengerEXT");
    std::cout << "[Vulkan] Debug messenger created.\n";
}

void VulkanContext::create_surface(SDL_Window* const window)
{
    if (SDL_Vulkan_CreateSurface(window, instance_, &surface_) != SDL_TRUE) {
        throw std::runtime_error(
            std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
    }
    std::cout << "[Vulkan] VkSurfaceKHR created from the SDL window.\n";
}

void VulkanContext::cleanup() noexcept
{
    // Surface 依赖 Instance，所以必须先销毁 Surface，最后销毁 Instance。
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        std::cout << "[Vulkan] VkSurfaceKHR destroyed.\n";
    }

    if (debug_messenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        const auto destroy_messenger =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_messenger != nullptr) {
            destroy_messenger(instance_, debug_messenger_, nullptr);
        }
        debug_messenger_ = VK_NULL_HANDLE;
        std::cout << "[Vulkan] Debug messenger destroyed.\n";
    }

    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
        std::cout << "[Vulkan] VkInstance destroyed.\n";
    }
}

} // namespace emberframe::renderer

