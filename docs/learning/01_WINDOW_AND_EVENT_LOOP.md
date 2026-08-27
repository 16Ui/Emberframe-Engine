# 第一课：SDL 窗口与事件循环

本课只解决一个问题：创建一个窗口，让程序持续响应操作系统事件，并在用户关闭窗口后正确释放资源。

对应源码：[samples/01_window/main.cpp](../../samples/01_window/main.cpp)。

## 1. 本课暂时不做什么

- 不初始化 Vulkan；
- 不调用 GPU；
- 不显示渲染图像；
- 不设计 Renderer；
- 不使用已经封装好的 `engine/platform/SdlWindow`。

窗口是操作系统对象，不等于渲染结果。先把窗口生命周期理解清楚，后面 Vulkan 才有一个可以连接的显示目标。

## 2. 三个核心概念

### SDL

| 字段 | 内容 |
|---|---|
| Full name | Simple DirectMedia Layer，简单直接媒体层 |
| 解决什么 | 用统一 API 创建窗口、读取键鼠和接收操作系统事件 |
| 输入 | 窗口标题、位置、尺寸、功能标志 |
| 过程 | SDL 调用 Windows 的窗口系统创建原生窗口 |
| 输出 | `SDL_Window*`，指向 SDL 管理的窗口对象 |
| 限制 | SDL 本身不会替我们完成 Vulkan 渲染 |
| 后续关系 | Vulkan 会通过 Surface 与这个窗口建立连接 |

### 主循环

窗口程序不能在创建窗口后立即退出。`while (running)` 让进程持续存活，每次循环都处理当前积累的事件。以后每一帧的输入更新、游戏逻辑和渲染也会放进这个循环。

### 事件

`SDL_Event` 是一份事件数据。关闭窗口、移动鼠标、按下键盘、改变窗口尺寸都会产生不同类型的事件。`SDL_PollEvent` 每次从队列中取出一个事件，返回非零表示确实取到了数据。

## 3. 程序的完整因果链

### 第一步：初始化 SDL Video 子系统

```cpp
SDL_Init(SDL_INIT_VIDEO)
```

输入是 `SDL_INIT_VIDEO`，表示本程序需要窗口显示能力。返回值为 `0` 表示成功，非零表示失败。失败原因不能猜测，要通过 `SDL_GetError()` 读取。

### 第二步：创建窗口

```cpp
SDL_Window* window = SDL_CreateWindow(...);
```

返回的不是窗口本体，而是一个指针。`nullptr` 表示创建失败。只要创建成功，后面就必须调用 `SDL_DestroyWindow(window)`。

### 第三步：进入主循环

```cpp
bool running = true;
while (running) {
    // 处理事件
}
```

`running` 是程序是否继续运行的状态。循环条件为真，程序就继续；收到退出事件后将其改为假，下一次检查条件时离开循环。

### 第四步：取出所有待处理事件

```cpp
while (SDL_PollEvent(&event) != 0) {
    if (event.type == SDL_QUIT) {
        running = false;
    }
}
```

这里使用内层 `while`，因为一次外层循环中可能已经积累多个事件。只处理一个事件会让输入队列不断堆积，程序表现为延迟或无响应。

### 第五步：逆序清理

创建顺序：

```text
初始化 SDL → 创建 Window
```

销毁顺序：

```text
销毁 Window → 关闭 SDL
```

先销毁依赖 SDL 的窗口，再关闭 SDL 子系统。这个“按依赖关系逆序销毁”的规则会贯穿 Vulkan：Device 资源必须在 Device 前销毁，Surface 必须在 Instance 前销毁。

## 4. 为什么有 `SDL_Delay(1)`

当前程序还没有渲染工作。如果循环完全不等待，它会尽可能快地重复检查空事件队列，占用一个 CPU 核心。`SDL_Delay(1)` 让当前线程至少短暂让出执行时间。

它不是正式引擎的帧率控制方案。后面我们会引入高精度时间、帧间隔和渲染同步。

## 5. CMake 在本课的工作

`samples/01_window/CMakeLists.txt` 完成三件事：

1. 把 `main.cpp` 编译成 `emberframe_01_window.exe`；
2. 链接 `SDL2::SDL2main` 和 `SDL2::SDL2`；
3. 构建后把运行所需的 SDL 动态库复制到可执行文件目录。

CMake 不是编译器。它读取 `CMakeLists.txt`，为 Visual Studio/MSVC 生成项目，再由编译器把 C++ 源码变成机器代码。

### `SDL2main` 与第一次链接失败

第一版 Sample 只链接了 `SDL2::SDL2`。`main.cpp` 成功编译，但链接器报告找不到 `main`。这不是 C++ 语法错误，而是程序入口没有连接完整。

在 Windows 上，SDL 可以用 `SDL2main` 提供平台入口适配，再转入源码中的 `SDL_main`。因此本目标需要同时链接：

```cmake
target_link_libraries(emberframe_01_window PRIVATE SDL2::SDL2main SDL2::SDL2)
```

这也区分了两个阶段：

- 编译：把每个 `.cpp` 转换为目标文件，语法或类型错误在这里出现；
- 链接：把目标文件和库组合成 `.exe`，缺少函数实现或入口时在这里失败。

## 6. 构建与运行

在仓库根目录执行：

```powershell
.\scripts\build-windows.ps1 -Config Release -Target emberframe_01_window
.\bin\Release\emberframe_01_window.exe
```

预期行为：

- 出现标题为 `EmberFrame - Lesson 01` 的 1280×720 窗口；
- 可以拖动和改变窗口尺寸；
- 点击关闭按钮后程序正常退出；
- 控制台没有 SDL 错误。

## 7. 理解检查

运行后尝试回答下面四个问题，不需要背代码：

1. 为什么创建窗口前必须先调用 `SDL_Init`？
2. 为什么事件处理使用内层 `while`，而不是只调用一次 `SDL_PollEvent`？
3. `SDL_Window*` 是窗口本体还是指向窗口对象的地址？
4. 为什么清理顺序与创建顺序相反？

能够用自己的话回答这四问，就具备进入下一课“使用 RAII 自动管理窗口生命周期”的前提。
