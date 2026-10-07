#pragma once

#include <SDL_video.h>

#include <cstdint>
#include <span>

namespace emberframe::platform {

struct WindowExtent;
struct WindowDragRegion;

namespace detail {

// 平台内部辅助；不拥有 SDL_Window。安装/移除必须发生在创建窗口的 SDL 视频线程。
void prepare_window_dpi(bool borderless);
int install_window_chrome(SDL_Window* window) noexcept;
void remove_window_chrome(SDL_Window* window) noexcept;
float window_display_scale(SDL_Window* window) noexcept;

// 纯逻辑分类，参数全部为 SDL 逻辑点；不查鼠标、不分配、不产生窗口操作。
SDL_HitTestResult hit_test_window_chrome(WindowExtent extent, const SDL_Point& point,
    std::uint32_t flags, std::span<const WindowDragRegion> regions,
    std::span<const WindowDragRegion> interactive_regions = {}) noexcept;

} // namespace detail
} // namespace emberframe::platform
