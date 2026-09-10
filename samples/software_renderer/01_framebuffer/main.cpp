#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

// 一个像素的颜色数据。本课只保存红、绿、蓝三个 8 位通道。
// 每个通道的取值范围是 0～255，因此一个 Color 共占 3 个字节。
struct Color
{
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};

// Framebuffer（帧缓冲区）是在 CPU 内存中保存整张图片的数据结构。
// 它暂时与窗口和 GPU 无关，只负责管理像素以及把像素保存成图片。
class Framebuffer
{
public:
    Framebuffer(std::size_t width, std::size_t height)
        : width_(width),
          height_(height),
          pixels_(width * height * kChannelsPerPixel, 0)
    {
    }

    bool setPixel(std::size_t x, std::size_t y, Color color)
    {
        // 越界坐标没有对应的像素，继续写入会访问不属于图片的内存。
        if (x >= width_ || y >= height_) {
            return false;
        }

        // 二维坐标先转换为第几个像素，再乘以每像素 3 个颜色通道。
        // 本课约定 (0, 0) 在图片左上角，x 向右增加，y 向下增加。
        const std::size_t byteOffset = (y * width_ + x) * kChannelsPerPixel;
        pixels_[byteOffset] = color.red;
        pixels_[byteOffset + 1] = color.green;
        pixels_[byteOffset + 2] = color.blue;
        return true;
    }

    bool savePpm(const std::filesystem::path& outputPath) const
    {
        std::ofstream output(outputPath, std::ios::binary);
        if (!output) {
            return false;
        }

        // P6 是 PPM 的二进制格式：文件头描述尺寸和最大颜色值，
        // 文件头之后紧跟按 RGB 顺序排列的全部像素字节。
        output << "P6\n" << width_ << ' ' << height_ << "\n255\n";
        output.write(
            reinterpret_cast<const char*>(pixels_.data()),
            static_cast<std::streamsize>(pixels_.size()));
        return output.good();
    }

private:
    static constexpr std::size_t kChannelsPerPixel = 3;

    std::size_t width_;
    std::size_t height_;
    std::vector<std::uint8_t> pixels_;
};

int main(int argc, char* argv[])
{
    (void)argc;

    constexpr std::size_t width = 256;
    constexpr std::size_t height = 256;
    Framebuffer framebuffer(width, height);

    constexpr Color red{255, 64, 64};
    constexpr Color green{64, 255, 64};
    constexpr Color blue{64, 128, 255};
    constexpr Color yellow{255, 220, 64};

    // 用四个色块验证坐标方向和内存索引。这里仍然只通过 setPixel 写像素，
    // 下一课的直线和三角形算法也会复用同一个最底层操作。
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const bool isTop = y < height / 2;
            const bool isLeft = x < width / 2;

            Color color = yellow;
            if (isTop && isLeft) {
                color = red;
            } else if (isTop) {
                color = green;
            } else if (isLeft) {
                color = blue;
            }

            if (!framebuffer.setPixel(x, y, color)) {
                std::cerr << "Pixel coordinate is outside the framebuffer.\n";
                return 1;
            }
        }
    }

    // 将图片放在可执行文件旁边，使从终端或 Visual Studio 启动时结果一致。
    const std::filesystem::path outputPath =
        std::filesystem::absolute(argv[0]).parent_path() / "framebuffer.ppm";

    if (!framebuffer.savePpm(outputPath)) {
        std::cerr << "Failed to save framebuffer: " << outputPath << '\n';
        return 1;
    }

    std::cout << "Framebuffer saved to: " << outputPath << '\n';
    return 0;
}
