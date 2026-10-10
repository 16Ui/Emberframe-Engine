# EmberFrame 演示包第三方许可清单

本清单对应 `scripts/package-workbench.ps1` 的 Windows x64 Workbench 包，不是整个仓库所有资源的再分发许可。静态链接、头文件实现、SDL 内嵌组件及字体也需要保留通知，不能只登记 DLL。

| 组件 | 仓库中的来源 | 许可及包内通知 |
|---|---|---|
| Vulkan Guide 上游与仓库许可文件 | 原始章节、保留的 `LICENSE.txt` | 根 `LICENSE.txt` 原样复制；不把其中网站模板版权声明当作新引擎或全部资产的权属证明；新原创部分的许可由项目维护者确定 |
| SDL 2.28.4 | `third_party/SDL` | zlib；`licenses/SDL2.txt` |
| SDL 内置 yuv2rgb | `third_party/SDL/src/video/yuv2rgb` | BSD-3-Clause；`licenses/SDL-yuv2rgb.txt` |
| SDL 内置 HIDAPI | `third_party/SDL/src/hidapi` | 选择 BSD-3-Clause；`licenses/SDL-hidapi-BSD.txt`，不选择 GPL 分发条件 |
| Dear ImGui 1.90.6 WIP | `third_party/imgui/imgui.cpp` | MIT；`licenses/WORKBENCH-NOTICES.txt` 保留 Omar Cornut 通知 |
| ImGui 内嵌 stb_truetype、stb_rectpack、stb_textedit | `third_party/imgui/imstb_*.h` | 各头文件的 MIT/Unlicense 双许可通知原样保留；各有 `licenses/stb_*-notices.txt` |
| GLM 0.9.9.7 | `third_party/glm/glm/detail/setup.hpp` | 选择 MIT；`licenses/WORKBENCH-NOTICES.txt`，通知依据 [该版本官方 copying.txt](https://raw.githubusercontent.com/g-truc/glm/0.9.9.7/copying.txt) |
| Vulkan Memory Allocator | `third_party/vma/vk_mem_alloc.h` | MIT，AMD 2017–2022；从当前 vendored 头文件提取到 `licenses/VMA-notices.txt` |
| vk-bootstrap | `third_party/vkbootstrap/VkBootstrap.h` | MIT，Charles Giessen 2020；`licenses/vk-bootstrap-notices.txt`。当前 Workbench 是否链接取决于构建；保留通知不代表个人实现 |
| stb_image | `third_party/stb_image/stb_image.h` | MIT/Unlicense 双许可通知；`licenses/stb_image-notices.txt` |
| fastgltf 0.6.1 | `third_party/fastgltf` | MIT，spnda 2022；`licenses/fastgltf.txt` |
| simdjson 3.3.0 及内嵌片段 | `third_party/fastgltf/deps/simdjson` | 主体 Apache-2.0；`licenses/Apache-2.0.txt`，保留 vendored `.h/.cpp` 中的 BSD/MIT/Boost 附加许可注释到 `simdjson-*-notices.txt`，另携带 `licenses/Boost-1.0.txt`；Grisu2 的 Florian Loitsch 2009 通知与 MIT 正文在 `WORKBENCH-NOTICES.txt` 中保留，不把内嵌代码全部重新标成 Apache |
| fmt | `third_party/fmt` | MIT 及其可执行嵌入例外；仍保留 `licenses/fmt.txt` |
| 可分发中文字体 | 默认 DroidSansFallback 来自 AOSP `android-4.4_r1`；完整 `data/fonts/NOTICE` 保留为 `assets/fonts/LICENSE.txt`，来源/哈希由 main 的 SOURCE 记录维护 | 规范化为 `assets/fonts/DroidSansFallback.ttf`，完整原始许可到 `licenses/UI-FONT-LICENSE.txt`；默认字体的 SOURCE 原样随包复制并记录哈希。外部替换字体须独立核对许可及 SOURCE，不得使用 Windows 微软雅黑文件替代 |
| 本项目图标 | `assets/branding` | 项目品牌图像；来源为内置图像生成服务，设计来源见仓库 `assets/branding/README.md`。不宣称图标来自上游 Vulkan Guide |
| 程序化默认场景与教学 Cube | 场景生成代码、`assets/software_renderer/textured_cube.obj` | 本项目默认内容；不包含上游 Lost Empire、第三方模型大库或商业资产 |
| 可选小型 GLB/HDR/贴图 | 显式 Asset Manifest | 按文件保留作者、原始来源、许可与哈希，许可复制为 `licenses/asset-N.txt`；清单存在不自动证明授权 |

## 不随包分发的系统组件

Windows DLL 与 Microsoft Visual C++ v14 运行库使用目标系统提供的组件。包不从 System32 复制运行库，也不重新分发 VC Redist 安装器；若缺失，由用户通过 [Microsoft 官方说明](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist) 安装。符合 Visual Studio 再分发资格才能采用其他 app-local 方案，本包没有采用。

`vulkan-1.dll`、GPU ICD 驱动、SDK、glslang、validation layer 不进入演示包。编译时使用 Vulkan API 头文件与链接库不等于把 SDK 分发给最终用户。相关系统 DLL 在 manifest 的 `systemProvidedImports` 中可追踪。

## 分发前许可核对

软件通知由打包脚本保留，而资源许可须由维护者逐项核实。尤其不能把下载网页写着“免费”理解为可以随 ZIP 再分发，也不能把根 `LICENSE.txt` 自动套用在外部纹理、HDR、模型或字体上。

默认不搬运 `assets/copyright.txt` 所描述的 Lost Empire 世界：它与本次实际 payload 不对应。若以后明确加入该资源，必须满足 CC BY 3.0 的署名条件并另加资产条目。无来源证明的资源不进入正式演示包。

仓库部分 vendor 缺少独立许可文件，因此脚本同时携带本清单、GLM/ImGui 的 MIT 通知和从当前头文件/实现中提取的完整许可注释。维护者更换 vendor 版本或启用新组件时必须更新此清单；提取脚本不是法律或依赖完整性的自动审计。
