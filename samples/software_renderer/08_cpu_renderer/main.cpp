#include "cpu_renderer.h"
#include "obj_loader.h"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using emberframe::software::CpuRenderer;
using emberframe::software::Mat4;
using emberframe::software::MeshTriangle;
using emberframe::software::RenderSettings;
using emberframe::software::RenderStats;

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 640;

struct FrameResult
{
    RenderStats stats;
    double milliseconds;
};

std::vector<MeshTriangle> makeFrameScene(
    const std::vector<MeshTriangle>& sourceMesh,
    float modelAngle)
{
    const Mat4 model =
        emberframe::software::rotationY(modelAngle) *
        emberframe::software::rotationX(-0.12F);
    std::vector<MeshTriangle> scene = emberframe::software::transformMesh(sourceMesh, model);
    auto ground = emberframe::software::makeGroundPlane();
    scene.insert(scene.end(), ground.begin(), ground.end());
    return scene;
}

FrameResult renderFrame(
    CpuRenderer& renderer,
    const std::vector<MeshTriangle>& sourceMesh,
    float modelAngle,
    float lightAngle)
{
    RenderSettings settings;
    settings.lightPosition = {
        std::cos(lightAngle) * 4.5F,
        5.0F,
        std::sin(lightAngle) * 4.5F};
    const auto scene = makeFrameScene(sourceMesh, modelAngle);

    const auto start = std::chrono::steady_clock::now();
    const RenderStats stats = renderer.render(scene, settings);
    const auto end = std::chrono::steady_clock::now();
    const double milliseconds =
        std::chrono::duration<double, std::milli>(end - start).count();
    return {stats, milliseconds};
}

int runHeadless(
    CpuRenderer& renderer,
    const std::vector<MeshTriangle>& sourceMesh,
    const std::filesystem::path& executablePath)
{
    const FrameResult result = renderFrame(renderer, sourceMesh, 0.55F, 2.45F);
    const auto outputDirectory = std::filesystem::absolute(executablePath).parent_path();
    const auto colorPath = outputDirectory / "cpu_renderer.ppm";
    const auto shadowPath = outputDirectory / "cpu_renderer_shadow.ppm";
    if (!renderer.saveColor(colorPath) || !renderer.saveShadowPreview(shadowPath)) {
        std::cerr << "Failed to save A8 headless outputs.\n";
        return 1;
    }

    std::cout << "CPU renderer image saved to: " << colorPath << '\n';
    std::cout << "CPU renderer shadow map saved to: " << shadowPath << '\n';
    std::cout << "triangles=" << result.stats.submittedTriangles
              << ", visible=" << result.stats.visibleTriangles
              << ", fragments=" << result.stats.shadedFragments
              << ", shadowTests=" << result.stats.shadowTests
              << ", renderMs=" << result.milliseconds << '\n';
    return 0;
}

int runViewer(
    CpuRenderer& cpuRenderer,
    const std::vector<MeshTriangle>& sourceMesh,
    const std::filesystem::path& executablePath)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        throw std::runtime_error(std::string("SDL initialization failed: ") + SDL_GetError());
    }

    SDL_Window* window = SDL_CreateWindow(
        "EmberFrame CPU Renderer",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        kWidth,
        kHeight,
        SDL_WINDOW_SHOWN);
    if (window == nullptr) {
        const std::string error = SDL_GetError();
        SDL_Quit();
        throw std::runtime_error("SDL window creation failed: " + error);
    }

    SDL_Renderer* displayRenderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (displayRenderer == nullptr) {
        displayRenderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (displayRenderer == nullptr) {
        const std::string error = SDL_GetError();
        SDL_DestroyWindow(window);
        SDL_Quit();
        throw std::runtime_error("SDL renderer creation failed: " + error);
    }

    SDL_Texture* displayTexture = SDL_CreateTexture(
        displayRenderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        kWidth,
        kHeight);
    if (displayTexture == nullptr) {
        const std::string error = SDL_GetError();
        SDL_DestroyRenderer(displayRenderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        throw std::runtime_error("SDL texture creation failed: " + error);
    }

    std::cout << "Controls: A/D or Left/Right rotate model, Q/E move light, R reset, S save, Esc quit.\n";
    bool running = true;
    float modelAngle = 0.45F;
    float lightAngle = 2.45F;
    auto previousTime = std::chrono::steady_clock::now();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                } else if (event.key.keysym.sym == SDLK_r) {
                    modelAngle = 0.45F;
                    lightAngle = 2.45F;
                } else if (event.key.keysym.sym == SDLK_s) {
                    const auto screenshot =
                        std::filesystem::absolute(executablePath).parent_path() /
                        "cpu_renderer_screenshot.ppm";
                    if (cpuRenderer.saveColor(screenshot)) {
                        std::cout << "Screenshot saved to: " << screenshot << '\n';
                    }
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds = std::min(
            std::chrono::duration<float>(now - previousTime).count(), 0.05F);
        previousTime = now;
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) {
            modelAngle -= 1.2F * deltaSeconds;
        }
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) {
            modelAngle += 1.2F * deltaSeconds;
        }
        if (keys[SDL_SCANCODE_Q]) {
            lightAngle -= 1.2F * deltaSeconds;
        }
        if (keys[SDL_SCANCODE_E]) {
            lightAngle += 1.2F * deltaSeconds;
        }

        const FrameResult result = renderFrame(
            cpuRenderer, sourceMesh, modelAngle, lightAngle);
        const auto& pixels = cpuRenderer.framebuffer().pixels();
        if (SDL_UpdateTexture(displayTexture, nullptr, pixels.data(), kWidth * 3) != 0) {
            std::cerr << "SDL texture upload failed: " << SDL_GetError() << '\n';
            running = false;
        }
        SDL_RenderClear(displayRenderer);
        SDL_RenderCopy(displayRenderer, displayTexture, nullptr, nullptr);
        SDL_RenderPresent(displayRenderer);

        std::ostringstream title;
        title << "EmberFrame CPU Renderer | "
              << result.stats.submittedTriangles << " triangles | "
              << result.stats.shadedFragments << " fragments | "
              << result.milliseconds << " ms";
        SDL_SetWindowTitle(window, title.str().c_str());
    }

    SDL_DestroyTexture(displayTexture);
    SDL_DestroyRenderer(displayRenderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        bool headless = false;
        std::filesystem::path modelPath;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--headless") {
                headless = true;
            } else {
                modelPath = argument;
            }
        }
        if (modelPath.empty()) {
            modelPath = outputPathBesideExecutable(argv[0], "assets/textured_cube.obj");
        }

        const auto sourceMesh = emberframe::software::loadObj(modelPath);
        std::cout << "Loaded OBJ triangles: " << sourceMesh.size() << " from " << modelPath << '\n';
        CpuRenderer renderer(kWidth, kHeight);
        return headless
            ? runHeadless(renderer, sourceMesh, argv[0])
            : runViewer(renderer, sourceMesh, argv[0]);
    } catch (const std::exception& error) {
        std::cerr << "A8 CPU Renderer failed: " << error.what() << '\n';
        return 1;
    }
}
