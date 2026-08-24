#pragma once

#include "platform/sdl_window.h"

union SDL_Event;

namespace emberframe::runtime {

class Application {
public:
    explicit Application(platform::WindowConfig window_config = {});
    virtual ~Application() = default;

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run();

protected:
    virtual void on_start(platform::SdlWindow& window) = 0;
    virtual void on_event(SDL_Event& event) = 0;
    virtual void on_frame() = 0;
    virtual void on_stop() noexcept = 0;

private:
    platform::WindowConfig window_config_;
};

} // namespace emberframe::runtime
