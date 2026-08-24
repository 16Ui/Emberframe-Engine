# Games 主线路线

选型：Vulkan Guide 作为可运行起点，Piccolo 作为模块边界参考。具体来源与个人贡献边界见 [`UPSTREAM_AND_ARCHITECTURE.md`](UPSTREAM_AND_ARCHITECTURE.md)。

## 阶段 0：冻结上游基线

- 固定 Vulkan Guide 来源分支和 commit；
- 保留许可证与 `UPSTREAM.md`；
- 在未修改状态下完成构建、运行和 RenderDoc 捕获；
- 记录场景、帧耗、Draw Call、显存和已知问题；
- 再整理 CMake、依赖、日志、断言和测试入口。

## 阶段 1：最小运行时

将教程式单体代码拆为 composition root、应用循环、时间、输入、窗口、Renderer 和 GPU Resource；参考 Piccolo 的模块边界，但不照搬全局上下文。

## 阶段 2：渲染框架

先保持单 Vulkan 后端，不提前建设空泛的多 API RHI。完成 Command/Frame 组织、Render Graph、Shader/Pipeline 管理、资源上传和基础场景渲染。

## 阶段 3：场景与资源

场景组织、组件模型、版本化序列化、代际资源句柄、后台解析、GPU 上传队列、依赖和热重载。

## 阶段 4：引擎能力

任务系统、调试绘制、CPU/GPU Profiler 和回归场景优先；动画与物理根据求职时间再扩展。

## 阶段 5：作品化

稳定 Demo、性能对比、架构文档、故障复盘、构建说明和面试问答。

学习 GAMES104 时按模块更新本路线，不要求学完整门课后才开始编码。
