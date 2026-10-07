#include "sync_present_probe.h"

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

        // 构造顺序体现依赖关系，退出作用域后按完全相反的顺序安全销毁。
        emberframe::platform::SdlWindow window({
            .title = "EmberFrame - B8 Synchronized Acquire, Clear and Present",
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
            device.present_queue_family(),
            // vkCmdClearColorImage 把 Swapchain Image 当作 Transfer Destination 写入。
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        emberframe::samples::SyncPresentProbe frame_loop(
            device.device(),
            device.graphics_queue(),
            device.present_queue(),
            device.graphics_queue_family(),
            swapchain.images().size());
        emberframe::runtime::InputState input;

        std::cout << "[B8] Full frame loop ready.\n"
                  << "[B8] The window color now comes from GPU clear commands.\n"
                  << "[B8] B9 will replace the clear-only body with Dynamic Rendering and a Draw Call.\n";

        bool running = true;
        bool minimized = false;
        bool swapchain_dirty = false;
        std::uint64_t rendered_frames = 0;

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
                frame_loop.rebuild_present_semaphores(swapchain.images().size());
                swapchain_dirty = false;
            }

            if (!minimized && !swapchain_dirty) {
                const auto result = frame_loop.draw_frame(
                    swapchain.swapchain(),
                    swapchain.images());
                ++rendered_frames;

                if (result == emberframe::samples::SyncPresentProbe::DrawResult::needs_swapchain_recreation) {
                    swapchain_dirty = true;
                }
            }

            if (smoke_test && rendered_frames >= 12) {
                std::cout << "[B8] Smoke test acquired, cleared and presented twelve frames.\n";
                running = false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    } catch (const std::exception& exception) {
        std::cerr << "[B8] Fatal error: " << exception.what() << '\n';
        return 1;
    }

    return 0;
}
