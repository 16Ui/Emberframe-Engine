# B5：Physical Device、Logical Device 与 Queue

> 对应目标：`emberframe_05_device_queue`  
> 前置课程：[B4 Vulkan Instance、Surface 与 Validation](04_VULKAN_INSTANCE_SURFACE_VALIDATION.md)

## 0. 先看完整因果链

B4 已经留下：

```text
VkInstance   → 可以发现 Vulkan Physical Device
VkSurfaceKHR → 表示当前 SDL 窗口的呈现目标
```

B5 使用这两个输入完成：

```text
VkInstance + VkSurfaceKHR
        ↓
枚举所有 VkPhysicalDevice
        ↓
逐个查询 Properties、Queue Family、Present、Extension、Feature
        ↓
不满足硬性条件 → 记录拒绝原因
满足硬性条件   → 计算偏好分数
        ↓
选择最高分 Physical Device
        ↓
为唯一 Queue Family 填写 VkDeviceQueueCreateInfo
        ↓
填写要启用的 Device Extension 和 Feature
        ↓
vkCreateDevice()
        ↓
VkDevice
        ↓
vkGetDeviceQueue()
        ↓
Graphics Queue + Present Queue
```

B5 结束时仍没有 Swapchain、Command Buffer 或绘制结果；它只建立后续 GPU 工作所需的设备与提交入口。

## 1. 构建和运行

如果终端提示符以 `PS` 开头，说明当前是 PowerShell。在项目根目录使用一整行命令构建并运行：

```powershell
.\scripts\build-windows.ps1 -Config Release -Target emberframe_05_device_queue -Run
```

如果提示符只是 `D:\games\Emberframe-Engine>`，说明当前是 CMD，需要显式调用 PowerShell：

```bat
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1 -Config Release -Target emberframe_05_device_queue -Run
```

已经构建完成后，也可以直接正常交互运行：

```powershell
.\bin\Release\emberframe_05_device_queue.exe
```

初始化完成后立即退出并验证清理顺序：

```powershell
.\bin\Release\emberframe_05_device_queue.exe --exit-after-init
```

如果直接运行时找不到 Validation Layer，使用构建脚本配置的 Vulkan SDK 环境，或者先确认 `VULKAN_SDK`、`PATH` 和 `VK_LAYER_PATH`。

当前开发机的实际输出表明：

```text
RTX 4060 Laptop GPU → 通过，score=1032
Radeon 780M         → 通过，score=516
最终选择 RTX 4060
Graphics Family = Present Family = 0
Graphics Queue Handle = Present Queue Handle
```

这不是硬编码结果；程序根据当前机器实时查询并计算。

## 2. 四个对象各自是什么

| 对象 | 谁提供或创建 | 准确含义 | 是否单独销毁 |
|---|---|---|---:|
| `VkPhysicalDevice` | 由 `VkInstance` 枚举 | 一张 Vulkan 可见 GPU 的能力入口 | 否 |
| Queue Family | 从 Physical Device 查询 | 一组拥有相同核心能力的 Queue | 否 |
| `VkDevice` | 应用基于选中的 Physical Device 创建 | 当前应用启用的 GPU 使用环境 | 是 |
| `VkQueue` | 创建 Device 时申请，之后取回 | 向驱动提交特定类别工作的入口 | 否 |

`VkPhysicalDevice`不是应用创建的 GPU，也不是完整驱动副本。它是一个 Handle，用来查询这张设备支持什么。

`VkDevice`是应用真正创建的 Logical Device。创建时必须明确：

```text
使用哪些 Queue Family
每个 Family 申请几条 Queue
启用哪些 Device Extension
启用哪些 Feature
```

## 3. 候选设备检查为什么分成硬性条件和评分

源码入口：`DeviceQueueProbe::inspect_candidate()`。

每张设备先经过硬性检查：

```text
Vulkan API >= 1.3
存在 Graphics Queue Family
存在能向当前 Surface Present 的 Queue Family
支持 VK_KHR_swapchain
支持 dynamicRendering
支持 synchronization2
```

缺少任意条件都会进入：

```cpp
candidate.rejection_reasons
```

只有没有拒绝原因的候选设备才计算分数：

```text
Discrete GPU   +1000
Integrated GPU  +500
Virtual GPU     +250
CPU Device      +100
最大二维纹理尺寸 / 1024
```

因此：

```text
硬性条件决定“能不能使用”
评分决定“合格设备中更偏好哪一个”
```

不要为了偏好独立显卡而直接拒绝集成显卡。笔记本、远程桌面和测试环境可能只暴露集成 GPU。

## 4. Queue Family 怎样查询

源码入口：`DeviceQueueProbe::inspect_queue_families()`。

先使用两次枚举取得：

```cpp
vkGetPhysicalDeviceQueueFamilyProperties(...);
```

每个 `VkQueueFamilyProperties`包含：

```text
queueCount → Family 中有多少条 Queue
queueFlags → Graphics、Compute、Transfer 等核心能力
```

Graphics 能力来自固定标志：

```cpp
(family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0
```

Present 不能只读 `queueFlags`，因为它依赖具体 Surface：

```cpp
vkGetPhysicalDeviceSurfaceSupportKHR(
    physical_device,
    family_index,
    surface,
    &present_supported);
```

完整问题是：

```text
这张 Physical Device
的这个 Queue Family
能否向这个 VkSurfaceKHR Present？
```

程序分别记录第一个 Graphics Family 和第一个 Present Family。两者可以相同，也可以不同。

## 5. Extension、Feature 和 Property

| 类别 | 作用 | B5 示例 |
|---|---|---|
| Device Extension | 增加一组设备级 API 或行为 | `VK_KHR_swapchain` |
| Feature | 可以查询并显式启用的具体能力 | `dynamicRendering`、`synchronization2` |
| Property/Limit | 设备的只读信息或数值限制 | `deviceType`、`maxImageDimension2D` |

### 5.1 检查 Device Extension

源码使用两次枚举：

```cpp
vkEnumerateDeviceExtensionProperties(...);
```

然后查找：

```cpp
VK_KHR_SWAPCHAIN_EXTENSION_NAME
```

B5 不创建 Swapchain，但 B6 必须使用它。因此设备选择阶段就要排除不支持 Swapchain 的设备。

### 5.2 查询 Feature

```cpp
VkPhysicalDeviceVulkan13Features features13 {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
};

VkPhysicalDeviceFeatures2 features2 {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
};

features2.pNext = &features13;
vkGetPhysicalDeviceFeatures2(physical_device, &features2);
```

这里是查询支持情况，不是启用：

```text
Physical Device 查询结果为 true
≠
当前 VkDevice 已启用该 Feature
```

真正启用发生在 `vkCreateDevice()`之前填写的 Feature 链中。

## 6. 创建 Logical Device

源码入口：`DeviceQueueProbe::create_logical_device()`。

### 6.1 去重 Queue Family

如果：

```text
Graphics Family = 0
Present Family  = 0
```

只需要一份 Queue 创建配置。

如果：

```text
Graphics Family = 0
Present Family  = 2
```

则需要为 Family 0 和 Family 2 各准备一份配置。

### 6.2 申请 Queue

```cpp
VkDeviceQueueCreateInfo queue_create_info {
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
};
queue_create_info.queueFamilyIndex = family_index;
queue_create_info.queueCount = 1;
queue_create_info.pQueuePriorities = &queue_priority;
```

`queue_priority`范围是 `0.0`到 `1.0`，表示同一个 Device 内 Queue 的调度优先级提示，不会提高显卡频率或性能等级。

### 6.3 启用 Feature 和 Extension

```cpp
VkPhysicalDeviceVulkan13Features enabled_features13 {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
};
enabled_features13.dynamicRendering = VK_TRUE;
enabled_features13.synchronization2 = VK_TRUE;
```

再把 Feature 链、Queue 配置和 Extension 名称放进：

```cpp
VkDeviceCreateInfo create_info {
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
};
```

最后执行：

```cpp
vkCreateDevice(
    physical_device_,
    &create_info,
    nullptr,
    &device_);
```

输入是选中的 Physical Device 和应用要求；输出是应用拥有的 `VkDevice`。

## 7. `vkGetDeviceQueue()`不是创建 Queue

Queue 已经通过 `VkDeviceQueueCreateInfo`在 `vkCreateDevice()`期间申请。

之后执行：

```cpp
vkGetDeviceQueue(
    device_,
    graphics_queue_family_,
    0,
    &graphics_queue_);

vkGetDeviceQueue(
    device_,
    present_queue_family_,
    0,
    &present_queue_);
```

函数只按照：

```text
Device + Queue Family Index + Queue Index
```

取回 Handle。它不认识“graphics_queue”或“present_queue”变量名。

如果两个 Family Index 都是 0，而且都取 Queue 0，那么两个变量得到同一个 Handle。

如果 Family Index 不同，就会得到不同 Queue。后续需要处理跨 Queue 同步和可能的图像所有权转移。

## 8. 后续两条工作路径

Graphics Queue 后续用于：

```text
CPU 录制 Command Buffer
→ vkQueueSubmit2()
→ Graphics Queue
→ GPU 执行 Family 支持的命令
```

Present Queue 后续用于：

```text
Swapchain Image 绘制完成
→ vkQueuePresentKHR()
→ Present Queue
→ 窗口系统显示该图像
```

`vkQueuePresentKHR()`不是录制进 Command Buffer 的绘制命令。Graphics Queue 负责执行渲染工作；Present Queue 把已经完成的 Swapchain Image 交给窗口系统。

具体的 Swapchain 在 B6，Command Buffer 在 B7，同步在 B8。

## 9. 生命周期与销毁顺序

`main()`中的声明顺序是：

```cpp
SdlWindow window(...);
VulkanBootstrapProbe vulkan(...);
DeviceQueueProbe device(...);
```

因此析构顺序是：

```text
DeviceQueueProbe
→ vkDestroyDevice()
→ Queue Handle 随 Device 失效
→ VulkanBootstrapProbe
→ Surface
→ Debug Messenger
→ Instance
→ SDL Window
```

没有：

```cpp
vkDestroyQueue(...);
vkDestroyPhysicalDevice(...);
```

因为 Queue 由 Device 管理，Physical Device 由 Instance 枚举而来。

B5 尚未提交 GPU 工作。后续存在在途 GPU 工作时，销毁前必须通过 Fence 或 `vkDeviceWaitIdle()`保证工作已经结束。

## 10. 源码阅读顺序

在 VS Code 的 Markdown 预览中直接点击；在 Markdown 源码编辑器中使用 `Ctrl+Click`：

1. [`main()`：串起 Window、B4 与 B5](../../samples/05_device_queue/main.cpp#L14)
2. [`DeviceQueueProbe`构造函数：确定初始化顺序](../../samples/05_device_queue/device_queue_probe.cpp#L110)
3. [`select_physical_device()`：枚举、筛选并选择 GPU](../../samples/05_device_queue/device_queue_probe.cpp#L328)
4. [`inspect_candidate()`：检查单个候选设备](../../samples/05_device_queue/device_queue_probe.cpp#L246)
5. [`inspect_queue_families()`：查找 Graphics/Present Family](../../samples/05_device_queue/device_queue_probe.cpp#L164)
6. [`supports_device_extension()`：检查 Device Extension](../../samples/05_device_queue/device_queue_probe.cpp#L215)
7. [`print_candidate_report()`：输出通过和拒绝原因](../../samples/05_device_queue/device_queue_probe.cpp#L295)
8. [`create_logical_device()`：申请 Queue 并创建 Device](../../samples/05_device_queue/device_queue_probe.cpp#L372)
9. [`cleanup()`：按所有权释放 Logical Device](../../samples/05_device_queue/device_queue_probe.cpp#L435)

这些链接使用仓库内相对路径，可以随项目一起移动；其中 `#L...` 是源码行号，后续大幅修改 `.cpp` 时需要同步刷新。

阅读时始终追踪下面四组值：

```text
候选 VkPhysicalDevice
Graphics/Present Family Index
选中的 VkDevice
Graphics/Present VkQueue
```

## 11. 两个修改实验

### 实验一：改变设备偏好

在 `score_device()`中交换 Discrete 与 Integrated 的基础分：

```text
Discrete   +500
Integrated +1000
```

运行前先预测最终选中设备。完成后恢复原值。

### 实验二：观察硬性拒绝

临时把必需 Extension 名称换成一个不存在的测试名称，使所有候选设备打印明确拒绝原因。不要把测试名称保留到正式代码中。

实验目的不是制造崩溃，而是验证：

```text
不可用能力
→ 候选检查阶段拒绝
→ 不进入 vkCreateDevice()
→ main() 接收可读错误
```

## 12. B5 验收

- [ ] 能区分 Physical Device、Logical Device、Queue Family 和 Queue；
- [ ] 能解释 Graphics 为什么读 `queueFlags`，Present 为什么结合 Surface 查询；
- [ ] 能区分 Device Extension、Feature 和 Property；
- [ ] 能复述候选设备的硬性拒绝与偏好评分流程；
- [ ] 能解释 Queue 在创建 Device 时申请、之后由 `vkGetDeviceQueue()`取回；
- [ ] 能解释 Graphics/Present Queue 相同和不同的两种情况；
- [ ] 能独立构建运行并完成一个修改实验；
- [ ] 能解释 Device、Queue、Physical Device 的销毁责任。

通过后，B6 将使用本章输出的：

```text
VkPhysicalDevice
VkDevice
Graphics/Present Queue Family
Graphics/Present Queue
VkSurfaceKHR
```

创建 Swapchain、Swapchain Image 和 Image View。
