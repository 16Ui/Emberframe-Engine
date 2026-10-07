#include "runtime/input_state.h"

#include <variant>

namespace emberframe::runtime {

void InputState::begin_frame() noexcept
{
    previous_ = current_;
    mouse_delta_x_ = 0.0F;
    mouse_delta_y_ = 0.0F;
}

void InputState::apply(const core::Event& event) noexcept
{
    if (const auto* key_event = std::get_if<core::KeyEvent>(&event)) {
        if (key_event->key != core::KeyCode::unknown) {
            current_[index(key_event->key)] = key_event->pressed;
        }
        return;
    }

    if (const auto* mouse_event = std::get_if<core::MouseMovedEvent>(&event)) {
        // 一帧内可能收到多个鼠标事件，因此需要累计相对位移。
        mouse_delta_x_ += mouse_event->delta_x;
        mouse_delta_y_ += mouse_event->delta_y;
    }
}

bool InputState::is_down(const core::KeyCode key) const noexcept
{
    return key != core::KeyCode::unknown && current_[index(key)];
}

bool InputState::was_pressed(const core::KeyCode key) const noexcept
{
    return key != core::KeyCode::unknown
        && current_[index(key)]
        && !previous_[index(key)];
}

bool InputState::was_released(const core::KeyCode key) const noexcept
{
    return key != core::KeyCode::unknown
        && !current_[index(key)]
        && previous_[index(key)];
}

float InputState::mouse_delta_x() const noexcept
{
    return mouse_delta_x_;
}

float InputState::mouse_delta_y() const noexcept
{
    return mouse_delta_y_;
}

std::size_t InputState::index(const core::KeyCode key) noexcept
{
    return static_cast<std::size_t>(key);
}

} // namespace emberframe::runtime
