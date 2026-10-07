#pragma once

#include <chrono>
#include <cstdint>

namespace emberframe::runtime {

struct FrameTime {
    double delta_seconds { 0.0 };
    double elapsed_seconds { 0.0 };
    std::uint64_t frame_index { 0 };
};

class FrameClock {
public:
    FrameClock() noexcept;

    void reset() noexcept;
    [[nodiscard]] FrameTime tick() noexcept;

private:
    // steady_clock 单调递增，不受用户修改系统时间影响，适合测量帧间隔。
    using Clock = std::chrono::steady_clock;

    Clock::time_point start_;
    Clock::time_point previous_;
    std::uint64_t frame_index_ { 0 };
};

} // namespace emberframe::runtime
