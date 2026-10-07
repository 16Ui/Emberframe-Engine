#include "platform/sdl_window.h"
#include "platform/sdl_event_translator.h"
#include "platform/window_chrome.h"

#include <SDL.h>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace emberframe::platform {
namespace {

std::vector<WindowDragRegion> copy_valid_regions(const std::span<const WindowDragRegion> regions)
{
    std::vector<WindowDragRegion> copy;
    copy.reserve(regions.size());
    for (const auto& region : regions) {
        if (std::isfinite(region.x) && std::isfinite(region.y)
            && std::isfinite(region.width) && std::isfinite(region.height)
            && region.width > 0.0F && region.height > 0.0F) {
            copy.push_back(region);
        }
    }
    return copy;
}

} // namespace

SdlWindow::SdlWindow(const WindowConfig& config)
{
    if (config.width == 0 || config.height == 0
        || config.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || config.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("SDL window dimensions must be positive and fit in int");
    }

    // DPI 感知和坐标策略必须先于视频初始化；否则 Windows 会放大低分辨率画面而造成模糊。
    detail::prepare_window_dpi(config.borderless);
    // RAII 的“获取”阶段：构造成功后，本对象负责 SDL Video 子系统和窗口的生命周期。
    // 每个对象只持有一个子系统引用，销毁其中一个窗口不会撤销其它对象的引用。
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        throw std::runtime_error(std::string("SDL video initialization failed: ") + SDL_GetError());
    }

    auto flags = static_cast<SDL_WindowFlags>(SDL_WINDOW_VULKAN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (config.resizable) {
        flags = static_cast<SDL_WindowFlags>(flags | SDL_WINDOW_RESIZABLE);
    }
    if (config.borderless) {
        // 先安装原生辅助和 hit-test 再显示，避免窗口已经可交互时仍缺少工作区约束。
        flags = static_cast<SDL_WindowFlags>(flags | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN);
    }

    try {
        window_ = SDL_CreateWindow(
            config.title.c_str(),
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            static_cast<int>(config.width),
            static_cast<int>(config.height),
            flags);

        if (window_ == nullptr) {
            throw std::runtime_error(std::string("SDL window creation failed: ") + SDL_GetError());
        }

        if (config.borderless) {
            if (detail::install_window_chrome(window_) != 0) {
                throw std::runtime_error(std::string("Window chrome installation failed: ") + SDL_GetError());
            }
            chrome_installed_ = true;
            if (SDL_SetWindowHitTest(window_, [](SDL_Window* window, const SDL_Point* point, void* user_data) noexcept {
                    const auto* self = static_cast<const SdlWindow*>(user_data);
                    if (self == nullptr || point == nullptr || window != self->window_) {
                        return SDL_HITTEST_NORMAL;
                    }
                    return detail::hit_test_window_chrome(
                        self->extent(), *point, SDL_GetWindowFlags(window), self->titlebar_drag_regions_,
                        self->titlebar_interactive_regions_);
                }, this) != 0) {
                throw std::runtime_error(std::string("SDL window hit-test installation failed: ") + SDL_GetError());
            }
            SDL_ShowWindow(window_);
        }
    } catch (...) {
        // 构造失败没有析构机会；先撤销借用 this/window 的回调，再销毁窗口与视频引用。
        if (window_ != nullptr) {
            if (chrome_installed_) {
                SDL_SetWindowHitTest(window_, nullptr, nullptr);
                detail::remove_window_chrome(window_);
                chrome_installed_ = false;
            }
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        throw;
    }
}

SdlWindow::~SdlWindow()
{
    // RAII 的“释放”阶段：正常返回、提前返回和后续代码抛异常都会走到这里。
    if (window_ != nullptr) {
        // SDL 回调借用 this，原生子类回调借用窗口；两者都必须先于各自所有者失效而撤销。
        if (chrome_installed_) {
            SDL_SetWindowHitTest(window_, nullptr, nullptr);
            detail::remove_window_chrome(window_);
        }
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

SDL_Window* SdlWindow::native_handle() const noexcept
{
    return window_;
}

WindowExtent SdlWindow::extent() const noexcept
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window_, &width, &height);
    return {
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
    };
}

WindowExtent SdlWindow::drawable_extent() const noexcept
{
    if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0) {
        return { 0, 0 };
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    return {
        width > 0 ? static_cast<std::uint32_t>(width) : 0U,
        height > 0 ? static_cast<std::uint32_t>(height) : 0U,
    };
}

float SdlWindow::display_scale() const noexcept
{
    // Windows 使用当前窗口 DPI，避免整数窗口尺寸取整后把 175% 算成 1.749…。
    if (const float scale = detail::window_display_scale(window_); scale > 0.0F) {
        return scale;
    }
    const auto logical = extent();
    const auto drawable = drawable_extent();
    if (logical.width != 0 && drawable.width != 0) {
        return static_cast<float>(drawable.width) / static_cast<float>(logical.width);
    }
    return 1.0F;
}

bool SdlWindow::maximized() const noexcept
{
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0;
}

bool SdlWindow::fullscreen() const noexcept
{
    // FULLSCREEN_DESKTOP 包含 FULLSCREEN 位，两种全屏都能被识别。
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
}

void SdlWindow::minimize() noexcept
{
    SDL_MinimizeWindow(window_);
}

void SdlWindow::toggle_maximized() noexcept
{
    if (fullscreen() || (SDL_GetWindowFlags(window_) & SDL_WINDOW_RESIZABLE) == 0) {
        return;
    }
    if (maximized()) {
        SDL_RestoreWindow(window_);
    } else {
        SDL_MaximizeWindow(window_);
    }
}

void SdlWindow::set_titlebar_drag_regions(const std::span<const WindowDragRegion> regions)
{
    auto copy = copy_valid_regions(regions);
    // 先完成复制再交换；分配失败也不会破坏当前区域，回调内部无需分配内存。
    titlebar_drag_regions_.swap(copy);
}

void SdlWindow::set_titlebar_interactive_regions(const std::span<const WindowDragRegion> regions)
{
    auto copy = copy_valid_regions(regions);
    titlebar_interactive_regions_.swap(copy);
}

bool SdlWindow::poll_event(SDL_Event& event) const noexcept
{
    return SDL_PollEvent(&event) != 0;
}

std::optional<core::Event> SdlWindow::poll_engine_event() const noexcept
{
    SDL_Event native_event;
    while (SDL_PollEvent(&native_event) != 0) {
        if (auto event = translate_sdl_event(native_event)) {
            return event;
        }
    }

    return std::nullopt;
}

} // namespace emberframe::platform
