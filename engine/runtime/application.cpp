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
    // Runtime 拥有窗口包装器；离开 run() 时窗口会自动析构。
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
        // 已完整启动后发生异常，先清理 Renderer；随后重新抛出，栈展开再销毁窗口。
        // 注意：on_start() 中途失败时 started 仍为 false，这是 B2 后半段要修复的异常安全缺口。
        if (started) {
            on_stop();
        }
        throw;
    }
}

} // namespace emberframe::runtime
