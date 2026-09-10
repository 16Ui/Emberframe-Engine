#include "framebuffer.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <ostream>
#include <string>

struct Point2i
{
    int x;
    int y;
};

void drawLine(
    Framebuffer& framebuffer,
    Point2i start,
    Point2i end,
    Color color,
    std::ostream* trace = nullptr)
{
    // Bresenham 的核心循环沿变化更大的轴前进。
    // 陡线的 y 变化更大，交换 x/y 后可复用同一套循环。
    const bool isSteep =
        std::abs(end.y - start.y) > std::abs(end.x - start.x);
    if (isSteep) {
        std::swap(start.x, start.y);
        std::swap(end.x, end.y);
    }

    // 统一从较小的 x 走向较大的 x，避免再写一套反向循环。
    if (start.x > end.x) {
        std::swap(start, end);
    }

    if (trace != nullptr) {
        *trace << "steep=" << (isSteep ? "yes" : "no")
               << ", normalizedStart=(" << start.x << ',' << start.y << ')'
               << ", normalizedEnd=(" << end.x << ',' << end.y << ")\n";
        *trace << "step  mainX  sideY  pixel       errorBefore  subtractDy  advanceY  nextError\n";
    }

    const int deltaX = end.x - start.x;
    const int deltaY = std::abs(end.y - start.y);
    int error = deltaX / 2;
    int y = start.y;
    const int yStep = start.y < end.y ? 1 : -1;

    int step = 0;
    for (int x = start.x; x <= end.x; ++x, ++step) {
        // 前面若交换过坐标，写像素时必须交换回来。
        const int pixelX = isSteep ? y : x;
        const int pixelY = isSteep ? x : y;
        // 本示例的端点都位于图片范围内，因此这里明确忽略成功返回值。
        (void)framebuffer.setPixel(pixelX, pixelY, color);

        // error 累积理想直线与当前整数像素行之间的偏差。
        // 偏差越过半个像素时，让 y 前进一步并补回 deltaX。
        const int errorBefore = error;
        error -= deltaY;
        const int errorAfterSubtract = error;
        bool advancedY = false;
        if (error < 0) {
            y += yStep;
            error += deltaX;
            advancedY = true;
        }

        if (trace != nullptr) {
            *trace << step << "     " << x << "      " << (isSteep ? pixelX : pixelY)
                   << "      (" << pixelX << ',' << pixelY << ")"
                   << "      " << errorBefore
                   << "            " << errorAfterSubtract
                   << "           " << (advancedY ? "yes" : "no")
                   << "       " << error << '\n';
        }
    }
}

int main(int argc, char* argv[])
{
    constexpr int width = 512;
    constexpr int height = 512;
    Framebuffer framebuffer(width, height, Color{20, 24, 32});

    if (argc == 6 && std::string(argv[1]) == "--trace") {
        try {
            const Point2i start{std::stoi(argv[2]), std::stoi(argv[3])};
            const Point2i end{std::stoi(argv[4]), std::stoi(argv[5])};
            std::cout << "inputStart=(" << start.x << ',' << start.y << ")"
                      << ", inputEnd=(" << end.x << ',' << end.y << ")\n";
            drawLine(framebuffer, start, end, Color{80, 230, 140}, &std::cout);

            const auto outputPath = outputPathBesideExecutable(argv[0], "line_trace.ppm");
            if (!framebuffer.savePpm(outputPath)) {
                std::cerr << "Failed to save line trace image: " << outputPath << '\n';
                return 1;
            }
            std::cout << "Line trace image saved to: " << outputPath << '\n';
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Invalid trace endpoints: " << error.what() << '\n';
            return 1;
        }
    }

    if (argc != 1) {
        std::cerr << "Usage: " << argv[0] << " [--trace x0 y0 x1 y1]\n";
        return 1;
    }

    constexpr Point2i center{width / 2, height / 2};
    constexpr std::array<Point2i, 12> endpoints{
        Point2i{32, 32}, Point2i{256, 24}, Point2i{480, 32},
        Point2i{488, 256}, Point2i{480, 480}, Point2i{256, 488},
        Point2i{32, 480}, Point2i{24, 256}, Point2i{128, 32},
        Point2i{480, 160}, Point2i{384, 480}, Point2i{32, 352}};

    constexpr std::array<Color, 6> palette{
        Color{255, 84, 84}, Color{255, 190, 64}, Color{246, 240, 96},
        Color{80, 230, 140}, Color{80, 170, 255}, Color{190, 110, 255}};

    // 从中心向不同方向画线，覆盖水平、垂直、缓坡、陡坡和反向端点。
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
        drawLine(framebuffer, center, endpoints[index], palette[index % palette.size()]);
    }

    const auto outputPath = outputPathBesideExecutable(argv[0], "lines.ppm");
    if (!framebuffer.savePpm(outputPath)) {
        std::cerr << "Failed to save line sample: " << outputPath << '\n';
        return 1;
    }

    std::cout << "Line sample saved to: " << outputPath << '\n';
    return 0;
}
