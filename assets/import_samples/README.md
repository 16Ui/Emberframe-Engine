# 免费导入资源

这些是外部下载资源，不是引擎内置网格。使用正常模型/纹理导入器和普通实时渲染管线。

| 资源 | 来源与作者 | 许可 | 用途 |
| --- | --- | --- | --- |
| BoxVertexColors | [Khronos / Marco Hutter](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/BoxVertexColors) | CC0-1.0 | 顶点颜色、静态网格、节点变换 |
| CesiumMilkTruck | [Khronos / Cesium](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/CesiumMilkTruck) | CC-BY-4.0，附 Cesium 标识说明 | 多节点、共享网格、材质、模型自带纹理；导入静态姿态，不播放动画 |
| Tiles074 1K-JPG | [ambientCG](https://ambientcg.com/view?id=Tiles074) | [CC0-1.0](https://docs.ambientcg.com/license/) | 地板颜色、法线、粗糙度贴图 |

Khronos 下载固定于提交 `edc7c9e67c639d230715049ee31f9a96a6babbbe`。每个模型的原始来源/许可说明随文件下载保存；二进制资源不默认送入 Git。送奶车须保留 Cesium 的署名和许可说明；标识不表示 Cesium 对 EmberFrame 的支持或背书。

在项目根目录的 PowerShell 中：

```powershell
.\scripts\download-import-samples.ps1
.\scripts\open-import-sample.ps1
```

资源已经下载、程序已经编译后，也可双击项目根目录的 `Open-Free-Assets.cmd`。

后者首次创建 `output/import-scenes/free-assets.ember`，之后只打开，不覆盖你保存的修改。工程包含：两份导入模型、地板三张贴图、方向光、点光源及可编辑矩形面积光。窗口内是普通实时前向渲染，不启动 CPU 对照或验证。

此入门工程明确使用 **最多 512px 的贴图预览副本**，由原图已有的正确过滤 Mip 生成，减少文本工程体积和启动解析；原始 GLB 和下载的地砖 1K 原图保持不变，可从资源库完整导入或重新绑定。最初候选的 CAD 金属球组展开后约 104 万三角形，因此未采用为默认入门场景。

也可从“工程 → 免费示例资源”单独追加模型或绑定贴图。文件下载失败时不要把 `.partial` 当成模型使用。

## 自己搭建

1. 文件 → 新建场景：选择空场景或带地板，并决定是否放初始方向光。新建前提示保存；不会沿用旧工程保存路径。
2. 文件 → 追加模型：GLB、glTF、OBJ/MTL。GLB 更方便；glTF/OBJ 的依赖图片、BIN、MTL 要保持相对目录结构。加载失败保留已有场景。
3. 工具 → 添加光源：方向光、点光源、矩形面积光。在场景树选中后修改参数，位置用平移工具；方向光改方向而非位置。
4. 材质 → 纹理贴图 → 导入：选择目标材质与槽位。颜色/自发光按 sRGB 解释；法线/AO/金属粗糙度按线性数据解释。
5. 文件 → 另存为：保存为独立 `.ember`。工程内包含网格、纹理、材质、节点和光源数据，原始模型不被改写。

## 贴图与当前限制

- 示例使用 `NormalGL`，不是符号约定不同的 `NormalDX`。
- 引擎金属/粗糙度槽采用 glTF：G 通道是粗糙度，B 通道是金属度。示例创建器将灰度 Roughness 显式打包成 G，并把 B 设为 0。**不能把任意灰度粗糙度图直接当作这张合成贴图**。
- 空场景不等于黑背景：默认解析天空仍可提供环境光；没有模型时没有实体可选择。
- 目前是静态模型编辑。蒙皮、骨骼动画、VRM、PMX/MMD、FBX 不作为直接兼容承诺；需要转换为兼容的静态 GLB/OBJ。外部 HDR 环境导入尚不支持。
- 一张图片最大 8192，每张基础图最多 8M 像素，整个场景含 Mip 的纹理总量最多 16M 像素；不能把大型场景无限追加。外部资产超限会明确拒绝，不静默减质。

2026-10-07 本次只针对新建场景、示例模型/纹理/光源的导入与显示进行试用；不等于全部渲染算法或性能回归通过。

## 本次实际试用范围

- 使用正常 C++ 导入器读取两份 GLB，绑定地砖三类贴图，添加方向光、点光源、矩形面积光，保存为独立 `.ember`；随后重新读取并使用 Vulkan 前向显示。
- 新版程序的空场景入口可打开，没有模型时保持正常界面，不自动启动 CPU 任务。
- 最终示例为 3640 个实例三角形；显示试用时按原生视口渲染为 1531×1319。截图为 `output/import-scenes/free-assets-light-preview.bmp`，运行记录为同目录的 `free-assets-light-display.json`。
- 不把统计栏的单帧 GPU 时间作为性能结论；不运行算法检查、CPU/GPU 回归或基准。该次进程未找到 Validation Layer，不能宣称验证层验收通过；系统文件选择器的实际点击和连续编辑仍需手动试用。
