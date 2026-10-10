# EmberFrame Windows 演示包交付

演示包用于在没有源码、Visual Studio 和 Vulkan SDK 的 Windows 电脑上展示渲染工作台。首版以程序化材质、室内和多光源场景为主，不需要下载外部模型。包内提供 Release EXE、应用依赖、预编译 SPV、品牌图标、可分发中文字体和许可；目标电脑仍须具备支持本引擎要求的显卡驱动及 Microsoft Visual C++ 运行库。

打包脚本不运行程序或测试；CI 的 CPU 检查、真实 GPU 检查和最终画面验收是三份不同证据。ZIP 与 manifest 证明交付内容，不证明渲染效果或性能。

## 便携运行接口

以下约定由运行时与打包脚本共同遵守。打包脚本创建文件，运行时负责路径解析及无 SDK 情况下的行为。

- **包根目录**是 `emberframe_workbench.exe` 所在目录，不是当前工作目录或编译时源码目录。EXE 旁存在 `portable.ini` 即进入便携模式；该文件只是存在标记，不解析 INI 内容。`--portable` 提供同样的行为。
- **资产根目录**为包根目录下的 `assets/`，包括 `branding/`、`fonts/` 和以后可追加的 `showcase/`。运行时读取 `assets/fonts/DroidSansFallback.ttf`；打包时外部字体也规范化为这一文件名。实际字体来源和完整许可记录于包内清单。
- **Shader 根目录**为 `lab_shaders/`，只读取构建阶段生成的 SPV。正常演示不调用 glslang、不检查 SDK 路径，也不要求源码 Shader。Shader 源码编译/热重载属于开发功能，便携演示没有相应输入时应禁用并解释原因。
- **驱动缓存根目录**为 `SDL_GetPrefPath("16Ui","EmberFrame")/<ABI>/cache`，其中 `<ABI>` 是运行时选定的 ABI 隔离目录名；由运行时创建，不携带旧 ABI 缓存、shader-cache 或驱动 pipeline cache。
- **用户输出根目录**为 `SDL_GetPrefPath("16Ui","EmberFrame")/output`，承载日志、工程保存及日常导出。不要把现有源码下的 `output/` 搬入 ZIP。使用 SDL 返回值，不自行假设所有系统上的 AppData 路径都一样。
- **目录写权限**只要求 SDL 用户目录可写；便携包目录可以只读。缓存及日常输出不写入 EXE 目录。
- **启动默认值**为 `--portable --showcase materials --fit-viewport`。`--showcase interior` 和 `--showcase many-objects` 是另两个明确入口，对应 `showcase.h` 的稳定 ID。程序化场景不能因缺失外部 GLB/HDR 而无法启动。

这里“便携”指运行不依赖源码和开发 SDK，不代表用户数据必定写在 U 盘上，也不代表显卡驱动会随包安装。

## 包内结构

```text
EmberFrame-Windows-x64/
├── Start-EmberFrame.cmd
├── emberframe_workbench.exe
├── SDL2.dll                       按 EXE 的实际依赖选择
├── portable.ini                   仅存在标记
├── START_HERE.txt
├── requirements.json
├── manifest.json
├── LICENSE.txt                    原仓库许可文件，原样保留
├── THIRD_PARTY_LICENSES.md
├── DEMO_DELIVERY.md
├── lab_shaders/                   当前 Workbench 的预编译 SPV
├── assets/
│   ├── branding/                  PNG 和 ICO
│   ├── fonts/DroidSansFallback.ttf
│   ├── software_renderer/textured_cube.obj
│   └── showcase/                  可选的小型外部资产及其依赖
└── licenses/                      软件、字体、可选资产的完整许可与通知
```

必要的其他项目 DLL 只在 PE 普通/延迟导入表确实引用时复制。Windows 系统 DLL、Microsoft C++ 运行库、`vulkan-1.dll` 都记录为系统前置条件，不从 System32 或 SDK 复制。未知 DLL 直接中止，要求先明确来源与许可。

不包含测试 EXE、PDB、LIB、源码、SDK、validation layer、显卡驱动、下载脚本、商业模型、模型大库、旧配置、临时验证输出或现有缓存。原始上游 `assets/` 大目录也不整体复制。

## 打包已有 Release

命令在源码根目录运行，Windows PowerShell 5.1 或 PowerShell 7 均可。已构建的 EXE 应先由 main 完成便携路径、字体和 showcase 集成；不能把仍依赖 `EMBERFRAME_SOURCE_DIR` 的旧 EXE 当作新便携版。

```powershell
.\scripts\package-workbench.ps1 -SkipBuild
```

`-SkipBuild` 完全跳过配置与编译，无须打包电脑安装 Vulkan SDK，但仍需要 Git 记录仓库版本。默认读取 `bin/Release/`。不指定该开关则只调用既有构建脚本构建 `emberframe_workbench` 的 Release，不执行测试，也不启动窗口。

字体和许可默认分别为 `assets/fonts/DroidSansFallback.ttf` 与 `assets/fonts/LICENSE.txt`。项目已准备来自 AOSP `android-4.4_r1` 的字体和完整 `data/fonts/NOTICE`，main 维护 SOURCE 来源/哈希记录。脚本对默认字体自动发现同目录的 `SOURCE.json`、`SOURCE.md` 或 `SOURCE.txt`，原样带入 `assets/fonts/` 并加入文件哈希清单；若同时存在多个记录，须显式选择。字体或许可缺失时中止，不从 Windows 字体目录复制文件、不自动下载。也可显式传入已有且已核实可分发的字体：

```powershell
.\scripts\package-workbench.ps1 -SkipBuild `
  -FontPath 'D:\approved-assets\DroidSansFallback.ttf' `
  -FontLicensePath 'D:\approved-assets\AndroidOpenSourceProject_License.txt'
```

脚本只能确认许可文件存在且包含已知许可文本；**不会自动证明该许可确实属于这份字体或字形完整**。分发者仍须核实字体来源、版权通知和中文覆盖，不能把任意字体与一份通用许可证配对。

其他参数：

| 参数 | 含义 |
|---|---|
| `-BinaryDirectory` | 已构建 Release EXE、项目 DLL 和 `lab_shaders/` 所在目录 |
| `-OutputDirectory` | 新交付批次的父目录，默认 `output/delivery/` |
| `-VulkanSdk` | 仅在未使用 `-SkipBuild` 时传给现有构建脚本 |
| `-FontSourcePath` | 显式字体来源记录，JSON/Markdown/文本；外部字体不会自动配上仓库内的 SOURCE |
| `-AssetManifestPath` | 显式批准的小外部资产清单，省略则不携带外部模型/HDR |
| `-BuildMetadataPath` | 与 EXE 哈希及当前 Git revision 一致的构建记录 |

每次输出新的 `EmberFrame-Windows-x64-<revision>-<UTC时间>-<随机后缀>/`。其中有可检查的包目录、ZIP、ZIP 的 `.sha256` 和 `delivery-summary.json`。不覆盖旧包，不删除失败批次；同一源码版本的两次 ZIP 因时间等元数据而不保证字节完全相同。

`manifest.json` 逐文件记录路径、长度和 SHA256，另记录 Git revision、tracked checkout 是否有改动、二进制哈希和系统导入。manifest 不自包含其自身哈希；邻接的 summary 记录 manifest 和 ZIP 哈希。没有匹配构建记录的 `-SkipBuild` 包标记为 `unknown-existing-binary`：仓库当前 revision 不能证明这个已有 EXE 确实由它构建。

打包前检查普通与延迟导入的 PE 边界/终止符，按字符串数组逐个收集 DLL，明确区分空集合、单个导入和多个导入。许可表采用具名记录，避免位置数组被 helper 解包；内嵌通知在创建交付输出前提取，并核对各 vendored 文件中的预期作者与许可片段。simdjson 的 Grisu2/Florian Loitsch MIT 说明也会保留，而不仅是主体 Apache、Boost 和 BSD 通知。这些静态防护不替代 main 的实际打包与运行验收。

## 追加小型 GLB 或 HDR

不要给脚本一个目录让它全量搬运。`-AssetManifestPath` 接受以下 JSON；`source` 与 `licenseFile` 相对清单文件，所有 payload 必须定位在包内 `assets/showcase/`，逐文件批准并提供完整许可。以下占位哈希必须替换为实际值。

```json
{
  "schemaVersion": 1,
  "files": [
    {
      "source": "approved/model.glb",
      "destination": "assets/showcase/model.glb",
      "sha256": "<64位SHA256>",
      "author": "原作者",
      "origin": "原资源链接",
      "license": "原资源明确的许可名称",
      "licenseFile": "approved/model-LICENSE.txt",
      "licenseSha256": "<许可文件的64位SHA256>"
    }
  ]
}
```

清单记录不替代许可审核。禁止放入商业、不可再分发、来源未知的资源；有署名要求的原始作者/链接必须保留。glTF 的外部 Buffer、纹理，OBJ 的 MTL 和纹理也须逐项列入，改成包内相对路径；GLB 也可能含外部 URI，不能只凭扩展名认为已自包含。main 在脱离源码的完整包中核验资源链接。首版无需这类资源即可演示。

## 目标电脑要求

最低功能条件来自设备与工作台代码，而不是给出未经测量的最低显卡型号：Windows 10/11 x64；系统 Vulkan loader 与 GPU 驱动支持 Vulkan 1.3、`VK_KHR_swapchain`、`dynamicRendering`、`synchronization2`；提供 graphics+compute 队列及可呈现 Surface；至少七个颜色 Attachment，并支持实际渲染/存储/过滤格式。具体不支持项由运行时明确报告。

Microsoft Visual C++ v14 x64 Redistributable 必须不早于用于编译该 EXE 的 MSVC toolset。包不自行重新分发 Microsoft 运行库，安装入口见 [Microsoft 官方运行库说明](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)。GPU loader/驱动由显卡厂商安装；SDK 并不提供目标显卡驱动，见 [LunarG Windows SDK 说明](https://vulkan.lunarg.com/doc/view/1.3.296.0/windows/getting_started.html)。

无 SDK、无 validation layer 是正常演示环境，不应因此拒绝启动；便携包不宣称 validation 已开启。GPU 帧率、显存最低容量、最高可用质量需在真实目标硬件上测量，不能由 CI CPU 通过推导。

## GitHub Windows CI

配置位于 `.github/workflows/windows-workbench.yml`，提供 push/PR 的 Windows Release 构建和 CPU 检查；手动 `workflow_dispatch` 的 `package` 默认为 false，可选生成 artifact。只有手动明确勾选且必需字体/许可已准备好时才打包；不创建 GitHub Release，没有写仓库权限。

构建环境固定 `windows-2022`、VS 2022 generator、x64、v143、Vulkan SDK `1.4.357.0`，与本机已验证的 Shader 编译器保持一致；保留中文路径、空格与特殊字符的编译回归，不因旧工具无法打开输入文件而跳过测试。下载与校验 URL 均使用完整 x64 安装包文件名 `vulkansdk-windows-X64-1.4.357.0.exe`，不依赖版本间行为不同的别名；核对 [LunarG 官方 SHA API](https://vulkan.lunarg.com/content/view/latest-sdk-version-api) 返回的版本、平台、文件名和 SHA256，无效响应或不匹配立即失败。SDK 仅属于临时 CI 构建环境，不进入演示包，也不改变引擎对运行时 Vulkan 1.3 的要求。SDK 请求记录及安装日志在安装前开始保存，编译器版本、配置与编译日志也随 CPU 证据上传，失败时仍可定位具体阶段。

第三方 Actions 固定到官方已核实提交：

- [actions/checkout v7.0.1](https://github.com/actions/checkout/commit/3d3c42e5aac5ba805825da76410c181273ba90b1)，Node 24。
- [actions/upload-artifact v7.0.2](https://github.com/actions/upload-artifact/commit/cf430e030ddbb5b0abf93d22962f4752f3646cd9)，Node 24。

CI 构建 workbench、lab CPU 检查、资源 Registry 检查及 CPU Profiler 检查。`ctest -R '^lab_'` 只选择已经注册的 CPU 组；shader-assets 组需要构建环境的 glslang 编译器，但不代表 GPU 测试。没有执行 `emberframe_gpu_tests`、GPU lighting 测试、隐藏窗口或画面比较。

Shader 编译使用每个子进程自己的隔离工作目录以及相对源码、Include、SPV 输出参数；Windows 的工作目录由宽字符 API 设置，不修改父进程的目录。这样避免系统代码页不同导致编译器无法打开中文绝对路径，回归目录还包含 Emoji，防止仅在中文系统上偶然通过。日志记录工作目录与实际参数，调用规则参与缓存身份；源码文件/Include 文件名本身仍需满足外部编译器的字符支持范围。

产物分别是 CPU 日志/JUnit/构建记录，以及可选演示 ZIP/哈希/summary。上传保留 14 天。运行时变化、新字体或新资产应先由 main 在真实机器验收；CI artifact 上传成功不能冒充这一步。

这里“可复现构建”指固定步骤、SDK 版本、架构和工具链系列并留下版本证据；托管 `windows-2022` 镜像的补丁版本仍会更新，MSVC/CMake 的实际版本由证据文件记录。不承诺相同 revision 一定产出相同二进制字节。

## 本轮实际验收与复现

新版已完成本机 Release 构建、CPU 检查、真实 Vulkan GPU 检查，以及三个代表场景、刚体运动历史和包围球缓存 A/B 对照。绑定 EXE 哈希的结果见 [统一验证记录](evidence/internship-demo.json)，对应截图见 `docs/media/`。帧分析记录测量的是已完成提交；CPU 几何准备是命令录制的一部分，不把局部优化倍数写成整帧 FPS 提升。

[便携运行记录](evidence/portable-runtime.json)验证了：包解压到带中文与空格的独立临时目录，从另一个工作目录启动；移除子进程的 SDK 路径，覆盖显式 Layer 搜索为空目录；三个场景均使用系统 Vulkan loader 和 GPU 驱动成功呈现，Khronos validation layer 不可用；核对全部 payload 哈希且包内文件列表保持不变。

这仍是同一台电脑：没有卸载已安装 SDK、隐藏原源码目录或执行只读 ACL 测试，也未完成异机验收。验证脚本不修改系统环境或注册表，不关闭用户窗口，不删除临时验收目录。Windows 托管 CI 的各次实际结果以 [GitHub Actions 记录](https://github.com/16Ui/Emberframe-Engine/actions/workflows/windows-workbench.yml)为准；本机通过不等于远端通过，远端 CPU 检查通过也不等于真实 GPU 或异机验收。

```powershell
# 使用新建的证据目录，保留失败结果；完整验证可能耗时数分钟。
.\scripts\verify-internship-demo.ps1 -SkipBuild
.\scripts\verify-portable-workbench.ps1 -Archive "<本轮完整 ZIP 路径>"
```

## 进一步交付验收清单

下面是目标电脑/后续版本的验收范围，不代表本轮已经全部覆盖：

1. 构建接入 `portable.ini`、`--portable`、字体及三个 showcase 的新版 Release。
2. 用 `-SkipBuild` 打包；核对字体许可、payload、manifest 与二进制对应记录。
3. 把 ZIP 解压到与源码无关的目录；从不同工作目录启动、在带空格/中文路径的目录启动，并覆盖只读包目录场景。确保 SDL 用户目录可写。
4. 在未安装 SDK/validation 的真实 GPU 环境启动三个场景，确认源目录不存在时仍可运行。
5. 查看中文、品牌图标、SPV、模型依赖、显示缩放、保存/重开；确认日常输出进入 SDL 用户目录下的 `output/`，驱动缓存进入同一用户目录下的 `<ABI>/cache/`，且不向包目录写入这两类数据。
6. 分别记录构建、CPU checks、GPU validation、画面观察、性能与未签名状态。不要用旧测量补齐新版本的空白。
