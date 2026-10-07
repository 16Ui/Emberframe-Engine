#include "buffer_mesh_probe.h"

#include "device_queue_probe.h"
#include "swapchain_probe.h"
#include "vulkan_bootstrap_probe.h"

#include "core/event.h"
#include "platform/sdl_window.h"
#include "runtime/input_state.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
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

        emberframe::platform::SdlWindow window({
            .title = "EmberFrame - B10 VMA Staging Vertex/Index Buffers",
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
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

        const std::filesystem::path executable_path =
            std::filesystem::absolute(argv[0]).lexically_normal();
        const std::filesystem::path shader_directory =
            executable_path.parent_path() / "shaders";

        emberframe::samples::BufferMeshProbe renderer(
            vulkan.instance(),
            device.physical_device(),
            device.device(),
            device.graphics_queue(),
            device.present_queue(),
            device.graphics_queue_family(),
            swapchain.images().size(),
            swapchain.image_format(),
            shader_directory);
        emberframe::runtime::InputState input;

        std::cout << "[B10] CPU mesh -> Staging Buffer -> Device Vertex/Index Buffers ready.\n"
                  << "[B10] Indexed Draw renders two triangles from four shared vertices.\n";

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
                renderer.rebuild_swapchain_resources(
                    swapchain.images().size(),
                    swapchain.image_format());
                swapchain_dirty = false;
            }

            if (!minimized && !swapchain_dirty) {
                const auto result = renderer.draw_frame(
                    swapchain.swapchain(),
                    swapchain.images(),
                    swapchain.image_views(),
                    swapchain.extent());
                ++rendered_frames;

                if (result == emberframe::samples::BufferMeshProbe::DrawResult::needs_swapchain_recreation) {
                    swapchain_dirty = true;
                }
            }

            if (smoke_test && rendered_frames >= 12) {
                std::cout << "[B10] Smoke test rendered twelve indexed mesh frames.\n";
                running = false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    } catch (const std::exception& exception) {
        std::cerr << "[B10] Fatal error: " << exception.what() << '\n';
        return 1;
    }

    return 0;
}

