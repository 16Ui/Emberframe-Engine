# EmberFrame 渲染实习能力主线

这条主线用于把 GAMES101、GAMES202、GAMES104 与 Vulkan Guide 转化为可运行、可解释、可量化的个人项目贡献。目标不是收集尽可能多的效果，而是围绕一条完整的实时渲染管线，形成算法、GPU API、引擎架构和性能证据之间的闭环。

岗位与技术依据的外部一手资料见 [`research/2026-09_RENDERING_INTERNSHIP_SKILLS.md`](research/2026-09_RENDERING_INTERNSHIP_SKILLS.md)。上游与个人贡献边界见 [`../UPSTREAM.md`](../UPSTREAM.md)。

## 2026-10-01 完整专题范围

详细专题规范见 [D01—D32 高级渲染与图形基础深化](learning/ADVANCED_RENDERING_TRACK.md)，当前状态见 [学习路线图](learning/LEARNING_PATH.md)。本次要求全部纳入独立实验，不再使用“只选一两个”限制；不同方案按批次实现、同场景对照，再渐进接入正式引擎。

这不是新增完成声明：B/C 概念首轮已讨论，完整 PBR、GI、降噪等仍需代码和运行证据。A1—A8 不改，补充裁剪/覆盖/过滤等另列 D 章。

## 当前基线审计

Vulkan Guide 基线已经提供 Vulkan 初始化、Swapchain、Command Buffer、同步、VMA、Descriptor、Pipeline、glTF 加载、ImGui 和场景绘制，可以作为稳定的 GPU 执行底座。

但当前 `chapter-6` 中的 `GLTFMetallic_Roughness` 和 `mesh_pbr.frag` 还不能视为完整 PBR：

- glTF Loader 已读取 base color、metallic 和 roughness 参数；
- Material 常量中已经预留 base color 与 metallic-roughness 数据；
- Fragment Shader 当前主要计算简单方向光与 SH 漫反射；
- metallic、roughness 尚未进入 Cook–Torrance BRDF；
- 尚无完整法线贴图、直接光 PBR、镜面 IBL、BRDF LUT、HDR 曝光与 Tone Mapping 闭环。

因此，真实的 Metallic-Roughness PBR 管线属于后续个人实现，而不是上游已完成能力。

## 主线优先级

### P0：Vulkan 最小闭环与可调试性

先完成后续所有渲染算法依赖的 GPU 基础：

- Instance、Surface、Physical/Logical Device 与 Queue；
- Swapchain、Image View 与窗口 Resize 重建；
- Command Buffer、Fence、Semaphore 与每帧资源；
- Dynamic Rendering、Image Layout 与 Pipeline Barrier；
- Vertex/Index Buffer、Image、Sampler 与 Descriptor；
- Validation Layer、对象命名、RenderDoc 截帧和错误路径。

这一阶段对应 Vulkan Guide 的基础能力，但必须能独立解释每个对象的输入、输出、依赖关系和销毁顺序。

### P1：先建立性能与回归基线

在增加 PBR 效果前先建立最小测量能力：

- CPU Scope Timer 与每帧统计；
- Vulkan Timestamp Query，至少能测量完整 Frame 与主要 Pass；
- 固定分辨率、固定相机、固定 glTF 场景和 Release 构建；
- Draw Call、Triangle、Descriptor 与上传量统计；
- 保存基线截图、RenderDoc Capture 和运行环境。

后续每个渲染功能都必须同时提交画质变化与 GPU 成本，不能等效果全部完成后再补 Profiler。

### P2：真实 Metallic-Roughness PBR

这是渲染主线的核心作品：

1. 正确处理线性空间、sRGB 纹理与 Gamma；
2. 支持 glTF Base Color、Metallic、Roughness、Normal、Occlusion、Emissive；
3. 实现 Cook–Torrance BRDF：
   - GGX/Trowbridge-Reitz 法线分布函数；
   - Smith/Schlick-GGX 几何遮蔽项；
   - Schlick Fresnel；
   - 能量守恒的漫反射与镜面反射组合；
4. 构建 TBN 切线空间并支持法线贴图；
5. 支持方向光、点光源和距离衰减；
6. 提供材质球矩阵，直观看到 metallic/roughness 参数变化；
7. 提供 Base Color、Normal、Roughness、Metallic、NDF、Geometry、Fresnel 等调试视图。

必须能回答：为什么粗糙度改变高光宽度、金属为什么几乎没有漫反射、F0 从哪里来、三个微表面项各自解决什么问题，以及颜色空间错误为什么会让结果失真。

### P3：IBL、HDR 与后处理闭环

在直接光 PBR 正确后加入：

- HDR 环境贴图与 Cubemap；
- 漫反射 Irradiance Cubemap 或 SH；
- 镜面环境预过滤；
- Split-Sum BRDF LUT；
- 曝光控制与 Tone Mapping；
- 必要的 Bloom，对比开启前后的 GPU 时间。

这一阶段把 GAMES101 的 BRDF、渲染方程与 GAMES202 的预计算光照知识落实为实时 GPU 管线。

### P4：阴影系统

按实用性而不是论文难度推进：

1. 基础 Shadow Map 与可视化；
2. Constant/Slope/Normal Bias 对比，解释 Shadow Acne 与 Peter Panning；
3. PCF 软化；
4. CSM 方向光级联阴影、稳定化和级联可视化；
5. PCSS 作为软阴影进阶；
6. D19—D21 完整实现 VSM/SAT、VSSM、MSM 的研究与对照 Sample，验证统计量、近似、漏光、精度和成本；不要求同时进入默认渲染路径。

每种方案都要保存质量、显存和 GPU 时间对比，不能只展示截图。

### P5：Render Graph 与资源生命周期

把多 Pass 渲染从手写调用顺序升级为可验证的引擎模块：

- Pass 声明读写资源；
- 构建 DAG、拓扑排序和循环检测；
- 自动推导 Image Layout 与 Barrier；
- 资源生命周期分析和临时资源复用；
- Graphviz/ImGui 显示 Pass、资源和依赖；
- 用 Shadow、Depth、Lighting、Post-process 验证真实多 Pass 场景。

同时实现代际 Handle、Resource Registry 和延迟销毁，明确 CPU 对象、Vulkan Handle 与 VMA Allocation 的所有权。

### 持续要求：性能与诊断证据

- CPU Scope Timer 与每帧统计；
- Vulkan Timestamp Query；
- Draw Call、Triangle、Descriptor、显存和上传量统计；
- RenderDoc 捕获与资源命名；
- 固定场景 Benchmark；
- 优化前后报告，记录画质、CPU 时间、GPU 时间与显存变化。

性能数据必须附带固定分辨率、固定场景、硬件、构建模式和采样方法。

### P6：完整专题覆盖，按依赖分批整合

用户要求的所有专题都需要明确实验与证据，不按“挑一两个”删减。分批顺序在 D 总纲中，第一批是 D01—D03：

- 基础正确性：裁剪、共享边、纹理足迹/过滤、深度精度与 CPU/GPU 对照；
- 几何与参考：包围体、BVH、八叉树、采样/MIS、离线路径追踪；
- 材质与积分：SH/PRT、IBL 预计算、Kulla–Conty、Disney 清漆/各向异性/织物、LTC；
- 管线与灯：Forward/Deferred、Tiled/Clustered；
- 阴影：VSM、SAT、VSSM、MSM，保留 PCF/CSM/PCSS 基线；
- 反射/GI：SSR、SSGI、RSM、LPV、体素/VCT、SDF；
- 信号重建：HW5 时空降噪、SVGF，保留 B20 的 TAA 关联；
- 风格化：NPR 卡通、两类描边与排线。

B20 中 SSAO/GTAO/TAA 按深入学习推进，GPU Driven 与 QEM LOD 保留基本概念范围。PBD/XPBD 可在未来另行确认，不是本次必做项。每章先形成独立实验，再按 C 模块归属整合；默认渲染路径选择一种阴影与明确间接光来源，不把相同能量重复相加。

## 课程知识到项目模块的映射

| 学习来源 | 明确专题 | EmberFrame 落点 |
|---|---|---|
| GAMES101 / CPU 光栅化扩展 | 齐次裁剪、Top-left、过滤、深度精度 | D01—D05，CPU/GPU 对照及正式 Pipeline |
| GAMES101 / 空间查询 | 包围体、BVH、八叉树 | D06—D08，参考相交与 C8 可见性 |
| GAMES101 / 光传输 | 蒙特卡洛、重要性/MIS、路径追踪 | D09—D10，离线参考与烘焙工具，不再仅是可选名词 |
| GAMES101 / GAMES202 | BRDF、渲染方程、PBR/IBL | B15—B17、D13，C4/C5 材质 |
| GAMES202 | SH / PRT / 采样预计算 | D09、D11—D13，传输缓存、系数与 GPU 求值 |
| GAMES202 | Kulla–Conty、Disney、LTC | D14—D16，能量与面积光实验 |
| GAMES202 / 工业管线 | Forward/Deferred、Tiled/Clustered | D17—D18，C5/C6/C8 灯列表与 Pass |
| GAMES202 | VSM/SAT/VSSM/MSM | D19—D21，统计阴影与参考比较 |
| GAMES202 | SSR / SSGI | D22—D23，屏幕追踪、分项合成 |
| GAMES202 HW5 / SVGF | 空间过滤、重投影、历史累积、方差 | D24—D25，C6 历史资源与降噪 |
| GAMES202 | RSM / LPV | D26—D27，受光样本、SH 网格与传播 |
| GAMES202 / 三维查询 | 体素、VXGI/VCT、SDF | D28—D30，3D 资源、层级查询与追踪 |
| GAMES202 | NPR | D31，风格材质、描边与后处理 |
| 全部渲染专题 | 固定测试、失败场景、成本与误差 | D32、C10/C12，实验室与作品集 |
| GAMES104 | Platform、Tick、Runtime | Platform/Runtime/事件与时间系统 |
| GAMES104 | RHI、Render Scene、Frame Graph | Vulkan Backend、Render Scene、Render Graph |
| GAMES104 | 生命周期、序列化、任务图 | Resource Registry、异步上传、Scene/Job System |

GAMES101 绳子/PBD/XPBD 不属于本次渲染专题范围；后续若拓展单独确认。课程“学过”只影响复习起点，不自动计入 EmberFrame 已实现能力。

## 面试知识验收矩阵

每个模块同时通过五类验收：

1. **原理：** 不看源码复述输入、计算、输出、假设和限制；
2. **API：** 指出该算法在 Vulkan 的 Shader、Descriptor、Image、Pass 和同步位置；
3. **工程：** 说明所有权、模块边界、创建与逆序销毁；
4. **调试：** 能用 Validation Layer、RenderDoc 和中间结果视图定位错误；
5. **性能：** 给出固定场景中的 CPU/GPU 时间、显存或 Draw Call 对比。

PBR 阶段至少能回答：

- Forward 与 Deferred Rendering 的数据与带宽差异；
- Vertex Shader、Rasterizer、Fragment Shader 各自负责什么；
- 为什么法线需要逆转置矩阵，法线贴图为什么需要 TBN；
- sRGB 纹理为什么要在线性空间参与光照；
- Cook–Torrance 中 D、G、F 三项的输入和作用；
- metallic、roughness、base color 如何共同决定漫反射和镜面反射；
- IBL 为什么需要 Irradiance、Prefiltered Environment 与 BRDF LUT；
- Shadow Acne、Peter Panning、PCF、PCSS 和 CSM 分别解决什么；
- Descriptor、Pipeline Layout、Push Constant 与 Uniform Buffer 的边界；
- Fence、Semaphore、Barrier 分别同步谁与谁；
- 怎样判断当前瓶颈在 CPU、顶点处理、片元处理、带宽还是同步等待。

### D 专题还要能讲清什么

- D01—D08：从 Clip 到覆盖/采样/深度的顺序；包围体与两种层次结构各存什么、查什么；
- D09—D16：目标积分、PDF/度量与预计算假设；SH 表示和 PRT 传输区别；能量补偿与场景反弹区别；LTC 不等于软阴影；
- D17—D21：灯列表如何交给着色；G-buffer 的带宽成本；VSM 上界、SAT 求和、VSSM 条件均值与 MSM 的额外约束；
- D22—D25：屏幕数据丢失什么；源颜色、输出和历史谁读写；拒绝历史与钳制的区别；降噪为何不能修复缺失几何；
- D26—D31：各 GI 表示存的真实量、哪一步估计可见性、漏光/离散误差来源；NPR 的风格目标与 PBR 物理目标分开；
- D32：同一场景中的方法取舍、测量条件、失败场景、个人实现边界，不背一串名词冒充实现经验。

面试准备以每章的“能够回答的问题”和实际报告为准，不未经岗位资料验证就声称某算法必考。尚未实现时应明确回答学习/实验计划，不能编造性能数字。

## 可进入简历的最低证据

- 一个可交互 PBR 材质查看器，支持 glTF Metallic-Roughness 和调试视图；
- 一套至少包含 Shadow、Lighting、Post-process 的多 Pass 管线；
- 一个真正由个人实现的 Render Graph 或资源生命周期模块；
- 一份 RenderDoc 截帧说明；
- 一份固定场景 Benchmark 与优化前后数据；
- 清楚标注 Vulkan Guide 上游基线与个人提交边界。

只有效果截图而没有原理、调试和性能证据的功能，不进入简历主描述。
