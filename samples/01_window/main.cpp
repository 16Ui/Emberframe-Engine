#include <SDL.h>

#include <iostream>

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    // 创建窗口前必须先启动 SDL 的 Video 子系统。
    // SDL_Init 返回 0 表示成功；失败时 SDL_GetError 会给出平台相关原因。
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL initialization failed: " << SDL_GetError() << '\n';
        return 1;
    }

    // SDL_Window* 是 SDL 管理的窗口对象指针，不是自动释放的 C++ 对象。
    // 创建成功后，本程序负责在退出前调用 SDL_DestroyWindow。
    SDL_Window* window = SDL_CreateWindow(
        "EmberFrame - Lesson 01",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1280,
        720,
        SDL_WINDOW_RESIZABLE);

    if (window == nullptr) {
        std::cerr << "Window creation failed: " << SDL_GetError() << '\n';

        // 虽然窗口创建失败，但 SDL 已经初始化成功，所以仍然需要关闭 SDL。
        SDL_Quit();
        return 1;
    }

    bool running = true;
    while (running) {
        SDL_Event event;

        // 一次外层循环中可能积累了多个系统事件，因此要把当前队列取空。
        // 如果每帧只取一个事件，键鼠或窗口事件可能堆积，造成明显延迟。
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }

        // 本课尚未渲染；短暂让出 CPU，避免空循环持续占满一个核心。
        // 这不是正式引擎的帧率控制方案，后续会由时间系统与渲染同步替代。
        SDL_Delay(1);
    }

    // 资源按依赖关系逆序释放：先销毁依赖 SDL 的窗口，再关闭 SDL。
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
