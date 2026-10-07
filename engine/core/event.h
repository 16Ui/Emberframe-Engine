#pragma once

#include <cstdint>
#include <variant>

namespace emberframe::core {

// 引擎内部使用的按键编号。这里故意不暴露 SDLK_*，避免上层依赖 SDL。
enum class KeyCode : std::uint8_t {
    unknown = 0,
    w,
    a,
    s,
    d,
    r,
    escape,
    count,
};

struct QuitRequestedEvent { };

struct WindowResizedEvent {
    std::uint32_t width { 0 };
    std::uint32_t height { 0 };
};

struct WindowMinimizedEvent { };
struct WindowRestoredEvent { };

struct KeyEvent {
    KeyCode key { KeyCode::unknown };
    bool pressed { false };
    bool repeated { false };
};

struct MouseMovedEvent {
    float x { 0.0F };
    float y { 0.0F };
    float delta_x { 0.0F };
    float delta_y { 0.0F };
};

// Event 是一组事件值的联合；任意时刻只保存其中一种具体事件。
using Event = std::variant<
    QuitRequestedEvent,
    WindowResizedEvent,
    WindowMinimizedEvent,
    WindowRestoredEvent,
    KeyEvent,
    MouseMovedEvent>;

} // namespace emberframe::core
