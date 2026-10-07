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

## 阶段 2：Vulkan 渲染闭环

先保持单 Vulkan 后端，不提前建设空泛的多 API RHI。完成设备、Swapchain、Command/Frame、同步、Shader/Pipeline、Descriptor、Buffer/Image 和 glTF 场景的可解释闭环；在增加 PBR 前加入 Vulkan Timestamp、固定场景和 RenderDoc 回归基线。

## 阶段 3：PBR 与实时渲染主线

将上游名义上的 `mesh_pbr.frag` 扩展为真实 Metallic-Roughness PBR：线性颜色空间、Cook–Torrance、法线贴图、直接光、Diffuse/Specular IBL、HDR 与 Tone Mapping。随后完成 Shadow Bias、PCF、CSM 和必要的后处理，并为每项保留调试视图和 GPU 时间。

详细范围与面试验收见 [`RENDERING_INTERVIEW_TRACK.md`](RENDERING_INTERVIEW_TRACK.md)。

## 阶段 4：Render Graph、场景与资源

用真实的 Shadow、Lighting 和 Post-process Pass 驱动 Render Graph；完成场景组织、版本化序列化、代际资源句柄、后台解析、GPU 上传队列、依赖和热重载。

## 阶段 5：引擎能力

任务系统、调试绘制、CPU/GPU Profiler 和回归场景优先；动画与物理根据求职时间再扩展。

## 阶段 6：作品化

稳定 Demo、性能对比、架构文档、故障复盘、构建说明和面试问答。

学习 GAMES104 时按模块更新本路线，不要求学完整门课后才开始编码。
