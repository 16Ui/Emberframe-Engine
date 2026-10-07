#include "core/event.h"
#include "platform/sdl_window.h"
#include "runtime/frame_clock.h"
#include "runtime/input_state.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>
#include <variant>

namespace {

void print_key_transition(
    const emberframe::runtime::InputState& input,
    const emberframe::core::KeyCode key,
    const char* name)
{
    if (input.was_pressed(key)) {
        std::cout << name << " pressed\n";
    }
    if (input.was_released(key)) {
        std::cout << name << " released\n";
    }
}

} // namespace

int main()
{
    using emberframe::core::Event;
    using emberframe::core::KeyCode;

    emberframe::platform::SdlWindow window({
        .title = "EmberFrame - B3 Time, Input and Engine Events",
        .width = 1280,
        .height = 720,
        .resizable = true,
    });

    emberframe::runtime::FrameClock clock;
    emberframe::runtime::InputState input;
    bool running = true;
    double next_report_time = 1.0;

    std::cout << "B3 controls: W/A/S/D, mouse movement, Escape to quit.\n";

    while (running) {
        // 上一帧状态必须在读取本帧事件前保存，边沿查询才有正确含义。
        input.begin_frame();

        while (auto event = window.poll_engine_event()) {
            input.apply(*event);

            if (std::holds_alternative<emberframe::core::QuitRequestedEvent>(*event)) {
                running = false;
            }

            if (const auto* resized =
                    std::get_if<emberframe::core::WindowResizedEvent>(&*event)) {
                std::cout << "window resized: "
                          << resized->width << " x " << resized->height << '\n';
            }
        }

        const emberframe::runtime::FrameTime time = clock.tick();

        print_key_transition(input, KeyCode::w, "W");
        print_key_transition(input, KeyCode::a, "A");
        print_key_transition(input, KeyCode::s, "S");
        print_key_transition(input, KeyCode::d, "D");

        if (input.was_pressed(KeyCode::escape)) {
            running = false;
        }

        if (time.elapsed_seconds >= next_report_time) {
            std::cout << std::fixed << std::setprecision(3)
                      << "frame=" << time.frame_index
                      << " dt_ms=" << time.delta_seconds * 1000.0
                      << " held(WASD)="
                      << input.is_down(KeyCode::w)
                      << input.is_down(KeyCode::a)
                      << input.is_down(KeyCode::s)
                      << input.is_down(KeyCode::d)
                      << " mouse_delta=("
                      << input.mouse_delta_x() << ", "
                      << input.mouse_delta_y() << ")\n";
            next_report_time += 1.0;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
