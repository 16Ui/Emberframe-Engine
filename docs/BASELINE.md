# 基线验证记录

验证日期：2026-08-24

## 环境

- Windows x64
- Visual Studio 2022 / MSVC 19.44
- CMake 4.2.0-rc1
- Vulkan SDK 1.4.357.0（copy-only 安装）
- Configuration: Release
- Targets: `Shaders`, `chapter_6`

## 结果

- CMake configure：通过；
- GLSL → SPIR-V：通过；
- C++ 编译和链接：通过；
- 输出：`bin/Release/chapter_6.exe`；
- 启动冒烟测试：持续运行 8 秒，无标准错误输出，随后由测试脚本主动结束。

## 已知基线问题

- 上游 CMake 最低版本声明较旧，配置阶段会产生兼容性警告；
- MSVC 中文代码页会对部分第三方源文件产生 C4819 警告；
- 上游存在若干浮点数或 `size_t` 到 `uint32_t` 的窄化转换警告；
- 当前测试证明程序可启动并进入循环，不等价于完整画面回归或长时间稳定性测试。

这些警告先记录为上游基线，不混入首个导入提交。后续只在模块重构触及对应代码时修复，并补充针对性验证。
