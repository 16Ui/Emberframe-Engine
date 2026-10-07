# 免费动漫人物模型：官方来源与许可核实

核实日期：2026-10-07（Asia/Shanghai）。用途：为个人本地资源整理及后续自行试用提供下载依据。

本轮仅查阅官方网页、官方 GitHub 元数据，并通过匿名 HTTP HEAD / Range 请求读取文件头、JSON 许可信息和少量现成预览图；没有保存模型、下载整仓库/大包、执行模型或项目程序、运行任何测试、启动引擎、进行格式转换或修改引擎源码。唯一写入文件为本清单。

## 选用结果

选入 4 个不同的人物/样例：AvatarSample_B、VRM1_Constraint_Twist_Sample、AliciaSolid、Seed-san。均为带面部表情及人形骨架的动漫风格人物。Seed-san 的人物本体为黑发动漫少年，附带机械臂背包；如希望服装也完全没有机械配件，优先下载前三项。官方截图依据见各项来源链接。

此次没有把来源或模型授权不清楚的 GLB/glTF/OBJ 强行列入。以下均为 **VRM 原始资源，需要转换后用于通用 GLB/glTF 流程**；此处没有承诺当前引擎能够直接导入。普通 OBJ 也不适合保留这些资源的骨骼蒙皮和表情。

所有主下载链接均固定到已核实的官方仓库提交，直接返回模型二进制，无需登录或付费。四个文件合计 50,620,100 字节，约 48.28 MiB；本轮没有下载这些完整文件。

| 模型 | 原始格式 / 文件大小 | 作者或权利人 | 修改 | 原样再分发 | 修改后再分发 |
| --- | --- | --- | --- | --- | --- |
| AvatarSample_B | VRM 1.0；21,047,556 字节，20.07 MiB | VRoid Project / pixiv Inc. | 允许 | 允许，须遵守附加条款 | 允许，须遵守附加条款 |
| VRM1_Constraint_Twist_Sample | VRM 1.0；10,776,032 字节，10.28 MiB | pixiv Inc. | 允许 | 允许 | 允许 |
| AliciaSolid | VRM 0.0；7,878,712 字节，7.51 MiB | DWANGO Co., Ltd. | 个人用户允许 | 没有找到明确授权，不按允许处理 | 个人可分发自己创作的衍生作品，附带限制 |
| Seed-san | VRM 1.0；10,917,800 字节，10.41 MiB | VirtualCast, Inc. | 允许 | 允许，须署名 | 允许，须署名 |

上表的“允许”均有对应条件，不表示 CC0 或公有领域。模型许可依据为模型文件自身的 `meta` 和下列官方条款，**不使用 three-vrm、ChatVRM 或 UniVRM 的代码 MIT 许可证代替模型授权**。

## 1. AvatarSample_B（推荐：完整服装的官方动漫人物）

- **直接下载 URL**：[AvatarSample_B.vrm](https://raw.githubusercontent.com/pixiv/ChatVRM/b542aa00e19dccf9fc48ba340cf7eee011d2329a/public/AvatarSample_B.vrm)
- **归属**：文件 `authors=["VRoid Project"]`、`copyrightInformation="pixiv Inc."`。下载来自 pixiv 自己的 ChatVRM 官方项目仓库。
- **官方文件页**：[固定版本文件](https://github.com/pixiv/ChatVRM/blob/b542aa00e19dccf9fc48ba340cf7eee011d2329a/public/AvatarSample_B.vrm)。仓库已归档，但文件直链当前仍可访问。
- **官方人物发布页**：[VRoid Project 发布的 AvatarSample_B](https://hub.vroid.com/en/characters/7939147878897061040/models/2292219474373673889)。页面提供人物预览，并确认允许改动、再分发及个人/法人商用。
- **许可原文**：[VRM Public License 1.0（日文）](https://vrm.dev/licenses/1.0/)、[英文](https://vrm.dev/en/licenses/1.0/)，以及文件 `otherLicenseUrl` 指向的 [AvatarSample A〜Z 附加条款](https://vroid.pixiv.help/hc/ja/articles/4402394424089-AvatarSample-A-Z)（[英文](https://vroid.pixiv.help/hc/en-us/articles/4402394424089-VRoidPreset-A-Z)）。文件内旧链接尾部为 `AvatarSample-A-B-C`，当前会跳转到同一文章的 A〜Z 版本。
- **文件许可设置**：`avatarPermission=everyone`、`commercialUsage=corporation`、`creditNotation=unnecessary`、`allowRedistribution=true`、`modification=allowModificationRedistribution`、`allowAntisocialOrHateUsage=false`。
- **修改/再分发结论**：允许修改，允许免费再分发原始资源，允许分发自己制作的衍生人物；允许个人及法人商用，无强制署名。禁止把模型或含其数据的 VRM 重新标成 CC0；禁止将其中数据用于开发/充实现有角色创建服务；日文条款禁止无正当理由有偿再分发原始样例及其内部数据；禁止冒称 pixiv 支持/推荐、侵权、犯罪、歧视和反社会等列明用途。附加条款明确举例允许出售自己制作的衍生人物，不能据此转售未修改样例。
- **资源规格**：文件内 `VRMC_vrm.specVersion=1.0`，模型版本 1.1，生成器 `VRoid Studio-1.22.0`；glTF 2.0 二进制容器；3 个 mesh、3 个 skin，每个 skin 含 113 个 joint 引用；54 个 VRM humanoid 映射骨骼；18 个材质、33 张内嵌图像。含 morph 表情、MToon、SpringBone、纹理变换；没有内嵌 glTF 动画片段。JSON 中没有外置 buffer/image URI。
- **不确定/注意**：上面 Hub 发布页显示其下载版本为 VRM 0.0，而本清单直链实际是官方仓库中的 **VRM 1.0**，两者不应混称为同一个二进制版本。许可内容需同时满足文件设置与附加条款。材质、头发/服饰动态的转换效果未运行验证。

## 2. VRM1_Constraint_Twist_Sample（推荐：日常服装的动漫少女）

- **直接下载 URL**：[VRM1_Constraint_Twist_Sample.vrm](https://raw.githubusercontent.com/vrm-c/vrm-specification/94e82dd346fa6cf0337c4421728640e5252dd38e/samples/VRM1_Constraint_Twist_Sample/vrm/VRM1_Constraint_Twist_Sample.vrm)
- **归属**：pixiv Inc.；文件及官方 README 均标记 `(c) 2022 pixiv Inc.`。
- **官方模型说明**：[模型 README / 许可说明](https://github.com/vrm-c/vrm-specification/blob/94e82dd346fa6cf0337c4421728640e5252dd38e/samples/VRM1_Constraint_Twist_Sample/README.md)。[官方截图](https://raw.githubusercontent.com/vrm-c/vrm-specification/94e82dd346fa6cf0337c4421728640e5252dd38e/samples/VRM1_Constraint_Twist_Sample/screenshot/screenshot.jpg)展示棕色长发、白色 T 恤、黑色短裤的动漫少女。
- **许可原文**：[VRM Public License 1.0（日文）](https://vrm.dev/licenses/1.0/)、[英文](https://vrm.dev/en/licenses/1.0/)，结合该下载文件的 `VRMC_vrm.meta` 设置。
- **文件许可设置**：`avatarPermission=everyone`、`commercialUsage=corporation`、`creditNotation=unnecessary`、`allowRedistribution=true`、`modification=allowModificationRedistribution`、`allowAntisocialOrHateUsage=false`；没有额外的 `otherLicenseUrl`。
- **修改/再分发结论**：允许修改、原样再分发、修改后再分发及个人/法人商用；无强制署名。禁止反社会/仇恨表达。再分发须继续遵守 VRM Public License 对原作品和衍生作品许可的条件，不能把权限解释为作者背书或商标授权。
- **资源规格**：VRM 1.0、模型版本 `v1.0.1`；glTF 2.0 二进制容器；3 个 mesh、3 个 skin，joint 引用数分别 83/137/137；54 个 humanoid 映射骨骼；13 个材质、19 张内嵌图像。含 morph 表情（相关 primitive 各有 57 个 morph target）、骨骼 LookAt、MToon、SpringBone，以及 Roll/Aim 节点约束。没有内嵌 glTF 动画片段，也没有外置 buffer/image URI。
- **three-vrm 官方副本（同一人物，不另计数）**：[pixiv/three-vrm 当前文件](https://github.com/pixiv/three-vrm/blob/1b4fc0cc7ef39a49d62bb7a66dcfeca8f65316f7/packages/three-vrm/examples/models/VRM1_Constraint_Twist_Sample.vrm)。其 Git blob SHA 与 VRM Consortium 文件相同：`8ee8717df1af676c4ab89c2344d5bcd907d039f9`。许可仍是上述模型许可，不是代码 MIT。
- **不确定/注意**：这是公开的技术人物样例，并非某部商业动画 IP 的授权人物。转成普通 GLB 时，扭转约束、MToon 与 SpringBone 是否保留取决于转换流程；未执行导入或转换验证。

## 3. AliciaSolid / アリシア・ソリッド（个人用途可选，分发限制较多）

- **直接下载 URL**：[AliciaSolid_vrm-0.51.vrm](https://raw.githubusercontent.com/vrm-c/UniVRM/0b540f390eca11f1841ca3f4a380c1a0f9414809/Tests/Models/Alicia_vrm-0.51/AliciaSolid_vrm-0.51.vrm)
- **归属**：DWANGO Co., Ltd.；文件 `author="© DWANGO Co., Ltd."`。官方专题页署名角色设计师为 **黒星紅白**。具体建模者的个人署名在本次可读的一手文本中未核实，不作推测。
- **官方来源链**：[DWANGO 官方专题页](https://3d.nicovideo.jp/alicia/) → [官方 VRM 发布页 td32797](https://3d.nicovideo.jp/works/td32797)；本清单实际直链来自 [VRM Consortium 的 UniVRM 官方项目文件](https://github.com/vrm-c/UniVRM/blob/0b540f390eca11f1841ca3f4a380c1a0f9414809/Tests/Models/Alicia_vrm-0.51/AliciaSolid_vrm-0.51.vrm)。文件位于 `Tests/Models` 是仓库目录名称，本轮没有执行其中任何测试。
- **许可原文**：[ニコニ立体ちゃんライセンス利用規約](https://3d.nicovideo.jp/alicia/rule.html)。这是自定义模型/角色许可；文件 `licenseName=Other`、`otherLicenseUrl` 及 `otherPermissionUrl` 均指向此原文。官方专题与条款本轮通过直接 HTTP 读取均返回 200；搜索阅读工具无法打开这些页面，不等于页面失效。
- **原文关键范围**：第 1 条的“利用者”是个人及非法人团体；第 3 条允许其创作二次创作物，并复制、发布、销售或分发**自己制作的**二次创作物，适用营利及非营利用途。第 3 条同时禁止侵权、损害角色/公司名誉、公序良俗违规、暴力、反社会、特定信条/宗教及政治发言用途，以及冒充官方商品等行为。不得再许可权利；在 niconico 投稿时，应努力把指定角色作品加入内容树的父作品。
- **修改/再分发结论**：个人本地使用及制作衍生作品有官方依据；允许个人按条款分发自己创作的衍生作品。**没有把原始 VRM 原样再分发给第三人的明确授权，不应把它直接装入公开模型合集或素材包。法人不能仅靠这份个人许可使用。** 文件的 `commercialUssageName=Allow` 仍受上述用户范围与具体条款约束。
- **其他文件设置**：`allowedUserName=Everyone`、`violentUssageName=Disallow`、`sexualUssageName=Disallow`。即使文件可匿名下载，也须遵守这些用途限制。
- **资源规格**：VRM 0.0、模型内部版本 `1.10`，文件名中的 `vrm-0.51` 不是 VRM 1.0；glTF 2.0 二进制容器，生成器 `UniGLTF-1.28`；12 个 mesh、12 个 skin，skin joint 引用数分别为 62/17/29/24/22/21/9/22/36/11/13/6；55 个 humanoid 映射骨骼；12 个材质、8 张内嵌图像。含面部/身体 morph target 及 VRM 0.x 角色数据；没有内嵌 glTF 动画片段，也没有外置 buffer/image URI。官方预览为金发、蓝眼的动漫少女。
- **不确定/注意**：DWANGO 原始下载服务的条款写明该服务下载需 niconico 登录；此处提供的是当前可匿名读取的 UniVRM 官方仓库样例直链，不能据此声称原始发布服务免登录，也没有绕过其下载接口。纯格式转换是否足以构成可独立分发的“自己制作的衍生作品”，原文没有专门说明，不能默认转换后即可公开再分发。后续法人使用或原样分发需要另行取得权利人许可。

## 4. Seed-san（官方动漫少年，带机械臂背包）

- **直接下载 URL**：[Seed-san.vrm](https://raw.githubusercontent.com/vrm-c/vrm-specification/94e82dd346fa6cf0337c4421728640e5252dd38e/samples/Seed-san/vrm/Seed-san.vrm)
- **归属**：VirtualCast, Inc.；官方 README 与模型 `authors` / `copyrightInformation` 一致。
- **官方模型说明**：[模型 README / 许可说明](https://github.com/vrm-c/vrm-specification/blob/94e82dd346fa6cf0337c4421728640e5252dd38e/samples/Seed-san/README.md)、[官方截图](https://raw.githubusercontent.com/vrm-c/vrm-specification/94e82dd346fa6cf0337c4421728640e5252dd38e/samples/Seed-san/screenshot/screenshot.png)。截图可辨识完整动漫人物脸部及人形身体，同时显示背包上的机械臂附件。
- **许可原文**：[VRM Public License 1.0（日文）](https://vrm.dev/licenses/1.0/)、[英文](https://vrm.dev/en/licenses/1.0/)，结合该下载文件的 `VRMC_vrm.meta` 设置。
- **文件许可设置**：`avatarPermission=everyone`、`commercialUsage=corporation`、`creditNotation=required`、`allowRedistribution=true`、`modification=allowModificationRedistribution`；四个 `allow…Usage` 表达限制字段均为 `true`；没有额外的 `otherLicenseUrl`。
- **修改/再分发结论**：允许修改、原样再分发、修改后再分发及个人/法人商用。**必须署名 VirtualCast, Inc.**；按 VRM Public License 第 3 条保留提供的作者/版权/许可和无保证信息，提供许可及来源链接，标明修改。衍生作品分发仍受原作品许可约束，不能暗示官方背书。
- **资源规格**：VRM 1.0、模型版本 `1`；glTF 2.0 二进制容器；5 个 mesh、5 个 skin，joint 引用数分别 23/7/1/21/80；51 个 humanoid 映射骨骼；17 个材质、15 张内嵌图像。含 PBR 与 MToon 材质、SpringBone、Rotation 约束、morph/UV 表情和 Expression LookAt；相关 primitive 含 43 个 morph target。没有内嵌 glTF 动画片段，也没有外置 buffer/image URI。
- **不确定/注意**：机械臂背包属于这个人物样例的一部分，文件并非纯日常服装人物。未确认各 mesh 与背包的对应关系，也未尝试删除附件；未执行导入、蒙皮显示或材质转换验证。

## 链接与规格核实记录

以下结果来自对上述**固定版本直链**的匿名读取；没有携带登录凭证或会话 Cookie。HTTP 可访问仅证明当前能取到官方文件及其元数据，不等同于模型导入、画面或引擎兼容性通过。

| 文件 | HEAD | Content-Type | JSON 部分请求 | 读取的 JSON 长度 | 官方 Git blob SHA |
| --- | --- | --- | --- | --- | --- |
| AvatarSample_B.vrm | 200 | application/octet-stream | 206 Partial Content | 125,612 字节 | `9d29c5e159202d1c274c60cfdf0218e66505a5f1` |
| VRM1_Constraint_Twist_Sample.vrm | 200 | application/octet-stream | 206 Partial Content | 240,248 字节 | `8ee8717df1af676c4ab89c2344d5bcd907d039f9` |
| AliciaSolid_vrm-0.51.vrm | 200 | application/octet-stream | 206 Partial Content | 105,900 字节 | `a5aac1fb97e6a2b8b633aab38bd0193e402f9fdf` |
| Seed-san.vrm | 200 | application/octet-stream | 206 Partial Content | 134,736 字节 | `ff4223f9b42c8f22be35dcf23abe477d93101300` |

Git blob SHA 来自官方 GitHub 仓库树 API，是 Git 对象标识，不是本轮完整下载后计算的 SHA-256。规格统计直接读取相应文件的 glTF JSON：`skin` 个数不等于角色个数；各 skin 的 joint 数可能重叠，不应相加当成总骨骼数；humanoid 数是 VRM 标准骨骼映射数。

## 后续下载整理时保留的事项

1. 将原始 `.vrm` 与本清单一起留存；下载和许可都以本清单的固定版本对应。
2. 这批资源都需要转换为通用 GLB/glTF；只改扩展名不能确保 MToon、SpringBone、LookAt、节点约束和表情语义正确保留。转换没有在本轮执行。
3. 个人使用可以优先整理 AvatarSample_B、VRM1_Constraint_Twist_Sample 和 AliciaSolid；如果需要宽松的修改/公开再分发条件，优先前两项及须署名的 Seed-san。
4. three-vrm 中与 VRM Consortium 同 blob 的样例不重复计数；额外查看过的旧版 three-vrm-girl 也未用于凑不同人物数量。没有选入无模型许可的 GitHub 合集、游戏角色提取包、机甲替代物或普通低模人形。
5. 本次只完成来源整理。模型完整文件尚未保存到本地，转换、程序运行及所有测试均未进行。
