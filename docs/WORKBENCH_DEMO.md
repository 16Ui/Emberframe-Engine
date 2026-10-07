# EmberFrame 工作台：三分钟演示与工程证据

新版简历的五条逐项演示，请使用 [简历演示操作稿](RESUME_DEMO.md) 与根目录 `Start-Resume-Demo.cmd`。本文保留原三分钟工作台讲解与历史测量；新的连续帧演示和统计不冒用下方旧版程序的性能数字。

核验日期：2026-10-05。现场操作约 3 分钟；会前构建与完整基准另计。推荐入口为根目录 `Start-EmberFrame.cmd`，也可运行 `scripts/start-workbench.ps1`。

本文下方保留补充简历回放之前的核验：193 项算法检查、135 项集成检查、64 组 CPU/GPU 整体回归通过（集合重叠）。新增简历演示后算法检查增至 200 项，最新带验证层回归与连续帧资料以 [简历演示操作稿](RESUME_DEMO.md#9-本次已核验的结果与材料) 为准。新版界面已检查四个页签与 800×600 窄窗口；下方性能报告、真实执行图和可解码 MP4 保留各自生成时的程序版本。操作名按 `app.cpp` 与窗口截图核对。个人对源码的掌握仍需另行验收。

## 0. 组会演讲稿

（先打开材质工作室，画面区域右键拖动一下。）

这个项目以 Vulkan Guide 的可运行代码为基线。我保留了上游 MIT 许可，也保留原来的章节和学习 Sample。现在演示的是新增的 EmberFrame 渲染实验室，重点是把算法、可交互的配置和工程证据接起来。上游已有的初始化、Swapchain、基础资源和章节渲染不能作为我的新增成果。

（在视口顶部切前向和延迟，显示模式选“世界法线”，再回最终颜色。）

这里的前向和延迟都录制真实 Vulkan 命令。延迟先写七张 G-buffer，再统一计算光照：除了位置、着色法线、材质颜色、发光信息、切线、准确的材质/对象 ID 与编码后的几何法线，还保留 PRT 烘焙照度。几何法线用于阴影接收平面校正，避免把倾斜表面自己误判为遮挡；着色法线仍用于光照。Compute 做对象视锥剔除及分块/聚簇灯表，每个 primitive 使用一条间接绘制，不要求额外的 multiDrawIndirect feature。切中间结果，是为了检查数据而不只是展示最终颜色。

（到“材质”页改粗糙度，Ctrl+Z 撤销，再回“渲染”页。）

材质和灯光参数确实进入渲染。工程保存场景、相机和参数；撤销会更新场景版本，资源准备期间保留已经提交的旧场景。上传使用有预算的 Staging Ring，通过 Fence 判断何时可以回收，而不是提交后立即释放。

（保持 Vulkan，切到阴影测试与 MSM；必要时切 CPU 参考对照。）

统计阴影、屏幕间接光、体积 GI 和时间过滤已经进入 GPU 多 Pass。MSM 用四个深度矩，VSSM 估计遮挡者统计；它们仍有近似与漏光。SH/PRT 与网格距离场现在也进入同一个场景：后台生成静态数据，GPU 正常光照查询，工程保存这些资源。自动 LOD 读取持久减面链，原网格保留。体积 GI 的 GPU 路径限定方向光、16³/32³ 网格和最多 4096 个三角形，稀疏查询读取 GPU compact 的节点；仍保留稠密构建中间态，不冒充物理显存优化。路径追踪是 CPU 参考，不将参考耗时介绍为实时 GPU 性能。

（回 Vulkan 延迟，在“工程”页展开 Pass 与屏障，展示预先生成的报告。）

工程页可查看实际执行的 Pass 和资源交接。导出的 DOT 边来自 RenderGraph 编译器保留的真实依赖，不是把所有 Pass 串起来；JSON 同时记录资源生命周期和自动 Barrier。任务系统基准对同一批固定种子对象做矩阵变换与六平面球测试，比较串行以及一、二、四、八个工作线程。每轮都核对完整输出，也保留并行更慢的结果。GPU 报告使用 Fence 完成后独立帧的 timestamp 样本；最后一帧的时间不能代替中位数和 p95。

## 1. 本次演示要讲清楚什么

| 主题 | 现场能看到的证据 | 可以说明的结论 |
|---|---|---|
| 执行位置 | 执行路径选择、CPU 参考提示、中间结果 | 有算法实现，不等于所有算法都有 GPU 实现 |
| 工程生命周期 | 上传进度、已提交/Fence 确认字节、旧画面保留 | 两个 FrameSlot 与 ring 回收受 Fence 约束 |
| RenderGraph | 本帧 Pass、屏障列表、JSON/DOT | 真实编译图驱动场景资源交接 |
| 可见性基准 | 输入规模、线程/grain、原始样本、完整输出校验 | 测真实计算与调度开销；不测空任务 |
| GPU 时间 | 唯一完成帧 serial、预热 30 帧、N 个独立样本 | scene + postprocess + UI 的 GPU 时间分布 |

上游边界以 [UPSTREAM.md](../UPSTREAM.md) 为准；已有运行说明见 [README.md](../README.md)。Vulkan Guide 为 MIT 基线，Piccolo 当前是架构阅读参考，不是代码依赖。不用整个仓库文件数、章节名或算法测试计数代替个人贡献说明。

## 2. 会前准备

从仓库根目录执行：

```powershell
# 会前构建，现场不等编译
.\scripts\start-workbench.ps1 -Rebuild
```

确认左侧为“场景 / 场景资源”，中间为视口，右侧属性检查器有“渲染 / 材质 / 实验 / 工程”四个页签。执行路径和显示模式位于视口顶部；灯光可从左侧场景树选择后在“材质”页检查。底部状态按钮展开通知、上传进度和错误，性能统计悬停看详情。先用内置“材质工作室”，不要依赖临时下载的模型。等资源准备及天空预过滤结束，再观察颜色和计时。

侧栏分隔线可拖动调宽，“视图”菜单可隐藏侧栏或还原布局。宽度不足 1100 逻辑像素时场景栏自动收起，视口与检查器保留。默认渲染尺寸适应视口；固定尺寸的脚本截图可能保留宽高比留边，这是明确传入 `--width/--height` 的行为，不是拉伸画面。

2026-10-06 显示修复：Windows 175% 下字体以 28px 栅格化、16 个逻辑点布局；视口以物理像素 1:1 渲染。顶部窗口控制已嵌入应用，可拖动空白区、双击最大化/还原、拖外沿缩放，F11 全屏。最大化客户区与屏幕工作区一致，底部状态栏不落入任务栏。状态栏与渲染页显示实际渲染/视口分辨率及 DPI；大尺寸受单轴 4096、4K 总像素量、设备灯表与显存预算约束，非无界分配。日常工作台与 PBR 演示默认阴影 1024；阴影专题的固定 256 对照保持不变。新增专用回归脚本 `scripts/verify-editor-display.ps1` 检查原生尺寸、窄窗口、最大化、固定尺寸及 2560×1440 GPU 读回，不把历史计时当作新版性能。

关闭其他工作台后预先生成性能报告：

```powershell
# 单独 CPU 任务系统基准，不创建 Vulkan 窗口
.\bin\Release\emberframe_workbench.exe --benchmark --benchmark-output output\demo-cpu-benchmark

# 真 GPU 隐藏窗口；预热 30 个有效完成帧，采集 180 个样本
.\bin\Release\emberframe_workbench.exe --gpu-hidden --path deferred --preset 3 --culling 2 --width 640 --height 360 --benchmark-frames 180 --benchmark-output output\demo-gpu-benchmark --graph-output output\demo-graph

# 串行运行六组 GPU 配置（前向/延迟 × all/tiled/clustered），每组重复三次；也跑 CPU
.\scripts\benchmark-workbench.ps1 -Cpu -Frames 180 -Repeats 3
```

基准脚本需要新的输出目录，会保存每个 case 的命令、程序 SHA256、日志与报告。所有 case 串行运行；当前工作台仍有固定 `output/workbench.log` 与 `output/last-run.json`，所以不要同时开第二个工作台。脚本默认场景为多光源，宽高为 640×360，GPU 窗口使用 `--gpu-hidden`；**不传 `--headless`**，因为当前 headless 入口强制 CPU 参考路径。

旧可执行文件只有 `--frames` 时，脚本保留一次运行的总耗时和末次 timestamp，`gpu_sample_count` 记 0，中位数/p95 留空。它不会把多个进程各自的末帧值拼成逐帧分布。`-WarmupFrames` 在新版 CLI 中必须为 30；固定预热是主入口的约定。

## 3. 第一步：说明来源并操作相机，约 25 秒

操作：启动工作台，保持“场景 = 材质工作室”“执行路径 = Vulkan 前向”。在画面中右键拖动，滚轮缩放，按 F 聚焦。

预期：相机变化，材质球重新构图。鼠标应放在画面区域，面板正在编辑文本时快捷键可能被 ImGui 捕获。

讲解：

> 这是从 Vulkan Guide 基线继续拆分的学习引擎。现在展示的是独立工作台入口；基础 Vulkan 初始化和原始章节仍明确归上游。

## 4. 第二步：前向、延迟与中间结果，约 35 秒

操作：在视口顶部把执行路径切到“Vulkan 延迟”，显示模式选“世界法线”，再选“最终颜色”。可在顶部将场景切到“多光源”，在右侧“渲染 → 光照与可见性”把“光源筛选”切为“Clustered 聚簇”，再从顶部显示模式选“每组灯数”，然后回最终颜色。

预期：法线视图显示表面方向；多光源的灯表视图随屏幕区域变化。前向与延迟属于真实 GPU 路径；UI 的 draw、三角形和可见对象统计具有各自的计数范围，不是相同含义的 FPS 指标。

讲解：

> 延迟路径先写 G-buffer 再算光照，聚簇路径用 Compute 建灯表。对象剔除输出实际间接命令；可见对象和三角形读回来自已完成的 FrameSlot。

## 5. 第三步：材质与版本化编辑，约 35 秒

操作：回“材质工作室”。进入“材质”页选择一个材质，拖动“粗糙度”，观察反射高光变化；Ctrl+Z 撤销，Ctrl+Y 重做。到“工程”页按“保存 Ctrl+S”，也可使用 Ctrl+S。

预期：粗糙度改变高光形状，撤销/重做恢复对应值。项目默认写 `output/session.ember`；文件保存的是场景、相机和参数，不是仅保存截图。可使用“读取项目”恢复。

讲解：

> 编辑更新场景版本，资源准备和提交完成是两个阶段。GPU 还在使用的旧版本必须等 Fence，上传完成以后才整体替换。

补充用例（不计入三分钟）：在“工程 → 资源上传”把“预算 MiB/帧”调到 1，拖入本地已验证的 GLB/glTF。点击底部状态按钮看准备阶段与 Fence 完成进度；小模型可能很快完成，不能保证现场有肉眼可见的长进度条。不要为演示临时换未经验证的大模型。

## 6. 第四步：GPU 算法与参考边界，约 30 秒

操作：保持顶部“Vulkan 延迟”，场景选“阴影测试”。在右侧“渲染 → 光照与可见性”把阴影选为“MSM”，顶部显示模式选“阴影可见性”，随后回“最终颜色”。可在右侧把间接光切到“LPV”观察方向光场景中的间接贡献；完整 CPU 对照留作会后补充。

预期：阴影视图白色表示可见，黑色表示遮挡。这里实际执行 GPU；切回最终颜色可观察阴影与间接光。若改为面积光房间并选体积 GI，工作台会说明不支持的组合，并提供“切换至 GPU 体积 GI 场景”。不能忽略回退提示后继续宣布 GPU 性能。

讲解：

> 这里展示的是实际 GPU 深度、矩统计和采样。CPU 参考也保留，两者都受近似条件约束；例如 VSM/VSSM 的漏光，不会因为换成 GPU 就消失。

画质修复后的矩阴影：平行接收面采用高低两部分补偿的 SAT 查询；倾斜面先逐采样点修正接收平面，再在 GPU 上累计 64 个固定圆盘样本，不能把平面自身的深度变化误判成遮挡者。VSSM 的最终核限制在遮挡者搜索支持域内，极宽半影会受到预算限制；MSM 使用连续的矩正则化。它们是有明确误差和成本的近似，不是无限核的精确面积光积分，也不自动切换 CPU。

### 补充：同一场景的预计算与几何资源（不计入三分钟）

操作：保持“阴影测试”和 Vulkan 前向/延迟。在“渲染 → 光照与可见性”将间接光选“环境光”，环境漫反射依次选“SH 球谐”“PRT 烘焙传输”，再启用“距离场阴影”。在“几何与场景资源操作”等待状态完成，Ctrl+S 保存；通过工程页“读取项目”恢复。

预期：没有切换成独立示例场景。PRT 影响环境漫反射、保留镜面 IBL；距离场影响直接光的遮挡。烘焙成功后的资源进入 `.ember`，失效资源不盲目重用。几何/变换编辑需要重新准备；天空改变可复用传输和距离数组。这里操作入口已由源码、模块测试和命令行正常场景输出核验；不把截图当作完整鼠标操作自动化测试。

讲解：

> 实验只是选择配置和对照，真正的光照都作用于当前场景。SH 表示天空，PRT 提前保存静态表面的可见性；距离场则保存到网格表面的距离。它们先烘焙后查询，并不是每个像素临时重新算一遍。

操作：左侧“场景资源 → 网格”选网格，在同一操作分组点“生成选中网格 LOD 链”，等待完成后启用“自动 LOD”，调整“屏幕误差 / px”并远近移动相机。查看源/当前三角形数，保存工程。可用原本测试材质球；仅两个三角形的地面可能因边界保护无法减面。

预期：减面在后台执行，原网格不被替换；正常绘制按实例距离、缩放和投影误差选择几何。无法减面的网格有明确提示；无有效链时保留 LOD0，不逐帧重试 QEM。PRT 配合 LOD 时使用该几何版本的传输，切换阶段可能重新准备。

讲解：

> 自动 LOD 不是把同一个网格复制几份。我先生成真实索引与顶点更少的链，渲染时只做误差选择；保存后不需要重新减面。BVH 和八叉树用于当前场景选物与 CPU 路径追踪，不是 VCT 的 GPU 体素数据。

操作：间接光选“Voxel cone tracing”，切“稀疏体素查询”；回最终颜色对照。方向光和体积预算不满足时读清回退提示。

预期：稀疏与稠密查询读取相同体素辐亮度，固定测试 HDR 一致。性能和显存不能只凭节点稀疏就宣称更优；结构构建仍有成本。

讲解：

> GPU 把非空 mip 体素压成节点和页表，锥体查询实际从节点 SSBO 取数据。验证还清空过节点缓冲，确认光照消失，证明不是一个只挂名却不参与渲染的结构。

## 7. 第五步：真实执行图与性能证据，约 55 秒

操作：在“工程”页展开“本帧执行的 Pass”和“资源交接与屏障”，按“导出真实渲染图 JSON / DOT”。展示预先生成的 `system-benchmark.md` 与 `gpu-timings.json`。必要时按“运行任务系统基准”触发后台任务，不在现场同时启动 GPU 性能采样。

预期：Pass 列表来自工作台最近一次实际场景编译图；Barrier 行显示作用于哪个执行索引、资源前后状态、访问类型和 memory dependency。导出位于 `output/graph`。任务基准完成后报告在 `output/benchmark`。

讲解：

> 黑色依赖边是编译器保存的真实 DAG，边的索引对应执行顺序，而不是声明序号。屏障与生命周期来自同一个计划。CPU 基准每轮核对全部输出；GPU 中位数与 p95 来自独立完成帧，不能重复读取没有更新的统计值。

DOT 可交给本地 Graphviz 查看（非必要依赖，不要求安装）；可读 JSON 本身就是完整交付。现场不需要打开终端安装绘图工具。

## 8. 证据文件与准确解释

| 文件/接口 | 保存内容 | 使用边界 |
|---|---|---|
| `output/graph/frame-*.json/.dot` | Pass 执行/声明索引、真实 dependencies、裁剪 pass、生命周期、自动 Barrier、快照元数据 | 仅当前 GraphPlan 范围；不是自动截获整个 Vulkan 命令流 |
| `system-benchmark.json` | 13 种默认模式、完整原始样本、校验 checksum、可见对象数与环境 | checksum 用十六进制字符串避免 JSON 数字精度损失 |
| `system-benchmark.csv` | 每配置一行，中位数/p95/加速比 | 粒度细导致变慢也保留，不能只选最快一行 |
| `system-benchmark.md` | 中文方法、计时范围、表格、限制 | 本 CPU 基准不代表 GPU 剔除吞吐或整机 FPS |
| `gpu-timings.json` | 预热 30 帧后，唯一完成帧 serial 与 GPU ms、分布 | 没有 timestamp 时 available=false、summary=null；不能写成 0 ms |
| 脚本 `runs.json/.csv`、`benchmark.md` | 独立进程各配置/重复的数据与采样状态 | wall_ms 包含启动、上传和退出；与 GPU 区间不同 |
| 脚本 `environment.json`、各 case `command.json` | 程序 SHA256、分辨率、场景、参数、采样能力 | 复现还需记录 CPU/GPU/驱动与电源状态 |
| `diagnostics.json` | category/name/severity/message/artifact/frame_serial | 自有 `emberframe.diagnostics.v1`；保留换行错误与日志位置 |

诊断部分为 **11/11** 正确性检查，属于总算法检查的一部分。覆盖重排 DAG、裁剪、同状态 hazard、名称转义、中文路径、错误输入、quantile 与完整输出校验。现有 31 个工作台 Shader 源文件使用 Vulkan 1.1 / SPIR-V 1.3 编译，避免依赖未开启的 LocalSizeId 等附加 feature；运行设备仍要求 Vulkan 1.3 dynamicRendering + synchronization2。

本次 Release 实测：262144 对象、固定种子 1162691141、每配置预热 3 次/实测 15 次。13 个模式的可见数均为 105750，checksum 为 `8387ee042b03ecc6`。串行中位数 14.641 ms；8 线程/grain 4096 为 2.547 ms（5.747×）；8 线程/grain 64 为 25.314 ms，反而更慢。数据在 `output/benchmark-final-v2-20261005/cpu-system/`，不作机器速度的通过条件或整机 FPS 承诺。

阴影修复后的六组 GPU 配置（前向/延迟 × 全灯/Tiled/Clustered）在 `output/benchmark-shadow-fixed-20261005/` 保存各自 60 个独立完成帧样本，先预热 30 帧。RTX 4060 Laptop GPU、640×360、多光源场景、含 UI、开启同步验证；中位数约 0.610–0.822 ms。小场景中 Clustered 比 Tiled 慢，保留原始结果，不能只宣传筛灯一定更快。驱动、电源方案、程序 SHA256 和参数在 `environment.json`/`command.json`。样本是 GPU 区间，不是帧率或输入响应延迟；旧版本的计时保留在原目录，单轮测量不作为微小性能差异的结论。

最后回归为 **193/193 算法检查、135 项 GPU 集成检查、64/64 整体用例**，包含真实高级模块、SH/PRT/距离场正常渲染、持久 LOD 组合与空间查询、GPU 稀疏体素、故障路径和新版编辑器的逐页/窄窗口呈现；集合重叠，不能相加。GPU 集成检查另覆盖四边视口边界、隐藏面板的布局计算及高 DPI 分辨率上限。Shader 包最后一项的布局故障在创建候选 Pipeline 前拒绝，退出检查无遗留对象。原始记录见 `output/verification/results.json` 及相邻日志，Validation Error / VUID 为 0。截图只能证明该帧呈现，不能代替任意操作的 UI 自动化验收。

阴影修复在整体 GPU 用例内部新增 16 项检查：倾斜平面在 64/256 阴影分辨率下保持全亮，距它 0.015 世界单位的遮挡平面仍能投下阴影。覆盖 Hard、PCF、PCSS、CSM，默认 Bias 保持 0.002 世界单位。旧 Shader 的 PCF/64 检查出现最小可见性 0.555556 并失败，修复后为 1；近遮挡检查仍为 0。前向与延迟的同一 PBR 场景也分别导出检查。日志与 BMP/PFM 在 `output/shadow-fix-20261005/`；这不是把阴影关闭或切换为 MSM 的绕过方案。

### 已验证的视频产物

`output/demo-shadow-fixed-20261005/EmberFrame-demo.mp4`：960×540、15 FPS 时间轴、32 秒。八幅实际 GPU 输出各保持 4 秒，依次展示 PBR、多光源、MSM、SSGI/GTAO/TAA/SVGF、RSM、LPV、VCT、Disney/NPR。普通 PBR 的自阴影条纹已消除；最后四秒显式开启 `--outline --hatching`，其中排线属于 NPR 效果。原始 BMP/PFM 和每幅命令/log 保留，编码后自动解码首帧与尾部并核对时长，旧视频不覆盖。**这是定帧结果展示，不是实时交互录屏，也不能用视频 FPS 证明渲染性能。**

```powershell
# 生成一个新目录，不覆盖旧视频；依赖 Windows 自带 Media Foundation
.\scripts\export-workbench-demo.ps1
```

## 9. 主集成约定与目前限制

诊断源码：[diagnostics.h](../engine/lab/diagnostics.h)、[diagnostics.cpp](../engine/lab/diagnostics.cpp)；检查：[diagnostics_tests.cpp](../engine/lab/diagnostics_tests.cpp)。GPU 模块按阴影、材质/预计算、屏幕/时间效果与体积 GI 分工，主程序统一生命周期和热重载事务。

CPU 调用 `run_system_benchmark(output_directory, options)`，默认 262,144 对象；可用 `SystemBenchmarkOptions` 降到小工作量。串行与 1/2/4/8 个工作线程分别测试 grain 64/512/4096。每次计时包含 submit/计算/get，排除数据生成、线程创建、输出清毒、逐项比对和报告写盘。线程数不含调用线程。对象总数限制 1..1048576，预热 1..32 次、实测 3..100 次，配置总数至多 32。

这里直接按 grain 提交实际分块；现有 `parallel_for` 会把任务数限制为 workers×4，不能用它假装测试默认 workload 上的所有 grain。JobSystem 未完成任务上限 4096；背压、句柄与调度锁的成本纳入实测。计时前还会检查单位立方体的六面 outside mask、球与平面相切、非均匀/负缩放和独立齐次 clip 判定。每轮先用 NaN 清毒输出，再逐 float 位比较完整世界中心、clip 中心、世界半径和六平面 outside mask。浮点结果相等证明同一构建的串行/并行一致，不宣称跨编译器逐位一致。

图导出 `save_render_graph(plan, path, info)` 或 `save_render_graph_snapshot(plan, directory, info)`；也提供 `RenderGraph` 重载。每帧可复制最新计划作为值快照，不持有 GPU 对象或 pass 回调。正常计时期间不要每帧写磁盘。DOT 的 pass 间黑线**只使用** `GraphPlan.dependencies`，没有人为的执行序串联。计划保存边的两端，尚不保存边的 RAW/WAR/WAW/显式分类，导出不伪造类型。Barrier 蓝线表示“作用于哪个 pass 之前”，资源虚线表示生命周期端点；生命期 first 不必是依赖 producer。

图快照的 `frame_serial` 应是录制/提交该计划的 serial，`scene_revision` 应是实际呈现场景的 revision；不能拿最近完成的 timestamp serial 标注最新录制图。`status` 由调用方填写 recorded/submitted/completed。计划可能只覆盖场景 passes；post/UI/present 或独立 effects 执行器若没有纳入该 GraphPlan，就不会被此导出自动覆盖。面板展示额外 effect pass 的文本不代表它们已经进入依赖 JSON。当前生命期是整个资源、单队列、无物理 alias 分配与跨队列 ownership 描述。

GPU 调用 `save_gpu_timings(report, path)`；主入口在 FrameSlot Fence 已完成且 timestamp query 返回成功时更新 `gpu_sample_serial`，样本必须与这次的 `gpu_ms` 绑定，仅追加一次。资源准备结束后预热 30 个有效完成帧，再采 N 个独立样本。两 FrameSlot 的完成统计存在延迟，不能把当前 draw 的统计和上一提交的时间混称同一帧。readback/截屏/热重载与正常渲染采样分开；GPU 区间包含 scene/post/UI，截图 copy 在区间之外，帧 HDR readback 会进入区间，因此不能用于正常性能比较。

timestamp 位数可能为 0，query 也可能尚未就绪；主入口需要有界退出并导出 unavailable。脚本额外设 120 秒进程上限，可用 `-TimeoutSeconds` 调整。它只会停止自己启动且超时的进程，并保存失败记录，不操作其他工作台。

设备保持 Vulkan 1.3 dynamicRendering + synchronization2，不额外要求 descriptor indexing、sampledImageArrayDynamicIndexing、samplerAnisotropy 或 multiDrawIndirect。高级 GPU Shader 和主入口已整体验证，不是只验证模块能编译。滤波用显式采样，阴影描述符用固定索引，LTC 采用粗拟合表；屏幕效果只掌握第一层深度，SVGF 未拆分镜面/漫反射及反照率解调。体积 GI 的占用/材质和各向同性 Mip 都是近似，不承诺消除漏光。外部 HDR、蒙皮/形变和生产级异步多队列不在当前能力内。

中位数使用排序中心值，偶数取两个中心值均值；p95 是 nearest-rank 第 ceil(0.95×N) 个样本。默认 CPU 15 个样本的 p95 就是最大值，不能冒充充分采样的长期尾延迟。性能受后台负载、功耗和优化配置影响，没有“并行必须更快”的测试断言。

## 10. 现场异常与快捷键

| 情况 | 处理 | 现场说明 |
|---|---|---|
| 新按钮不存在 / CLI 报 Unknown argument | 用重建后的工作台，检查 `--help` | 新源码已交付不等于旧 exe 已集成 |
| 参考渲染还在计算 | 等待或恢复 PCF + Vulkan 路径；界面可继续操作 | CPU 参考具有实际计算成本 |
| GPU 路径出现回退提示 | 按提示去掉不支持的参数或明确选 CPU | 如实说明本配置执行位置 |
| 模型准备失败 | 看资源错误和日志，旧场景继续显示 | 不把失败解释为加载成功 |
| Shader 编译失败 | 展示工程页错误，已有 Pipeline 保留 | 编译失败回退不等于新 Shader 已生效 |
| 时间显示 -1 / 无可用样本 | 报告 unavailable；展示 CPU 证据 | 不声称 0 ms，也不反复复制旧 timestamp |
| 图中缺少后处理/某些 effects | 检查是否属于当前 GraphPlan | 真实图的覆盖范围必须说明 |
| 多线程比串行慢 | 保留 grain/线程数与原始样本 | 调度成本也是工程结果 |

画面区域：右键旋转；Shift+右键平移；滚轮缩放；F 聚焦；F11 全屏；Esc 退出。Ctrl+S 保存工程；Ctrl+Z / Ctrl+Y 撤销/重做。执行路径和显示模式使用视口顶部下拉框；算法参数使用右侧检查器。截图使用“文件 → 保存当前画面”或右侧“渲染”页“保存当前画面”，保存为 `output/capture.bmp`。布局核验截图位于 `output/editor-layout-20261005/`。
