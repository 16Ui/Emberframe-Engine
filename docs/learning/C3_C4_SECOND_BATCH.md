# C 第二批：从清屏到 Draw，建立资源回收边界

第一批只让 GPU 清屏：命令直接写 `VkImage`，没有 Shader、Pipeline 和 Draw。第二批要让同一个正式 Launcher 画出三角形。由此出现一个新问题：Pipeline 是可重建的 GPU 资源；重新加载 Shader 后，旧 Pipeline 可能仍被已提交的 Command Buffer 使用，不能立即销毁。因此 C4 的最小绘制和 C3 的代际 Handle、延迟回收在这里连成一条路径。

## 从已有对象接到画面

启动时，`VulkanSwapchain` 已提供多张 Image、相应的 ImageView 和颜色格式。`TrianglePipeline` 读取构建出的 SPIR-V，临时创建 Vertex/Fragment Shader Module；再创建 Pipeline Layout（声明 16 字节的材质 tint Push Constant）和与 Swapchain 颜色格式兼容的 Graphics Pipeline。Shader Module 在 Pipeline 创建后释放；Pipeline 与 Layout 持续保留，供每帧命令引用。当前还有一个进程内 `VkPipelineCache`，用于驱动在创建 Pipeline 时复用缓存信息，但没有磁盘持久化。

每帧仍由 `FrameContext` 负责 Acquire、Fence、Semaphore、Submit、Present。三角形路径把取得的 Image 转为 `COLOR_ATTACHMENT_OPTIMAL`；`VkRenderingAttachmentInfo` 引用这张 Image 对应的 ImageView，规定开始清屏、结束保存。`vkCmdBeginRendering` 后绑定 Pipeline，设置 Viewport/Scissor，把 tint 作为 Push Constant 提供给 Fragment Shader，录制 `vkCmdDraw(3, 1, 0, 0)`；结束 Rendering，再转为 `PRESENT_SRC_KHR`。这三个顶点目前由 Vertex Shader 根据 `gl_VertexIndex` 生成，不使用 Vertex Buffer。一次成功提交统计一个 Draw Call。

`TriangleMaterial` 现在只含颜色 tint，它证明“材质参数 → Pipeline Layout → Draw → Fragment Shader”的传递关系；它不是完整 glTF 材质系统。当前不含纹理 Descriptor、PBR 或 Material Instance。

## 为什么旧 Pipeline 要延迟销毁

`ResourceRegistry` 存放拥有 Vulkan Pipeline/Layout 的资源，Registry API 使用 `{registry_id, index, generation}` Handle。释放一个 Handle 会使其立即失效；同一索引以后即使被复用，代数也不同，旧 Handle 不能误取新资源。不同 Registry 的 Handle 也不能交叉使用。

热重载先尝试构造新 Pipeline：成功后才替换正在使用的 Handle；失败则保留旧 Pipeline 继续绘制。替换时，旧资源从 Registry 移出，但放进 `DeferredReleaseQueue`，记录它最后可能被哪个 GPU 提交使用。`FrameContext` 在等待 Slot Fence 后报告该提交完成；考虑到两个 Slot 可能反序完成，只把**连续完成的提交序号**作为安全回收前沿。前沿到达旧资源的退休序号时，才真正析构旧 Pipeline/Layout。手动按 R 可以读取新的 SPIR-V 并触发这个过程。

## 运行与验证

在项目根目录的 PowerShell 中：

```powershell
.\scripts\build-windows.ps1 -Config Release -Target emberframe_launcher
.\bin\Release\emberframe_launcher.exe --smoke-test
.\bin\Release\emberframe_launcher.exe --reload-smoke-test
.\bin\Release\emberframe_launcher.exe --resource-smoke-test
.\bin\Release\emberframe_launcher.exe --resize-smoke-test
.\bin\Release\emberframe_launcher.exe --clear --smoke-test
```

默认是三角形，`--clear` 保留第一批清屏路径。`--reload-smoke-test` 先故意从缺失目录重载，检查旧 Pipeline 未丢失，再成功重载，并在 GPU 完成后检查旧资源已回收。资源句柄测试：

```powershell
.\scripts\build-windows.ps1 -Config Release -Target emberframe_registry_test
.\bin\Release\emberframe_registry_test.exe
```

想在程序运行期间改 Shader：以 `--shader-dir .\build\engine\renderer\shaders` 启动程序；修改 `engine/renderer/shaders` 的源文件，另开终端构建 `emberframe_engine_shaders` 目标，回到窗口按 R。这样不会尝试重新链接正在运行的 exe。Shader 构建失败时不会触发新版本替换；若按 R 时 SPIR-V 缺失或 Pipeline 创建失败，旧 Pipeline 会继续使用。

本机 Release + Validation Layer、1280×720、Mailbox、热身 20 帧后测量 120 帧：清屏路径输出 `drawCalls=0`，三角形路径输出 `drawCalls=120`，两个路径均取得 120 个 GPU 时间戳样本。这只验证计时和 Draw 统计已接通；两个极轻的路径不足以得出真实渲染优化结论。

## 当前边界

C3 已有可测试的 Handle 和一次真实 Pipeline 的安全延迟回收。现在 `GpuResources` 也用 VMA 创建 Buffer/Image：`VkBuffer`/`VkImage` 是 GPU 命令引用的对象，`VmaAllocation` 是它们的内存分配，两者由同一个 RAII 资源一起持有；Image 另有 ImageView。Buffer Registry 与 Image Registry 各自拥有独立 ID，Handle 不能跨类型误用。资源退休后立即无法从 Registry 获取，但实际销毁要等 `FrameContext` 的连续完成序号到达退休序号。

可运行 `emberframe_launcher.exe --resource-smoke-test` 验证真实 Vulkan/VMA 创建、跨 Registry 误用、重复释放、索引复用后的旧 Handle 失效、跨帧回收与 VMA 活跃分配数归零。注意这个测试创建的 Buffer/Image **尚未参与 GPU Copy 或 Draw**，所以它证明资源管理路径接通，不证明顶点/纹理上传正确，也不证明正在被 GPU 读取的 Buffer/Image 的销毁时机。VMA 统计只覆盖本资源层管理的分配，不包含 Swapchain 或驱动内部内存；`allocatedBytes=0` 但 `reservedBlockBytes>0` 表示 VMA 保留了空闲的大块内存，并非资源泄漏。

C4 已有 Shader、Dynamic Rendering Pipeline、进程内 Cache、Push Constant 材质参数和手动热重载失败回退；仍缺少完整 Pipeline Key/磁盘缓存、Shader 反射、自动编译/文件监视、纹理 Descriptor 与通用 Material 系统。因此 C3、C4 在路线图中仍标记为进行中，而非全部完成。
