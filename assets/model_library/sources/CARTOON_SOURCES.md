# 免费卡通 / 低多边形 3D 模型来源核实

核实日期：2026-10-07（Asia/Shanghai）。原作者均为 Kenney；只使用其官网页面及页面实际链接的官方 ZIP。

## 推荐结论

优先组合：**Mini Characters + Nature Kit + City Kit (Suburban)**，分别覆盖卡通人物、树木与自然场景、房屋与建筑道具。若需要动物，补充 Cube Pets；需要食物与厨房小道具，补充 Food Kit。五包均为官方免费 CC0 资源，并已确认 ZIP 内存在独立 OBJ 静态版。

| 包 | 适用内容 | 官网标称数量 | ZIP 实查 OBJ 数 | ZIP 大小 | 当前引擎建议 |
| --- | --- | ---: | ---: | ---: | --- |
| Mini Characters | 卡通人物、辅助器具 | 25 | 26 | 2,403,059 字节 / 2.29 MiB | 选择 OBJ 人物静态版 |
| Nature Kit | 树木、岩石、桥梁、自然场景道具 | 330 | 329 | 10,537,521 字节 / 10.05 MiB | 选择 OBJ 静态版 |
| City Kit (Suburban) | 房屋、郊区建筑、配套道具 | 40 | 40 | 3,038,740 字节 / 2.90 MiB | 选择 OBJ 静态版 |
| Cube Pets | 猫、狗、牛、猪、兔等卡通动物 | 24 | 24 | 2,812,444 字节 / 2.68 MiB | 选择 OBJ 动物静态版 |
| Food Kit | 水果、食物、容器等小道具 | 200 | 200 | 4,606,270 字节 / 4.39 MiB | 选择 OBJ 静态版 |

数量口径：官网的 Files / assets 数与 ZIP 中 `.obj` 文件数分别列出；多种导出格式、贴图和预览图不重复计为独立模型。Mini Characters 和 Nature Kit 存在上述标称数与实查数差异，采购/收录时应采用实查结果。

## 1. Mini Characters — 首选人物包

- 官网：[Mini Characters](https://kenney.nl/assets/mini-characters)。官网标为 3D / Mini、含动画、25 项资源，版本 1.0。
- 官方 ZIP 直链：[kenney_mini-characters.zip](https://kenney.nl/media/pages/assets/mini-characters/bfc7e272b4-1774770718/kenney_mini-characters.zip)。从该官网页面 `id="donate-text"` 的 `href` 读取，对应 “Continue without donating...” 链接。
- 许可：**CC0 1.0**。官网许可栏与 ZIP 根目录 `License.txt` 均确认 CC0；包内许可说明允许个人、教育及商业用途，署名非强制。
- 大小：匿名 HEAD 返回 `Content-Length: 2403059`，约 2.29 MiB。
- 格式：ZIP 目录实查包含 **26 OBJ + 26 MTL、26 GLB、26 FBX**，以及 PNG 贴图。
- 可选模型：`Models/OBJ format/character-female-a.obj`、`character-female-b.obj`、`character-male-a.obj` 等；实查有 12 个 `character-female-*` / `character-male-*` 人物 OBJ，其余为辅助器具等模型。
- 静态兼容性：选 `Models/OBJ format/` 下的 OBJ。少量读取 `character-female-a.obj`，确认其引用 `character-female-a.mtl`，材质名为 `colormap`；ZIP 中存在 `Models/OBJ format/Textures/colormap.png`。OBJ 仅供静态网格使用，不保留动画。鉴于当前引擎拒绝带骨骼 GLB，本任务不推荐该包的动画 GLB。

## 2. Nature Kit — 首选树木 / 自然场景包

- 官网：[Nature Kit](https://kenney.nl/assets/nature-kit)。官网标为 3D、330 项资源，版本 1.0，标签包含树木、岩石和植被。
- 官方 ZIP 直链：[kenney_nature-kit.zip](https://kenney.nl/media/pages/assets/nature-kit/37ac38a37b-1677698939/kenney_nature-kit.zip)。来自该官网页面 “Continue without donating...” 的实际 `href`。
- 许可：**CC0 1.0**，由官网许可栏确认；ZIP 目录中存在根目录 `License.txt`。
- 大小：匿名 HEAD 返回 `Content-Length: 10537521`，约 10.05 MiB。
- 格式：ZIP 目录实查包含 **329 OBJ + 329 MTL、329 GLB、329 FBX、329 DAE、329 STL**，以及 PNG 文件。
- 可选模型：`Models/OBJ format/tree_default.obj`、`tree_oak.obj`、`tree_palm.obj`、`tree_pineDefaultA.obj`、`bridge_stone.obj`、`bridge_wood.obj` 等。官网标称 330 项，但当前 ZIP 只实查到 329 个 OBJ，不将标称数当作实查数。
- 静态兼容性：选择 OBJ 静态版并保留对应 MTL、贴图及目录关系。GLB 文件存在已确认，但未逐个检查是否带骨骼；本次统一优先 OBJ。

## 3. City Kit (Suburban) — 首选房屋 / 郊区场景包

- 官网：[City Kit (Suburban)](https://kenney.nl/assets/city-kit-suburban)。官网标为 3D / City、40 项资源，版本 2.0，含颜色变体。
- 官方 ZIP 直链：[kenney_city-kit-suburban_20.zip](https://kenney.nl/media/pages/assets/city-kit-suburban/2c871b7af2-1745479373/kenney_city-kit-suburban_20.zip)。来自该官网页面 “Continue without donating...” 的实际 `href`。
- 许可：**CC0 1.0**。官网许可栏与 ZIP 根目录 `License.txt` 均确认 CC0；包内许可说明允许个人、教育及商业用途，署名非强制。
- 大小：匿名 HEAD 返回 `Content-Length: 3038740`，约 2.90 MiB。
- 格式：ZIP 目录实查包含 **40 OBJ + 40 MTL、40 GLB、40 FBX**，以及 PNG 贴图。
- 可选模型：`Models/OBJ format/building-type-a.obj` 至 `building-type-u.obj` 等房屋/建筑模型。
- 静态兼容性：选择 OBJ 静态版。少量读取 `building-type-a.obj`，确认其引用 `building-type-a.mtl`，材质名为 `colormap`；ZIP 中存在 `Models/OBJ format/Textures/colormap.png`，另有颜色变体贴图。GLB 的骨骼状态未逐个核查。

## 4. Cube Pets — 动物备选包

- 官网：[Cube Pets](https://kenney.nl/assets/cube-pets)。官网标为 3D、含动画、24 项资源；更新栏标为 2.0。
- 官方 ZIP 直链：[kenney_cube-pets_1.0.zip](https://kenney.nl/media/pages/assets/cube-pets/44e58e945f-1774520254/kenney_cube-pets_1.0.zip)。来自该官网页面 “Continue without donating...” 的实际 `href`，保留官方原始文件名，没有自行修改版本号。
- 版本核对：**ZIP 文件名仍为 `_1.0.zip`，但官网更新栏与包内 `License.txt` 均标为 Cube Pets 2.0**；记录此差异，不猜测另一个下载地址。
- 许可：**CC0 1.0**。官网许可栏与 ZIP 根目录 `License.txt` 均确认 CC0；包内许可说明允许个人、教育及商业用途，署名非强制。
- 大小：匿名 HEAD 返回 `Content-Length: 2812444`，约 2.68 MiB。
- 格式：ZIP 目录实查包含 **24 OBJ + 24 MTL、24 GLB、24 FBX**，以及 PNG 贴图。
- 可选模型：`Models/OBJ format/animal-cat.obj`、`animal-dog.obj`、`animal-cow.obj`、`animal-pig.obj`、`animal-bunny.obj`、`animal-fox.obj`、`animal-chick.obj` 等。
- 静态兼容性：选择 OBJ 动物静态版。少量读取 `animal-beaver.obj`，确认其引用 `animal-beaver.mtl`，材质名为 `colormap`；ZIP 中存在 `Models/OBJ format/Textures/colormap.png`。官网含动画标记，当前引擎应避开动画 GLB，使用无动画的 OBJ 导出。

## 5. Food Kit — 食物 / 小道具备选包

- 官网：[Food Kit](https://kenney.nl/assets/food-kit)。官网标为 3D、200 项资源，版本 2.0。
- 官方 ZIP 直链：[kenney_food-kit.zip](https://kenney.nl/media/pages/assets/food-kit/83086fa91c-1719418518/kenney_food-kit.zip)。来自该官网页面 “Continue without donating...” 的实际 `href`。
- 许可：**CC0 1.0**，由官网许可栏确认；ZIP 目录中存在根目录 `License.txt`。
- 大小：匿名 HEAD 返回 `Content-Length: 4606270`，约 4.39 MiB。
- 格式：ZIP 目录实查包含 **200 OBJ + 200 MTL、200 GLB、200 FBX**，以及 PNG 贴图。
- 可选模型：`Models/OBJ format/apple.obj`、`banana.obj`、`avocado.obj`、`barrel.obj`、`bottle-ketchup.obj`、`bag.obj` 等。
- 静态兼容性：选择 OBJ 静态版，保留相应 MTL 与贴图。GLB 文件存在已确认，但未逐个检查骨骼状态；本次统一优先 OBJ。

## 核验依据与导入交接

1. 上述五个 ZIP URL 均来自对应 Kenney 官方页面 HTML 中的下载链接，未猜测哈希、版本号或路径。没有使用镜像、第三方转载或 All-in-1 付费合集。
2. 对五个直链进行无账号、无登录 Cookie、无 Authorization 的 HEAD 请求，均返回 **HTTP 200、`Content-Type: application/zip`**，最终 URL 与所列直链一致，没有跳转登录页。
3. 对 ZIP 尾部及中央目录进行限量 HTTP Range 读取，均返回 **HTTP 206**，核实实际文件扩展名、数量和路径。另少量读取 Mini Characters、City Kit (Suburban)、Cube Pets 的许可与单个 OBJ 内容；未将完整 ZIP 下载到磁盘。
4. 免费入口的链接文字为 “Continue without donating...”。本次未进入支付或捐赠流程；直接使用该入口所指的官方 ZIP 地址。
5. OBJ 是静态网格交接方案。后续实际下载时，完整保留 `Models/OBJ format/` 内的 OBJ、对应 MTL 和相对路径引用的 PNG；只取 OBJ 文件可能丢失原配色或材质。OBJ 不包含骨骼动画，因此人物和动物将以导出姿态作为静态模型使用。
6. **未运行任何测试或引擎，未执行导入。**“静态兼容性”仅表示存在可直接交给 OBJ 导入器的静态文件，不表示已验证当前引擎的坐标轴、缩放、材质或渲染效果。GLB 是否带骨骼未逐个检查，不将 GLB 的存在等同于当前引擎可接受。
7. 本次仅写入本 Markdown 文件，没有新增模型、贴图、压缩包、脚本或其他项目文件。链接和大小为本次核实结果，官网更新后可能变化。
