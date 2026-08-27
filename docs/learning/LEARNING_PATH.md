# EmberFrame 零基础共建路线

这条路线服务于“理解后再沉淀到正式引擎”，而不是快速复制 Vulkan Guide。每一课必须同时满足四个条件：能解释输入与输出、能独立运行、能观察失败、能说明它如何进入正式引擎。

## 两条并行线路

### 学习 Sample

`samples/` 中的程序一次只引入一个新问题。它们保留必要的展开代码，方便观察 API 调用顺序，不提前隐藏在复杂封装后面。

### 正式 Engine

当一个 Sample 的概念通过解释和运行验证后，再把稳定职责沉淀到 `engine/`。教程代码、学习代码和正式引擎代码必须能明确区分。

## 依赖顺序

1. SDL 窗口、事件循环和显式生命周期；
2. RAII 与 Platform 窗口封装；
3. 时间、输入状态和引擎事件；
4. Vulkan Instance、Surface 和校验层；
5. Physical Device、Logical Device 与 Queue；
6. Swapchain、Image View 和窗口尺寸变化；
7. Command Buffer 与 GPU/CPU 同步；
8. Dynamic Rendering 清屏；
9. Shader、Pipeline 与三角形；
10. Buffer、Image、Descriptor 与资源所有权；
11. Mesh、纹理和 glTF 场景；
12. 代际 Handle、延迟销毁、Render Graph 与性能工具。

## 每课工作方式

1. 先定义当前问题，不引入后续术语；
2. 写最小可运行代码；
3. 构建并观察行为；
4. 主动制造一个可解释的失败；
5. 用自己的话复述对象、顺序和所有权；
6. 通过后才进入重构或下一课。

## 当前进度

- 第一课：SDL 窗口与事件循环，代码和讲义已经建立；
- 下一道关口：学习者能够解释 SDL 初始化、窗口创建、事件轮询和逆序清理的因果关系。

第一课讲义见 [01_WINDOW_AND_EVENT_LOOP.md](01_WINDOW_AND_EVENT_LOOP.md)。
