# B10：GPU Buffer、VMA 与 Staging Upload

对应源码入口：[samples/10_gpu_buffers/main.cpp](../../samples/10_gpu_buffers/main.cpp)。

B9 把三个顶点的位置和颜色直接写在 Vertex Shader 中。B10 要把它们改成真实的 Mesh 数据链：

```text
CPU 中的 Vertex/Index 数组
→ CPU 可写的 Staging Buffer
→ vkCmdCopyBuffer
→ GPU 读取效率更合适的 Vertex/Index Buffer
→ Bind Buffer
→ vkCmdDrawIndexed
```

本章最终画出一个由两个三角形组成的彩色矩形。四个顶点被六个索引复用，从而同时验证 Vertex Buffer 和 Index Buffer。

---

## 1. B10 在 B9 的哪个位置

B9 的 Vertex Shader 保存：

```glsl
const vec2 positions[3] = ...;
const vec3 colors[3] = ...;
```

Draw Call 只需要产生三个 `gl_VertexIndex`，Shader 再用索引读取自己的数组。

这种写法适合验证 Pipeline，但无法承载真实模型。真实场景需要：

- 在 CPU 端加载或生成任意数量的顶点；
- 把数据上传到 GPU 可访问的内存；
- 让多个 Draw 使用不同 Mesh；
- 使用索引复用重复顶点；
- 明确资源所有权、同步和销毁时间。

B10 保留 B9 的 Dynamic Rendering、Graphics Pipeline 和帧同步，只替换顶点来源与 Draw 命令：

```text
B9：Shader内部数组 → vkCmdDraw

B10：Vertex/Index Buffer → vkCmdDrawIndexed
```

---

## 2. 先建立完整对象关系

### 2.1 CPU Mesh 数据

应用先在普通 CPU 内存中准备：

```cpp
struct Vertex {
    float position[2];
    float color[3];
};
```

本课包含四个 Vertex：

```text
每个Vertex：2个float位置 + 3个float颜色
一个Vertex：5 × 4 = 20字节
四个Vertex：4 × 20 = 80字节
```

索引使用六个 `uint16_t`：

```text
6 × 2 = 12字节
```

### 2.2 VkBuffer 与内存

`VkBuffer` 是 Vulkan Buffer 资源 Handle。它描述大小、用途和访问方式，但 Buffer Handle 本身不等于一块已经可用的显存。

原生 Vulkan 通常需要：

```text
创建VkBuffer
→ 查询内存需求
→ 选择Memory Type
→ 分配VkDeviceMemory
→ 把内存绑定给Buffer
```

Buffer 依赖底层内存才能保存实际字节。

### 2.3 VMA

VMA（Vulkan Memory Allocator，Vulkan 内存分配器）是一个帮助管理 Vulkan Buffer/Image 内存的库。

本项目创建一个 `VmaAllocator`，它引用：

```text
VkInstance
VkPhysicalDevice
VkDevice
```

调用：

```cpp
vmaCreateBuffer(...)
```

会一起得到：

```text
VkBuffer
+
VmaAllocation
```

其中：

```text
VkBuffer：交给Vulkan命令绑定、复制和读取
VmaAllocation：VMA管理的底层内存区域和分配记录
```

销毁时必须把二者一起交回：

```cpp
vmaDestroyBuffer(allocator, buffer, allocation);
```

VMA 负责选择和管理内存，不会自动把 CPU 数据上传到 Device Buffer；Copy 命令和同步仍由应用负责。

### 2.4 Staging 与 Device Buffer

B10 创建三类 Buffer：

```text
一个临时Staging Buffer
→ CPU映射并写入顶点和索引
→ 用作TRANSFER_SRC

一个长期Vertex Buffer
→ 用作TRANSFER_DST和VERTEX_BUFFER
→ Draw时读取顶点属性

一个长期Index Buffer
→ 用作TRANSFER_DST和INDEX_BUFFER
→ DrawIndexed时读取顶点编号
```

Staging Buffer 上传完成后销毁；Vertex/Index Buffer 一直存活到 Renderer 销毁。

---

## 3. 为什么不总是让 CPU 直接写 Vertex Buffer

GPU 内存类型具有不同属性。这里最关键的两个概念是：

```text
HOST_VISIBLE
→ CPU可以Map并写入

DEVICE_LOCAL
→ 对当前GPU设备访问更合适，独立显卡上通常位于显存
```

离散显卡上，最适合 GPU 高频读取的 Device-local 内存往往不能直接让 CPU Map；CPU 可写内存又可能要求 GPU 跨 PCIe 读取。

因此静态 Mesh 常用：

```text
CPU写Host-visible Staging
→ GPU执行Copy
→ 数据长期保存在Device-local Buffer
```

集成显卡或 ReBAR 平台可能提供同时 `HOST_VISIBLE` 和 `DEVICE_LOCAL` 的内存。Vulkan 不保证所有硬件具有同一内存结构，所以代码应根据属性和用途选择，而不是把“系统内存/显存”硬编码成绝对结论。

本课主动采用 Staging，是为了学习通用的静态资源上传链路。

---

## 4. 当前 VMA 分配方式

### 4.1 Staging Buffer

创建 Staging Buffer 时同时填写了两类不同的配置：一类交给 VMA 决定“底层内存怎样分配和映射”，另一类交给 Vulkan 说明“这个 Buffer 以后允许参加什么 GPU 操作”。它们不能混成一个结论。

第一项是 VMA 的自动内存选择策略：

```cpp
allocation.usage = VMA_MEMORY_USAGE_AUTO;
```

读取者是 VMA。它表示让 VMA 根据 Buffer Usage、下面的 Host Access Flags 和硬件提供的 Memory Type 自动选择内存，不等于单独保证 CPU 可以访问。

第二项仍然由 VMA 读取：

```cpp
VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
```

它告诉 VMA：“CPU 会按顺序向这块 Allocation 写数据。”为了满足这种 Host Access 请求，VMA 必须选择带 `HOST_VISIBLE` 属性的内存类型。`HOST_VISIBLE` 才是这块内存能够映射进 CPU 地址空间的基础条件。

第三项也由 VMA 读取：

```cpp
VMA_ALLOCATION_CREATE_MAPPED_BIT
```

它要求 VMA 在 Allocation 创建成功后保持映射，并在 `VmaAllocationInfo::pMappedData` 中返回 CPU 可使用的地址。代码随后仍检查 `pMappedData != nullptr`；请求 Mapped 与取得可用指针是前因和结果，不能只凭 Flag 名字跳过结果检查。

第四项不是 VMA Allocation Flag，而是 Vulkan 的 Buffer Usage：

```cpp
VK_BUFFER_USAGE_TRANSFER_SRC_BIT
```

它由 Vulkan 实现读取，表示该 `VkBuffer` 允许作为 `vkCmdCopyBuffer` 的复制源。它不负责让内存变成 `HOST_VISIBLE`，也不负责产生 CPU 指针。

所以完整因果链是：

```text
HOST_ACCESS_SEQUENTIAL_WRITE
→ VMA选择HOST_VISIBLE内存

MAPPED
→ VMA创建时建立映射
→ pMappedData返回CPU地址

TRANSFER_SRC
→ Vulkan允许GPU Copy把该Buffer作为源
```

三者组合后，Staging Buffer 才同时具备：

```text
CPU可以通过pMappedData写入
+
GPU Copy可以从这个VkBuffer读取
```

因此代码先验证映射指针，再写入：

```cpp
if (stagingInfo.pMappedData == nullptr) {
    throw std::runtime_error("VMA returned an unmapped staging allocation");
}

std::byte* mappedBytes =
    static_cast<std::byte*>(stagingInfo.pMappedData);
memcpy(mappedBytes, vertices.data(), 80);
memcpy(mappedBytes + 80, indices.data(), 12);
```

这里的 `memcpy` 只完成 CPU 到映射内存的写入；真正从 Staging 复制到 Device Buffer，要等后面录制并提交 `vkCmdCopyBuffer`。

### 4.2 Device Buffer

```cpp
allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
```

表示优先选择设备访问合适的内存类型。

Vertex Buffer Usage：

```cpp
VK_BUFFER_USAGE_TRANSFER_DST_BIT |
VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
```

Index Buffer Usage：

```cpp
VK_BUFFER_USAGE_TRANSFER_DST_BIT |
VK_BUFFER_USAGE_INDEX_BUFFER_BIT
```

同一个 Buffer 可以声明多个用途；创建时必须把未来会用到的用途全部写入。

本项目没有沿用旧式 `VMA_MEMORY_USAGE_GPU_ONLY/CPU_ONLY`，而是使用当前 VMA 的 `AUTO`、偏好和 Host Access Flags 表达需求。

---

## 5. Staging Buffer 中怎样排列数据

Vertex 数据占 80 字节，Index 数据占 12 字节。本课把两者放入一个临时 Staging Buffer：

```text
Offset 0  ─────────────── Offset 79
4个Vertex，共80字节

Offset 80 ────────────── Offset 91
6个uint16_t Index，共12字节
```

Index 起始位置通过：

```cpp
indexOffset = align_up(vertexBytes, 4);
```

对齐到四字节边界。本例 `vertexBytes=80` 已经对齐，所以 `indexOffset=80`。

总 Staging 大小：

```text
80 + 12 = 92字节
```

CPU 使用 `memcpy` 写入映射地址：

```cpp
memcpy(mapped + 0, vertices, 80);
memcpy(mapped + 80, indices, 12);
```

随后调用：

```cpp
vmaFlushAllocation(...);
```

如果内存不是 Host-coherent，它负责把 CPU 写入刷新到设备可见范围；如果本机分配恰好是 Host-coherent，这个调用也保持代码在不同内存类型上的正确性。

---

## 6. Upload Command 怎样复制数据

初始化阶段创建一个临时 Command Pool、Command Buffer 和 Fence。

Command Buffer 录制两次 Copy：

```text
Staging[0, 80)
→ Vertex Buffer[0, 80)

Staging[80, 92)
→ Index Buffer[0, 12)
```

对应命令：

```cpp
vkCmdCopyBuffer(staging, vertexBuffer, ...);
vkCmdCopyBuffer(staging, indexBuffer, ...);
```

这些调用仍然只是录制。之后通过：

```cpp
vkQueueSubmit2(...);
```

才交给 GPU 执行。

---

## 7. Upload Barrier 与 Fence 分别解决什么

Copy 后录制一个 Memory Barrier：

```text
srcStage  = TRANSFER
srcAccess = TRANSFER_WRITE

dstStage  = VERTEX_INPUT
dstAccess = VERTEX_ATTRIBUTE_READ | INDEX_READ
```

它建立 GPU 侧关系：

```text
Copy对Buffer的写入完成并可见
→ 后续Vertex Input才能读取同一批字节
```

Upload Fence 建立 CPU 与 GPU 的关系：

```text
Upload Submit完成
→ Fence Signaled
→ CPU Wait返回
→ CPU可以销毁Staging Buffer和临时Command Pool
```

因此二者不能互相替代：

```text
Barrier：保证GPU写入和GPU读取的依赖及可见性
Fence：让CPU知道何时可以回收临时上传资源
```

本课上传阶段会等待 Fence，所以属于同步初始化上传。正式引擎的异步上传会在后续 C7 使用长期 Staging Ring、上传预算和延迟回收，不会为每个资源立即阻塞 CPU。

---

## 8. Vertex Buffer 的字节怎样对应 Shader 输入

CPU Vertex：

```cpp
struct Vertex {
    float position[2];
    float color[3];
};
```

内存排列：

```text
Offset 0：position.x，4字节
Offset 4：position.y，4字节
Offset 8：color.r，4字节
Offset 12：color.g，4字节
Offset 16：color.b，4字节
Stride：20字节
```

Pipeline 的 Binding Description 说明：

```text
Binding 0
每隔20字节是下一个Vertex
每个顶点前进一次，而不是每个实例前进一次
```

Attribute Description 建立：

```text
Location 0
→ Binding 0的Offset 0
→ 两个32位float
→ Shader的inPosition

Location 1
→ Binding 0的Offset 8
→ 三个32位float
→ Shader的inColor
```

Vertex Shader：

```glsl
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;
```

因此关系不是 Shader 自动看懂 C++ 结构体，而是应用用 Binding、Stride、Format、Offset 和 Location 明确描述同一块字节。

---

## 9. Index Buffer 为什么存在

一个矩形由两个三角形组成：

```text
Triangle 0：0, 1, 2
Triangle 1：2, 3, 0
```

如果没有索引，需要重复保存：

```text
0, 1, 2, 2, 3, 0
→ 6份Vertex数据
```

使用 Index Buffer 后：

```text
Vertex Buffer只保存4个唯一Vertex
Index Buffer保存6个顶点编号
```

索引内容：

```cpp
{ 0, 1, 2, 2, 3, 0 }
```

其中顶点 0 和 2 被两个三角形复用。

本课使用 `uint16_t`，绑定时必须声明：

```cpp
VK_INDEX_TYPE_UINT16
```

否则 GPU 会按错误的字节宽度读取索引。

---

## 10. 每帧 Draw 与 B9 的变化

B9：

```cpp
vkCmdDraw(commandBuffer, 3, 1, 0, 0);
```

B10：

```cpp
vkCmdBindVertexBuffers(...);
vkCmdBindIndexBuffer(...);
vkCmdDrawIndexed(commandBuffer, 6, 1, 0, 0, 0);
```

三条命令分别说明：

```text
BindVertexBuffers
→ Vertex Input从哪个Buffer、哪个Offset读取属性

BindIndexBuffer
→ 从哪个Buffer读取索引，以及每个索引是什么类型

DrawIndexed
→ 本次读取6个索引，绘制1个实例
```

`vkCmdDrawIndexed` 的参数：

```text
indexCount    = 6
instanceCount = 1
firstIndex    = 0
vertexOffset  = 0
firstInstance = 0
```

GPU 对每个索引执行：

```text
从Index Buffer取顶点编号
→ 加上vertexOffset
→ 按Vertex Binding的Stride找到Vertex字节
→ 按Attribute的Offset和Format拆出position/color
→ 作为Vertex Shader输入
```

`vkCmdBind*` 和 `vkCmdDrawIndexed` 都只是录进 Command Buffer，真正读取 Buffer 发生在 Queue Submit 后。

---

## 11. B10 的完整数据流

### 初始化上传

```text
CPU创建4个Vertex和6个Index
→ VMA创建92字节Host-visible Staging Buffer
→ CPU memcpy写入映射地址
→ VMA创建80字节Vertex Buffer和12字节Index Buffer
→ vkCmdCopyBuffer录制两次Copy
→ Barrier建立Transfer Write到Vertex/Index Read依赖
→ Queue Submit
→ GPU完成Copy
→ Upload Fence通知CPU
→ 销毁Staging、临时Command Pool和Fence
```

### 每帧绘制

```text
B8同步外壳取得Swapchain Image
→ B9 Dynamic Rendering选择Color Attachment
→ Bind Graphics Pipeline
→ Bind Vertex Buffer
→ Bind Index Buffer
→ DrawIndexed读取6个Index
→ Index选择4个Vertex中的数据
→ Vertex Shader处理位置和颜色
→ 两个Triangle被光栅化
→ Fragment Shader输出颜色
→ 写入Swapchain Image
→ Present
```

---

## 12. 怎样理解本机 VMA 统计

本机冒烟测试得到：

```text
Staging allocation: 92 bytes, HOST_VISIBLE|HOST_COHERENT
Vertex Buffer: 80 bytes, DEVICE_LOCAL
Index Buffer: 12 bytes, DEVICE_LOCAL
VMA live allocations=2
requested bytes=96
backing block bytes=67108864
```

`live allocations=2` 表示 Staging 已经销毁，只剩 Vertex 与 Index 两个长期分配。

`requested bytes=96` 可能大于 `80+12=92`，因为实际分配区域可能按内存要求进行对齐。

`backing block bytes=64 MiB` 不表示这个矩形实际消耗了 64 MiB。VMA 通常向 Vulkan 申请较大的 `VkDeviceMemory` Block，再从中为多个 Buffer/Image 子分配小区域：

```text
Vulkan Block：较大的底层内存块
VMA Allocation：Block中的一个子区域
```

后续资源可以继续复用这个 Block，避免每个小 Buffer 都单独创建一个 `VkDeviceMemory`。

---

## 13. 所有权和销毁顺序

初始化后的所有权：

```text
BufferMeshProbe拥有VmaAllocator
VmaAllocator管理Vertex/Index Allocation
VkBuffer由对应VmaAllocation支撑
```

退出时：

```text
等待Device空闲
→ 销毁Graphics Pipeline
→ 销毁帧同步和Command Pool
→ vmaDestroyBuffer(Index)
→ vmaDestroyBuffer(Vertex)
→ vmaDestroyAllocator
→ 外层DeviceQueueProbe最后销毁VkDevice
```

Allocator 必须晚于它管理的所有 Allocation 销毁，又必须早于 VkDevice 销毁。

---

## 14. 构建和运行

```powershell
cd D:\games\Emberframe-Engine

.\scripts\build-windows.ps1 -Config Release -Target emberframe_10_gpu_buffers

.\bin\Release\emberframe_10_gpu_buffers.exe
```

12 帧冒烟测试：

```powershell
.\bin\Release\emberframe_10_gpu_buffers.exe --smoke-test
```

预期看到彩色矩形，并在日志看到：

```text
Staging allocation
Uploaded 80 vertex bytes and 12 index bytes
Vertex Buffer: 80 bytes, DEVICE_LOCAL
Index Buffer: 12 bytes, DEVICE_LOCAL
DrawIndexed(6)
```

当前环境仍报告 `VK_LAYER_KHRONOS_validation: unavailable`。程序运行成功不等于已经经过 Validation Layer 检查，最终验收前需要恢复 Layer。

---

## 15. 源码阅读顺序

1. [main.cpp：总体对象创建和帧循环](../../samples/10_gpu_buffers/main.cpp)
2. [Vertex 与 AllocatedBuffer 定义](../../samples/10_gpu_buffers/buffer_mesh_probe.h)
3. [create_allocator()：VMA 与 Vulkan 对象关系](../../samples/10_gpu_buffers/buffer_mesh_probe.cpp)
4. [create_mesh_buffers()：CPU Vertex/Index 数据](../../samples/10_gpu_buffers/buffer_mesh_probe.cpp)
5. [upload_mesh_with_staging()：完整上传链](../../samples/10_gpu_buffers/buffer_mesh_probe.cpp)
6. [create_graphics_pipeline()：Vertex 字节布局](../../samples/10_gpu_buffers/buffer_mesh_probe.cpp)
7. [Vertex Shader 输入](../../samples/10_gpu_buffers/shaders/b10_mesh.vert)
8. [record_draw_commands()：Bind 与 DrawIndexed](../../samples/10_gpu_buffers/buffer_mesh_probe.cpp)
9. [cleanup()：资源反向销毁](../../samples/10_gpu_buffers/buffer_mesh_probe.cpp)

阅读时始终追踪：

```text
数据当前位于CPU数组、Staging还是Device Buffer？
当前Buffer声明了什么Usage？
谁会读写它？
读写之间使用什么同步？
谁拥有并销毁Allocation？
```

---

## 16. 三个学习实验

### 实验 1：修改一个 Vertex 位置

把右上角的 x 从 `0.68F` 改成 `0.30F`。运行前预测矩形会变成什么形状，再构建验证。

### 实验 2：修改 Index 顺序

把：

```cpp
{ 0, 1, 2, 2, 3, 0 }
```

临时改成：

```cpp
{ 0, 1, 2 }
```

同时把数组长度和 Index Count 改成 3，预测只剩哪个三角形。验证后恢复。

### 实验 3：制造 Vertex Layout 错误

把颜色 Attribute 的 Format 临时改成：

```cpp
VK_FORMAT_R32G32_SFLOAT
```

预测蓝色分量如何读取错误，并观察画面。这个实验最好在 Validation Layer 恢复后进行。

---

## 17. 常见错误

### Buffer Usage 缺少 TRANSFER_DST

目标 Buffer 不能合法接收 `vkCmdCopyBuffer`。

### Staging 在 Copy 完成前销毁

GPU 可能仍在读取已被回收的 Buffer。必须等待 Upload Fence 或使用延迟销毁机制。

### Vertex Stride/Offset/Format 与 C++ 结构不一致

GPU 会按错误字节位置解释属性，出现形状或颜色异常。

### Index Type 与实际数据不一致

用 `uint16_t` 存储却绑定为 `UINT32`，GPU 会按四字节组合索引并越界读取。

### 缺少 Copy 到 Vertex Input 的同步

Transfer 写入对后续 Vertex/Index Read 的可见性没有被正确表达。

### 先销毁 Allocator

Allocator 管理的 Buffer/Allocation 仍然存活，会破坏资源生命周期。

---

## 18. 面试时怎样回答

### 为什么使用 Staging Buffer？

> 静态 Mesh 希望长期放在设备访问更合适的 Device-local 内存，但这种内存不一定能被 CPU 直接映射。我先创建 Host-visible Staging Buffer，将 CPU 顶点和索引 memcpy 进去，再用 vkCmdCopyBuffer 上传到带 TRANSFER_DST 和 VERTEX/INDEX 用途的 Device Buffer。Copy 后用 Barrier 建立 Transfer Write 到 Vertex/Index Read 的依赖，并用 Fence 等待上传完成后回收 Staging。

### VMA 做了什么？

> VMA 根据资源用途、Host Access 要求和硬件 Memory Type 管理 VkDeviceMemory Block 与子分配，并把 VkBuffer 和 VmaAllocation 一起返回。它减少手写查询、分配和绑定内存的代码，但不会替应用完成数据拷贝、Barrier、Queue Submit 或资源生命周期同步。

### Vertex Buffer 与 Index Buffer 怎样配合？

> Vertex Buffer 保存唯一顶点的属性字节，Pipeline 的 Binding/Attribute Description 用 Stride、Offset、Format 和 Location 解释这些字节；Index Buffer 保存顶点编号。vkCmdDrawIndexed 先读取索引，再定位对应 Vertex，因此相邻三角形可以复用共享顶点，减少重复属性数据。

---

## 19. B10 完成验收

- [ ] 能说明 VkBuffer 与底层内存不是同一个概念；
- [ ] 能说明 VmaAllocator、VkBuffer、VmaAllocation 的关系；
- [ ] 能解释 Host-visible、Device-local 与不同硬件内存结构；
- [ ] 能复述 CPU → Staging → Copy → Device Buffer；
- [ ] 能区分 Upload Barrier 与 Upload Fence；
- [ ] 能根据 Vertex 结构计算 Stride 和 Attribute Offset；
- [ ] 能解释 Index 怎样复用 Vertex；
- [ ] 能解释 BindVertex/BindIndex/DrawIndexed 的先后关系；
- [ ] 能独立构建并看到彩色矩形；
- [ ] 能修改一个 Vertex 或 Index 并预测结果；
- [ ] 能解释正确的 Buffer、Allocation、Allocator 销毁顺序。

完成 B10 后，B11 会继续处理 GPU Image、纹理上传、ImageView、Sampler 和 Descriptor，使 Fragment Shader 能从纹理中采样颜色。
