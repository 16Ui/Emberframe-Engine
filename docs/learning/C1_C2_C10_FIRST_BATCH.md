# C 第一批：正式入口、Vulkan 帧闭环与性能基线

这批代码的目标不是再做一个 B8 演示，而是让正式 `engine` 拥有一个可运行、可验证的最小 Vulkan 帧闭环。当前画面仍是 GPU 清屏颜色，不是 PBR 场景；它验证架构和测量通路，不代表渲染功能已完成。

这是第一批完成时的清屏基线；正式入口在 [第二批](C3_C4_SECOND_BATCH.md) 已加入三角形绘制，原清屏路径可用 `--clear` 保留。

## 先看整体关系

```text
launcher/main.cpp
  ├─ platform::SdlWindow       拥有 SDL_Window，提供窗口事件
  ├─ renderer::VulkanContext   借用窗口；拥有 Instance / Surface / Debug Messenger
  ├─ renderer::VulkanDevice    借用 Instance / Surface；选择 GPU，拥有 Device / Queue
  ├─ renderer::VulkanSwapchain 借用窗口、Device、Surface；拥有 Swapchain / ImageView
  ├─ renderer::FrameContext    借用 Device / Queue；拥有 Frame Slot、同步对象、GPU Query Pool
  ├─ runtime::FrameClock       提供逐帧时间
  ├─ runtime::InputState       消费平台层翻译后的按键事件
  └─ profiling::CpuProfiler   收集 CPU 作用域耗时
```

对象按图中顺序构造，程序结束时反向析构。`VkImage` 本身属于 Swapchain；我们取得它的 Handle，但不单独销毁。每张 ImageView 由引擎创建、销毁。Frame Slot 按 CPU 帧号轮换，`renderFinished` Semaphore 按 Swapchain Image 编号取用；这两种编号不是同一回事。

## 一帧实际发生什么

1. Runtime 开始本帧，处理 SDL 翻译后的 `core::Event`，更新输入状态。
2. CPU 等当前 Frame Slot 的 Fence，确认上次使用这个 Slot 的 GPU 工作已结束；此时读回上次记录的 GPU 时间戳。
3. `vkAcquireNextImageKHR` 取得本帧可用 Image 的索引；Acquire 关联 `imageAvailable` Semaphore。
4. 重置并录制当前 Slot 的 Command Buffer：给 Image 转入 Transfer Destination 布局，清屏，再转为 Present 布局。首尾各写一次 GPU Timestamp。
5. Graphics Queue 提交 Command Buffer，等待 `imageAvailable`，完成时发信号给这张 Image 对应的 `renderFinished`。
6. Present Queue 等 `renderFinished`，再呈现 Image。Fence 用来限制 CPU 重用 Frame Slot；Semaphore 用来约束 GPU/呈现之间的先后。

窗口 Resize、最小化、恢复或 Acquire/Present 报告过期时，先暂停使用旧 Swapchain Image。恢复为非零窗口大小后，等待旧工作退出，重建 Swapchain/ImageView，并按新的 Image 数量重建 `renderFinished` Semaphore。这个保守的 `vkDeviceWaitIdle` 只在低频重建路径使用；以后可再细化。

## 构建与验证

在项目根目录的 PowerShell 里：

```powershell
.\scripts\build-windows.ps1 -Config Release -Target emberframe_launcher
.\bin\Release\emberframe_launcher.exe --smoke-test
.\bin\Release\emberframe_launcher.exe --resize-smoke-test
.\bin\Release\emberframe_launcher.exe --benchmark-frames 120 --warmup-frames 20
```

`--smoke-test` 自动运行 12 帧；`--resize-smoke-test` 自动调整为 960×540，再最小化/恢复并检查重建结果；普通运行不带参数，按 Esc 退出。若终端显示 Validation Layer 为 `unavailable`，可先运行构建脚本设置 SDK，或在启动进程中设置正确的 `VK_LAYER_PATH`；不要把“未加载验证层”误写成“验证层零错误”。

性能输出里：

- `cpu.frame` 是 CPU 事件处理与提交/呈现的整帧耗时；它可能包含 Acquire/Present 等待。
- `cpu.acquire_submit_present` 是 CPU 从等待 Fence 到调用 Present 返回的耗时，不是 GPU 绘制时间。
- `gpu.clear_and_transitions` 来自 GPU Timestamp，只覆盖 Command Buffer 内清屏和布局转换；不包含 Present，也不等于完整渲染帧时间。
- `drawCalls=0` 是因为当前仅清屏，没有 Draw；本阶段也没有自有 GPU 内存分配统计。不能拿这份数据宣称 PBR 性能。

固定 Benchmark 需同时记录 GPU、分辨率、Present Mode、Validation Layer 状态、Release/Debug、热身帧数与测量帧数。不同模式的结果不要直接比较。

## 本次边界与后续工作

正式引擎代码位于 `engine/renderer`，B4–B8 的教学 Sample 仍保留在 `samples`，没有被改写。当前正式后端借鉴这些已经验证的样例实现，刻意保持 Sample 独立，但产生了一定重复；后续稳定后应抽取共享的 Vulkan 基础工具，避免两套逻辑长期漂移。

C1/C2 的第一条可运行路径已建立，C10 的 CPU Scope、GPU Timestamp 和固定清屏基线已建立。完整的 C10 仍需要在真实 Draw/材质场景里统计 Draw Call、显存分配及优化前后对照；现在只有“测量基础设施”，不是最终性能报告。
