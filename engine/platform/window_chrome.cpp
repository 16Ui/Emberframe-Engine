#include "platform/window_chrome.h"
#include "platform/sdl_window.h"

#include <SDL.h>

#include <bit>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <SDL_syswm.h>
#include <commctrl.h>
#endif

namespace emberframe::platform::detail {
namespace {

bool contains_point(const WindowDragRegion& region, const SDL_Point& point) noexcept
{
    // 半开区间避免相邻控件边界被吞；转 double 相加避免有限 float 的矩形边界溢出。
    return static_cast<double>(point.x) >= region.x && static_cast<double>(point.y) >= region.y
        && point.x < static_cast<double>(region.x) + region.width
        && point.y < static_cast<double>(region.y) + region.height;
}

#if defined(_WIN32)
constexpr UINT_PTR chrome_subclass_id = 1;

template<typename Function>
Function user32_function(const HMODULE module, const char* name) noexcept
{
    // Windows ABI 的函数指针表示相同；按位转换保留 GetProcAddress 地址，避免签名转换警告。
    return std::bit_cast<Function>(GetProcAddress(module, name));
}

HWND window_hwnd(SDL_Window* window) noexcept
{
    SDL_SysWMinfo info {};
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) == SDL_TRUE && info.subsystem == SDL_SYSWM_WINDOWS) {
        return info.info.win.window;
    }
    return nullptr;
}

LRESULT CALLBACK chrome_subclass(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR subclass_id, DWORD_PTR user_data)
{
    if (message == WM_NCDESTROY) {
        // 防御原生窗口提前销毁的情形；不保留任何可在 HWND 销毁后调用的窗口借用。
        RemoveWindowSubclass(hwnd, chrome_subclass, subclass_id);
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }

    // 先让 SDL 处理尺寸下限、事件与其它子类逻辑，再覆盖其无边框最大化的主屏假设。
    const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
    if (message == WM_NCCALCSIZE && wparam != 0 && lparam != 0 && IsZoomed(hwnd)) {
        auto* window = reinterpret_cast<SDL_Window*>(user_data);
        const Uint32 flags = SDL_GetWindowFlags(window);
        if ((flags & SDL_WINDOW_BORDERLESS) != 0 && (flags & SDL_WINDOW_FULLSCREEN) == 0) {
            MONITORINFO monitor {};
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                // Windows 最大化会把不可见调整边框放在工作区外。SDL 无边框默认把它也当作
                // client，造成内容超出屏幕约 7 个逻辑点；显式让可绘制客户区恰好等于 rcWork。
                reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0] = monitor.rcWork;
                return 0;
            }
        }
    }
    if (message == WM_GETMINMAXINFO && lparam != 0) {
        auto* window = reinterpret_cast<SDL_Window*>(user_data);
        const Uint32 flags = SDL_GetWindowFlags(window);
        if ((flags & SDL_WINDOW_BORDERLESS) != 0 && (flags & SDL_WINDOW_RESIZABLE) != 0
            && (flags & SDL_WINDOW_FULLSCREEN) == 0) {
            MONITORINFO monitor {};
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
                // Win32 消息/rcWork 用物理像素，并以当前屏幕左上角为最大化位置原点。
                // 这里不能混入 SDL 逻辑点，也不能用主屏尺寸，否则副屏和任务栏会被覆盖。
                limits->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
                limits->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
                limits->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
                limits->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
                if (limits->ptMaxTrackSize.x < limits->ptMaxSize.x) {
                    limits->ptMaxTrackSize.x = limits->ptMaxSize.x;
                }
                if (limits->ptMaxTrackSize.y < limits->ptMaxSize.y) {
                    limits->ptMaxTrackSize.y = limits->ptMaxSize.y;
                }
            }
        }
    }
    return result;
}

void enable_per_monitor_v2()
{
    // 动态查询避免把新版 DPI API 变成旧 Windows 的加载期依赖；SDL 为旧系统提供降级。
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
        return;
    }
    using SetProcessContext = BOOL(WINAPI*)(HANDLE);
    using GetThreadContext = HANDLE(WINAPI*)();
    using EqualContexts = BOOL(WINAPI*)(HANDLE, HANDLE);
    using SetThreadContext = HANDLE(WINAPI*)(HANDLE);
    const auto set_process = user32_function<SetProcessContext>(user32, "SetProcessDpiAwarenessContext");
    if (set_process == nullptr) {
        return;
    }
    const HANDLE per_monitor_v2 = reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4));
    const bool process_enabled = set_process(per_monitor_v2) != FALSE;
    DWORD error = process_enabled ? ERROR_SUCCESS : GetLastError();
    if (!process_enabled && error == ERROR_INVALID_PARAMETER) {
        return; // 不支持 v2 的旧系统交由 SDL 选择可用的 per-monitor/system 模式。
    }
    if (process_enabled || error == ERROR_ACCESS_DENIED) {
        // 清单或前置库可能已经固定进程 DPI；当前窗口线程仍可显式使用 v2，避免静默模糊。
        const auto get_thread = user32_function<GetThreadContext>(user32, "GetThreadDpiAwarenessContext");
        const auto equal = user32_function<EqualContexts>(user32, "AreDpiAwarenessContextsEqual");
        if (get_thread != nullptr && equal != nullptr && equal(get_thread(), per_monitor_v2)) {
            return;
        }
        const auto set_thread = user32_function<SetThreadContext>(user32, "SetThreadDpiAwarenessContext");
        if (set_thread != nullptr && set_thread(per_monitor_v2) != nullptr) {
            return;
        }
        if (set_thread != nullptr) {
            error = GetLastError();
        }
    }
    throw std::runtime_error("Could not enable Windows per-monitor v2 DPI awareness (Win32 error "
        + std::to_string(error) + ")");
}
#endif

} // namespace

void prepare_window_dpi(const bool borderless)
{
#if defined(_WIN32)
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0
        && SDL_GetHintBoolean(SDL_HINT_WINDOWS_DPI_SCALING, SDL_FALSE) != SDL_TRUE) {
        throw std::runtime_error("SDL video was already initialized without DPI scaling; create SdlWindow before video initialization");
    }
    // 这些是本进程的 DPI/SDL 策略，不修改 Windows 显示设置。OVERRIDE 防止环境变量关闭缩放。
    if (SDL_SetHintWithPriority(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2", SDL_HINT_OVERRIDE) != SDL_TRUE
        || SDL_SetHintWithPriority(SDL_HINT_WINDOWS_DPI_SCALING, "1", SDL_HINT_OVERRIDE) != SDL_TRUE) {
        throw std::runtime_error("Could not configure SDL Windows DPI scaling");
    }
    enable_per_monitor_v2();
    if (borderless) {
        // 保留窗口管理器的最小化/最大化/边缘缩放语义；SDL 的 NCCALCSIZE 隐去原生栏和边框。
        if (SDL_SetHintWithPriority("SDL_BORDERLESS_WINDOWED_STYLE", "1", SDL_HINT_OVERRIDE) != SDL_TRUE
            || SDL_SetHintWithPriority("SDL_BORDERLESS_RESIZABLE_STYLE", "1", SDL_HINT_OVERRIDE) != SDL_TRUE) {
            throw std::runtime_error("Could not configure SDL borderless window styles");
        }
    }
#else
    (void)borderless;
#endif
}

int install_window_chrome(SDL_Window* window) noexcept
{
#if defined(_WIN32)
    const HWND hwnd = window_hwnd(window);
    if (hwnd == nullptr) {
        return SDL_SetError("Could not obtain SDL window HWND");
    }
    // SetWindowSubclass 链接 SDL 的原有窗口过程，不覆盖它；数据仅借用受 SdlWindow 管理的窗口。
    if (!SetWindowSubclass(hwnd, chrome_subclass, chrome_subclass_id, reinterpret_cast<DWORD_PTR>(window))) {
        return SDL_SetError("SetWindowSubclass failed (Win32 error %lu)", GetLastError());
    }
#else
    (void)window;
#endif
    return 0;
}

void remove_window_chrome(SDL_Window* window) noexcept
{
#if defined(_WIN32)
    if (const HWND hwnd = window_hwnd(window); hwnd != nullptr) {
        RemoveWindowSubclass(hwnd, chrome_subclass, chrome_subclass_id);
    }
#else
    (void)window;
#endif
}

float window_display_scale(SDL_Window* window) noexcept
{
#if defined(_WIN32)
    const HWND hwnd = window_hwnd(window);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using GetWindowDpi = UINT(WINAPI*)(HWND);
    const auto get_dpi = user32 != nullptr
        ? user32_function<GetWindowDpi>(user32, "GetDpiForWindow") : nullptr;
    if (hwnd != nullptr && get_dpi != nullptr) {
        return static_cast<float>(get_dpi(hwnd)) / 96.0F;
    }
#else
    (void)window;
#endif
    return 0.0F;
}

SDL_HitTestResult hit_test_window_chrome(const WindowExtent extent, const SDL_Point& point,
    const std::uint32_t flags, const std::span<const WindowDragRegion> regions,
    const std::span<const WindowDragRegion> interactive_regions) noexcept
{
    if ((flags & SDL_WINDOW_BORDERLESS) == 0
        || (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED)) != 0
        || point.x < 0 || point.y < 0
        || static_cast<std::uint32_t>(point.x) >= extent.width
        || static_cast<std::uint32_t>(point.y) >= extent.height) {
        return SDL_HITTEST_NORMAL;
    }

    for (const auto& region : interactive_regions) {
        if (contains_point(region, point)) {
            return SDL_HITTEST_NORMAL;
        }
    }

    if ((flags & SDL_WINDOW_RESIZABLE) != 0 && (flags & SDL_WINDOW_MAXIMIZED) == 0) {
        constexpr double edge = 6.0;
        const bool left = point.x < edge;
        const bool right = point.x >= static_cast<double>(extent.width) - edge;
        const bool top = point.y < edge;
        const bool bottom = point.y >= static_cast<double>(extent.height) - edge;
        // 控件已优先排除；角优先于边，边优先于空白拖动区。最大化/全屏不提供边缘缩放。
        if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (top) return SDL_HITTEST_RESIZE_TOP;
        if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
        if (left) return SDL_HITTEST_RESIZE_LEFT;
        if (right) return SDL_HITTEST_RESIZE_RIGHT;
    }

    for (const auto& region : regions) {
        if (contains_point(region, point)) {
            return SDL_HITTEST_DRAGGABLE;
        }
    }
    return SDL_HITTEST_NORMAL;
}

} // namespace emberframe::platform::detail
