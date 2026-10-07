#include "runtime/frame_clock.h"

namespace emberframe::runtime {

FrameClock::FrameClock() noexcept
{
    reset();
}

void FrameClock::reset() noexcept
{
    start_ = Clock::now();
    previous_ = start_;
    frame_index_ = 0;
}

FrameTime FrameClock::tick() noexcept
{
    const Clock::time_point now = Clock::now();
    const std::chrono::duration<double> delta = now - previous_;
    const std::chrono::duration<double> elapsed = now - start_;

    previous_ = now;

    const FrameTime result {
        delta.count(),
        elapsed.count(),
        frame_index_,
    };
    ++frame_index_;
    return result;
}

} // namespace emberframe::runtime
