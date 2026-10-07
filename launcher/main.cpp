#include "core/event.h"
#include "platform/sdl_window.h"
#include "profiling/profiler.h"
#include "renderer/frame_context.h"
#include "renderer/gpu_resources.h"
#include "renderer/triangle_pipeline.h"
#include "renderer/vulkan_context.h"
#include "renderer/vulkan_device.h"
#include "renderer/vulkan_swapchain.h"
#include "runtime/frame_clock.h"
#include "runtime/input_state.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <variant>

namespace {

struct Options {
    std::uint64_t benchmark_frames { 0 };
    std::uint64_t warmup_frames { 20 };
    bool resize_smoke_test { false };
    bool reload_smoke_test { false };
    bool resource_smoke_test { false };
    bool clear_only { false };
    std::filesystem::path shader_directory;
};

[[nodiscard]] std::uint64_t parse_count(const std::string_view value)
{
    std::uint64_t count = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
    if (error != std::errc {} || end != value.data() + value.size() || count == 0) {
        throw std::invalid_argument("Frame count must be a positive integer");
    }
    return count;
}

[[nodiscard]] Options parse_options(const int argc, char* argv[])
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--smoke-test") {
            options.benchmark_frames = 12;
            options.warmup_frames = 0;
        } else if (argument == "--resize-smoke-test") {
            options.benchmark_frames = 12;
            options.warmup_frames = 0;
            options.resize_smoke_test = true;
        } else if (argument == "--reload-smoke-test") {
            options.benchmark_frames = 12;
            options.warmup_frames = 0;
            options.reload_smoke_test = true;
        } else if (argument == "--resource-smoke-test") {
            options.benchmark_frames = 12;
            options.warmup_frames = 0;
            options.resource_smoke_test = true;
        } else if (argument == "--clear") {
            options.clear_only = true;
        } else if (argument == "--shader-dir" && i + 1 < argc) {
            options.shader_directory = argv[++i];
        } else if (argument == "--benchmark-frames" && i + 1 < argc) {
            options.benchmark_frames = parse_count(argv[++i]);
        } else if (argument == "--warmup-frames" && i + 1 < argc) {
            options.warmup_frames = parse_count(argv[++i]);
        } else {
            throw std::invalid_argument("Usage: emberframe_launcher [--clear] [--shader-dir PATH] [--smoke-test | --resize-smoke-test | --reload-smoke-test | --resource-smoke-test | --benchmark-frames N [--warmup-frames N]]");
        }
    }
    return options;
}

void print_row(const std::string_view label, const emberframe::profiling::Summary& summary)
{
    std::cout << "[Benchmark] " << label << ": count=" << summary.count
              << " mean=" << summary.mean_ms << " ms"
              << " median=" << summary.median_ms << " ms"
              << " p95=" << summary.p95_ms << " ms\n";
}

[[nodiscard]] const char* present_mode_name(const VkPresentModeKHR mode) noexcept
{
    switch (mode) {
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    default: return "other";
    }
}

} // namespace

int main(const int argc, char* argv[])
{
    using namespace emberframe;

    try {
        const Options options = parse_options(argc, argv);
        const bool benchmark = options.benchmark_frames != 0;

        // 构造顺序就是依赖顺序；离开 main 时反向析构，避免悬空 Vulkan Handle。
        platform::SdlWindow window({
            .title = "EmberFrame Engine",
            .width = 1280,
            .height = 720,
            .resizable = true,
        });
        renderer::VulkanContext context(window.native_handle());
        renderer::VulkanDevice device(context.instance(), context.surface());
        renderer::VulkanSwapchain swapchain(
            window.native_handle(), device.physical_device(), device.device(), context.surface(),
            device.graphics_queue_family(), device.present_queue_family(),
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        renderer::FrameContext frames(
            device.physical_device(), device.device(), device.graphics_queue(),
            device.present_queue(), device.graphics_queue_family(), swapchain.images().size());
        std::unique_ptr<renderer::GpuResources> gpu_resources;
        core::ResourceHandle smoke_buffer;
        core::ResourceHandle smoke_image;
        if (options.resource_smoke_test) {
            gpu_resources = std::make_unique<renderer::GpuResources>(
                context.instance(), device.physical_device(), device.device());
            smoke_buffer = gpu_resources->create_buffer(256,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
            smoke_image = gpu_resources->create_image({ 4, 4, 1 }, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT);
            if (gpu_resources->buffer(smoke_buffer).buffer == VK_NULL_HANDLE
                || gpu_resources->image(smoke_image).view == VK_NULL_HANDLE
                || gpu_resources->image(smoke_buffer).image != VK_NULL_HANDLE
                || gpu_resources->memory_statistics().allocation_count != 2) {
                throw std::runtime_error("GPU resource ownership smoke setup failed");
            }
        }
        const std::filesystem::path shader_directory = options.shader_directory.empty()
            ? std::filesystem::absolute(argv[0]).parent_path() / "shaders"
            : std::filesystem::absolute(options.shader_directory);
        std::unique_ptr<renderer::TrianglePipeline> pipeline;
        if (!options.clear_only) {
            pipeline = std::make_unique<renderer::TrianglePipeline>(
                device.device(), swapchain.image_format(), shader_directory);
        }
        const renderer::TriangleMaterial material {};

        runtime::FrameClock clock;
        runtime::InputState input;
        profiling::CpuProfiler cpu_profiler;
        VkPhysicalDeviceProperties gpu_properties {};
        vkGetPhysicalDeviceProperties(device.physical_device(), &gpu_properties);

        std::cout << "[Launcher] GPU=" << gpu_properties.deviceName
                  << " resolution=" << swapchain.extent().width << 'x' << swapchain.extent().height
                  << " presentMode=" << present_mode_name(swapchain.present_mode())
                  << " validation=" << (context.validation_enabled() ? "on" : "off")
                  << " gpuTimestamps=" << (frames.gpu_timestamps_supported() ? "on" : "unsupported")
                  << " mode=" << (pipeline ? "triangle" : "clear")
                  << '\n';

        bool running = true;
        bool minimized = false;
        bool swapchain_dirty = false;
        std::uint64_t presented_frames = 0;
        std::uint64_t rebuild_count = 0;
        std::uint64_t loop_iterations = 0;
        std::uint64_t minimize_iteration = 0;
        bool warmup_reset = false;
        bool resize_requested = false;
        bool minimize_requested = false;
        bool restore_requested = false;
        bool reload_test_done = false;
        bool resource_test_retired = false;
        std::uint64_t measured_draw_baseline = 0;
        const auto test_started = std::chrono::steady_clock::now();
        while (running) {
            ++loop_iterations;
            const bool measuring = benchmark && presented_frames >= options.warmup_frames;
            profiling::CpuProfiler* active_profiler = measuring ? &cpu_profiler : nullptr;
            {
                profiling::CpuScope frame_scope(active_profiler, "cpu.frame");
                const runtime::FrameTime frame_time = clock.tick();
                (void)frame_time;
                input.begin_frame();

                {
                    profiling::CpuScope event_scope(active_profiler, "cpu.events");
                    while (auto event = window.poll_engine_event()) {
                        input.apply(*event);
                        if (std::holds_alternative<core::QuitRequestedEvent>(*event)) {
                            running = false;
                        } else if (std::holds_alternative<core::WindowResizedEvent>(*event)) {
                            swapchain_dirty = true;
                        } else if (std::holds_alternative<core::WindowMinimizedEvent>(*event)) {
                            minimized = true;
                        } else if (std::holds_alternative<core::WindowRestoredEvent>(*event)) {
                            minimized = false;
                            swapchain_dirty = true;
                        }
                    }
                }

                if (input.was_pressed(core::KeyCode::escape)) {
                    running = false;
                }
                if (pipeline && input.was_pressed(core::KeyCode::r)) {
                    if (pipeline->reload(swapchain.image_format(), frames.last_submitted_serial())) {
                        std::cout << "[Pipeline] Reloaded; old version retires after GPU submissions finish.\n";
                    } else {
                        std::cerr << "[Pipeline] Reload failed; previous version retained: "
                                  << pipeline->last_error() << '\n';
                    }
                }
                if (!running) {
                    break;
                }

                if (swapchain_dirty && !minimized) {
                    profiling::CpuScope resize_scope(active_profiler, "cpu.resize");
                    if (swapchain.recreate()) {
                        frames.rebuild_present_semaphores(swapchain.images().size());
                        if (pipeline && pipeline->color_format() != swapchain.image_format()
                            && !pipeline->reload(swapchain.image_format(), frames.last_submitted_serial())) {
                            throw std::runtime_error("Pipeline format rebuild failed: " + pipeline->last_error());
                        }
                        swapchain_dirty = false;
                        ++rebuild_count;
                    }
                }

                if (!minimized && !swapchain_dirty) {
                    renderer::FrameContext::DrawResult result;
                    {
                        profiling::CpuScope draw_scope(active_profiler, "cpu.acquire_submit_present");
                        result = frames.draw_frame(swapchain.swapchain(), swapchain.images(),
                            swapchain.image_views(), swapchain.extent(), pipeline.get(), material);
                    }
                    if (pipeline) {
                        pipeline->collect(frames.completed_serial());
                    }
                    if (gpu_resources) {
                        gpu_resources->collect(frames.completed_serial());
                    }
                    if (result == renderer::FrameContext::DrawResult::needs_swapchain_recreation) {
                        swapchain_dirty = true;
                    } else {
                        ++presented_frames;
                    }
                }
            }

            if (benchmark && !warmup_reset && presented_frames >= options.warmup_frames
                && options.warmup_frames != 0) {
                // 等待热身阶段 GPU 工作结束，避免它混入固定帧数测量。
                frames.reset_gpu_samples();
                cpu_profiler.clear();
                measured_draw_baseline = frames.draw_call_count();
                warmup_reset = true;
            }
            if (options.reload_smoke_test && !reload_test_done && presented_frames >= 4) {
                if (!pipeline) {
                    throw std::runtime_error("Reload smoke test requires triangle mode");
                }
                const VkPipeline previous = pipeline->pipeline();
                const bool invalid_accepted = pipeline->reload_from(
                    shader_directory / "missing", swapchain.image_format(),
                    frames.last_submitted_serial());
                if (invalid_accepted || pipeline->pipeline() != previous) {
                    throw std::runtime_error("Failed reload did not preserve the previous Pipeline");
                }
                if (!pipeline->reload(swapchain.image_format(), frames.last_submitted_serial())) {
                    throw std::runtime_error("Valid reload failed: " + pipeline->last_error());
                }
                if (pipeline->pending_releases() != 1) {
                    throw std::runtime_error("Old Pipeline was not deferred after reload");
                }
                reload_test_done = true;
                std::cout << "[Pipeline] Failure fallback and successful reload: PASS\n";
            }
            if (gpu_resources && !resource_test_retired && presented_frames >= 4) {
                // 此时旧帧可能仍在 GPU 执行：先使 CPU Handle 失效，等待提交完成再回收。
                const std::uint64_t last_use = frames.last_submitted_serial();
                if (!gpu_resources->retire_buffer(smoke_buffer, last_use)
                    || !gpu_resources->retire_image(smoke_image, last_use)
                    || gpu_resources->retire_buffer(smoke_buffer, last_use)
                    || gpu_resources->buffer(smoke_buffer).buffer != VK_NULL_HANDLE
                    || gpu_resources->image(smoke_image).image != VK_NULL_HANDLE
                    || gpu_resources->pending_releases() != 2) {
                    throw std::runtime_error("GPU resource retirement smoke check failed");
                }
                const core::ResourceHandle replacement = gpu_resources->create_buffer(64,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
                if (replacement.index != smoke_buffer.index
                    || replacement.generation == smoke_buffer.generation
                    || gpu_resources->buffer(smoke_buffer).buffer != VK_NULL_HANDLE
                    || !gpu_resources->retire_buffer(replacement, last_use)
                    || gpu_resources->pending_releases() != 3) {
                    throw std::runtime_error("Reused GPU resource slot revived a stale handle");
                }
                resource_test_retired = true;
            }
            if (options.resize_smoke_test) {
                if (!resize_requested && presented_frames >= 4) {
                    SDL_SetWindowSize(window.native_handle(), 960, 540);
                    resize_requested = true;
                }
                if (!minimize_requested && presented_frames >= 7) {
                    SDL_MinimizeWindow(window.native_handle());
                    minimize_requested = true;
                    minimize_iteration = loop_iterations;
                }
                if (minimize_requested && !restore_requested
                    && loop_iterations >= minimize_iteration + 2) {
                    SDL_RestoreWindow(window.native_handle());
                    restore_requested = true;
                }
                if (std::chrono::steady_clock::now() - test_started > std::chrono::seconds(10)) {
                    throw std::runtime_error("Resize/minimize regression timed out");
                }
            }
            if (benchmark && presented_frames >= options.warmup_frames + options.benchmark_frames) {
                running = false;
            }
            if (minimized) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }

        if (options.resize_smoke_test && (!resize_requested || !minimize_requested
            || !restore_requested || rebuild_count < 1
            || swapchain.extent().width != 960 || swapchain.extent().height != 540)) {
            throw std::runtime_error("Resize/minimize regression did not reach expected state");
        }
        if (options.reload_smoke_test && !reload_test_done) {
            throw std::runtime_error("Reload smoke test did not run");
        }
        if (gpu_resources && (!resource_test_retired || gpu_resources->pending_releases() != 0
            || gpu_resources->memory_statistics().allocation_count != 0)) {
            throw std::runtime_error("GPU resources were not reclaimed after completed submissions");
        }
        if (gpu_resources) {
            const auto stats = gpu_resources->memory_statistics();
            std::cout << "[Resources] VMA Buffer/Image handles and deferred reclaim: PASS"
                      << " liveAllocations=" << stats.allocation_count
                      << " allocatedBytes=" << stats.allocation_bytes
                      << " reservedBlockBytes=" << stats.block_bytes << '\n';
        }

        if (benchmark) {
            frames.collect_gpu_samples();
            if (pipeline) {
                pipeline->collect(frames.completed_serial());
                if (options.reload_smoke_test && pipeline->pending_releases() != 0) {
                    throw std::runtime_error("Old Pipeline was not reclaimed after GPU completion");
                }
            }
            std::cout << std::fixed << std::setprecision(3)
                      << "[Benchmark] measured=" << options.benchmark_frames
                      << " warmup=" << options.warmup_frames
                      << " rebuilds=" << rebuild_count
                      << " drawCalls=" << (frames.draw_call_count() - measured_draw_baseline)
                      << " swapchainImages=" << swapchain.images().size() << '\n';
            for (const auto& [name, values] : cpu_profiler.samples()) {
                print_row(name, profiling::summarize(values));
            }
            if (frames.gpu_timestamps_supported()) {
                print_row(pipeline ? "gpu.triangle_commands" : "gpu.clear_and_transitions",
                    profiling::summarize(frames.gpu_samples_ms()));
            }
            std::cout << "[Benchmark] Note: CPU frame includes Acquire/Present waits; GPU query covers recorded commands, not Present.\n";
            std::cout << "[Benchmark] Memory statistics cover GpuResources VMA allocations only;"
                      << " Swapchain and driver memory are excluded.\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "[Launcher] Fatal error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
