# 2026 年游戏引擎 / 实时渲染实习能力研究

> 调研日期：2026-09-15  
> 适用目标：游戏引擎、实时渲染、图形开发、GPU/图形系统实习与初级岗位  
> 项目落点：EmberFrame Engine

## 1. 研究口径

本文只使用以下一手来源：

- 公司官方招聘页，或公司用于正式招聘的 Workday / Ashby 页面；
- Khronos Vulkan / glTF 规范、Vulkan Guide 与官方 Samples；
- Epic、Unity、NVIDIA、AMD 的官方技术文档；
- Disney、Epic/NVIDIA 作者公开的 SIGGRAPH/GDC 原始论文或课程讲义。

本文把证据明确分为两类：

- **岗位需求证据**：证明招聘方正在考察或看重什么；
- **技术实现依据**：说明相关能力在 EmberFrame 中应如何正确实现和验证。

岗位页面会随招聘进度下线，因此本文记录的是 2026-09-15 可核实的需求快照，而不是对所有公司的统计普查。高级岗位只用于判断行业技术方向，不会冒充实习生硬门槛。

## 2. 结论摘要

### 2.1 最重要的能力排序

| 优先级 | 能力 | 对实习求职的意义 | EmberFrame 应提供的证据 |
|---|---|---|---|
| P0 | 现代 C++、数据结构、数学与调试 | 多个引擎/图形岗位的共同基础 | 清晰所有权、RAII、测试、可解释代码 |
| P0 | GPU 图形管线与 Shader | 渲染岗位最常见的知识入口 | 从顶点输入到颜色输出的 Vulkan 闭环与截帧 |
| P0 | Vulkan 资源、命令与同步 | 证明理解现代显式图形 API | Swapchain、Command Buffer、Descriptor、Barrier、帧同步 |
| P0 | Metallic-Roughness PBR | 最值得作为项目视觉与理论核心的模块 | glTF 材质、Cook–Torrance、法线贴图、线性/HDR 调试视图 |
| P0 | 性能分析与验证 | 区分“能跑”与“工程上可信” | Timestamp、固定场景 Benchmark、Nsight/RGP 截图与前后对比 |
| P1 | IBL、HDR、曝光与 Tone Mapping | 让 PBR 在没有大量解析光源时仍成立 | Irradiance、Prefiltered Cubemap、BRDF LUT、HDR 后处理 |
| P1 | Shadow Map、Bias、PCF、CSM | 高频渲染问题，能体现多 Pass 和质量/性能权衡 | 阴影调试视图、Bias 对照、PCF/CSM GPU 时间 |
| P1 | GPU 资源生命周期 | 体现真正的引擎能力 | VMA、代际 Handle、上传、延迟销毁、显存统计 |
| P1 | Render Graph | 体现多 Pass 依赖、Barrier 与临时资源管理 | Pass DAG、循环检测、自动状态转换、图可视化 |
| P2 | Bloom、FXAA/TAA、SSAO 等后处理 | 可形成完整画面，但不应早于 PBR 和分析工具 | 只选一到两个效果，提供开关、调试视图与耗时 |
| P2 | 光追、GI、神经渲染 | 有前沿价值，但不是当前本科实习路线的必要前置 | 作为后续差异化，不阻塞主线 |

### 2.2 对 EmberFrame 的总判断

Vulkan Guide 应作为**可运行的 Vulkan 执行基线**，而不是简历成果本身。GAMES101 提供变换、光栅化、BRDF 与渲染方程；GAMES202 提供阴影、IBL/预计算光照和时域算法；GAMES104 提供生命周期、资源系统和 Render Graph 的组织思路。EmberFrame 的价值在于把三类课程知识放进同一条可运行、可调试、可量化的 Vulkan 管线，并明确个人实现边界。

最强的作品组合不是堆十种效果，而是：

1. 一个正确且可调试的 glTF Metallic-Roughness PBR Viewer；
2. 一条包含 Shadow、Lighting、Post-process 的多 Pass 管线；
3. 一个个人实现的资源生命周期或 Render Graph 模块；
4. 一份固定场景的 CPU/GPU 性能报告和失败案例复盘。

## 3. 岗位需求证据

### 3.1 实习与 Early Career 的共同门槛

Epic 的 Early Career 指南明确把 C++、Unreal Engine、数据结构与算法列为游戏/引擎编程的重要基础，并把 Rendering 作为独立编程方向；其招聘建议还要求简历只展示相关项目，并能详细说明本人贡献。  
**岗位需求证据：** [Epic Career Paths](https://www.epicgames.com/site/earlycareers/career-paths)、[Epic Internships](https://www.epicgames.com/site/earlycareers)

NVIDIA 2026 Tegra Graphics Intern 要求 C/C++，工作包含 Vulkan/OpenGL/NVN 驱动问题复现、调试、测试与规范维护；加分项是现代图形 API 和对运行时/驱动行为的理解。NVIDIA 2026 Systems Software Intern 进一步列出图形理论、实现与优化、计算机体系结构、操作系统和测试调试。  
**岗位需求证据：** [NVIDIA Graphics Engineer Intern, Tegra System Software](https://nvidia.wd5.myworkdayjobs.com/en-US/NVIDIAExternalCareerSite/job/Graphics-Engineer-Intern--Tegra-System-Software---Summer-2026_JR2011824)、[NVIDIA 2026 Systems Software Engineering Internships](https://nvidia.wd5.myworkdayjobs.com/en-US/NVIDIAExternalCareerSite/job/NVIDIA-2026-Internships--Systems-Software-Engineering_JR2003204)

Genesis AI 的 Rendering Engineer Internship 明确出现 C++、HLSL、GPU 编程、Vertex/Fragment/Compute Shader、PBR、实时优化，并将 Vulkan、光追和实时 GI 列为加分能力。  
**岗位需求证据：** [Genesis AI Rendering Engineer Internship](https://jobs.ashbyhq.com/genesis-ai/072f65f2-3913-4502-a211-6c486bc3ceb1)

Meshy 的 Graphics Engineer Intern 强调现代 C++、GPU 编程、数学基础、渲染/几何处理、高性能和内存效率，还要求能构建较完整的图形系统、资产工具或管线，并产出 Demo、文档、开源成果或技术报告。  
**岗位需求证据：** [Meshy Graphics Engineer Intern](https://jobs.ashbyhq.com/meshy/e0805db2-f584-47d2-9b9e-ef31ced8c11f)

Roblox 的通用 Software Engineer Intern 会在 Engine、Rendering 等团队匹配，强调端到端拥有一个项目；其当前 Rendering Engineer 岗位把 C++、着色语言、3D 数学、图形 API、性能和内存推理列为核心能力。后者不是实习门槛，但能说明渲染团队的长期能力模型。  
**岗位需求证据：** [Roblox Software Engineer Intern](https://careers.roblox.com/jobs/8072713?gh_jid=8072713)、[Roblox Rendering Engineer](https://careers.roblox.com/jobs/8036287?gh_jid=8036287)

### 3.2 性能不是附加项

NVIDIA 上海 2026 Performance Engineering Intern 的工作包括图形 Benchmark、系统监控数据采集、自动化和性能报告；NVIDIA Tegra 实习也直接要求驱动问题复现和验证。由此可见，实习作品应包含可重复数据，而不只是截图。  
**岗位需求证据：** [NVIDIA Performance Engineering Intern - 2026](https://nvidia.wd5.myworkdayjobs.com/en-US/NVIDIAExternalCareerSite/job/Performance-Engineering-Intern---2026_JR2008055)、[NVIDIA Tegra Graphics Intern](https://nvidia.wd5.myworkdayjobs.com/en-US/NVIDIAExternalCareerSite/job/Graphics-Engineer-Intern--Tegra-System-Software---Summer-2026_JR2011824)

Unity 当前高级图形岗位要求理解渲染管线、GPU 架构、现代图形 API、HLSL/GLSL，以及用 Nsight、PIX、RenderDoc 等工具分析 CPU/GPU/内存。它不能作为实习生硬门槛，但证明“实现效果 + 性能诊断”是行业成熟度方向。  
**岗位需求证据（行业方向，不是实习门槛）：** [Unity Senior Graphics Engineer](https://unity.com/de/careers/positions/7768650)

## 4. 技术能力与 EmberFrame 落地

## 4.1 GPU 管线与 Vulkan：必须能从数据流解释，而不只是会调用 API

### 应掌握

- Input Assembly 如何把顶点/索引组成图元；
- Vertex Shader 如何完成坐标变换并输出插值属性；
- 光栅器如何产生 Fragment，深度测试、混合和附件写入在哪里发生；
- Graphics Pipeline 的固定状态和可编程阶段；
- Swapchain 图像获取、命令录制、Queue Submit 与 Present；
- Fence、Semaphore、Pipeline Barrier 的不同作用；
- Stage Mask、Access Mask、Image Layout 如何共同表达执行和内存依赖；
- Descriptor Set、Uniform/Storage Buffer、Image Sampler、Push Constant 如何把数据送入 Shader；
- GLSL/HLSL/Slang 到 SPIR-V，再到 Vulkan Pipeline 的过程。

Vulkan 规范说明 Graphics Pipeline 由多个 Shader 阶段、固定功能阶段和 Pipeline Layout 组成；Command Buffer 记录绑定、绘制、调度和资源拷贝命令；没有显式同步时，提交的命令与内存副作用不能被应用程序想当然地视为有序可见。  
**技术实现依据：** [Vulkan Pipelines](https://docs.vulkan.org/spec/latest/chapters/pipelines.html)、[Vulkan Command Buffers](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html)、[Vulkan Synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)

Shader 的跨阶段输入输出要通过 Location/Component 等接口匹配，资源通过 DescriptorSet/Binding 与 Pipeline Layout 对应；Vulkan Shader Module 使用 SPIR-V。  
**技术实现依据：** [Vulkan Shader Interfaces](https://docs.vulkan.org/spec/latest/chapters/interfaces.html)、[Vulkan Shaders](https://docs.vulkan.org/spec/latest/chapters/shaders.html)、[Mapping Data to Shaders](https://docs.vulkan.org/guide/latest/mapping_data_to_shaders.html)、[Push Constants](https://docs.vulkan.org/guide/latest/push_constants.html)

### EmberFrame 验收物

- B4—B10 每个 Vulkan 对象都有创建者、使用者、依赖和逆序销毁说明；
- 开启 `VK_LAYER_KHRONOS_validation`，常规运行无 Validation Error；
- RenderDoc/Nsight 截帧中可定位 Vertex Buffer、Descriptor、Pipeline、Depth 与最终附件；
- 文档能把 A5/A6 的 CPU MVP、光栅化和透视插值映射到 GPU 阶段；
- 至少设计一次错误 Barrier 或错误 Descriptor 的复现与修复记录。

Validation Layer 在开发期用于捕获不正确 API 使用；Khronos 同时指出同步错误会产生难定位 Bug，过度同步又会让 GPU 无谓空闲。  
**技术实现依据：** [Vulkan Validation Overview](https://docs.vulkan.org/guide/latest/validation_overview.html)、[Vulkan Synchronization Guide](https://docs.vulkan.org/guide/latest/synchronization.html)

### 高频面试问题

- Fence 与 Semaphore 分别同步谁？为什么不能只用 `vkDeviceWaitIdle`？
- Pipeline Stage 与 Access Mask 为什么必须匹配真实生产者/消费者？
- Image Layout 是什么，为什么颜色附件转采样纹理需要 Barrier？
- Descriptor Set 与 Push Constant 分别适合什么数据？
- Frames in Flight 为什么要求每帧独立的 Fence、Command Buffer 和临时资源？
- CPU 软件渲染中的重心插值和深度测试，在 GPU 管线哪里发生？

## 4.2 PBR：应成为 EmberFrame 的第一渲染主线

### 为什么优先

PBR 在实习岗位中被直接点名，也能同时考察数学、Shader、材质、颜色空间、资产格式和调试能力。相比单独实现许多屏幕特效，一条正确的 PBR 管线更容易形成完整面试故事。

### 必须实现的最小正确集合

1. **材质输入**：glTF Metallic-Roughness 的 Base Color、Metallic、Roughness、Normal、Occlusion、Emissive；
2. **颜色空间**：Base Color/Emissive 从 sRGB 解码到线性空间，Metallic/Roughness/Normal 在线性数据意义下读取；所有光照在线性 HDR 空间计算，最后 Tone Map 并编码到显示空间；
3. **直接光 BRDF**：Diffuse + Cook–Torrance Specular，至少包含 GGX 法线分布 `D`、Smith 几何项 `G`、Schlick Fresnel `F`；
4. **金属/非金属**：非金属使用约 0.04 的 F0 并保留漫反射，金属由 Base Color 决定 F0 且不保留普通漫反射；
5. **能量守恒**：镜面反射增加时相应减少漫反射贡献；
6. **法线贴图**：正确构造或导入 Tangent，使用 TBN 把切线空间法线变换到着色空间；
7. **调试视图**：Base Color、Metallic、Roughness、Normal、NdotL、D/F/G、Diffuse、Specular、最终颜色可单独查看。

glTF 2.0 规范定义 Metallic-Roughness 材质及其 BRDF：Base Color 纹理是 sRGB，必须先解码到线性值；Metallic-Roughness 纹理是线性数据，G 通道为 Roughness、B 通道为 Metallic；其附录给出金属/介质混合、F0、Schlick Fresnel 与 BRDF 结构。  
**技术实现依据：** [glTF 2.0 Specification — Materials and BRDF](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#materials)

Disney 的原始课程讲义阐述了面向生产的微表面材质模型和“少量、直观、稳健参数”的设计目标；Epic 的 UE4 SIGGRAPH 讲义给出了实时 Cook–Torrance、GGX、Schlick 近似和后续 IBL 的工程化方案。  
**技术实现依据：** [Physically-Based Shading at Disney](https://disneyanimation.com/publications/physically-based-shading-at-disney/)、[Real Shading in Unreal Engine 4](https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf)

Epic 官方材质文档也将 Base Color、Roughness、Metallic、Specular 定义为 PBR 直接相关输入，并解释金属与非金属及 Roughness 的视觉含义。  
**技术实现依据：** [Epic Physically Based Materials](https://dev.epicgames.com/documentation/unreal-engine/physically-based-materials-in-unreal-engine)

### EmberFrame 验收物

- 使用公开 glTF 测试资产，不再以 OBJ 作为最终材质管线；
- 输出 Metallic × Roughness 材质球矩阵；
- 增加直射光开关和每个 BRDF 分项调试视图；
- 对比当前上游 `mesh_pbr.frag` 与个人实现的画面、Shader 输入和 GPU 时间；
- 用文档记录至少三个错误案例：sRGB 重复转换、Normal Map 方向错误、Roughness 通道/感知粗糙度错误。

### 高频面试问题

- BRDF 是什么，为什么需要满足能量守恒和 Helmholtz Reciprocity？
- Cook–Torrance 中 D、F、G 分别描述什么？
- Roughness 怎样影响高光形状？为什么实现中经常使用 `alpha = roughness²`？
- 金属为什么通常没有普通漫反射？Base Color 对金属和非金属分别代表什么？
- 为什么光照计算必须在线性空间？哪些纹理应按 sRGB 读取？
- Normal Map 为什么需要 Tangent/TBN？非均匀缩放后法线为什么要用逆转置矩阵？

## 4.3 IBL、HDR 与 Tone Mapping：PBR 的必要闭环

### 应掌握与实现

- HDR Cubemap 的采样和预计算；
- Diffuse Irradiance Cubemap，或用 SH 表达低频漫反射环境光；
- Specular Prefiltered Environment Map，以 Roughness 选择 Mip；
- 2D BRDF LUT；
- Split-Sum Approximation 的假设与误差；
- HDR Scene Color、曝光和 Tone Mapping；
- 最终 Linear → 显示输出转换。

Epic 的实时 PBR 讲义给出 Specular IBL 的 Split-Sum 近似：把环境卷积结果与 BRDF 积分项拆开预计算。Khronos 官方 Vulkan 教程的 glTF PBR 实现也明确包含预过滤 Cubemap Mip、IBL 参数、BRDF 的 D/F/G、能量守恒、曝光与 Gamma。  
**技术实现依据：** [Real Shading in Unreal Engine 4](https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf)、[Khronos Vulkan Tutorial — PBR for glTF](https://github.khronos.org/Vulkan-Site/tutorial/latest/Building_a_Simple_Engine/Loading_Models/05_pbr_rendering.html)

Epic 的 Reflection Environment 文档说明间接镜面反射、Cubemap Mip 与 Roughness 的联系，也指出金属尤其依赖环境反射。  
**技术实现依据：** [Epic Reflection Captures](https://dev.epicgames.com/documentation/unreal-engine/reflections-captures-in-unreal-engine)

### EmberFrame 验收物

- 可切换 Direct Only、Diffuse IBL、Specular IBL、Combined；
- 可显示 Irradiance Cubemap、不同 Roughness 的 Prefilter Mip 和 BRDF LUT；
- HDR Render Target 与单独 Tone Mapping Pass；
- 固定相机下对不同曝光和 Roughness 生成对照图；
- 用 GPU Timestamp 分别记录 PBR、IBL、Tone Mapping 成本。

### 高频面试问题

- 只有直接光为什么金属阴影处容易全黑？
- Diffuse IBL 与 Specular IBL 为什么不能用同一张未经处理的 Cubemap？
- Roughness 为什么对应 Prefiltered Environment 的不同 Mip？
- BRDF LUT 的两个维度通常是什么？Split-Sum 拆分了什么积分？
- HDR 为什么不能直接写入普通 8-bit Swapchain？Tone Mapping 与 Gamma 的顺序是什么？

## 4.4 阴影：从正确性、质量和性能三方面回答

### 实施顺序

1. 基础 Shadow Map 与深度可视化；
2. Constant Bias、Slope-Scale Bias 与 Peter Panning/Acne 对照；
3. 硬件比较采样 + PCF；
4. Directional Light 的 CSM，包含 Cascade 划分、稳定化与边界可视化；
5. PCSS 或 VSM/VSSM 只作为进阶对照，不强制进入默认路径。

PCF 的核心是对多个深度比较结果求平均，而不是先平均深度再比较；采样数与质量/成本之间存在直接权衡。  
**技术实现依据：** [NVIDIA GPU Gems — Shadow Map Antialiasing](https://developer.nvidia.com/gpugems/gpugems/part-ii-lighting-and-shadows/chapter-11-shadow-map-antialiasing)

PCSS 包含 Blocker Search、Penumbra Size Estimation 和 Shadow Filtering 三步；VSM/SAVSM 可以过滤统计量，但会引入泄漏等不同问题。  
**技术实现依据：** [NVIDIA GPU Gems 3 — Summed-Area Variance Shadow Maps](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-8-summed-area-variance-shadow-maps)、[NVIDIA Variance Shadow Mapping Paper](https://developer.download.nvidia.com/SDK/10.5/direct3d/Source/VarianceShadowMapping/Doc/VarianceShadowMapping.pdf)

Epic 文档说明 CSM 通过按相机距离划分视锥，让近处得到更高阴影分辨率，并明确 Cascade 数量带来质量/性能权衡。  
**技术实现依据：** [Epic Shadowing — Cascaded Shadow Maps](https://dev.epicgames.com/documentation/en-us/unreal-engine/shadowing-in-unreal-engine)

### EmberFrame 验收物

- Shadow Map、Cascade Index、Bias、PCF Kernel 调试视图；
- 阴影 Acne 与 Peter Panning 的可复现场景；
- 1×1、3×3、5×5 PCF 的 GPU 时间和边缘质量对比；
- CSM 的 Cascade 数量、分辨率、显存和 GPU 时间表。

### 高频面试问题

- Shadow Map 为什么要从光源视角渲染一次？
- Shadow Acne 为什么出现？Bias 太大会出现什么？
- PCF 为什么平均“比较结果”而不是平均深度？
- CSM 为什么能改善方向光近处阴影？Cascade 接缝和 Shimmering 如何处理？
- PCF、PCSS、VSM 的主要质量与性能差异是什么？

## 4.5 后处理：做完整链路，不做效果清单

### 推荐最小集合

```text
HDR Scene Color
→ Bright-pass / Bloom Downsample
→ 多级 Blur / Upsample
→ 合成 Bloom
→ Exposure + Tone Mapping
→ 可选 FXAA
→ UI / Present
```

Bloom、曝光和 Tone Mapping 是实时引擎常见后处理；Epic 还列出 FXAA、TAA、MSAA、TSR 等抗锯齿方案，并强调质量与 GPU 成本的可伸缩配置。  
**技术实现依据：** [Epic Post Process Effects](https://dev.epicgames.com/documentation/unreal-engine/post-process-effects-in-unreal-engine)、[Epic Anti-Aliasing and Upscaling](https://dev.epicgames.com/documentation/unreal-engine/anti-aliasing-and-upscaling-in-unreal-engine)

### EmberFrame 验收物

- 每个 Pass 可独立开关并显示输入/输出；
- Bloom 阈值、Mip 数与耗时对比；
- Tone Mapping 前后的 HDR/LDR 直方图或像素读数；
- 若做 TAA，必须解释 Jitter、Motion Vector、History、Reprojection、Disocclusion 和 Ghosting；不能只做历史颜色混合。

### 不建议现在优先做

- 同时实现 Bloom、SSAO、SSR、TAA、景深、运动模糊；
- 在没有 Motion Vector 和稳定历史管理时强行做 TAA；
- 把 Tone Mapping、Gamma 和 UI 合成混在一个无法观察的 Shader 中。

## 4.6 性能分析：先测量，再优化

### 最小性能体系

- CPU：帧时间、逻辑更新、命令录制、提交等待、资产解析/上传；
- GPU：每个 Render Pass 的 Timestamp；
- 帧统计：Draw Call、Triangle、Descriptor/Pipeline Bind、Barrier 数；
- 显存：Heap Budget、使用量、资源种类与峰值；
- 场景：固定相机、固定分辨率、Release 构建、关闭 Validation Layer 后测性能；
- 报告：平均值、P50/P95/P99、测试硬件与驱动、优化前后截图和数据。

Vulkan Timestamp Query 是应用追踪 GPU 命令执行时间的标准机制。  
**技术实现依据：** [Vulkan Timestamp Queries](https://docs.vulkan.org/spec/latest/chapters/queries.html#queries-timestamps)

Nsight Graphics 的 GPU Trace 用于观察 GPU 单元利用率、Queue 同步、吞吐瓶颈和低利用率；Shader Profiler 支持 Vulkan 与 GLSL/HLSL/Slang，可查看 Shader Stall。AMD RGP 可观察 Queue、Event Timing、Wavefront Occupancy、Cache、Barrier 与 Shader 指令热点。  
**技术实现依据：** [NVIDIA Nsight GPU Trace](https://docs.nvidia.com/nsight-graphics/UserGuide/gpu-trace-overview.html)、[NVIDIA Shader Profiler](https://docs.nvidia.com/nsight-graphics/UserGuide/shader-profiler.html)、[AMD Radeon GPU Profiler](https://gpuopen.com/rgp/)

Khronos 性能样例说明频繁分配/释放 Command Buffer 很昂贵，按帧重置 Command Pool 通常更合适；过多且过严的 Barrier 也会损害性能。  
**技术实现依据：** [Khronos Command Buffer Usage Sample](https://github.khronos.org/Vulkan-Site/samples/latest/samples/performance/command_buffer_usage/README.html)、[Khronos Vulkan Performance Samples](https://docs.vulkan.org/samples/latest/samples/README.html)

### 高频面试问题

- 如何判断 CPU Bound 还是 GPU Bound？只看 FPS 为什么不够？
- GPU Timestamp 测到的是哪段时间？如何避免查询结果跨帧误用？
- Occupancy 低一定代表性能差吗？Barrier 为什么会制造 Pipeline Bubble？
- 为什么 Benchmark 要用 Release、固定场景并记录驱动/硬件？
- 优化前后如何证明画质没有回退？

## 4.7 GPU 资源生命周期：引擎岗位的重要区分项

### 应实现

- Buffer/Image 与底层 Allocation 的所有权关系；
- VMA Sub-allocation、Staging Upload 和 Heap Budget；
- 代际 Handle，检测悬空句柄与重复释放；
- 每帧 Deletion Queue / Timeline 值，确保 GPU 不再使用后才销毁；
- 默认资源和加载失败占位资源；
- 上传队列、资源状态和热重载边界；
- 显存统计与泄漏诊断。

Khronos 明确把 Sub-allocation 视为 Vulkan 的一等方法，并指出系统/驱动级频繁分配通常很慢；离散 GPU 常用 Staging Buffer 把数据传至 Device Local Memory。  
**技术实现依据：** [Vulkan Memory Allocation Guide](https://docs.vulkan.org/guide/latest/memory_allocation.html)

AMD 的 VMA 官方文档覆盖 Buffer/Image 分配、Memory Budget、Pool、映射、资源别名、统计、泄漏诊断和碎片整理。别名资源必须具有不重叠生命周期并配合正确同步。  
**技术实现依据：** [Vulkan Memory Allocator 3.4](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/)、[VMA Resource Aliasing](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/resource_aliasing.html)、[VMA Memory Budget](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/staying_within_budget.html)

### 高频面试问题

- 为什么不能在 CPU 释放对象时立即销毁对应 VkBuffer/VkImage？
- Handle 为什么需要 Generation？如何检测旧 Handle 指向新对象？
- Staging Buffer、Host Visible、Device Local 分别适合什么？
- 为什么不为每个小 Buffer 单独调用 `vkAllocateMemory`？
- Transient Resource 如何复用显存，复用前为什么需要同步？

## 4.8 Render Graph：在真实多 Pass 之后实现

### 为什么有价值

Render Graph 不是为了“画图好看”，而是把每个 Pass 对资源的读写声明转成：

```text
Pass / Resource 声明
→ 构建依赖 DAG
→ 拓扑排序与循环检测
→ 计算资源首次/最后使用
→ 生成 Barrier 与 Layout Transition
→ 剔除未使用 Pass
→ 执行并采集每 Pass 时间
```

Epic 的 RDG 官方文档说明 Render Graph 能自动处理异步计算 Fence、临时资源生命周期与内存别名、子资源转换、并行命令录制、无用 Pass 剔除、依赖验证和图可视化。资源只能在声明使用它的 Pass 中访问。  
**技术实现依据：** [Unreal Engine Render Dependency Graph](https://dev.epicgames.com/documentation/unreal-engine/render-dependency-graph-in-unreal-engine)

Render Graph 的自动 Barrier 必须建立在 Vulkan 正确同步模型上：应用负责明确资源访问顺序和可见性，Image Layout Transition 需要资源级 Barrier。  
**技术实现依据：** [Vulkan Synchronization Specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)、[Vulkan Pipeline Barriers and Layout Transitions](https://github.khronos.org/Vulkan-Site/tutorial/latest/Synchronization/Pipeline_Barriers_Transitions/01_introduction.html)

### EmberFrame 最小范围

- `PassNode`、`ResourceNode` 和 Read/Write/ReadWrite 声明；
- DAG 构建、拓扑排序、循环依赖报错；
- Image/Buffer 状态跟踪；
- 自动生成 Vulkan 1.3 `vkCmdPipelineBarrier2`；
- Dead Pass Culling；
- Graphviz 或 ImGui 图，显示 Pass、资源、生命周期、Barrier；
- 用 Shadow → Lighting → Bloom/ToneMap → Present 的真实链路验收。

### 暂不追求

- 第一版就实现跨 Queue 自动调度；
- 第一版就实现完整 Subresource 级别别名优化；
- 为不存在的 DirectX/Metal 后端设计庞大 RHI；
- 在只有一个 Pass 时提前造 Render Graph。

### 高频面试问题

- Render Graph 如何从资源读写推出 Pass 依赖？
- 为什么需要检测环？拓扑排序结果是否唯一？
- 如何计算资源生命周期并复用 Transient Memory？
- 自动 Barrier 需要记录哪些状态？如何避免过度同步？
- External、Persistent、Transient Resource 的生命周期有什么不同？

## 5. 课程知识如何融入 EmberFrame

| 知识来源 | 应融入的高价值内容 | 项目落点 |
|---|---|---|
| GAMES101 | MVP、光栅化、插值、深度、纹理、BRDF、渲染方程 | CPU Renderer 对照；GPU Pipeline；Cook–Torrance PBR |
| GAMES101 | Path Tracing/BVH | 可选离线参考图，用来校验材质或作为后续 Sample，不阻塞主线 |
| GAMES202 | Shadow Map、Bias、PCF、PCSS、VSM/VSSM | 默认路径做 Bias+PCF+CSM；其余做质量/性能研究 Sample |
| GAMES202 | SH、PRT、环境光照 | Diffuse IBL/SH 与预计算验证 |
| GAMES202 | SSAO、SSR、时域方法 | PBR 主线后只选一个做差异化扩展 |
| GAMES104 | Platform、Tick、生命周期 | Platform / Runtime / Event / Time |
| GAMES104 | RHI、Render Scene、Frame Graph | Vulkan Backend、Render Scene、Render Graph |
| GAMES104 | 资源、任务、序列化 | Resource Registry、上传队列、Job System、Scene |
| Vulkan Guide | 可运行 Vulkan、VMA、glTF、Descriptor、Pipeline、ImGui | 上游基线和 API 学习样本；个人改造必须用独立提交和验证证明 |

## 6. 推荐实施顺序

### 阶段 1：补齐 Vulkan 可解释闭环

完成 B3—B12：事件/时间、Instance/Device、Swapchain、Command/Sync、Dynamic Rendering、Shader/Pipeline、Buffer/Image/Descriptor、glTF、Validation 与 Timestamp。

退出条件：能从一个 glTF Primitive 开始，解释它如何通过 Descriptor、Shader、Rasterization、Depth 和 Swapchain 成为屏幕像素；能用截帧工具找到对应资源。

### 阶段 2：把性能基线提前建立

在增加 PBR 前就加入 CPU Scope、GPU Timestamp、固定场景与统计输出。以后每一个效果都必须提交“画质变化 + GPU 成本”。不要等所有效果完成后再补 Profiler。

### 阶段 3：真实 PBR Viewer

按以下顺序实现：

```text
线性颜色空间
→ glTF Metallic-Roughness 输入
→ GGX/Smith/Schlick 直接光
→ 法线贴图/TBN
→ 分项调试视图
→ 材质球矩阵与错误案例
```

这是最应优先完成并写入简历的渲染模块。

### 阶段 4：IBL + HDR + Tone Mapping

加入 Irradiance、Prefiltered Cubemap、BRDF LUT、HDR Scene Color、曝光和 Tone Mapping，让 PBR 形成完整视觉闭环。

### 阶段 5：阴影和最小后处理

实现 Bias 可视化、PCF、CSM，再加入 Bloom + Tone Mapping。此时已经形成至少四个真实 Pass，足以驱动 Render Graph。

### 阶段 6：资源生命周期与 Render Graph

资源系统可与 PBR/glTF 同步推进；Render Graph 应在 Shadow、Lighting、Post-process 已经真实存在后接入。第一版重正确性、调试和可视化，不急着做 Async Compute。

### 阶段 7：只选一个差异化扩展

根据投递岗位选择：

- 偏渲染：SSAO/GTAO、TAA 或 GPU Driven Culling；
- 偏引擎：异步资产上传、Job System 或 Shader 热重载；
- 偏算法：QEM LOD、BVH/Path Tracing 或 PBD/XPBD。

## 7. 简历完成门槛

满足以下条件后，EmberFrame 才适合作为引擎/渲染实习核心项目：

- 可运行的 Vulkan glTF PBR Viewer；
- 正确的 Metallic-Roughness、Normal Map、直接光、IBL、HDR 与 Tone Mapping；
- Shadow Bias + PCF，最好再有 CSM；
- 至少一个真正由本人实现的引擎模块：Resource Registry 或 Render Graph；
- Validation Layer 常规运行无错误；
- 至少一份 Nsight/RGP/RenderDoc 截帧分析；
- 固定场景的 GPU Timestamp 与优化前后数据；
- 文档明确标注 Vulkan Guide 上游代码与个人贡献；
- 能现场解释一个 Bug、一次性能误判和一次架构取舍。

未完成前，不应在简历中写“自主研发完整 Vulkan 游戏引擎”；可以写“基于 Vulkan Guide 基线，正在实现/已实现具体模块”，并只列已验收内容。

## 8. 推荐简历表述模板（仅在完成后使用）

> 基于 Vulkan 1.3 构建实时渲染学习引擎，在保留 Vulkan Guide 上游边界的基础上，个人实现 glTF Metallic-Roughness PBR 管线，完成 GGX/Smith/Schlick 直接光、法线贴图、Diffuse/Specular IBL、HDR 与 Tone Mapping，并提供材质分项调试视图。

> 实现基于资源读写声明的 Render Graph，支持 Pass DAG、拓扑排序、循环检测、Vulkan Barrier/Layout 自动转换、无用 Pass 剔除与图可视化，以 Shadow、Lighting、Bloom/ToneMap 多 Pass 验证正确性。

> 建立 CPU/GPU 性能诊断链路，使用 Vulkan Timestamp 与 Nsight/RGP 分析 Pass 耗时、Barrier 和 Shader 瓶颈，在固定场景记录优化前后帧时间、显存和画质回归结果。

以上文字只能保留已经完成并能当场讲清的部分。

## 9. 面试前自检题

如果以下问题不能不用背稿讲清，就说明对应模块还不能写进简历：

1. 从 CPU 的 Vertex/Index Buffer 到 Fragment Output，Vulkan 数据经历了什么？
2. 为什么投影后的属性需要透视正确插值？GPU 替你完成了哪部分？
3. Vulkan 中 Fence、Binary/Timeline Semaphore、Barrier 分别解决什么问题？
4. PBR 的 D、F、G 是什么？Metallic 与 Roughness 如何改变最终光照？
5. Base Color 为什么通常是 sRGB，而 Roughness/Metallic 不应该按 sRGB 解码？
6. IBL 为什么需要 Irradiance、Prefiltered Cubemap 和 BRDF LUT？
7. Shadow Bias 为什么同时会造成 Acne 和 Peter Panning 的权衡？
8. PCF、PCSS、VSM、CSM 各自解决什么问题，成本在哪里？
9. HDR、Exposure、Tone Mapping、Gamma 的正确顺序是什么？
10. 如何证明某一帧是 CPU Bound 还是 GPU Bound？
11. GPU 仍在使用资源时，为什么不能立即销毁？
12. Render Graph 怎样自动推出 Barrier 和资源生命周期？
13. 你的实现中哪部分来自 Vulkan Guide，哪部分是个人设计与代码？
14. 你做过哪次性能优化，数据、工具、假设和结果分别是什么？

## 10. 最终取舍

对当前 EmberFrame，最合理的主线是：

```text
Vulkan 可解释闭环
→ 性能基线
→ glTF Metallic-Roughness PBR
→ IBL + HDR + Tone Mapping
→ Bias + PCF + CSM
→ Bloom
→ Resource Lifetime
→ Render Graph
→ 一个差异化扩展
```

不建议当前把精力分散到完整编辑器、物理、动画、全套后处理、光追和神经渲染。对于实习筛选，**正确、可解释、可测量、能明确证明个人贡献的 PBR 多 Pass Vulkan Renderer**，比功能数量更多但无法验证的“全功能引擎”更有含金量。
