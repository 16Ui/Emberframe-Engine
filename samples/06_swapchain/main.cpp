#include "swapchain_probe.h"

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

        // 依赖关系从上到下构造；退出作用域时按相反顺序销毁。
        // Window → Instance/Surface → Physical/Logical Device/Queue → Swapchain/ImageView。
        emberframe::platform::SdlWindow window({
            .title = "EmberFrame - B6 Swapchain and Image Views",
            .width = 1280,
            .height = 720,
            .resizable = true,
        });
        emberframe::samples::VulkanBootstrapProbe vulkan(window.native_handle());
        emberframe::samples::DeviceQueueProbe device(
            vulkan.instance(),
            vulkan.surface());
        emberframe::samples::SwapchainProbe swapchain(
            window.native_handle(),
            device.physical_device(),
            device.device(),
            vulkan.surface(),
            device.graphics_queue_family(),
            device.present_queue_family());
        emberframe::runtime::InputState input;

        std::cout << "[B6] Initialization complete. Resize the window to rebuild the Swapchain.\n"
                  << "[B6] This chapter creates presentation images but does not draw/present yet.\n";

        if (exit_after_init) {
            std::cout << "[B6] Smoke-test mode: leaving scope to verify cleanup order.\n";
            return 0;
        }

        bool running = true;
        bool minimized = false;
        bool swapchain_dirty = false;

        while (running) {
            input.begin_frame();

            while (auto event = window.poll_engine_event()) {
                input.apply(*event);

                if (std::holds_alternative<emberframe::core::QuitRequestedEvent>(*event)) {
                    running = false;
                } else if (std::holds_alternative<emberframe::core::WindowResizedEvent>(*event)) {
                    // Resize 事件只标记状态；离开事件循环后统一重建，避免同一帧重复创建。
                    swapchain_dirty = true;
                } else if (std::holds_alternative<emberframe::core::WindowMinimizedEvent>(*event)) {
                    minimized = true;
                } else if (std::holds_alternative<emberframe::core::WindowRestoredEvent>(*event)) {
                    minimized = false;
                    swapchain_dirty = true;
                }
            }

            if (input.was_pressed(KeyCode::escape)) {
                running = false;
            }

            if (swapchain_dirty && !minimized && swapchain.recreate()) {
                swapchain_dirty = false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const std::exception& exception) {
        std::cerr << "[B6] Fatal error: " << exception.what() << '\n';
        return 1;
    }

    return 0;
}
