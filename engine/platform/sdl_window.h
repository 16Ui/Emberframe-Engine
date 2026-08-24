#pragma once

#include <cstdint>
#include <string>

union SDL_Event;
struct SDL_Window;

namespace emberframe::platform {

struct WindowConfig {
    std::string title { "EmberFrame Engine" };
    std::uint32_t width { 1700 };
    std::uint32_t height { 900 };
    bool resizable { true };
};

struct WindowExtent {
    std::uint32_t width;
    std::uint32_t height;
};

class SdlWindow {
public:
    explicit SdlWindow(const WindowConfig& config);
    ~SdlWindow();

    SdlWindow(const SdlWindow&) = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;
    SdlWindow(SdlWindow&&) = delete;
    SdlWindow& operator=(SdlWindow&&) = delete;

    [[nodiscard]] SDL_Window* native_handle() const noexcept;
    [[nodiscard]] WindowExtent extent() const noexcept;
    bool poll_event(SDL_Event& event) const noexcept;

private:
    SDL_Window* window_ { nullptr };
};

} // namespace emberframe::platform
