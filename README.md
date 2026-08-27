# EmberFrame Engine

一个面向游戏引擎实习的现代 C++ / Vulkan 学习型引擎项目。

项目以 Vulkan Guide 的可运行代码为渲染基线，参考 Piccolo 的模块职责和生命周期设计，但不会把教程代码或参考引擎原样包装成个人成果。后续工作会通过独立 Git 提交逐步完成模块拆分、资源系统、Render Graph 和性能工具，并为关键能力保留 Demo、测试与性能数据。

## 当前状态

- [x] 固定并导入 Vulkan Guide 上游基线；
- [x] 在 Windows + MSVC + Vulkan SDK 环境完成 `chapter_6` Release 构建；
- [x] 完成 8 秒启动冒烟测试，程序稳定进入渲染循环；
- [x] 建立独立 Platform 窗口层与 Runtime 主循环；
- [ ] 将 Vulkan Renderer、输入和 Sample 完全移出教程式 `chapter-*`；
- [ ] 实现代际资源句柄和延迟销毁；
- [ ] 实现 Render Graph；
- [ ] 实现异步资产上传与性能分析。

当前里程碑只是“可信、可运行的上游基线”，还不作为简历中的个人引擎成果。个人贡献边界见 [UPSTREAM.md](UPSTREAM.md)，阶段目标见 [PROJECT_PLAN.md](PROJECT_PLAN.md)。

如果从零开始学习和共建，请从 [零基础共建路线](docs/learning/LEARNING_PATH.md) 和
[第一课：SDL 窗口与事件循环](docs/learning/01_WINDOW_AND_EVENT_LOOP.md) 开始，不要直接阅读完整的 `chapter-6/vk_engine.cpp`。

## Windows 构建

要求：Visual Studio 2022（Desktop development with C++）、CMake、Vulkan SDK。

```powershell
.\scripts\build-windows.ps1 -Config Release -Target chapter_6
```

脚本会优先读取 `VULKAN_SDK`，也会尝试发现 `C:\VulkanSDK` 或 `D:\develop\VulkanSDK` 下已安装的版本。

构建后可执行文件位于：

```text
bin/Release/chapter_6.exe
```

运行自动冒烟测试：

```powershell
.\scripts\smoke-test-windows.ps1 -Seconds 8
```

## 目标结构

```text
engine/
  core/          日志、断言、句柄、任务系统
  platform/      窗口、输入、文件与时间
  runtime/       引擎循环、World 与系统调度
  renderer/      Vulkan 后端、Render Graph、GPU 资源与 Profiler
  asset/         导入、缓存、异步加载与热重载
editor/          场景视图、Inspector 与诊断工具
samples/         各模块的最小可运行样例
tests/           单元、集成和回归测试
```

`engine/platform` 和 `engine/runtime` 已开始承载真实代码；其余目录仍将在对应模块真正实现时创建，避免先堆空壳架构。

## 来源与许可

上游代码来自 [Vulkan Guide](https://github.com/vblanco20-1/vulkan-guide)，采用 MIT License。架构阅读参考为 [Piccolo](https://github.com/BoomingTech/Piccolo)。详细版本与使用边界见 [UPSTREAM.md](UPSTREAM.md)。
