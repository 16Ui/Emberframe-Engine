#include "command_frame_probe.h"

#include "device_queue_probe.h"
#include "swapchain_probe.h"
#include "vulkan_bootstrap_probe.h"

#include "core/event.h"
#include "platform/sdl_window.h"
#include "runtime/input_state.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>
#include <variant>

int main(const int argc, char* argv[])
{
    using emberframe::core::KeyCode;

    try {
        const bool smoke_test = argc > 1
            && std::string_view(argv[1]) == "--smoke-test";

        // 生命周期按依赖顺序构造，离开作用域时会反向销毁：
        // Command Pools → Swapchain → Device → Surface/Instance → Window。
        emberframe::platform::SdlWindow window({
            .title = "EmberFrame - B7 Command Buffers and Frame Slots",
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
        emberframe::samples::CommandFrameProbe command_frames(
            device.device(),
            device.graphics_queue(),
            device.graphics_queue_family());
        emberframe::runtime::InputState input;

        std::cout << "[B7] Command submission loop ready.\n"
                  << "[B7] B7 submits empty command buffers and waits for Queue idle.\n"
                  << "[B7] B8 will add per-frame Fence/Semaphore and Swapchain Acquire/Present.\n";

        bool running = true;
        bool minimized = false;
        bool swapchain_dirty = false;
        std::uint64_t frame_number = 0;

        while (running) {
            input.begin_frame();

            while (auto event = window.poll_engine_event()) {
                input.apply(*event);

                if (std::holds_alternative<emberframe::core::QuitRequestedEvent>(*event)) {
                    running = false;
                } else if (std::holds_alternative<emberframe::core::WindowResizedEvent>(*event)) {
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

            if (!minimized) {
                command_frames.record_and_submit(frame_number);
                ++frame_number;
            }

            if (smoke_test && frame_number >= 6) {
                std::cout << "[B7] Smoke test recorded and submitted six frames.\n";
                running = false;
            }

            // 空命令提交会非常快；短暂停顿避免教学 Sample 无意义地占满 CPU 并刷屏。
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    } catch (const std::exception& exception) {
        std::cerr << "[B7] Fatal error: " << exception.what() << '\n';
        return 1;
    }

    return 0;
}

