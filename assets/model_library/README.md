# 本地免费模型库

下载日期：2026-10-07。供个人本地导入、材质观察和场景搭建使用。2026-10-07 已在 RTX 4060 Laptop GPU 上验证 12 个静态 GLB、8 个代表性卡通 OBJ 和 3 个原有导入样例的前向及延迟渲染，共 46 项通过；同时检查实际可见对象和画面占比，不只检查导入是否返回成功。

## 先从这里用起

新建一个空场景，每次先导入一个模型，确认尺寸后再加入其他模型、调整光源。不要一次性把整个素材库放进同一场景。

| 想试什么 | 推荐文件 |
| --- | --- |
| 小型卡通人物 | [女性角色 A（OBJ）](02_cartoon/MiniCharacters/Models/OBJ%20format/character-female-a.obj) |
| 卡通动物 | [猫（OBJ）](02_cartoon/CubePets/Models/OBJ%20format/animal-cat.obj) |
| 树木 / 自然场景 | [橡树（OBJ）](02_cartoon/NatureKit/Models/OBJ%20format/tree_oak.obj) |
| 房屋建筑 | [建筑 A（OBJ）](02_cartoon/CityKitSuburban/Models/OBJ%20format/building-type-a.obj) |
| 小道具 | [苹果（OBJ）](02_cartoon/FoodKit/Models/OBJ%20format/apple.obj) |
| 写实扫描物体 | [牛油果，512px 静态 GLB](04_ready_static/realistic/Avocado/Avocado_static_512.glb) |
| 写实硬表面 / 多材质 | [飞行头盔，512px 静态 GLB](04_ready_static/realistic/FlightHelmet/FlightHelmet_static_512.glb) |
| 动漫人物 | [AvatarSample B，512px 静态 GLB](04_ready_static/anime/AvatarSample_B/AvatarSample_B_static_512.glb) |

OBJ 必须与相应 `.mtl`、`Textures` 贴图目录一起保留，不能只移动一个 OBJ。静态 GLB 已包含所需几何与贴图。

上表推荐文件包含在已通过的样例中。测试没有逐个覆盖全部 619 个 OBJ，也不代表任意第三方资源或原始 VRM 都能导入。详细记录见 [导入验证结果](D:/games/Emberframe-Engine/output/verification/full-20261007-105254/release-ready/models/results.json)。如果其他文件导入出错，请保留文件路径和程序提示，再定位具体问题。

## 目录怎么分

```text
model_library/
├─ 01_anime/          4 个动漫人物的原始 VRM
├─ 02_cartoon/        Kenney 五套原始包的模型、材质和贴图
├─ 03_realistic/      8 个写实 / 材质展示模型的原始 GLB/glTF
├─ 04_ready_static/   动漫与写实资源的静态、512px GLB 副本
├─ _archives/         五套 Kenney 原始下载 ZIP
├─ sources/           原作者、官方下载地址、逐项许可说明
└─ download-manifest.json
```

模型文件、贴图、原始压缩包和准备后的副本均已设为 Git 忽略，不会默认随引擎源码上传。索引、下载脚本与许可来源说明可以跟随源码保存。

## 1. 动漫人物：4 个

| 人物 | 原作者 / 权利人 | 原始资源 | 静态试用版 |
| --- | --- | --- | --- |
| AvatarSample B | VRoid Project / pixiv Inc. | [VRM](01_anime/AvatarSample_B/AvatarSample_B.vrm) | [GLB](04_ready_static/anime/AvatarSample_B/AvatarSample_B_static_512.glb) |
| Constraint Twist Sample | pixiv Inc. | [VRM](01_anime/ConstraintTwist/VRM1_Constraint_Twist_Sample.vrm) | [GLB](04_ready_static/anime/ConstraintTwist/ConstraintTwist_static_512.glb) |
| AliciaSolid / アリシア・ソリッド | DWANGO Co., Ltd. | [VRM](01_anime/AliciaSolid/AliciaSolid_vrm-0.51.vrm) | [GLB，仅个人本地试用](04_ready_static/anime/AliciaSolid/AliciaSolid_static_512.glb) |
| Seed-san | VirtualCast, Inc. | [VRM](01_anime/SeedSan/Seed-san.vrm) | [GLB](04_ready_static/anime/SeedSan/SeedSan_static_512.glb) |

### 动漫模型必须知道的区别

- 当前引擎不能直接导入 VRM，也不支持带骨骼的 GLB；**不能只把 `.vrm` 改后缀为 `.glb`**。
- 静态版将默认骨骼姿势和默认形变权重烘焙进顶点，去掉动画、表情控制、弹簧骨骼和节点约束。人物是可摆放的静态网格，不是可驱动角色。
- 静态版使用普通 glTF PBR 回退材质，**没有实现原版 MToon 卡通着色、描边、头发物理等效果**。四个静态副本已通过本机前向及延迟渲染检查；通过不代表保留了原版全部材质或动画效果。
- 每个准备后的模型旁有 `SOURCE.json`，保留修改记录与原始 VRM 的许可元数据；原始文件未被改写。

### 许可提醒

这四个人物**不等于 CC0**。AvatarSample B 还受 VRoid 附加条款约束；Constraint Twist 按 VRM Public License 的模型设置使用；**Seed-san 必须署名 VirtualCast, Inc.**；**AliciaSolid 只整理为个人本地试用资源，不应原样或仅转换后打包公开分发**。代码仓库的 MIT 许可不是人物模型许可。

完整条款、来源与版本差异：[动漫模型许可与来源](sources/ANIME_SOURCES.md)。准备后的副本继续适用原资源的许可，不会因为转换而获得更宽松的分发权。

## 2. 卡通 / 低多边形：五套，619 个 OBJ 条目

| 素材包 | OBJ 条目数 | 包含什么 | 导入目录 |
| --- | ---: | --- | --- |
| Mini Characters | 26 | 其中 12 个男女角色，另有辅助器具 | `02_cartoon/MiniCharacters/Models/OBJ format/` |
| Cube Pets | 24 | 猫、狗、兔、牛、猪、狐狸等动物 | `02_cartoon/CubePets/Models/OBJ format/` |
| Nature Kit | 329 | 树、灌木、岩石、桥梁等 | `02_cartoon/NatureKit/Models/OBJ format/` |
| City Kit (Suburban) | 40 | 房屋、郊区建筑及配套道具 | `02_cartoon/CityKitSuburban/Models/OBJ format/` |
| Food Kit | 200 | 水果、食物、容器和小道具 | `02_cartoon/FoodKit/Models/OBJ format/` |

均为 **Kenney 官方 CC0** 包；根目录的原版 `License.txt` 已保留。619 的统计口径是 OBJ 文件条目数，不把同一模型的 GLB/FBX 等不同导出格式重复计数，也不等于 619 个角色。

优先选 OBJ 静态版。包内 FBX 不能直接用于当前引擎；动画 GLB 也不应当作支持骨骼的承诺。

官方下载地址及许可：[卡通素材来源](sources/CARTOON_SOURCES.md)。

## 3. 写实 / 材质观察：8 个

| 模型 | 观察用途 | 512px 静态试用版 |
| --- | --- | --- |
| Avocado | 扫描物体、表面凹凸 | [GLB](04_ready_static/realistic/Avocado/Avocado_static_512.glb) |
| WaterBottle | 塑料、金属、法线 | [GLB](04_ready_static/realistic/WaterBottle/WaterBottle_static_512.glb) |
| BoomBox | 多种表面与细节 | [GLB](04_ready_static/realistic/BoomBox/BoomBox_static_512.glb) |
| Lantern | 硬表面、粗糙度与金属感 | [GLB](04_ready_static/realistic/Lantern/Lantern_static_512.glb) |
| AntiqueCamera | 相机、多材质硬表面 | [GLB](04_ready_static/realistic/AntiqueCamera/AntiqueCamera_static_512.glb) |
| FlightHelmet | 多网格、多材质的头盔 | [GLB](04_ready_static/realistic/FlightHelmet/FlightHelmet_static_512.glb) |
| ToyCar | 小汽车、清漆 / 材质扩展对照 | [GLB](04_ready_static/realistic/ToyCar/ToyCar_static_512.glb) |
| SheenChair | 椅子、多 UV / 材质扩展边界 | [GLB](04_ready_static/realistic/SheenChair/SheenChair_static_512.glb) |

模型资产许可为 CC0，来自 Khronos 官方 glTF Sample Assets。部分说明文档另有 CC-BY 许可；AntiqueCamera 的 UX3D 标识还有独立许可文件，均随原始目录保留。[详细来源与限制](sources/REALISTIC_SOURCES.md)。

原始模型可能带 2K 以上贴图，容易触及当前引擎的场景纹理预算。**先用 512px 副本**；需要研究原始细节时，再从 `03_realistic` 逐个导入。

准备后的副本保留基础 PBR、清漆等当前导入流程认识的核心信息，去除非通用扩展；**不保留 sheen、transmission、材质 variants 等原版扩展效果**。转换不表示引擎已实现了这些特性。

ToyCar 和 SheenChair 已生成适合当前单 UV 管线的兼容副本：以颜色贴图等主纹理的 UV 为基准，把其纹理变换烘焙到 UV；需要其他 UV 的贴图不强行错绑，而是使用材质常量或几何法线回退。清漆保留常量参数，不保留当前不支持的清漆贴图。每项取舍记录在模型旁的 `SOURCE.json` 中，原始资产未改动。因此兼容副本可用于当前演示，但不是原版材质的高保真复现。

## 重新下载 / 重新准备

只在需要恢复资源时使用，无须为了导入而先执行：

```powershell
# 在引擎项目根目录执行。只下载资源，不启动引擎。
.\scripts\download-model-library.ps1 -Group All
```

下载脚本保留已存在文件，发现已知大小不一致时停止，不覆盖原文件。只展开模型、贴图、材质与许可文档，不运行资源包内的脚本或程序。

`scripts/prepare-model-library.py` 用 Python + numpy + Pillow 生成静态 GLB。转换后的副本不会覆盖原模型；也不覆盖已存在副本。它是这批本地样例的整理工具，**不是完整 VRM 导入器或高保真通用资产管线**。
