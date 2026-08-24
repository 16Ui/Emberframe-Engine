#include "platform/sdl_window.h"

#include <SDL.h>

#include <stdexcept>

namespace emberframe::platform {

SdlWindow::SdlWindow(const WindowConfig& config)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        throw std::runtime_error(std::string("SDL video initialization failed: ") + SDL_GetError());
    }

    auto flags = static_cast<SDL_WindowFlags>(SDL_WINDOW_VULKAN);
    if (config.resizable) {
        flags = static_cast<SDL_WindowFlags>(flags | SDL_WINDOW_RESIZABLE);
    }

    window_ = SDL_CreateWindow(
        config.title.c_str(),
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        static_cast<int>(config.width),
        static_cast<int>(config.height),
        flags);

    if (window_ == nullptr) {
        const std::string error = SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        throw std::runtime_error("SDL window creation failed: " + error);
    }
}

SdlWindow::~SdlWindow()
{
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

SDL_Window* SdlWindow::native_handle() const noexcept
{
    return window_;
}

WindowExtent SdlWindow::extent() const noexcept
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window_, &width, &height);
    return {
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
    };
}

bool SdlWindow::poll_event(SDL_Event& event) const noexcept
{
    return SDL_PollEvent(&event) != 0;
}

} // namespace emberframe::platform
