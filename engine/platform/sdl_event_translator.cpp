#include "platform/sdl_event_translator.h"

#include <SDL_events.h>
#include <SDL_keycode.h>

namespace emberframe::platform {
namespace {

core::KeyCode translate_key(const SDL_Keycode key) noexcept
{
    switch (key) {
    case SDLK_w:
        return core::KeyCode::w;
    case SDLK_a:
        return core::KeyCode::a;
    case SDLK_s:
        return core::KeyCode::s;
    case SDLK_d:
        return core::KeyCode::d;
    case SDLK_r:
        return core::KeyCode::r;
    case SDLK_ESCAPE:
        return core::KeyCode::escape;
    default:
        return core::KeyCode::unknown;
    }
}

} // namespace

std::optional<core::Event> translate_sdl_event(const SDL_Event& event) noexcept
{
    if (event.type == SDL_QUIT) {
        return core::QuitRequestedEvent {};
    }

    if (event.type == SDL_WINDOWEVENT) {
        switch (event.window.event) {
        case SDL_WINDOWEVENT_CLOSE:
            return core::QuitRequestedEvent {};
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_SIZE_CHANGED:
            return core::WindowResizedEvent {
                static_cast<std::uint32_t>(event.window.data1),
                static_cast<std::uint32_t>(event.window.data2),
            };
        case SDL_WINDOWEVENT_MINIMIZED:
            return core::WindowMinimizedEvent {};
        case SDL_WINDOWEVENT_RESTORED:
            return core::WindowRestoredEvent {};
        default:
            return std::nullopt;
        }
    }

    if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
        const core::KeyCode key = translate_key(event.key.keysym.sym);
        if (key == core::KeyCode::unknown) {
            return std::nullopt;
        }

        return core::KeyEvent {
            key,
            event.type == SDL_KEYDOWN,
            event.key.repeat != 0,
        };
    }

    if (event.type == SDL_MOUSEMOTION) {
        return core::MouseMovedEvent {
            static_cast<float>(event.motion.x),
            static_cast<float>(event.motion.y),
            static_cast<float>(event.motion.xrel),
            static_cast<float>(event.motion.yrel),
        };
    }

    return std::nullopt;
}

} // namespace emberframe::platform
