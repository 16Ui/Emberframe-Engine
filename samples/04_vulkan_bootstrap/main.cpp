#include "vulkan_bootstrap_probe.h"

#include "core/event.h"
#include "platform/sdl_window.h"
#include "runtime/input_state.h"

#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>
#include <variant>

int main(const int argc, char* argv[])
{
    using emberframe::core::KeyCode;

    try {
        const bool exit_after_init = argc > 1
            && std::string_view(argv[1]) == "--exit-after-init";

        // 声明顺序决定逆序析构顺序：Vulkan 对象会先销毁，SDL Window 最后销毁。
        emberframe::platform::SdlWindow window({
            .title = "EmberFrame - B4 Vulkan Instance and Surface",
            .width = 1280,
            .height = 720,
            .resizable = true,
        });
        emberframe::samples::VulkanBootstrapProbe vulkan(window.native_handle());
        emberframe::runtime::InputState input;

        std::cout << "[B4] Initialization complete. Press Escape or close the window.\n";

        if (exit_after_init) {
            std::cout << "[B4] Smoke-test mode: leaving scope to verify cleanup order.\n";
            return 0;
        }

        bool running = true;
        while (running) {
            input.begin_frame();

            while (auto event = window.poll_engine_event()) {
                input.apply(*event);
                if (std::holds_alternative<emberframe::core::QuitRequestedEvent>(*event)) {
                    running = false;
                }
            }

            if (input.was_pressed(KeyCode::escape)) {
                running = false;
            }

            // B4 还没有 Swapchain 和渲染循环，短暂让出 CPU 即可。
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const std::exception& exception) {
        std::cerr << "[B4] Fatal error: " << exception.what() << '\n';
        return 1;
    }

    return 0;
}
