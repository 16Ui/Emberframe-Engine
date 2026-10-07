# EmberFrame Engine 项目计划

这是后续新引擎源码的固定目录。项目采用“Vulkan Guide 渲染起点 + Piccolo 架构参考”；上游代码已经固定到独立基线分支，并完成 Windows Release 构建和启动验证。

## 2026-10-05 工程实现快照

新的统一入口是 `emberframe_workbench`，源码位于 `engine/lab`。已接通四种测试场景、Vulkan 前向/延迟、CPU 光栅/路径追踪参考、专题选择与图像输出。几何、材质预计算、统计阴影、GI、时空降噪、QEM、任务系统、场景序列化与 Shader 热重载有独立实现和确定性检查。

真实 GPU 已接入 PCSS/CSM/VSM/SAT/VSSM/MSM、SSR/SSGI、RSM/LPV/VCT、SSAO/GTAO、TAA/HW5/SVGF、多级 Bloom/NPR、Disney 与拟合 LTC/预过滤环境光。SH/PRT、网格距离场已从专门示例接入当前场景的正常前向/延迟光照；静态数据后台烘焙、内容指纹失效、可随工程保存。自动 LOD 消费持久减面链并改变实际绘制；BVH/八叉树服务选物和 CPU 路径追踪。GPU VCT 查询实际 compact 的非空节点 SSBO，稠密构建中间态仍保留。路径追踪明确在 CPU。运行入口与实现限制见 [README](README.md#当前实现和执行位置)；以下旧规划保留为范围记录，不代表所有生产级优化已实现。

资产链路已经包含真实 VMA Staging Ring、每帧预算、Fence 完成统计和取消/失败保留旧场景；VkPipelineCache 按驱动、UUID、校验和验证及损坏恢复。场景资源集成后最终 193 项算法检查、135 项 GPU 集成检查和 64 组回归通过，集合重叠不能相加，见 `output/verification/`。C6 已有真实依赖/生命周期/Barrier 的 JSON/DOT 导出；C9 已有 262144 对象、13 配置的逐位校验扩展性报告；C10 已有六组 GPU 独立完成帧的中位数/p95；C12 已有三分钟操作稿和可解码 MP4。历史性能/视频仍绑定原生成版本，不冒充本轮新资源组合的性能。具体证据与计时边界见 [演示指南](docs/WORKBENCH_DEMO.md)。这些是工程实现证据，不替代个人的源码学习验收。

同日修复普通 PBR 画面的自阴影条纹：以实际三角形几何法线建立接收平面，对 Hard/PCF/PCSS/CSM 各采样位置校正深度，前向/延迟均接入。默认 Bias 不增大，新增 16 项 GPU 检查兼顾倾斜表面与近遮挡保留；完整 49 组回归再次通过。修复后视频位于 `output/demo-shadow-fixed-20261005/EmberFrame-demo.mp4`，旧视频保留；末段 NPR 排线仍是主动启用的风格效果。

随后将原生工作台调整为场景/资源栏、中心视口、右侧属性检查器与底部状态。参数采用折叠分组和名称/数值两列，侧栏可调宽或隐藏；默认视口按剩余区域自适应，高 DPI 换算与离屏尺寸有界。保留固定尺寸 CLI 回归行为，当时新增四组界面呈现用例，总计 53 组通过；本轮加上场景预计算验证后为 64 组。布局采用固定分区，并非完整 ImGui Docking / 多窗口编辑器。

## 边界

- 不复制 `D:\games\VulkanEngineMVP` 作为项目主体；
- Vulkan 初始化与基础渲染允许从 Vulkan Guide 的教程代码起步，保留 MIT License、上游地址和基线 commit；
- Piccolo 默认只读学习，不直接整仓合并；复制具体实现时必须记录来源；
- GAMES101/202 作业只作为算法和渲染知识参考，不直接堆入引擎源码；
- 每个模块必须包含可运行 Demo、测试或性能证据，以及对应设计说明；
- 优先建立平台层、资源生命周期、渲染框架和调试能力，再扩展动画、物理与编辑器。
- 渲染能力以真实 Metallic-Roughness PBR 为主线，逐步接入 IBL、阴影、后处理、Render Graph 与性能诊断；详细范围见 [`docs/RENDERING_INTERVIEW_TRACK.md`](docs/RENDERING_INTERVIEW_TRACK.md)。

## 2026-10-01 范围补全

新增 [D01—D32 高级渲染与图形基础深化专题](docs/learning/ADVANCED_RENDERING_TRACK.md)。本次要求的图形基础、离线参考、采样/预计算、材质/面积光、多光源、统计阴影、屏幕空间/三维 GI、时空降噪和 NPR 全部进入明确学习与独立实现计划，不再限制为“只选一两个”。

A1—A8 的完成记录保持不动；B/C 的概念讨论不代表工程完成。此段保留为最初范围说明；2026-10-05 的实现状态见上方快照，不把概念掌握或完整 GPU 接入自动视为完成。不同阴影、GI、渲染路径按实验选择和明确融合规则运行，不把所有效果全量叠加。

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

### M1：从教程程序拆出引擎骨架（C1—C2）

- [x] 建立 Platform 窗口层和 Engine Runtime 主循环；
- [x] 将窗口所有权与事件轮询从 `VulkanEngine` 中剥离；
- [ ] 建立独立 Launcher / Sample 和 Vulkan Backend 目标；
- [ ] 将 SDL 输入事件翻译为引擎事件，消除 Renderer 对 SDL 输入的直接依赖；
- [ ] 把时间统计从渲染器中剥离；
- [ ] 用 Composition Root 明确创建、启动和销毁顺序；
- [ ] 拆出 Vulkan Context、Swapchain 与 Frame Context；
- [ ] 覆盖 Resize、最小化、Swapchain 重建和初始化失败路径；
- [ ] 保留一个与上游画面一致的回归 Sample。

### M2：资源生命周期与测量基线（C3、C10）

- 引入代际 Handle 和 Resource Registry；
- 明确 CPU 对象、Vulkan Handle 与 VMA Allocation 的所有权；
- 用每帧回收队列实现安全延迟销毁；
- 添加无效 Handle、重复释放、过期 Handle 和跨帧销毁测试；
- 在增加 PBR 前建立 CPU Scope、Vulkan Timestamp、显存统计和 RenderDoc 回归基线；
- 固定测试机器、分辨率、相机路径和统计区间，让后续优化可重复比较。

### M3：Shader、Material 与真实 PBR（C4—C5）

- 建立 Shader/Pipeline Cache、材质参数声明与 Shader 热重载失败回退；
- 正确处理线性空间、sRGB、HDR、曝光与 Tone Mapping；
- 实现 glTF Metallic-Roughness、Cook–Torrance BRDF 与法线贴图；
- 加入直接光、Diffuse/Specular IBL 与 BRDF LUT；
- 提供材质球矩阵和 PBR 分项调试视图；
- 验证当前上游 `mesh_pbr.frag` 与完整 PBR 实现的画质和 GPU 时间差异。
- 基础稳定后完成 D11—D16：SH/PRT、IBL 烘焙与采样验证、Kulla–Conty、Disney 清漆/各向异性/织物、LTC 面积光；每项先独立实验，再整合材质与光照模块。

### M4：Render Graph 与多 Pass 渲染（C6）

- 声明 Pass 的资源读写关系；
- 构建 DAG、拓扑排序并检测循环依赖；
- 分析瞬时资源生命周期，裁剪无输出贡献的 Pass；
- 自动计算 Barrier、Stage/Access 与 Image Layout；
- 导出 Graphviz/ImGui 调试视图，并与手写路径对比；
- 用 Shadow、Lighting、Post-process 的真实依赖验证图执行结果；
- 阴影先完成 Bias、PCF、CSM、PCSS，再完整建立 VSM/SAT、VSSM、MSM 的 D19—D21 对照，不只列名词；统计近似、漏光和数值稳定都要验收。
- 用 D17/D18 比较 Forward/Deferred 与 Tiled/Clustered 多光源，保留同一 BRDF 和全灯参考。
- 用 D22—D30 的屏幕/三维 GI 与时空降噪验证真实多 Pass、历史资源和同步；各间接分量防重复计数。

### M5：资产流水线与 Render Scene（C7—C8）

- 后台读取、解析和解码 glTF/纹理，渲染线程只接收上传任务；
- 实现 Staging Ring、单帧上传预算、缓存、失败占位资源和热重载；
- 建立 Render Scene、Frustum Culling、批次与可见物统计；
- 按岗位方向选择 Hi-Z Occlusion、Instancing 或 Indirect Draw 做深；
- 形成优化前后的帧时间、Draw Call、显存和加载耗时报告。

### M6：Job System 与差异化扩展（C9）

- 建立任务依赖、Work Stealing、线程本地队列与可控退出；
- 用资产解析、可见性计算或命令准备验证真实任务，而不是只跑空任务；
- 比较单线程、多线程在不同任务粒度下的扩展性和调度开销；
- 保留 B20 的 SSAO/GTAO/TAA 深入学习与 GPU Driven/QEM LOD 基本概念；D 系列按 13 批全部建立专题实验，不要求一次铺开；
- PBD/XPBD 不属于本次必做范围，若以后扩展再独立确认，不挤占已要求的渲染专题；
- 每项必须提供调试视图、失败案例和量化结果，不以功能数量作为完成标准。

### M7：场景工作流与作品集收口（C11—C12）

- 建立版本化场景格式、Schema Migration、未知字段保留和资源引用修复；
- 完成支持属性查看、修改和 Undo/Redo 的最小 Inspector，不扩张为完整编辑器；
- 建立一键构建、固定场景、图像回归和性能回归；
- 整理架构图、关键取舍、失败记录、演示视频和三分钟面试讲稿；
- 在 README 和简历中明确 Vulkan Guide/Piccolo 参考边界与个人贡献。

### M8：完整渲染实验室与参考验证（D01—D32）

- D01—D05：齐次裁剪、Top-left 覆盖、纹理足迹与过滤、Reversed-Z、CPU/GPU 差分；不改写 A 章完成记录；
- D06—D10：包围体、BVH、八叉树、蒙特卡洛/重要性/MIS 与小型离线路径追踪；
- D11—D16：SH/PRT、采样预计算、Kulla–Conty、Disney 材质分量与 LTC；
- D17—D21：前向/延迟与分块/聚簇灯表，VSM/SAT/VSSM/MSM 的质量/成本比较；
- D22—D25：SSR、SSGI、HW5 重投影/过滤/累积与 SVGF；
- D26—D30：RSM、LPV、体素化/稀疏结构、VXGI 路线的 VCT 实验、SDF 查询/阴影；
- D31—D32：NPR 与统一场景、分项颜色、失败案例、CPU/GPU/显存和参考误差报告。

每章包含前置、输入输出、核心流程、工程归属、正确与失败案例和量化验收。独立 CPU/GPU 实验与正式模块集成分别记状态；完整范围不等于所有算法同时进入默认路径。先共享场景/材质/光源约定，再复用 C3—C8 资源与 Pass 职责，C10/C12 收集证据。小型 CPU 路径追踪作为参考，不要求硬件光追或神经网络。

D32 不重复包装上游：只记录个人完成的代码和实际测量。新增实现与执行边界以本文件开头的工程快照及 README 为准；每项简历表述仍必须对应自己的代码、测试和限制。
