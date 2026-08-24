# M1：Platform 与 Runtime 生命周期拆分

## 问题

Vulkan Guide 基线中的 `VulkanEngine` 同时负责：

- SDL 初始化和窗口创建；
- Vulkan 初始化与销毁；
- SDL 事件轮询；
- 相机和 ImGui 输入；
- 主循环和帧时间统计；
- 场景更新与渲染。

这使窗口生命周期、应用循环和渲染器无法独立验证，也让后续 Editor、测试程序或多个 Sample 必须复制整个大类。

## 本次切分

```text
BaselineApplication              Composition Root
├─ Runtime::Application          主循环与退出条件
│  └─ Platform::SdlWindow        SDL 初始化、窗口与事件轮询
└─ VulkanEngine                  Vulkan 生命周期与逐帧渲染
```

明确的所有权顺序为：

```text
创建 SDL Window
→ VulkanEngine::init(window)
→ process_event / tick
→ VulkanEngine::cleanup()
→ 销毁 SDL Window
```

因此 Vulkan Surface 始终在 SDL Window 之前销毁，窗口也不再由 Renderer 创建或释放。

## 新增边界

### Platform

`SdlWindow` 使用 RAII 管理 SDL video subsystem 和 `SDL_Window`，提供原生窗口句柄、尺寸查询和事件轮询。

### Runtime

`Application` 定义 `on_start`、`on_event`、`on_frame`、`on_stop` 生命周期，并统一处理退出事件和异常路径下的停止操作。

### Renderer

`VulkanEngine` 不再拥有窗口和主循环：

- `init(SDL_Window*)` 接收外部窗口；
- `process_event(SDL_Event&)` 暂时处理相机、ImGui 和 Resize；
- `tick()` 更新并渲染一帧；
- `cleanup()` 只销毁 Vulkan 侧资源。

## 验证

- Windows / MSVC Release 构建通过；
- `emberframe_platform`、`emberframe_runtime`、`chapter_6` 链接通过；
- `chapter_6` 在新生命周期下持续运行 8 秒，无标准错误输出；
- 编译器警告与基线一致，没有新增错误。

## 有意保留的技术债务

- Runtime 和 Renderer 仍能看到 `SDL_Event`；下一步引入引擎事件类型和 Input State；
- Vulkan 实现仍位于 `chapter-6`；下一步迁移到 `engine/renderer/vulkan`；
- `VulkanEngine` 仍是大型类；资源、Swapchain 和场景按真实依赖逐步拆分；
- 冒烟测试目前通过限时运行验证，后续增加可主动退出的回归模式。
