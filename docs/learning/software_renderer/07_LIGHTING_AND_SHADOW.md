# 第 7 课：基础光照与 Shadow Map

## 本课解决什么问题

A6 已经能得到表面颜色，但颜色不会随表面朝向和遮挡改变。本课完成两个互相关联的问题：

1. 用法线和光照方向估计一个像素有多亮；
2. 从光源视角记录最近深度，判断该像素能否直接看到光源。

| 项目 | 内容 |
|---|---|
| 输入 | 世界坐标位置、世界空间法线、相机位置、光源位置、材质颜色 |
| Pass 1 输出 | 光源视角的最近深度 `shadow_map.ppm` |
| Pass 2 输出 | 带漫反射、高光和阴影的 `lighting_shadow.ppm` |
| 复用 | 深度缓冲、坐标变换、重心坐标、透视正确属性插值 |
| 假设 | 单个点光源位置用于方向计算，阴影投影使用固定正交范围，场景不会越过裁剪面 |
| 限制 | 单次最近邻阴影采样会产生锯齿；固定 Bias 只能缓解精度伪影 |

## 方向约定

本课所有光照向量都在世界空间：

| 符号 | 定义 | 方向 |
|---|---|---|
| $n$ | 单位表面法线 | 从表面指向外部 |
| $l$ | Point-to-Light Direction | 从当前表面点指向光源 |
| $v$ | Point-to-Camera Direction | 从当前表面点指向相机 |
| $h$ | Half Vector，半程向量 | $normalize(l+v)$ |

如果法线在模型空间、光方向在世界空间，直接点乘没有几何意义。必须先把它们放到同一坐标空间。

## 基础光照

### Lambert 漫反射

$$
I_d=\max(n\cdot l,0)
$$

$I_d$ 是 `[0,1]` 的漫反射强度。点积为 1 表示法线正对光源；小于或等于 0 表示光在表面背后，所以截断为 0。

例如 $n=(0,1,0)$、$l=(0,0.8,0.6)$ 且两者已归一化，则 $I_d=0.8$。

### Blinn-Phong 高光

$$
I_s=\max(n\cdot h,0)^p
$$

- $h=normalize(l+v)$；
- $p$ 是高光指数，本课使用 32；
- $I_s$ 是高光强度，不是最终颜色。

$p$ 越大，高光范围越窄。这个模型便于理解局部光照组成，但不是现代物理渲染的最终模型。

### 本课最终组合

$$
C=C_{base}(I_a+V\,k_dI_d)+C_{white}V\,k_sI_s
$$

- $C_{base}$：材质基础颜色；
- $I_a$：环境项，防止阴影区域完全为黑；
- $V$：阴影可见性，受光时为 1，遮挡时本课使用 0.28；
- $k_d,k_s$：漫反射与高光权重；
- $C_{white}$：白色高光。

## Shadow Map 的两个 Pass

Shadow Map 是 Shadow Mapping（阴影映射）使用的二维深度数据，不是最终阴影颜色。

### Pass 1：从光源看场景

1. 用光源的 View 和 Projection 变换所有三角形；
2. 光栅化三角形；
3. 每个像素只保存距离光源最近的深度 $d_{map}$。

结果表示“光源在每个方向上最先看到哪里”。

### Pass 2：从相机看场景

对相机看到的世界坐标点 $p$：

1. 再用光源的 View-Projection 投影 $p$；
2. 得到它在 Shadow Map 中的坐标和当前深度 $d_{current}$；
3. 与 Pass 1 保存的 $d_{map}$ 比较。

$$
V=\begin{cases}
0.28,&d_{current}-bias>d_{map}\\
1.0,&\text{otherwise}
\end{cases}
$$

如果当前点比光源最先看到的表面更远，说明两者之间存在遮挡物。

## 为什么需要 Bias

同一表面在两次离散光栅化中的深度可能有微小差异。若直接比较，表面可能错误遮挡自己，出现条纹或黑点，这叫 Shadow Acne（阴影痤疮）。

本课根据 $n\cdot l$ 调整 Bias：表面越倾斜，允许的误差稍大。Bias 太小会自阴影；太大则会让阴影与物体分离，称为 Peter Panning。

Bias 是精度补偿，不是阴影算法的核心可见性定义。

## 构建与运行

```powershell
.\scripts\build-windows.ps1 -Config Release -Target emberframe_sr_07_lighting_shadow
.\bin\Release\emberframe_sr_07_lighting_shadow.exe
```

输出：

- `bin/Release/lighting_shadow.ppm`：相机视角最终颜色；
- `bin/Release/shadow_map.ppm`：光源视角深度可视化，越亮表示越靠近光源。

## 必做修改与故障实验

### 修改实验

改变 `lightPosition` 的 x 分量。在运行前预测地面阴影向哪个方向移动，并同时观察 Shadow Map 中立方体投影的位置变化。

### 故障实验一：移除阴影

让 `shadowVisibility` 始终返回 `1.0F`。漫反射和高光仍存在，但地面不再接收立方体阴影。这证明光照强度与遮挡可见性是两个计算。

### 故障实验二：Bias 设为零

把 `bias` 临时设为 0，观察是否出现自阴影噪点。若当前固定场景不明显，可移动光源或降低 Shadow Map 分辨率放大精度差异。

## 完成检查

1. $n,l,v,h$ 分别指向哪里，为什么必须处于同一坐标空间？
2. Lambert 点积输出的语义和范围是什么？
3. Blinn-Phong 指数增大时高光怎样变化？
4. Shadow Map 保存的是颜色、距离还是可见性？
5. 为什么需要先从光源渲染，再从相机渲染？
6. Shadow Acne 和 Peter Panning 分别对应 Bias 太小还是太大？
7. 阴影分辨率不足会造成什么可见伪影？
