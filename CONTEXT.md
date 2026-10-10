# EmberFrame Engine

这个上下文统一正式引擎中的资源与帧生命周期术语，避免把引擎层身份、Vulkan 对象和显存分配混为一谈。

## Language

**Frame Slot（帧槽）**：CPU 按帧轮换使用的一组命令与同步资源，不等同于一张 Swapchain Image。
_Avoid_: 当前画面、Swapchain 槽位

**Submission Serial（提交序号）**：FrameContext 给成功提交的 GPU 工作分配的递增编号，用于判断旧资源何时可以回收。
_Avoid_: 帧号、Image Index

**Resource Handle（引擎资源句柄）**：由 Registry ID、索引和代数组成的引擎层资源身份；不是 `VkBuffer`、`VkImage` 等 Vulkan Handle。
_Avoid_: Vulkan Handle、内存指针

**Retired Resource（已退休资源）**：已不能被新命令通过引擎句柄取得、但可能仍被旧 GPU 提交使用的资源。
_Avoid_: 已销毁资源

### 场景编辑

**Scene Node（场景节点）**：场景中具有独立名称、父子关系和变换的实例，可引用一份共享网格资源。节点属性描述这个实例，而不是它引用的数据本身。
_Avoid_: 模型文件、网格资源

**Mesh Asset（网格资源）**：可被多个场景节点引用的几何数据，包含顶点、索引和分部材质引用。修改共享网格会影响所有引用它的实例。
_Avoid_: 场景节点、单个物体实例

**Material Asset（材质资源）**：描述表面反射与纹理引用的共享数据；多个网格分部可以使用同一材质。独立材质是明确复制后重新绑定的数据，不等同于节点变换。
_Avoid_: 节点属性、纹理图片

**Texture Asset（纹理资源）**：由图像层级、颜色解释和采样方式构成的共享表面数据，可被材质引用。
_Avoid_: 材质、场景模型

### 演示与性能分析

**Showcase（代表场景）**：具有固定观察目标、默认镜头和质量配置的可编辑场景；不是替代实时渲染的截图或视频。
_Avoid_: 性能基准、离线效果图

**Completed Frame Profile（已完成帧分析）**：与一个已完成提交序号对应的 CPU/GPU 分阶段测量，不能与正在录制的帧或另一帧的统计混称同一结果。
_Avoid_: 当前帧 FPS、录制耗时即 GPU 耗时
