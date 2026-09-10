#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

// Software Renderer 各课共用的 RGB 颜色。
// 每个通道使用 8 位无符号整数，合法范围为 0～255。
struct Color
{
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};

// CPU Framebuffer 只负责保存像素和输出图片，不负责决定画什么图形。
// 直线、三角形等算法计算出像素坐标后，统一调用 setPixel 写入结果。
class Framebuffer
{
public:
    Framebuffer(int width, int height, Color clearColor = {0, 0, 0});

    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;

    [[nodiscard]] bool setPixel(int x, int y, Color color);
    void clear(Color color);

    // A8 的 SDL 查看器直接上传 RGB 字节；只提供只读访问，避免绕过边界检查修改内容。
    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept;

    [[nodiscard]] bool savePpm(const std::filesystem::path& outputPath) const;

private:
    static constexpr std::size_t kChannelsPerPixel = 3;

    int width_;
    int height_;
    std::vector<std::uint8_t> pixels_;
};

// 统一把输出放到可执行文件旁边，避免终端和 Visual Studio 使用不同工作目录。
[[nodiscard]] std::filesystem::path outputPathBesideExecutable(
    const char* executablePath,
    std::string_view fileName);
