#pragma once

#include "core/event.h"

#include <optional>

union SDL_Event;

namespace emberframe::platform {

// 将 SDL 的平台事件翻译为不依赖 SDL 的引擎事件。
// 未纳入 B3 范围的事件返回 std::nullopt，由事件泵继续读取下一项。
[[nodiscard]] std::optional<core::Event> translate_sdl_event(
    const SDL_Event& event) noexcept;

} // namespace emberframe::platform
