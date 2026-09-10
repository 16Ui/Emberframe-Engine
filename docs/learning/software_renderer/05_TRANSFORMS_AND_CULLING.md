# 第 5 课：坐标变换、透视投影与背面剔除

## 本课解决什么问题

前四课直接给出屏幕坐标。本课第一次从三维物体坐标出发，计算顶点最终落在哪个像素，并在光栅化前丢弃背向相机的三角形。

| 项目 | 内容 |
|---|---|
| 输入 | 立方体局部坐标、模型姿态、相机位置、透视参数 |
| 计算 | Model → View → Projection → 透视除法 → Viewport |
| 输出 | 三维立方体的二维像素图 `transforms.ppm` |
| 复用 | A3 的三角形光栅化、A4 的深度缓冲 |
| 限制 | 暂未实现裁剪；顶点跨过近裁剪面时不能直接使用本课代码 |

## 四个坐标空间分别做什么

| 阶段 | 输入 | 立即完成的工作 | 输出 |
|---|---|---|---|
| Model（模型变换） | 模型局部坐标 | 旋转、缩放、平移整个物体 | 世界坐标 |
| View（观察变换） | 世界坐标和相机参数 | 把世界改写到以相机为原点的坐标系 | 观察坐标 |
| Projection（投影变换） | 观察坐标和视场角 | 建立透视缩短并产生齐次裁剪坐标 | Clip 坐标 |
| Viewport（视口变换） | NDC | 把 `[-1, 1]` 映射到图片像素范围 | 屏幕坐标 |

它们不是四种可替换方案，而是同一个顶点依次经过的四个阶段。

## 完整计算链

设局部坐标顶点为齐次向量：

$$
p_{local}=(x,y,z,1)^T
$$

组合矩阵为：

$$
p_{clip}=P\,V\,M\,p_{local}
$$

- $M$：Model Matrix，模型矩阵；
- $V$：View Matrix，观察矩阵；
- $P$：Projection Matrix，投影矩阵；
- $p_{clip}=(x_c,y_c,z_c,w_c)$：齐次裁剪坐标。

代码采用“矩阵乘列向量”，所以表达式从右向左执行：先 Model，再 View，最后 Projection。顺序交换通常会得到完全不同的结果。

### 透视除法

$$
p_{ndc}=\left(\frac{x_c}{w_c},\frac{y_c}{w_c},\frac{z_c}{w_c}\right)
$$

NDC 是 Normalized Device Coordinates（标准化设备坐标）。透视矩阵让 $w_c$ 与相机空间深度相关，因此更远的顶点除以更大的 $w_c$ 后更靠近画面中心，形成近大远小。

### Viewport 映射

图片宽高分别为 $W,H$，本项目图片的 y 轴向下：

$$
x_s=\left(\frac{x_{ndc}+1}{2}\right)(W-1)
$$

$$
y_s=\left(1-\frac{y_{ndc}+1}{2}\right)(H-1)
$$

例如 $W=512$、$x_{ndc}=0.5$，则 $x_s=0.75\times511=383.25$。光栅化阶段再判断哪些整数像素中心落入三角形。

## 背面剔除

三角形世界坐标为 $p_0,p_1,p_2$，先计算面法线：

$$
n=normalize((p_1-p_0)\times(p_2-p_0))
$$

从三角形指向相机的方向是：

$$
v=normalize(cameraPosition-p_0)
$$

本课顶点从物体外部看按逆时针排列。当 $n\cdot v\le 0$ 时，法线没有朝向相机，这个三角形是背面，可以在光栅化前跳过。

背面剔除解决的是“减少不会被看到的三角形”，深度缓冲解决的是“多个可见候选片元中谁离相机最近”，两者不是同一件事。

## 构建与运行

```powershell
.\scripts\build-windows.ps1 -Config Release -Target emberframe_sr_05_transforms
.\bin\Release\emberframe_sr_05_transforms.exe
```

输出位于 `bin/Release/transforms.ppm`。

## 阅读源码的顺序

1. 在 `main` 中找到局部坐标立方体和三角形索引；
2. 分别找到 `model`、`view`、`projection`；
3. 确认 `mvp = projection * view * model`；
4. 进入 `projectToScreen`，观察 Clip → NDC → Screen；
5. 回到循环，观察背面剔除发生在光栅化之前；
6. 最后复习 A3/A4 已经学过的三角形覆盖和深度测试。

## 必做修改与故障实验

### 修改实验

把 Model 的两个旋转角至少改动一个。在运行前先判断：顶部、左侧、右侧哪一个面积会增大，再用图片验证。

### 故障实验

把 `mvp` 故意改为 `model * view * projection`。记录画面异常，再恢复。原因是列向量约定下，最右边的矩阵最先作用，错误顺序让 Projection 不再最后处理观察坐标。

### 额外观察

暂时注释背面剔除的 `continue`。图片可能因深度缓冲仍看似正确，但提交到光栅器的三角形会增加；这说明“最终画面对”不等于“没有多余工作”。

## 完成检查

完成本课前应能独立回答：

1. Model、View、Projection、Viewport 的输入输出分别是什么？
2. 为什么组合矩阵按 `projection * view * model` 书写？
3. 透视除法为什么会产生近大远小？
4. 图片 y 轴向下时 Viewport 为什么需要翻转 y？
5. 背面剔除和深度缓冲分别排除了什么？
6. 本课为什么还不能正确处理穿过相机近裁剪面的三角形？
