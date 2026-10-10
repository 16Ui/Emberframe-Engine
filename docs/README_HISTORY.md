# 公开 README 历史存档

这里保留公开 README 精简前的完整内容，供追溯项目演进、学习路线和历史实现边界。

下文的日期、检查数量、截图及 `output/` 路径均属于对应历史版本，**不是当前版本的验证或性能证据**；其中“尚未实现”等描述也可能已经过时。当前使用入口与本轮证据以根目录 [README](../README.md) 为准。

本轮公开验收记录另存于 [internship-demo.json](evidence/internship-demo.json) 与 [同机便携运行记录](evidence/portable-runtime.json)，不回填到下方旧正文，也不将历史结果与本轮计数合并。

当前执行图已包含后处理、编辑器 UI 与 Present 交接；下方历史描述及旧导出范围不能代表当前实现。程序重建后，以重新生成并绑定对应程序/包哈希的公开记录为准。

---

# EmberFrame Engine

一个面向游戏引擎实习的现代 C++ / Vulkan 学习型引擎项目。

项目以 Vulkan Guide 的可运行代码为渲染基线，参考 Piccolo 的模块职责和生命周期设计。新增 `engine/lab` 工作台把渲染实验、资源上传、执行图、性能诊断和场景编辑接到一个入口；原始教程章节和来源许可保留，不把上游代码包装成个人成果。

渲染方向以真实 Metallic-Roughness PBR 为核心，将 GAMES101 的 BRDF 与渲染方程、GAMES202 的阴影和实时光照、GAMES104 的 Render Graph 与资源生命周期落实到同一条 Vulkan 管线。具体优先级和面试验收见 [渲染实习能力主线](docs/RENDERING_INTERVIEW_TRACK.md)。

新增完整专题范围见 [D01—D32 高级渲染与图形基础深化](docs/learning/ADVANCED_RENDERING_TRACK.md)：图形基础、参考路径追踪、采样/预计算、材质/面积光、多光源、统计阴影、GI、时空降噪与 NPR 全部按批次学习和实验，不再只列名词。专题的实验代码现位于 `engine/lab`；执行位置与限制见下文。A1—A8 的完成记录保持不动。

## 运行渲染实验室

**演示入口：**顶部“演示场景”菜单或同名检查器页提供五类 GPU 场景：材质与环境光、阴影与半影、多光源与灯表、屏幕空间间接光、几何与 LOD。每类说明观察目标，并提供当前场景的 A/B 参数切换；材质类另有白炉能量对照。历史简历演示与证据导出说明仍见 [简历演示操作稿](docs/RESUME_DEMO.md)，运动视频不是实时 FPS 证据。

双击根目录的 `Start-EmberFrame.cmd`。首次启动会构建；平时直接运行。中文控制面板中可切换场景、执行路径、材质、阴影、间接光和中间结果。

原生编辑器采用“场景 / 资源 — 视口 — 属性检查器”分区：左侧是真实节点层级与当前场景的资源列表；中间顶部切场景、实时执行路径和显示模式；右侧“渲染设置 / 对象与资源 / 演示场景 / 工程”按职责分区，属性名称在左、控件在右；底部集中显示状态与 GPU 统计。“开发者工具”默认隐藏，通过“视图 → 开发者工具”手动开启；仅开启页面不启动 CPU 对照、验证或基准。参考 [UE 编辑器界面](https://dev.epicgames.com/documentation/unreal-engine/unreal-editor-interface?lang=en-US) 与 [Godot Inspector](https://docs.godotengine.org/en/stable/tutorials/editor/inspector_dock.html) 的工具分区，不宣称完整 Docking 编辑器。

拖动侧栏分隔线可调宽；“视图”菜单可隐藏面板或还原布局。窗口宽度低于 1100 逻辑点时自动隐藏场景栏；默认视口自适应实际剩余区域，不拉伸或覆盖画面。Windows 使用 Per-Monitor V2：界面按逻辑点布局，字体按当前 DPI 栅格化，`适应视口 + 1.00x` 按物理像素渲染。已取消 2048 单轴限制；当前实验缓冲上限为单轴 4096、总像素不超过 3840×2160，并额外受设备灯表范围及显存预算限制，触发时界面明确提示。显式 `--width/--height` 保持固定尺寸；在其后传 `--fit-viewport` 可改回自适应，比例在“质量与算法参数”调整。状态栏显示实际渲染尺寸，悬停可查看物理视口与 DPI。

顶部已合并应用图标、菜单和窗口控制：空白区拖动、双击最大化/还原，右侧最小化/最大化/关闭，外沿可缩放，F11 全屏。初始窗口及最大化使用当前屏幕工作区，不遮住任务栏。图标 PNG/多尺寸 ICO 位于 `assets/branding/`，ICO 嵌入 EXE，PNG 随程序复制到 `bin/Release/assets/branding/`。完整显示回归可运行 `scripts/verify-editor-display.ps1 -SkipBuild`；不同显示器 DPI 的实机切换仍需在对应硬件上核验。

```powershell
# 修改源码后只编译，不启动程序、不运行测试
.\scripts\build-windows.ps1 -Config Release -Target emberframe_workbench
# 用户手动启动日常编辑器
.\bin\Release\emberframe_workbench.exe
# 仅显示开发者工具，不自动运行对照或验证
.\bin\Release\emberframe_workbench.exe --developer-tools
```

工作约定：曾按用户要求暂停测试、只编译；随后用户明确要求“全面检查”“完整检查并修复”，本轮按该授权执行算法、实际 GPU 与编辑器回归。普通启动仍不自动运行验证或基准，不关闭用户正在使用的窗口。

**自己新建场景：**“文件 → 新建场景”（Ctrl+N）可以创建空场景或带地板的场景，初始方向光也可取消。新建前提示保存，且清空旧工程保存路径；“另存为”（Ctrl+Shift+S）保存到独立 `.ember`，不会改写原模型。导入首个模型到空场景后自动聚焦。

**免费资源试用：**新增 Khronos 彩色立方体（CC0）、Cesium 送奶车（CC BY，保留署名，静态姿态）与 ambientCG Tiles074 地砖颜色/法线/粗糙度贴图（CC0）。来源、许可、规格和限制见 [免费资源说明](assets/import_samples/README.md)。入门工程明确使用最多 512px 贴图预览副本，原始 GLB 和地砖 1K 原图保持不变，仍可完整导入或单独绑定。工程页“免费示例资源”可以单独追加；下面的脚本首次创建包含模型、贴图以及方向光/点光源/矩形面积光的独立工程，之后只打开，不覆盖保存的修改。

```powershell
.\scripts\download-import-samples.ps1
.\scripts\open-import-sample.ps1
```

本机资源已经准备完毕，可直接双击根目录 `Open-Free-Assets.cmd`。2026-10-07 此独立示例完成实际导入、保存/读取和前向显示试用；记录见免费资源说明，不覆盖其他算法或编辑操作。

画面区域右键拖动或 Alt + 左键拖动环绕相机，Shift 同时按住时平移相机；滚轮缩放，F 聚焦，F11 全屏。先选中模型，再在画面区域 Ctrl + 左键拖动旋转物体；额外按 Shift 为 15° 吸附，Esc 取消，松开后形成一条撤销记录。相机与物体旋转不会共用同一个组合键。

场景树的节点或空白处右键可新建立方体、球体、平面、空节点和三类光源，也可选择导入模型。基础模型在右键节点下创建子节点；空白处创建根节点。光源和导入模型加入场景根级，菜单明确说明此区别。拖入 GLB/glTF/OBJ 默认后台解析并**追加**到当前场景，多文件依次处理；“文件 → 追加模型…”也可浏览选择。工程页可明确选择“替换场景”，需确认且可撤销。FBX 暂不支持，可先导出静态 GLB/OBJ；蒙皮、动画播放仍不属于当前静态加载器。

选中模型后，用 W / E / R 切换平移、旋转、缩放工具，也可使用工具菜单或视口工具栏。平移沿世界 XYZ 轴；旋转拖动世界轴圆环；缩放沿模型局部轴，拖动中心方块进行等比缩放。Shift 分别启用 0.1 单位平移、15° 旋转、0.1 倍缩放吸附；Esc 取消正在进行的拖动。节点检查器提供世界位置、相对父节点的局部旋转角度和局部缩放数值输入；负缩放表示镜像，编辑时保留原有剪切分量。

“对象 / 资源”页按选择目标切换内容：节点只编辑实例名称、变换、网格与材质引用；网格资源显示共享几何、引用者与 LOD；材质资源编辑颜色、粗糙度、反射层和贴图槽；纹理资源提供图像通道预览与采样设置。点击资源会清除物体变换工具，不再显示上一个节点的变换。修改共享材质会影响其引用者；节点内“创建独立材质并编辑”明确复制并只重绑这个实例，保留旧资源。

非方向光仍使用世界空间平移箭头；方向和尺寸通过光源检查器修改。选择彩色房间的白色灯板会定位实际面积光；改变光源的位置、方向、尺寸、颜色/强度，灯板和照明一起更新。方向光只编辑方向与强度，不用位置改变照明；场景树右键或“工具 → 新建光源”提供方向光、点光源、矩形面积光。

拖入 PNG/JPG/TGA/BMP，或在“材质 → 纹理贴图”点击某个槽位的“导入…”，先选择目标材质与用途，再后台解码并绑定。支持基础颜色、金属/粗糙度（G/B 通道）、法线、AO、自发光；颜色贴图按 sRGB 解码，数据贴图按线性读取，并生成 Mip。导入保留材质因子，基础颜色/粗糙度/金属度等仍与贴图相乘，共享材质会影响所有引用它的对象。可在槽位下拉复用编码相容的现有纹理，选择“无贴图”解除绑定。

模型导入、贴图绑定、光源平移，以及模型平移/旋转/缩放都接入 Ctrl+Z / Ctrl+Y；连续拖动只生成一条记录。工程页用独立的 `.ember` 路径保存场景、相机、参数、纹理和灯板绑定，避免把模型路径当作保存路径。解析失败保留原场景，切换工程后旧后台结果不提交。撤销快照使用有预算的内存副本，而不是逐次序列化大纹理；超限操作明确失败，不悄悄丢失撤销能力。编辑器资源预算与 OBJ/MTL 的具体兼容范围见 `engine/lab/editor_assets.h`。截图、HDR 参考与测试结果保存在 `output/`。

普通 LOD0 编辑将姿态与静态资源版本分开：连续移动模型或灯板时只更新节点矩阵、灯光和逐帧数据，不重复复制/上传整套纹理。导入、材质改变和撤销恢复仍走有预算的安全上传；自动 LOD 或 PRT/距离场几何更新需要重新准备对应资源，不冒充零上传。

编辑流程回归：`scripts/verify-editor-assets.ps1 -SkipBuild`，覆盖真实箭头拾取/平移计算、灯板联动、OBJ 追加、PNG 绑定、撤销/重做、工程往返与前向/延迟 GPU 渲染，并检查连续六帧同时移动模型与灯板时静态上传字节为零。系统文件选择器的人工点击，以及跨 DPI 显示器拖动仍需实机操作验收。

2026-10-06 平移/导入版历史验证：236/236 算法与编辑器检查、169 项 GPU 与配套检查通过，前向/延迟实际绘制新增的第九个 Primitive。连续拖动无静态资源重传，GPU 读回另外验证变换确实移动画面、材质变更正确失效；验证层无报错，原生 175% DPI 视口仍为 1531×1319。结果分别见 [编辑流程证据](output/editor-assets-20261006-174018/results.json) 与 [房间/GPU 回归证据](output/colored-room-20261006-174125/results.json)，均绑定该次程序 SHA256，不作为实时性能报告，也不覆盖后续旋转/缩放与专题恢复改动。

历史阶段记录：2026-10-07 早先的模型旋转/缩放、预览恢复和界面分流当时只编译，未运行验收。后续回归证据必须绑定各自的程序版本，不能用旧结果代替新版验证。

2026-10-07 本轮实际验证：260 项算法/编辑逻辑检查、205 项 GPU/配套检查、28 组隐藏窗口实际运行通过，Vulkan Validation 无错误。覆盖四个内置场景与空场景的 RSM/LPV/VCT、只有点光源的三种间接光、未降噪与 SVGF 两种 SSR/SSGI，以及前向/延迟编辑诊断。结果、设置与截图见 [本轮证据](output/verification/rendering-editor-20261007-155211/summary.json)，绑定程序 SHA256；不是逐项人工鼠标验收或全分辨率性能保证。可通过 `scripts/verify-rendering-editor-update.ps1` 在新的证据目录重现，脚本不会关闭用户窗口。

打开 `.ember` 工程时，显式指定的 `--gi`、`--debug`、`--width/--height` 等启动参数现在优先于工程保存值，避免请求了某算法却实际运行旧设置。SSR/SSGI 修正了粗步进距离分档、固定采样环和离散颜色采样产生的规则条纹；未降噪低采样仍会有随机颗粒，日常可使用 32 样本并开启 SVGF。LPV 使用三线性光通量注入、有限方向余弦传播与逐格非负照度重建；六面采样按体素场密度准备并缓存，仍是低阶低分辨率近似。

日常入口按用途区分：

- **渲染设置**：直接管理当前场景的材质模型、阴影、环境光、屏幕/体积间接光、遮蔽、时域和后处理等算法；使用它们不需要先进入专题。
- **演示场景**：只保留五类有明确观察目标的 GPU 场景，临时切换并提供 A/B 参数。进入页面本身不切场景，需点击“打开这个演示场景”。
- **开发者工具**：默认隐藏，集中放置七种 CPU 参考、算法验证、Shader 热重载/回退、执行图导出和任务系统基准。所有任务需手动点击；日常执行路径下拉不提供 CPU 选项。

首次打开演示或 CPU 参考前，在内存中保存场景、材质、光源、相机、渲染设置、选择状态和原撤销历史；连续切换仍以这份原场景为返回点。“返回原场景”按钮或 Esc（未在拖动、未打开弹出菜单时）恢复进入前状态，丢弃预览中的临时修改。预览期间不允许覆盖原工程文件；这不是磁盘自动存档，关闭应用后不能依靠内存快照恢复未保存内容。

独立 CPU 对照默认限制到不超过 640×480，并关闭 Bloom；返回后恢复原渲染尺寸与视口适配设置。关闭开发者工具会结束其 CPU 预览；读取工程不会自动启动保存的 CPU 模式，需重新明确选择。SH/PRT、距离场阴影仍进入正常前向/延迟路径；IBL 预过滤等另有离线对照图。选择节点后在各网格分部的材质引用进入共享或独立材质编辑。选择光源可检查任意光源，不再只限制前八盏。

**不支持实时配置时不自动转 CPU。** 检查器和状态栏显示中文原因，例如 CSM/SDF 缺少方向光、体积 GI 网格不支持、硬件存储范围/显存不足或光源超过 64 盏。暂停新的 GPU 场景执行，保留最近完成的 GPU 结果及其显示模式/曝光，界面继续可操作；修正配置后恢复。显式批处理 GPU 请求则明确报错，不用 CPU 输出冒充 GPU 结果。RSM / LPV / VCT 不再因场景名称、没有方向光或超过 4096 个三角形而禁用。

专题预览的更新方式：

- SH、PRT、IBL 图集、SDF 独立对照在相机或参数变化后计算一次，完成后保持结果；不会借用时序参考的帧计数无限重算。HW5/SVGF 等真正的时序参考仍进行有限历史累积。
- CPU 参考使用分帧准备的独立场景快照，时序累积复用不可变源；计算失败后保留旧画面，修改参数或“重新渲染”才重试。后台计算期间 UI 和上一张完成的画面继续显示，CPU 对照本身不承诺实时更新。
- 参考图的 Image 与暂存 Buffer 按 Frame Slot 保活、同尺寸复用。只更新已完成 fence 的帧槽，复制与采样屏障随正常帧提交，不再为每张参考图等待整个 GPU 空闲。切换尺寸、窗口或重载管线仍可能需要一次性资源重建。
- 显示 CPU 图像时，不再每帧准备它的 GPU 网格、空间索引、阴影或环境资源，也不同时启动一份不可见的场景烘焙。GPU 预览在显式资源版本不变时复用网格指纹与空间索引；静态资源编辑增加 `asset_revision`，姿态编辑增加 `revision`。未采用此版本契约的调用者仍保留完整指纹路径。

以上是更新调度与资源交接的源码改动，不是实测性能结论。独立 CPU 对照的计算耗时与重型 GPU 算法的实际帧率，仍需用户恢复验收后再确认。

### 在同一个场景使用新增能力

1. **SH / PRT：**“渲染 → 光照与可见性”将“间接光”设为环境光，再切“环境漫反射”。SH 表示天空；PRT 在后台为实际静态实例烘焙可见性传输。它们替换环境漫反射，不重复叠加，镜面 IBL 保留。
2. **距离场阴影：**同一分组启用“距离场阴影”。后台从当前网格烘焙距离格点，正常 GPU 光照查询它；不是只显示一个 SDF 演示球。网格开放时使用无符号距离，不能把它当作可靠的实体内外判定。
3. **资源保存：**“几何与场景资源”调整采样/网格；“几何与场景资源操作”查看状态或手动重烘焙。任务期间保留旧画面，完成后 Ctrl+S 将烘焙数据一并存进 `.ember`。几何/变换变化使旧缓存失效；天空变化复用传输和距离场，只更新照明。
4. **自动 LOD：**左侧“场景资源 → 网格”选网格，点“生成选中网格 LOD 链”，再启用“自动 LOD”。原网格保留为 LOD0；距离、FOV、视口与缩放共同决定屏幕误差。链可随工程保存，逐帧不运行 QEM；缺链、预算超限或无法安全减面明确保留原网格。
5. **空间结构与稀疏体素：**“射线空间结构”选 BVH/八叉树，实际用于视口选物和 CPU 路径追踪；不是 GPU 体素树。“间接光 = Voxel cone tracing”时，“稀疏体素查询”让 GPU compact 非空体素并从节点 SSBO 查询；关闭可对照稠密查询。

SH/PRT/距离场等正常能力直接在“渲染设置”使用；只有开发者工具中的手动 CPU 对照进入对应独立示例。CLI `--topic` 仍明确是独立对照请求，而 `--developer-tools` 只显示工具。不同阴影/GI 方案互为替代与比较，不能合理地全部同时相加。

### 彩色房间的配置响应

房间只有一盏顶灯矩形光、黑色天空，且没有纹理。面积光现在参与阴影，并响应 PBR / Blinn–Phong / Disney 模型与适用的 KC 能量补偿；Disney 清漆、各向异性、Sheen 使用实际材质积分，不是统一调亮。PBR 保留 LTC 单次散射近似；Blinn / Disney 与 KC 增量采用固定 8×8 矩形求积，尖锐高光和近灯仍可能欠采样。

想看明显变化：先选铜球再改粗糙度 / 金属度；切换 Hard / PCSS 观察地面的阴影边缘；选择 SSGI 观察屏幕可见表面的间接光。不要只改默认选中的地板，再期望铜球变化。右侧“当前配置适用性”说明当前数据能驱动哪些选项，并提供显式 SSGI / AO 调试入口。

黑色天空下，IBL / SH / PRT 没有环境输入，AO 只调制间接光，不会人为压暗直接光；仍可查看 AO 调试图。无纹理时过滤选项禁用；无方向光时 CSM / GPU SDF 不适用，加载或脚本中的不适用实时设置显示原因并暂停，不自动转 CPU。前向 / 延迟理应得到相同颜色，Tiled / Clustered 在单灯场景主要不是效果差异。

关闭工作台后可运行 `scripts/verify-colored-room.ps1`。它构建并校验算法、面积光 CPU/GPU 数值、真实房间 HDR A/B、条纹与接触阴影、原生界面，保存 BMP / PFM / 验证层日志与程序 SHA256 到新的 `output/colored-room-*` 目录。

2026-10-06 彩色房间修复验证：203/203 算法检查、167 项 GPU 与配套检查通过，独立面积光数值诊断无验证层错误。受控房间的前向 / 延迟最大 HDR 差异为 0.000977；Hard / PCSS、材质模型、Disney lobes 和 KC 均有真实 A/B 响应。175% DPI 下实际渲染与物理视口同为 1531×1319。详见 [本次结果](output/colored-room-20261006-125707/results.json)、同目录 `lighting-numerical.log` 与 `gpu-integration.log`；历史计时和旧回归结果保留，不混作当前版本的性能数据。

## 当前实现和执行位置

新增主入口是 [`engine/lab/app.cpp`](engine/lab/app.cpp)，共用数据约定在 [`types.h`](engine/lab/types.h)。原有 `chapter-*`、A 系列 Sample 和学习完成记录保留，不再把上游章节当成新增个人功能。

| 模块 | 当前实现 | 执行位置 |
|---|---|---|
| 几何与参考 | 六面齐次裁剪、Top-left、透视属性/UV 导数、Reversed-Z、AABB/球/OBB、BVH/SAH/refit、八叉树、QEM/LOD | CPU 查询/选物/路径追踪；持久 LOD 链改变正常 CPU/GPU 实际绘制几何 |
| 材质与积分 | GGX/Smith/Fresnel、Blinn–Phong、KC、Disney 清漆/各向异性/Sheen、预过滤环境光/BRDF LUT、拟合 LTC、SH/PRT | GPU + CPU 对照；SH/PRT 后台预计算、正常渲染查询；MC/MIS 和离线积分在 CPU |
| 统计阴影 | Hard/PCF/PCSS、稳定化三层 CSM、VSM/SAT/VSSM/四矩 MSM | GPU 深度、矩/SAT 计算与采样；CPU 数值参考 |
| 屏幕/体积 GI | SSR/SSGI、RSM、LPV、保守三角形体素化/辐亮度 Mip/VCT、稀疏页表/节点查询 | GPU 多 Pass + CPU 参考；完整几何 BVH、16³/32³，方向光/点光六面/面积光灯心近似 |
| 时间与后处理 | SSAO/GTAO、真实投影 jitter、重投影/拒绝/钳制、HW5、SVGF 基础版、多级 Bloom、NPR | GPU Compute 与 CPU 连续帧会话；只处理线性 HDR，最终统一曝光/Tone Mapping/sRGB |
| 场景预计算 | 静态实例 PRT、网格距离场、不可变有界缓存、指纹失效、校验持久化 | CPU 后台烘焙、正常 CPU/GPU 光照查询；路径追踪仍为 CPU，不冒充硬件光追 |
| Vulkan | 前向/延迟、glTF 材质/纹理、TBN、深度、HDR、GPU 灯表、GPU 视锥剔除与 Indirect Draw | 实际 GPU 命令、读回与数值回归 |
| 工程系统 | 任务依赖/工作窃取、真实 DAG/生命周期/Barrier/JSON/DOT、版本化场景、撤销、Shader 缓存/反射/整批热重载与回退 | 主程序与独立确定性测试；附 CPU 扩展性与 GPU 完成帧分布 |
| 资产上传与驱动缓存 | 后台 glTF 解析、分预算场景快照、持久 Staging Ring、Fence 回收、取消与失败保留旧场景、原生 VkPipelineCache 持久化 | 实际 GPU Copy/读回；不是仅模拟上传队列 |

高级效果已进入真实 GPU 路径。超出体积 GI 的规模/光源限制时，面板说明原因并暂停新渲染，不启动后台 CPU 参考；显式 GPU 输出请求明确失败，不能拿 CPU 输出充当 GPU 结果。SH/PRT、网格距离场在 CPU 烘焙后进入 GPU 光照；路径追踪仍是开发者工具中的显式 CPU 参考。不同 GI 方案作为替换/对照选择，不把环境基线重复叠加。

工程页可调整单帧上传预算；进度条区分 CPU 快照、已提交和 Fence 确认完成的字节数。新场景上传期间保留旧画面，新版本完成才整体替换。取消上传不提前销毁 GPU 仍在使用的资源。Shader 热重载与回退位于开发者工具；内容缓存和驱动管线缓存分别管理：前者用于编译/反射/回退，后者按设备、驱动版本和 UUID 校验，损坏时重新建立。

### 可以复现的验证

```powershell
# 同一相机与场景，比较 CPU/GPU 的线性材质颜色
.\bin\Release\emberframe_workbench.exe --compare --width 320 --height 180
# 真 GPU 间接光与统计阴影；--headless 是 CPU，不要混用
.\bin\Release\emberframe_workbench.exe --gpu-hidden --no-ui --frames 16 --preset 2 --gi 3 --output output\rsm-gpu.bmp
.\bin\Release\emberframe_workbench.exe --gpu-hidden --no-ui --frames 8 --preset 2 --shadow 5 --output output\msm-gpu.bmp
# GPU SSGI/GTAO/TAA/SVGF，并导出本帧依赖图
.\bin\Release\emberframe_workbench.exe --gpu-hidden --no-ui --frames 16 --preset 1 --path deferred --gi 2 --ao 2 --taa --svgf --output output\temporal-gpu.bmp --graph-output output\graph
# 连续 8 帧的低采样路径追踪与 SVGF，不是单张图反复滤波
.\bin\Release\emberframe_workbench.exe --headless --preset 1 --path trace --samples 1 --svgf --frames 8 --output output\svgf.bmp
# 当前场景 GPU PRT 与距离场阴影；非独立专题图
.\bin\Release\emberframe_workbench.exe --gpu-hidden --no-ui --frames 12 --preset 2 --path deferred --environment-diffuse 2 --bake-samples 16 --sdf-shadows --output output\scene-baked-gpu.bmp
# GPU 稀疏 VCT；加 --dense-voxels 可改为稠密查询对照
.\bin\Release\emberframe_workbench.exe --gpu-hidden --no-ui --frames 8 --preset 2 --gi 5 --output output\sparse-vct-gpu.bmp
# SH、PRT、IBL、SDF 专门实验分别使用 11、12、13、30
.\bin\Release\emberframe_workbench.exe --headless --topic 13 --output output\ibl.bmp
```

2026-10-05 阴影修复后的 RTX 4060 Laptop GPU 回归中，前向/延迟最大 HDR 差异为 0.00100589，All/Tiled/Clustered 输出一致；主入口白炉（包含 Bloom）粗糙金属由 0.306772 补偿到 1.00949。CPU/GPU 材质颜色的 8326 个内部像素平均绝对误差为 0.000115111。它们是固定测试的结果，不代表任意场景都达到该误差或性能。

本次补充简历演示后的验证：**200/200 算法检查、135 项集成检查、64/64 组整体回归通过**。集成程序包含 110 项实际 GPU 检查和 25 项配套检查；算法原 193 项加 7 项演示约束检查。覆盖高级阴影/GI/AO/时间过滤、SH/PRT/SDF 正常前向/延迟、PRT 与序列化 LOD 的组合、实际 BVH/八叉树查询、GPU 稀疏节点/页表/容量保护、分预算上传、取消保留旧资源、损坏缓存恢复、整批 Shader 回退与 Resize，以及材质/实验/工程页呈现和 800×600 编辑器窗口。最后一组 Shader 的布局故障也在创建任何候选 Pipeline 前整包拒绝；故障注入后退出不泄漏资源。35 个 Vulkan 回归批次验证层实际启用，未发现 Validation Error / VUID。详细结果在 [本次回归结果](output/verification-resume-demo-validation-20261005/results.json)、同目录 `algorithm-tests.log` 和 `gpu-integration.log`；这些检查均属于整体批次，三个计数不能相加。五条简历技术的连续帧、受控数值与独立计时见 [当前演示包](output/resume-demo-validated-20261005/README.md)。

倾斜表面的自阴影条纹已修复：Hard/PCF/PCSS/CSM 按实际三角形平面校正各采样位置的深度比较，PCSS 的遮挡者搜索也使用相同校正。光照继续使用平滑/法线贴图的着色法线，阴影比较使用独立几何法线；延迟路径将其编码在现有 G-buffer 元数据的空闲两通道，不新增 Attachment。默认 Bias 保持 0.002 世界单位。GPU 检查新增 16 项，覆盖 64/256 分辨率、四种模式、无自阴影和保留 0.015 世界单位间隔的近遮挡；旧 Shader 在其中 PCF/64 检查失败（最小可见性 0.555556），新 Shader 全部通过。记录在 `output/shadow-fix-20261005/`。

当前证据：[`三分钟操作稿`](docs/WORKBENCH_DEMO.md)、`output/graph-shadow-fixed/` 的实际依赖图、`output/benchmark-final-v2-20261005/cpu-system/` 的 CPU 原始样本、`output/benchmark-shadow-fixed-20261005/` 的修复后 GPU 样本，以及 `output/demo-shadow-fixed-20261005/EmberFrame-demo.mp4`（960×540、32 秒，首尾可解码）。视频是八幅实际 GPU 结果的定帧展示，不是实时录屏或 FPS 证据；最后四秒 Disney/NPR 的排线是明确开启的效果，不是自阴影错误。旧视频与原始 HDR/PFM 保留。运行数据不作为源码提交。

新版布局的四个原生页签、800×600 窄窗口截图与日志位于 `output/editor-layout-20261005/`。基准报告绑定生成时的程序 SHA256；阴影修复阶段的计时报告采用旧布局，不把它冒充新版编辑器性能数据。

2026-10-06 高 DPI / 内嵌标题栏修复回归：200/200 算法检查、137 项 GPU 与配套检查、9 组显示用例通过；这些是重叠的不同检查集合。175% 下最大化客户区为 2560×1530，物理视口与实际渲染均为 1573×1361；另验证 2560×1440 GPU HDR 读回、640×360 固定尺寸、640×480 最小窗口与四个检查器页签。窗口生命周期检查覆盖工作区最大化、还原、最小化、全屏往返；标题栏命中规则有独立断言。验证层启用，未发现 Validation Error / VUID。截图、日志、程序 SHA256 和逐项分辨率见 [显示回归证据](output/editor-display-20261006-112418/results.json)。跨不同 DPI 显示器的真实拖动和人工逐按钮操作未纳入本次自动检查。

### 已知边界

- 当前为有界学习/实验引擎，不是生产引擎或硬件光追。屏幕空间效果只掌握第一层深度，历史对相机切换/节点变化会拒绝或重建。
- GPU 最多 64 盏灯；优先首个方向光生成阴影，无方向光时首个矩形光生成灯心透视阴影。CSM 与 GPU SDF 仅用于方向光。体积 GI 网格为 16³/32³，完整几何由 GPU BVH 查询，容量由设备 SSBO/显存决定，不截断。GI 选第一盏发光的方向光、面积光或点光作为主源，其余灯仅参与直接光；面积光用灯心辐射近似。体素材质不烘焙贴图/Alpha；各向同性体素、低阶 LPV 与有限 RSM 采样仍有漏光、低频误差或斑点。
- GPU 环境为后台预过滤解析天光 + 运行时 LUT 查询，无外部 HDR 导入；环境各向异性/清漆未做完整专属积分。LTC 是粗拟合表，不宣称任意参数全局误差上界。
- VSM/VSSM 会漏光；MSM 退化分布回退 VSM；Float32 SAT 大分辨率小区域查询有精度限制。阴影 2048 档显著增加显存。
- 接收平面校正支持方向光正交投影与矩形光灯心透视投影；透视矩/SAT 与软阴影距离计算先还原线性光源深度。矩形光中心深度图与 PCSS/VSSM 半影仍是近似，不是对整个发光面的精确可见性积分。阴影分辨率、退化平面和跨几何边界的大滤波核仍限制效果。
- QEM 当前限制 4096 个输入三角形，保护接缝但不做全局自相交检查；自动 LOD 消费后台生成的真实链，切换没有 geomorphing。选级变化可能触发该几何版本的 PRT/距离场准备。
- SH/PRT 为九系数低阶漫反射近似；正常 PRT 为静态可见性 transfer，不等同于多次反弹/动态可变形 PRT。法线贴图不重烘焙几何传输。距离格点为无符号网格距离，保守查询和格距偏置会损失细薄遮挡，不能代替高精度 Shadow Map。烘焙有限额，超额明确报错。
- GPU 稀疏体素查询已实际使用节点 SSBO；体素化/Mip 仍有稠密中间态和最坏容量预留，不宣称节省物理显存，也不是商用 VXGI 的完整稀疏八叉树。
- SVGF 尚无独立的镜面/漫反射重建与反照率解调。
- Render Graph 覆盖模块交接，模块内部计算屏障由各自执行器管理；不是所有内部 Dispatch 的完整自动 DAG。当前单队列、整资源生命周期，无物理 Alias 分配。
- 静态 glTF 导入保留材质采样器；蒙皮/形变等不能正确表达的输入会明确拒绝或报告限制。参考路径不实现透明折射。

如果从零开始学习和共建，请从 [学习与项目进度路线图](docs/learning/LEARNING_PATH.md) 和
[第一课：SDL 窗口与事件循环](docs/learning/01_WINDOW_AND_EVENT_LOOP.md) 开始，不要直接阅读完整的 `chapter-6/vk_engine.cpp`。

图形算法学习线见 [Software Renderer 零基础学习索引](docs/learning/software_renderer/README.md)。A1—A8 已形成从 Framebuffer 到可交互 OBJ CPU Renderer 的连续 Sample；代码准备完成不等于学习验收完成，掌握状态以总路线图为准。

C 系列第一批已建立独立 Launcher、Vulkan 帧闭环和 CPU/GPU 基线；运行步骤与当前限制见 [C1/C2/C10 第一批实现说明](docs/learning/C1_C2_C10_FIRST_BATCH.md)。

C 系列第二批将真实三角形 Draw 接入正式入口，并用代际 Handle、提交序号与延迟回收管理 Pipeline 热重载；见 [C3/C4 第二批实现说明](docs/learning/C3_C4_SECOND_BATCH.md)。

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

上面是长期目标结构；当前新算法和交互工具统一放在 `engine/lab`，平台/Runtime/Renderer 的入门模块保留。没有为了凑目录把同一功能再复制一份。

## 来源与许可

上游代码来自 [Vulkan Guide](https://github.com/vblanco20-1/vulkan-guide)，采用 MIT License。架构阅读参考为 [Piccolo](https://github.com/BoomingTech/Piccolo)。详细版本与使用边界见 [UPSTREAM.md](UPSTREAM.md)。
