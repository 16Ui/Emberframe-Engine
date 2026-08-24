#include "runtime/application.h"

#include <SDL_events.h>

#include <utility>

namespace emberframe::runtime {

Application::Application(platform::WindowConfig window_config)
    : window_config_(std::move(window_config))
{
}

int Application::run()
{
    platform::SdlWindow window(window_config_);
    bool started = false;

    try {
        on_start(window);
        started = true;

        bool quit_requested = false;
        while (!quit_requested) {
            SDL_Event event;
            while (window.poll_event(event)) {
                if (event.type == SDL_QUIT) {
                    quit_requested = true;
                }
                on_event(event);
            }

            if (!quit_requested) {
                on_frame();
            }
        }

        on_stop();
        return 0;
    } catch (...) {
        if (started) {
            on_stop();
        }
        throw;
    }
}

} // namespace emberframe::runtime
