#include "device_queue_probe.h"
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

        // 构造：Window → B4 Instance/Surface → B5 Device/Queue。
        // 析构自动反向执行，保证 Device 先于 Surface 和 Instance 销毁。
        emberframe::platform::SdlWindow window({
            .title = "EmberFrame - B5 Device and Queues",
            .width = 1280,
            .height = 720,
            .resizable = true,
        });
        emberframe::samples::VulkanBootstrapProbe vulkan(window.native_handle());
        emberframe::samples::DeviceQueueProbe device(
            vulkan.instance(),
            vulkan.surface());
        emberframe::runtime::InputState input;

        std::cout << "[B5] Initialization complete. Press Escape or close the window.\n";

        if (exit_after_init) {
            std::cout << "[B5] Smoke-test mode: leaving scope to verify cleanup order.\n";
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

            // B5 只有 Device 和 Queue，还没有 Swapchain 或 Command Buffer。
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const std::exception& exception) {
        std::cerr << "[B5] Fatal error: " << exception.what() << '\n';
        return 1;
    }

    return 0;
}
