# 上游策略与目标架构

## 1. 两个上游的职责

### Vulkan Guide：代码起点

仓库：[vblanco20-1/vulkan-guide](https://github.com/vblanco20-1/vulkan-guide)

使用范围：Vulkan 初始化、Swapchain、基础资源创建、命令提交和初始渲染流程。导入前固定分支与 commit，保留 MIT License，不把原有功能列为个人实现。

### Piccolo：架构参考

仓库：[BoomingTech/Piccolo](https://github.com/BoomingTech/Piccolo)

重点阅读 Runtime、Render、Resource、Scene、Editor 的职责和启动/关闭顺序。默认不将 Piccolo 作为代码基底，避免项目退化成 GAMES104 同款 Fork。

## 2. 目标架构

```text
Launcher / Composition Root
├─ Core
│  ├─ Log / Assert / Result
│  ├─ Memory / Handle
│  └─ Job System
├─ Platform
│  ├─ Window / Input
│  └─ File / Time
├─ Runtime
│  ├─ Engine Loop
│  ├─ World / Scene
│  └─ System Scheduling
├─ Renderer
│  ├─ Vulkan Backend
│  ├─ GPU Resource Registry
│  ├─ Render Graph
│  ├─ Shader / Pipeline
│  └─ Profiler
├─ Asset
│  ├─ Import / Cache
│  ├─ Async Loading
│  └─ Hot Reload
└─ Editor
   ├─ Scene View
   ├─ Inspector
   └─ Diagnostics
```

由 Composition Root 显式创建和注入模块，避免到处访问全局单例。启动顺序和销毁顺序必须可测试。

## 3. 不直接照搬的设计

- 不因为“像引擎”就先抽象 DX12/Metal；当前只实现 Vulkan，接口由真实需求长出来。
- 不先造复杂 ECS、反射和脚本系统；先证明渲染、资源和场景生命周期闭环。
- 不把 Editor 与 Runtime 相互包含；Editor 调用公开 Runtime API。
- 不允许 GPU 资源由裸指针跨帧随意持有；采用句柄、所有权和延迟销毁。
- 不只展示成功画面；每个核心模块必须有错误路径、测试或性能证据。

## 4. 第一批个人核心模块

### Render Graph

- Pass 声明资源读写；
- 构建依赖 DAG 和执行顺序；
- 计算资源生命周期；
- 生成 Vulkan Barrier/Layout Transition；
- 检测循环依赖与未初始化读取；
- 导出 Graphviz 或 ImGui 可视化。

### Asset Streaming

- 代际 Handle 防止悬空引用；
- 后台文件读取和 glTF/纹理解码；
- Staging Ring 与 GPU 上传队列；
- 主线程/GPU 同步边界；
- 缓存、失败占位资源和热重载；
- 延迟销毁在安全帧之后回收资源。

### Profiling

- CPU scope 和线程时间线；
- Vulkan timestamp query；
- Pass、Draw Call、三角形和资源统计；
- 固定场景 Benchmark；
- 优化前后报告，而不是只写主观结论。

## 5. 上游记录格式

初始化代码仓库时创建 `UPSTREAM.md`：

```text
Upstream: https://github.com/vblanco20-1/vulkan-guide
Branch:
Commit:
License: MIT
Imported paths:
Local changes:

Architecture reference: https://github.com/BoomingTech/Piccolo
Copied implementation, if any:
```

Git 历史中先提交未修改的上游基线，再分模块重构。这样面试官可以直接通过 diff 判断个人贡献。

## 6. 简历解锁标准

只有同时具备代码、设计说明、演示和验证数据的模块才写入简历。至少完成 Render Graph、Asset Streaming、Profiler 三者中的两个，再把项目作为引擎实习的核心项目。
