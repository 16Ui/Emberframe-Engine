# 上游与个人贡献边界

## 代码基线

- Upstream: https://github.com/vblanco20-1/vulkan-guide
- Branch: `all-chapters-2`
- Commit: `dcf72a8b3cf93e27b917639a012be1b4b24b5e7d`
- Commit date: 2024-06-09
- License: MIT，保留仓库中的 `LICENSE.txt`
- Imported paths: 该提交的完整源码、着色器、测试资源与第三方依赖
- Baseline branch: `upstream-baseline`
- Personal development branch: `main`

Vulkan 初始化、Swapchain、基础 GPU 资源、glTF 加载、Immediate Submit、ImGui 集成以及 `chapter-*` 示例均属于上游能力，不能作为个人实现描述。

## 架构参考

- Reference: https://github.com/BoomingTech/Piccolo
- Usage: 阅读 Runtime、Render、Resource、Scene、Editor 的职责边界与生命周期组织
- Copied implementation: 无

Piccolo 当前仅作为设计参考，不是代码依赖。未来如果复制或改写具体实现，必须在本文件中追加源文件、原始 commit、改动范围和许可证说明。

## 本项目新增内容

第一阶段新增内容仅包括项目规划、上游追踪、可复现构建脚本和基线验证记录。后续个人模块必须以独立提交进入 `main`，并配套：

1. 设计文档与关键取舍；
2. 可独立运行的 Sample；
3. 错误路径或自动测试；
4. 核心模块的性能或正确性证据。

当前 `main` 已开始 M1 架构拆分：`engine/platform`、`engine/runtime`、`chapter-6/main.cpp` 和 `VulkanEngine` 生命周期接口属于个人改造。`chapter-6` 中其余渲染、资源、材质、场景和加载逻辑仍属于 Vulkan Guide 上游能力。

比较个人改动时，可以使用：

```powershell
git diff upstream-baseline..main
```
