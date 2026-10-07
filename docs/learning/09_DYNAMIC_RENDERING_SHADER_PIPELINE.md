# B9：Dynamic Rendering、Shader 与 Graphics Pipeline

对应源码入口：[samples/09_triangle_pipeline/main.cpp](../../samples/09_triangle_pipeline/main.cpp)。

本章目标不是记住一批 `Vk*CreateInfo`，而是建立下面这条完整关系：

```text
GLSL Shader
→ 编译为 SPIR-V
→ 创建 Shader Module
→ 与固定功能状态一起创建 Graphics Pipeline
→ 每帧把一个 Swapchain ImageView 指定为 Color Attachment
→ 录制并提交 vkCmdDraw(3, 1, 0, 0)
→ GPU 生成 Fragment 并把颜色写进 Swapchain Image
→ Present 到窗口
```

---

## 1. B9 在 B8 的哪个位置

B8 已经建立了完整外层帧循环：

```text
Wait Fence
→ Acquire Swapchain Image
→ 录制 Command Buffer
→ Graphics Queue Submit
→ Present Queue Present
```

B8 的 Command Buffer 中间只有：

```text
Layout Barrier
→ vkCmdClearColorImage
→ Layout Barrier
```

B9 保留外层同步，只替换中间真正的 GPU 工作：

```text
Layout Barrier
→ Begin Dynamic Rendering
→ Bind Graphics Pipeline
→ Set Viewport / Scissor
→ vkCmdDraw
→ End Rendering
→ Layout Barrier
```

所以两章分别回答：

```text
B8：什么时候可以安全写 Image，写完什么时候可以显示？
B9：GPU 根据哪些程序和状态，把一个三角形变成 Image 中的像素？
```

---

## 2. 先建立 B9 的完整对象关系

### 2.1 Swapchain、Image、ImageView 与 Attachment

一个 Swapchain 管理多张 `VkImage`。Image 保存真实像素数据，但应用在渲染时通常不直接把“裸 Image”作为颜色输出接口，而是使用引用该 Image 的 `VkImageView`。

在本项目中，每张 Swapchain Image 对应一个 ImageView：

```text
Swapchain Image 0 ← ImageView 0 引用它
Swapchain Image 1 ← ImageView 1 引用它
Swapchain Image 2 ← ImageView 2 引用它
```

ImageView 说明本次怎样访问 Image，例如：

- 使用哪种像素格式解释数据；
- 访问哪个颜色、深度或模板方面；
- 访问哪些 Mip Level 和 Array Layer。

每帧 Acquire 返回 `imageIndex` 后，B9 选择：

```cpp
swapchain_image_views[imageIndex]
```

然后把这个 View 放进 `VkRenderingAttachmentInfo`，让它在本次 Rendering 中承担 **Color Attachment** 角色。

Color Attachment 不是另一张图片。它表示：

> 本次 Rendering 将 Fragment Shader 的颜色输出写进哪个 ImageView，并且开始时如何处理旧内容、结束后是否保存结果、使用什么 Layout。

本课的 Attachment 配置是：

```cpp
color_attachment.imageView = swapchain_image_view;
color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
```

具体含义：

```text
imageView：颜色最终写入哪张 Image
loadOp=CLEAR：Rendering 开始时先清除旧内容
storeOp=STORE：Rendering 结束后保留结果，之后才能 Present
imageLayout：本次把它作为颜色输出目标使用
```

### 2.2 Shader 与 Graphics Pipeline

Shader 只描述 GPU 上某个可编程阶段怎样处理输入。Graphics Pipeline 则把 Shader 与一组固定功能状态组合成一套可绑定的绘制配置。

本章只有两个可编程阶段：

```text
Vertex Shader
→ 逐顶点运行，输出 Clip Space 位置和顶点颜色

Fragment Shader
→ 逐 Fragment 运行，输出最终颜色
```

Pipeline 还保存或约束：

```text
顶点数据怎样输入
顶点怎样组成三角形
三角形怎样光栅化
是否进行背面剔除
是否启用多重采样
颜色怎样混合和写入
哪些状态在录制命令时动态提供
Shader 能访问哪些外部资源
输出 Attachment 使用什么像素格式
```

因此 Pipeline 不是一条命令，也不是一张 Image。它是 Draw Call 执行时使用的一套长期配置对象。

### 2.3 Draw Call 把两边连接起来

每帧录制：

```cpp
vkCmdBeginRendering(...); // 指定本次输出到哪个 Attachment
vkCmdBindPipeline(...);   // 指定使用哪套 Shader 和固定功能状态
vkCmdDraw(...);           // 发起顶点处理和三角形绘制
vkCmdEndRendering(...);
```

三者分别回答：

```text
BeginRendering：结果写到哪里？
BindPipeline：用什么程序和规则处理？
Draw：处理多少顶点和实例？
```

这就是 B9 最核心的关系。

---

## 3. Shader 从源码到 GPU Pipeline

### 3.1 GLSL 源码

B9 的 Shader 位于：

1. [b9_triangle.vert](../../samples/09_triangle_pipeline/shaders/b9_triangle.vert)
2. [b9_triangle.frag](../../samples/09_triangle_pipeline/shaders/b9_triangle.frag)

`.vert` 是 Vertex Shader，`.frag` 是 Fragment Shader。源码使用 GLSL（OpenGL Shading Language）语法，但本项目把它编译为 Vulkan 使用的 SPIR-V。

### 3.2 SPIR-V

SPIR-V 是 Vulkan Shader 使用的标准中间二进制表示。构建时执行：

```text
GLSL 源文件
→ glslangValidator -V
→ .spv 二进制文件
```

运行时程序读取 `.spv`，再调用：

```cpp
vkCreateShaderModule(...);
```

得到 `VkShaderModule`。

完整关系是：

```text
人编写 GLSL
→ 构建工具编译成 SPIR-V
→ CPU 程序读取 SPIR-V
→ Vulkan 创建 Shader Module
→ Graphics Pipeline 创建时引用 Shader Module
```

Pipeline 创建成功后，Shader Module 可以销毁。因为 Module 只是创建 Pipeline 时提供 Shader 代码的临时 Vulkan 对象；已经创建的 Pipeline 不依赖 Module 继续存活。

---

## 4. Vertex Shader 做了什么

B9 暂时没有 Vertex Buffer。三个顶点位置直接保存在 Vertex Shader 中：

```glsl
const vec2 positions[3] = vec2[3](
    vec2( 0.0, -0.65),
    vec2( 0.65, 0.55),
    vec2(-0.65, 0.55)
);
```

调用：

```cpp
vkCmdDraw(commandBuffer, 3, 1, 0, 0);
```

会产生三个 Vertex Shader Invocation。三次运行分别得到：

```text
gl_VertexIndex = 0
gl_VertexIndex = 1
gl_VertexIndex = 2
```

Shader 用这个索引取得对应位置和颜色：

```glsl
gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
vertexColor = colors[gl_VertexIndex];
```

`gl_Position` 是 Vertex Shader 必须输出的 Clip Space 齐次坐标：

```text
(x, y, z, w)
```

本课直接令 `w=1`，没有 Model/View/Projection 变换，所以除以 `w` 后位置不变，直接落在 NDC 范围中。

`vertexColor` 使用：

```glsl
layout(location = 0) out vec3 vertexColor;
```

表示 Vertex Shader 的 location 0 输出一个三维颜色，交给后续光栅化与 Fragment Shader。

---

## 5. 三个顶点怎样变成许多 Fragment

Pipeline 的 Input Assembly 设置为：

```cpp
input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
```

它把连续三个顶点组成一个三角形：

```text
Vertex 0 + Vertex 1 + Vertex 2
→ 一个 Triangle Primitive
```

随后固定功能阶段完成：

```text
Clip Space
→ 裁剪
→ Perspective Divide
→ NDC
→ Viewport 变换
→ 屏幕上的三角形
→ Rasterization
→ 生成覆盖区域内的 Fragments
```

一个三角形不是只对应一个 Fragment。一个三角形通常覆盖很多像素，因此光栅化会生成很多 Fragment。每个 Fragment 都携带从三个顶点插值得到的颜色。

这与 A3 的 CPU 光栅化是同一件事：

```text
A3：CPU 遍历包围盒，用边函数和重心坐标找覆盖像素并插值
B9：GPU 固定功能光栅化阶段并行完成覆盖判断和属性插值
```

B9 不需要自己在 C++ 中计算重心坐标，但硬件光栅化阶段仍然完成了对应工作。

---

## 6. Fragment Shader 做了什么

Fragment Shader 接收插值后的颜色：

```glsl
layout(location = 0) in vec3 vertexColor;
```

其中 `location = 0` 必须与 Vertex Shader 的输出位置匹配。

然后输出：

```glsl
layout(location = 0) out vec4 outColor;

outColor = vec4(vertexColor, 1.0);
```

这个 `location = 0` 表示 Fragment Shader 的第 0 个颜色输出。当前 Rendering 也只配置了一个 Color Attachment，因此关系是：

```text
Fragment Shader location 0 颜色输出
→ Rendering 的 Color Attachment 0
→ Attachment 引用的 Swapchain ImageView
→ ImageView 引用的 Swapchain Image
```

Fragment Shader 并不保存一张独立图片。它只为当前 Fragment 给出颜色结果；Pipeline 的颜色输出阶段负责按照 Attachment 与混合状态，把结果写进目标 Image。

B9 没有 Depth Attachment 和深度测试，所以三角形的 Fragment 不需要与深度缓冲比较。

---

## 7. Graphics Pipeline 创建时配置了什么

创建逻辑位于 [create_graphics_pipeline()](../../samples/09_triangle_pipeline/triangle_pipeline_probe.cpp)。

### 7.1 Shader Stage

```text
Vertex Shader Module + main 入口
Fragment Shader Module + main 入口
```

### 7.2 Vertex Input

本课为空，因为位置和颜色直接来自 `gl_VertexIndex`：

```cpp
VkPipelineVertexInputStateCreateInfo vertex_input {...};
```

B10 才会创建 GPU Buffer 并描述真实顶点内存布局。

### 7.3 Input Assembly

```text
TRIANGLE_LIST
→ 每三个顶点组成一个独立三角形
```

### 7.4 Viewport 与 Scissor

Viewport 负责把 NDC 坐标映射到窗口区域；Scissor 进一步限制哪些像素允许写入。

Pipeline 声明它们为 Dynamic State：

```cpp
VK_DYNAMIC_STATE_VIEWPORT
VK_DYNAMIC_STATE_SCISSOR
```

因此窗口尺寸不写死在 Pipeline 中，而是在每帧录制时调用：

```cpp
vkCmdSetViewport(...);
vkCmdSetScissor(...);
```

窗口 Resize 后只要颜色 Format 不变，Pipeline 不需要因为宽高变化而重建。

### 7.5 Rasterization

```text
polygonMode = FILL：填充三角形内部
cullMode = NONE：暂时不剔除正面或背面
lineWidth = 1：默认线宽
```

### 7.6 Multisampling

```text
rasterizationSamples = 1
```

当前没有 MSAA，每个像素只有一个样本。

### 7.7 Color Blend

```text
blendEnable = false
colorWriteMask = RGBA
```

表示不与 Attachment 中的旧颜色混合，直接写入 Fragment Shader 输出，并允许写四个颜色通道。

### 7.8 Pipeline Layout

`VkPipelineLayout` 描述 Shader 能通过哪些 Descriptor Set 与 Push Constant 接收外部资源和少量参数。

B9 的数据全部写在 Shader 内，没有纹理、Uniform 或 Push Constant，因此 Pipeline Layout 为空。它仍必须创建，因为 Graphics Pipeline 必须引用一个 Pipeline Layout。

### 7.9 Dynamic Rendering 输出格式

Dynamic Rendering 不使用 `VkRenderPass`，但 Pipeline 仍然必须提前知道颜色输出的格式：

```cpp
VkPipelineRenderingCreateInfo rendering_info {...};
rendering_info.colorAttachmentCount = 1;
rendering_info.pColorAttachmentFormats = &color_format;
```

这个 `color_format` 必须与实际 Begin Rendering 时绑定的 Swapchain ImageView 格式兼容。

所以 Resize 时：

```text
只有 Extent 改变
→ 动态 Viewport/Scissor 更新即可

Swapchain Format 改变
→ Graphics Pipeline 需要重建
```

---

## 8. Dynamic Rendering 解决了什么

一次 Rendering 需要建立下面的关系：

```text
本次颜色输出槽位
→ 使用哪个 ImageView
→ 开始时 Clear 还是 Load
→ 结束后 Store 还是丢弃
→ 当前使用什么 Layout
```

传统 Vulkan 主要使用两个持久对象提前描述：

```text
VkRenderPass：描述 Attachment 结构和处理规则
VkFramebuffer：把具体 ImageView 绑定到 Attachment
```

Dynamic Rendering 在录制 Command Buffer 时直接填写：

```text
VkRenderingAttachmentInfo
→ 具体 ImageView + Load/Store/Layout

VkRenderingInfo
→ Render Area + Layer + 本次使用的 Attachment 列表
```

然后录制：

```cpp
vkCmdBeginRendering(commandBuffer, &rendering_info);
```

它没有删除 Image、ImageView 或 Attachment 这些概念，而是不再要求为这组关系预先创建 `VkRenderPass` 和 `VkFramebuffer` 持久对象。

---

## 9. B9 每帧录制的完整命令

录制逻辑位于 [record_triangle_commands()](../../samples/09_triangle_pipeline/triangle_pipeline_probe.cpp)。

### 第一步：转换为 Color Attachment Layout

```text
UNDEFINED
→ COLOR_ATTACHMENT_OPTIMAL
```

因为本帧会先 Clear 整张图，所以旧内容不需要保留。

Barrier 的目标阶段和访问类型为：

```text
dstStage = COLOR_ATTACHMENT_OUTPUT
dstAccess = COLOR_ATTACHMENT_WRITE
```

### 第二步：描述本次 Color Attachment

```text
ImageView = imageViews[imageIndex]
LoadOp = CLEAR
StoreOp = STORE
Layout = COLOR_ATTACHMENT_OPTIMAL
```

### 第三步：Begin Rendering

```cpp
vkCmdBeginRendering(...);
```

从这里开始，到 `vkCmdEndRendering()` 结束，Draw Call 的颜色输出都写入刚才指定的 Attachment。

### 第四步：绑定 Graphics Pipeline

```cpp
vkCmdBindPipeline(
    commandBuffer,
    VK_PIPELINE_BIND_POINT_GRAPHICS,
    graphicsPipeline);
```

后续 Draw 使用这套 Shader 与固定功能状态。

### 第五步：设置动态 Viewport 和 Scissor

```cpp
vkCmdSetViewport(...);
vkCmdSetScissor(...);
```

二者都覆盖完整 Swapchain Extent。

### 第六步：Draw Call

```cpp
vkCmdDraw(commandBuffer, 3, 1, 0, 0);
```

四个参数依次表示：

```text
vertexCount   = 3：每个实例处理三个顶点
instanceCount = 1：只画一个实例
firstVertex   = 0：第一个 gl_VertexIndex 从 0 开始
firstInstance = 0：第一个实例索引从 0 开始
```

这条命令仍然只是被录入 Command Buffer。真正执行要等 `vkQueueSubmit2()`，并且 `imageAvailable` 等待得到满足。

### 第七步：End Rendering

```cpp
vkCmdEndRendering(...);
```

结束本次 Attachment 写入范围。

### 第八步：转换为 Present Layout

```text
COLOR_ATTACHMENT_OPTIMAL
→ PRESENT_SRC_KHR
```

然后才可以沿用 B8 的 `renderFinished → Present` 链路显示这张 Image。

---

## 10. 一次 Draw 的完整 GPU 数据流

把前面所有对象串起来：

```text
vkCmdDraw(vertexCount=3)
→ Vertex Shader 运行 3 次
→ gl_VertexIndex 取得 3 个顶点位置和颜色
→ Input Assembly 组成 1 个三角形
→ Clip / Perspective Divide / Viewport
→ Rasterizer 生成许多 Fragments
→ 顶点颜色在三角形内部插值
→ Fragment Shader 为每个 Fragment 输出 outColor
→ Color Output 阶段应用 Color Blend 状态
→ 写入 Color Attachment 0
→ Attachment 指向 imageViews[imageIndex]
→ ImageView 引用 Swapchain Images[imageIndex]
→ Image 转成 PRESENT_SRC_KHR
→ Present Queue 显示
```

这一条就是 B9 最终需要掌握的主线。

---

## 11. B8 到 B9 的同步变化

B8 第一条访问 Image 的 GPU 工作是：

```text
Transfer Clear
```

所以等待 `imageAvailable` 的 Stage 是：

```cpp
VK_PIPELINE_STAGE_2_TRANSFER_BIT
```

B9 第一条真正需要取得 Image 的工作变为颜色输出，因此使用：

```cpp
VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
```

外层同步关系没有变化：

```text
Acquire Signal imageAvailable
→ Graphics Submit Wait
→ 执行 Rendering 与 Draw
→ Signal renderFinished[imageIndex]
→ Present Wait
```

改变的是 Command Buffer 中实际访问 Image 的 Pipeline Stage。

---

## 12. 构建和运行

在 PowerShell 中：

```powershell
cd D:\games\Emberframe-Engine
```

构建：

```powershell
.\scripts\build-windows.ps1 -Config Release -Target emberframe_09_triangle_pipeline
```

运行：

```powershell
.\bin\Release\emberframe_09_triangle_pipeline.exe
```

12 帧冒烟测试：

```powershell
.\bin\Release\emberframe_09_triangle_pipeline.exe --smoke-test
```

应看到深色背景上的红、绿、蓝渐变三角形，以及：

```text
[B9] Graphics Pipeline ready
[B9] BeginRendering -> BindPipeline -> Draw(3) -> Present
[B9] Smoke test rendered and presented twelve triangle frames.
```

如果日志显示：

```text
VK_LAYER_KHRONOS_validation: unavailable
```

只能说明程序成功运行，不能据此判断已经通过 Validation Layer 检查。需要先恢复 Layer 可用性，再进行错误实验和最终验收。

---

## 13. 源码阅读顺序

1. [main.cpp：整体对象依赖和帧循环](../../samples/09_triangle_pipeline/main.cpp)
2. [Vertex Shader：三个位置和颜色](../../samples/09_triangle_pipeline/shaders/b9_triangle.vert)
3. [Fragment Shader：输出插值颜色](../../samples/09_triangle_pipeline/shaders/b9_triangle.frag)
4. [create_graphics_pipeline()：Pipeline 创建](../../samples/09_triangle_pipeline/triangle_pipeline_probe.cpp)
5. [record_triangle_commands()：每帧命令录制](../../samples/09_triangle_pipeline/triangle_pipeline_probe.cpp)
6. [draw_frame()：B8 同步外壳如何包住 B9 Draw](../../samples/09_triangle_pipeline/triangle_pipeline_probe.cpp)
7. [CMakeLists.txt：GLSL 编译与 SPIR-V 复制](../../samples/09_triangle_pipeline/CMakeLists.txt)

阅读时不要先记所有字段。持续追踪四个问题：

```text
顶点数据从哪里来？
使用哪套 Shader 和状态？
Fragment 输出写到哪里？
什么时候真正提交给 GPU？
```

---

## 14. 三个学习实验

### 实验 1：修改一个顶点位置

在 Vertex Shader 中把：

```glsl
vec2(0.0, -0.65)
```

改成：

```glsl
vec2(0.0, -0.9)
```

运行前先预测三角形哪个顶点会移动、面积怎样变化，再重新构建验证。

### 实验 2：修改顶点颜色

把第一个顶点的红色改成黄色：

```glsl
vec3(1.0, 1.0, 0.0)
```

观察三角形内部颜色插值怎样变化。

### 实验 3：修改 Draw 的 vertexCount

把：

```cpp
vkCmdDraw(commandBuffer, 3, 1, 0, 0);
```

临时改成：

```cpp
vkCmdDraw(commandBuffer, 2, 1, 0, 0);
```

`TRIANGLE_LIST` 无法用两个顶点组成三角形，所以只会看到 Clear 后的背景。验证后改回 3。

---

## 15. 常见错误与排查

### 错误 1：Shader 编译失败

检查 GLSL 语法和构建日志中的 `glslangValidator` 输出。`.spv` 是构建产物，不应手工编辑。

### 错误 2：找不到 SPIR-V

确认下面文件存在：

```text
bin/Release/shaders/b9_triangle.vert.spv
bin/Release/shaders/b9_triangle.frag.spv
```

### 错误 3：Pipeline 输出格式与 Attachment 不匹配

Dynamic Rendering 创建 Pipeline 时声明的 `colorAttachmentFormat` 必须与实际 ImageView 的格式兼容。Swapchain Format 变化时要重建 Pipeline。

### 错误 4：忘记 Begin Rendering

`vkCmdDraw()` 不能凭空知道颜色输出目标。必须先通过 `VkRenderingInfo` 和 Attachment 建立当前 Rendering 范围。

### 错误 5：忘记绑定 Pipeline

Draw Call 不会自动选择 Shader。必须在 Draw 前绑定兼容的 Graphics Pipeline。

### 错误 6：Image Layout 不正确

Rendering 前必须进入颜色 Attachment 可用布局，Present 前必须进入 `PRESENT_SRC_KHR`。

---

## 16. 面试时怎样回答

### Graphics Pipeline 是什么？

> Graphics Pipeline 把 Vertex/Fragment Shader 等可编程阶段与 Input Assembly、Rasterization、Multisampling、Color Blend 等固定功能状态组合成可绑定对象。录制 Draw Call 前绑定 Pipeline，GPU 才知道怎样解释顶点、怎样组装图元、使用哪些 Shader，以及如何把 Fragment 结果写入 Attachment。

### Dynamic Rendering 做了什么？

> Dynamic Rendering 在命令录制阶段通过 VkRenderingInfo 和 VkRenderingAttachmentInfo 直接指定本次使用的 ImageView、Render Area、Load/Store 和 Layout，不再要求预先创建 VkRenderPass 和 VkFramebuffer。Pipeline 仍需通过 VkPipelineRenderingCreateInfo 声明兼容的 Attachment Format。

### `vkCmdDraw(3, 1, 0, 0)` 发生了什么？

> 它向 Command Buffer 录制一次非索引绘制：处理三个顶点和一个实例。本项目的 Vertex Shader 根据 gl_VertexIndex 生成三个顶点，Input Assembly 将其组成一个三角形，Rasterizer 产生 Fragments 并插值颜色，Fragment Shader 输出颜色，最后写入 Dynamic Rendering 指定的 Swapchain Color Attachment。真正执行发生在 Queue Submit 之后。

---

## 17. B9 完成验收

- [ ] 能说明 B8 外层同步与 B9 绘制内容的关系；
- [ ] 能说明 GLSL、SPIR-V、Shader Module 和 Pipeline 的先后关系；
- [ ] 能解释 Image、ImageView、Color Attachment 的引用关系；
- [ ] 能说明 Graphics Pipeline 包含哪些主要状态；
- [ ] 能复述 BeginRendering → BindPipeline → Draw → EndRendering；
- [ ] 能解释三个顶点怎样变成许多 Fragment；
- [ ] 能解释 Fragment Shader 输出怎样到达 Swapchain Image；
- [ ] 能独立构建并看到彩色三角形；
- [ ] 能完成一次 Shader 小改动并预测结果；
- [ ] 能解释一种 Pipeline、Shader 或 Layout 错误。

完成 B9 后，B10 会把 Shader 内硬编码的顶点替换成真正的 GPU Vertex/Index Buffer，并学习 VMA 与 Staging Upload。

