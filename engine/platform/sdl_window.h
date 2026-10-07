#pragma once

#include "core/event.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

union SDL_Event;
struct SDL_Window;

namespace emberframe::platform {

struct WindowConfig {
    std::string title { "EmberFrame Engine" };
    std::uint32_t width { 1700 };
    std::uint32_t height { 900 };
    bool resizable { true };
    bool borderless { false };
};

struct WindowExtent {
    std::uint32_t width;
    std::uint32_t height;
};

// 与 SDL 鼠标、hit-test 坐标一致，单位是逻辑点；只上报标题栏空白，须避开按钮和菜单。
struct WindowDragRegion {
    float x;
    float y;
    float width;
    float height;
};

class SdlWindow {
public:
    explicit SdlWindow(const WindowConfig& config);
    ~SdlWindow();

    // 这个对象独占 SDL_Window，禁止复制和移动可避免两个包装器重复销毁同一窗口。
    SdlWindow(const SdlWindow&) = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;
    SdlWindow(SdlWindow&&) = delete;
    SdlWindow& operator=(SdlWindow&&) = delete;

    // 返回的是借用指针：调用者可以使用，但不能销毁，也不能保存到本对象生命周期之外。
    [[nodiscard]] SDL_Window* native_handle() const noexcept;
    // extent 是逻辑点；渲染目标/交换链须使用 drawable_extent 的物理像素。
    [[nodiscard]] WindowExtent extent() const noexcept;
    // 最小化时返回 {0, 0}，便于上层跳过没有可见 drawable 的渲染。
    [[nodiscard]] WindowExtent drawable_extent() const noexcept;
    [[nodiscard]] float display_scale() const noexcept;
    [[nodiscard]] bool maximized() const noexcept;
    [[nodiscard]] bool fullscreen() const noexcept;
    void minimize() noexcept;
    // 全屏和不可缩放窗口不切换最大化；最大化后仍可从已上报的空白区域拖动还原。
    void toggle_maximized() noexcept;

    // 所有窗口操作（含本方法）均在 SDL 视频线程调用。保存副本，调用者无需维持 span 存活。
    // 传空 span 立即禁止标题栏拖动；无效矩形忽略。可缩放窗口外沿预留 6 个逻辑点。
    void set_titlebar_drag_regions(std::span<const WindowDragRegion> regions);
    // 菜单/按钮的交互矩形，坐标和所有权规则同上；优先于拖动与边缘缩放，防止贴边控件被吞。
    // 同样使用 WindowDragRegion 作为矩形值类型；布局变化时同步更新两组区域。
    void set_titlebar_interactive_regions(std::span<const WindowDragRegion> regions);
    bool poll_event(SDL_Event& event) const noexcept;

    // B3 的引擎事件入口：内部读取并翻译 SDL_Event，调用者不需要包含 SDL 事件定义。
    [[nodiscard]] std::optional<core::Event> poll_engine_event() const noexcept;

private:
    SDL_Window* window_ { nullptr };
    std::vector<WindowDragRegion> titlebar_drag_regions_;
    std::vector<WindowDragRegion> titlebar_interactive_regions_;
    bool chrome_installed_ { false };
};

} // namespace emberframe::platform
