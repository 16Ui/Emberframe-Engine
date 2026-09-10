# Software Renderer 零基础学习索引

这条线路通过 CPU 软件光栅化理解 GPU 渲染管线。代码已经按依赖关系准备好，但学习时仍应一次只进入一课：先运行、再读输入与输出、最后自己复述计算链。

跨 Software Renderer、Vulkan 和正式引擎的状态统一记录在 [学习与项目进度路线图](../LEARNING_PATH.md)。本页只负责 Software Renderer 的逐课入口。

| 顺序 | 主题 | 代码状态 | 学习状态 |
|---:|---|---|---|
| 1 | [CPU Framebuffer](01_CPU_FRAMEBUFFER.md) | 已完成 | 已完成 |
| 2 | [Bresenham 直线](02_BRESENHAM_LINES.md) | 已完成 | 下一课 |
| 3 | [三角形与重心坐标](03_TRIANGLE_RASTERIZATION.md) | 已完成 | 待学习 |
| 4 | [深度缓冲](04_DEPTH_BUFFER.md) | 已完成 | 待学习 |
| 5 | [坐标变换与背面剔除](05_TRANSFORMS_AND_CULLING.md) | 已完成 | 待学习 |

## 为什么按这个顺序

```text
Framebuffer 提供 setPixel
→ 直线算法决定一系列离散像素
→ 三角形算法决定二维区域内的像素和插值权重
→ 深度缓冲用插值权重计算每个像素的深度并解决遮挡
```

后一步依赖前一步产生的概念。可以提前浏览目录，但不要把后续实现当作需要背诵的答案。

## 构建目标

```powershell
.\scripts\build-windows.ps1 -Config Release -Target emberframe_sr_02_lines
.\scripts\build-windows.ps1 -Config Release -Target emberframe_sr_03_triangle
.\scripts\build-windows.ps1 -Config Release -Target emberframe_sr_04_depth
.\scripts\build-windows.ps1 -Config Release -Target emberframe_sr_05_transforms
```

每个程序会在 `bin/Release` 中生成同名可执行文件，并在运行后输出对应的 PPM 图片。
