# 写实资源来源核对

2026-10-07；本次只下载和整理资源，不编译、不启动 EmberFrame、不执行测试。

采用 KhronosGroup 官方 `glTF-Sample-Assets`，固定提交 `edc7c9e67c639d230715049ee31f9a96a6babbbe`。下载普通 GLB 或完整 glTF/BIN/PNG，不采用 Draco、KTX2 压缩分支。模型文件保持原样。

| 名称 | 第一方来源 | 模型许可 | 用途与注意点 |
| --- | --- | --- | --- |
| Avocado | [Microsoft / Khronos](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/Avocado) | CC0-1.0 | 颜色、法线、金属粗糙度纹理；原图较大，不宜重复追加很多份 |
| WaterBottle | [Microsoft / Khronos](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/WaterBottle) | CC0-1.0 | 法线、ORM、自发光与多种表面区域 |
| BoomBox | [Microsoft / Khronos](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/BoomBox) | CC0-1.0 | 金属外壳和自发光面板，多贴图可能触及像素预算 |
| Lantern | [Microsoft / Khronos](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/Lantern) | CC0-1.0 | 木质/金属/发光区域，多张原图可能超出当前总像素预算 |
| AntiqueCamera | [UX3D / Maximillan Kamps](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/AntiqueCamera) | CC0-1.0，附 UX3D 标识说明 | 复杂静态道具和多材质；标识不表示作者背书 |
| ToyCar | [Guido Odendahl、Eric Chadwick / Khronos](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/ToyCar) | CC0-1.0 | 清漆/透射/Sheen；当前渲染器不保证复现其全部材质扩展，来源相机不会自动应用 |
| SheenChair | [Khronos](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/SheenChair) | CC0-1.0 | 绒布/木材、多 UV、材质变体；当前引擎不保证完整兼容 Sheen、变体或第二套 UV |
| FlightHelmet | [Gary Hsu / Khronos](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/FlightHelmet) | CC0-1.0 | 皮革、木质、金属、镜片；glTF 必须连同 BIN 和图片保留。原图多且大，需要先缩小贴图才能适应当前编辑器预算；透射并非现有引擎完整能力 |

每个下载目录保存上游 `LICENSE.md` 和 `SOURCE.md`（原 README）。这些源说明及其他元数据本身可能是 CC-BY-4.0，不能由模型 CC0 推断文档或标识也全部 CC0。具体条款以随资源保存的上游文件为准。

当前编辑器限制来自 `engine/lab/editor_assets.h`：单文件 64 MiB、基础贴图单张 8M 像素、全场景纹理含 Mip 合计 16M 像素。这里的格式/容量说明不是已经通过引擎运行测试的承诺。
