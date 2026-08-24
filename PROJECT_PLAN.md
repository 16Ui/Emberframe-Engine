# EmberFrame Engine 项目计划

这是后续新引擎源码的固定目录。项目采用“Vulkan Guide 渲染起点 + Piccolo 架构参考”；上游代码已经固定到独立基线分支，并完成 Windows Release 构建和启动验证。

## 边界

- 不复制 `D:\games\VulkanEngineMVP` 作为项目主体；
- Vulkan 初始化与基础渲染允许从 Vulkan Guide 的教程代码起步，保留 MIT License、上游地址和基线 commit；
- Piccolo 默认只读学习，不直接整仓合并；复制具体实现时必须记录来源；
- GAMES101/202 作业只作为算法和渲染知识参考，不直接堆入引擎源码；
- 每个模块必须包含可运行 Demo、测试或性能证据，以及对应设计说明；
- 优先建立平台层、资源生命周期、渲染框架和调试能力，再扩展动画、物理与编辑器。

## 演进后的目标结构

```text
Emberframe-Engine/
├─ engine/       # Runtime 与各引擎模块
├─ editor/       # 后续编辑器和工具
├─ samples/      # 可独立运行的功能样例
├─ tests/        # 单元、集成与回归测试
├─ assets/       # 可公开测试资源
├─ docs/         # 架构、性能报告和开发记录
└─ CMakeLists.txt
```

## 个人实现边界

上游基线不算个人成果。以下模块实现并完成验证后，才进入简历：

- Render Graph 与自动资源状态转换；
- 代际资源句柄、延迟销毁和异步资产上传；
- Shader/Pipeline 缓存与热重载；
- Job System 和任务依赖；
- CPU/GPU Profiler 与可复现 Benchmark；
- Scene 序列化和最小 Editor 工作流。

## 实施顺序

### M0：可信基线（已完成）

- 固定上游 branch、commit 和 License；
- 建立 `upstream-baseline` 与 `main` 的贡献边界；
- 编译 Shader 和 `chapter_6`；
- 验证程序能够进入渲染循环。

### M1：从教程程序拆出引擎骨架

- [x] 建立 Platform 窗口层和 Engine Runtime 主循环；
- [x] 将窗口所有权与事件轮询从 `VulkanEngine` 中剥离；
- [ ] 建立独立 Launcher / Sample 和 Vulkan Backend 目标；
- [ ] 将 SDL 输入事件翻译为引擎事件，消除 Renderer 对 SDL 输入的直接依赖；
- [ ] 把时间统计从渲染器中剥离；
- 用 Composition Root 明确创建、启动和销毁顺序；
- 保留一个与上游画面一致的回归 Sample。

### M2：GPU 资源生命周期

- 引入代际 Handle 和 Resource Registry；
- 明确 CPU 对象、Vulkan Handle 与 VMA Allocation 的所有权；
- 用每帧回收队列实现安全延迟销毁；
- 添加无效 Handle、重复释放和跨帧销毁测试。

### M3：Render Graph

- 声明 Pass 的资源读写关系；
- 构建 DAG、拓扑排序并检测循环依赖；
- 自动计算 Barrier 与 Image Layout；
- 导出 Graphviz/ImGui 调试视图，并与手写路径对比。

### M4：资产流送与性能证据

- 后台读取和解码 glTF/纹理；
- Staging Ring、上传队列、失败占位资源和热重载；
- CPU scope、Vulkan timestamp 和固定场景 Benchmark；
- 形成优化前后的帧时间、显存与加载耗时报告。
