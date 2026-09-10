# Software Renderer 第一课：CPU Framebuffer

本课只解决一个问题：怎样在 CPU 内存中表示一张二维彩色图片，并把它保存到磁盘。

对应源码：[samples/software_renderer/01_framebuffer/main.cpp](../../../samples/software_renderer/01_framebuffer/main.cpp)。

## 1. 本课输入与输出

| 字段 | 内容 |
|---|---|
| 输入 | 图片宽高、像素坐标 `(x, y)`、RGB 颜色 |
| 处理 | 把二维坐标换算成一维字节数组中的位置 |
| 输出 | 内存中的完整像素数组和 `framebuffer.ppm` 图片 |
| 暂不包含 | SDL 窗口、Vulkan、GPU、直线、三角形和模型 |

## 2. Framebuffer 是什么

Framebuffer 的中文含义是帧缓冲区。它属于一种存储数据结构：保存最终图片中每个像素的颜色。

本课使用的帧缓冲区包含：

- `width_`：一行有多少个像素；
- `height_`：一共有多少行；
- `pixels_`：连续保存全部 RGB 字节的一维数组。

它不是渲染算法。直线、三角形和光栅化算法会计算“哪些像素应该是什么颜色”，Framebuffer 负责保存计算结果。

## 3. RGB 与一个像素

RGB 是 Red、Green、Blue，即红、绿、蓝三个颜色通道。本课每个通道使用一个 `std::uint8_t`：

| 值 | 含义 |
|---:|---|
| `0` | 该颜色通道没有强度 |
| `255` | 该颜色通道达到本格式的最大强度 |

例如：

```cpp
Color red{255, 64, 64};
```

这个颜色以红色为主，同时保留少量绿色和蓝色，所以不是最纯的 `(255, 0, 0)`。

## 4. 二维坐标怎样进入一维内存

程序真正分配的是一段连续的一维内存，而图片使用 `(x, y)` 二维坐标。先计算目标像素是整张图片中的第几个像素：

```text
pixelIndex = y × width + x
```

每个像素有 RGB 三个字节，所以目标像素第一个字节的位置为：

```text
byteOffset = (y × width + x) × 3
```

最小数值例子：图片宽度为 `4`，要写坐标 `(2, 1)`。

```text
pixelIndex = 1 × 4 + 2 = 6
byteOffset = 6 × 3 = 18
```

因此：

- `pixels[18]` 保存红色；
- `pixels[19]` 保存绿色；
- `pixels[20]` 保存蓝色。

本课约定 `(0, 0)` 在左上角，x 向右增加，y 向下增加。坐标约定本身可以不同，但同一个渲染流程内必须保持一致。

## 5. 为什么必须检查越界

宽度和高度都是 `256` 时，有效坐标范围为 `0～255`。`x == 256` 或 `y == 256` 已经超出图片。

如果不检查就写入，程序可能修改其他对象的内存，产生崩溃或难以定位的数据错误。因此 `setPixel` 在越界时返回 `false`，不执行写入。

## 6. PPM 文件

PPM 是 Portable Pixmap Format，即可移植像素图格式。本课使用二进制 `P6` 版本，因为它只需要一个短文件头和连续的 RGB 数据，不需要图片压缩库。

文件的数据顺序是：

```text
P6
宽度 高度
255
全部 RGB 字节
```

PPM 只是本阶段观察 CPU 像素结果的出口，不是引擎最终使用的纹理格式。

## 7. 构建与运行

在仓库根目录执行：

```powershell
.\scripts\build-windows.ps1 -Config Release -Target emberframe_sr_01_framebuffer
.\bin\Release\emberframe_sr_01_framebuffer.exe
```

程序会在可执行文件旁边生成：

```text
bin/Release/framebuffer.ppm
```

预期图片由四个色块组成：左上红色、右上绿色、左下蓝色、右下黄色。

## 8. 与 Vulkan 的关系

本课的 `std::vector<std::uint8_t>` 是 CPU 可以直接读写的图片内存。以后 Vulkan 的 `VkImage` 通常位于 GPU 可使用的内存中，CPU 不能再把它当普通数组随意写入。

两者解决的共同问题都是保存二维图像，但内存位置、访问方式和同步要求不同。先理解 CPU 版本，后面才能明确 Vulkan 额外解决了什么。

## 9. 理解检查

1. 为什么 `256 × 256` 的 RGB 图片需要 `256 × 256 × 3` 个字节？
2. 宽度为 `4` 时，坐标 `(2, 1)` 为什么对应第 `6` 个像素？
3. `setPixel` 为什么需要越界检查？
4. Framebuffer 与画直线算法分别负责什么？

能够用自己的话回答这四问后，再进入下一课 Bresenham 直线光栅化。
