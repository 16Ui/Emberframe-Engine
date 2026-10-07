#pragma once

#include "core/event.h"

#include <array>
#include <cstddef>

namespace emberframe::runtime {

class InputState {
public:
    // 必须在每帧处理新事件前调用：保存上一帧状态，并清空逐帧鼠标增量。
    void begin_frame() noexcept;
    void apply(const core::Event& event) noexcept;

    [[nodiscard]] bool is_down(core::KeyCode key) const noexcept;
    [[nodiscard]] bool was_pressed(core::KeyCode key) const noexcept;
    [[nodiscard]] bool was_released(core::KeyCode key) const noexcept;

    [[nodiscard]] float mouse_delta_x() const noexcept;
    [[nodiscard]] float mouse_delta_y() const noexcept;

private:
    static constexpr std::size_t key_count =
        static_cast<std::size_t>(core::KeyCode::count);

    [[nodiscard]] static std::size_t index(core::KeyCode key) noexcept;

    std::array<bool, key_count> current_ {};
    std::array<bool, key_count> previous_ {};
    float mouse_delta_x_ { 0.0F };
    float mouse_delta_y_ { 0.0F };
};

} // namespace emberframe::runtime
