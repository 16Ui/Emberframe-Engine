#include "framebuffer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>

struct Point2f
{
    float x;
    float y;
};

struct Vertex2D
{
    Point2f position;
    Color color;
};

float edgeFunction(Point2f start, Point2f end, Point2f point)
{
    // 二维叉积的符号表示 point 位于有向边 start→end 的哪一侧。
    return (end.x - start.x) * (point.y - start.y) -
           (end.y - start.y) * (point.x - start.x);
}

std::uint8_t interpolateChannel(float a, float b, float c)
{
    const float value = std::clamp(a + b + c, 0.0F, 255.0F);
    return static_cast<std::uint8_t>(std::lround(value));
}

void rasterizeTriangle(
    Framebuffer& framebuffer,
    const Vertex2D& vertex0,
    const Vertex2D& vertex1,
    const Vertex2D& vertex2)
{
    const float signedArea =
        edgeFunction(vertex0.position, vertex1.position, vertex2.position);
    if (std::abs(signedArea) < 0.0001F) {
        // 三点共线时面积为零，不存在可以填充的三角形区域。
        return;
    }

    // 包围盒把需要检查的像素限制在三角形附近，而不是扫描整张图片。
    const int minX = std::max(0, static_cast<int>(std::floor(std::min(
        {vertex0.position.x, vertex1.position.x, vertex2.position.x}))));
    const int maxX = std::min(framebuffer.width() - 1, static_cast<int>(std::ceil(std::max(
        {vertex0.position.x, vertex1.position.x, vertex2.position.x}))));
    const int minY = std::max(0, static_cast<int>(std::floor(std::min(
        {vertex0.position.y, vertex1.position.y, vertex2.position.y}))));
    const int maxY = std::min(framebuffer.height() - 1, static_cast<int>(std::ceil(std::max(
        {vertex0.position.y, vertex1.position.y, vertex2.position.y}))));

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            // 像素代表一个小方格，光栅化判断使用方格中心而不是左上角。
            const Point2f samplePoint{
                static_cast<float>(x) + 0.5F,
                static_cast<float>(y) + 0.5F};

            const float weight0 =
                edgeFunction(vertex1.position, vertex2.position, samplePoint);
            const float weight1 =
                edgeFunction(vertex2.position, vertex0.position, samplePoint);
            const float weight2 =
                edgeFunction(vertex0.position, vertex1.position, samplePoint);

            const bool allNonNegative =
                weight0 >= 0.0F && weight1 >= 0.0F && weight2 >= 0.0F;
            const bool allNonPositive =
                weight0 <= 0.0F && weight1 <= 0.0F && weight2 <= 0.0F;
            if (!allNonNegative && !allNonPositive) {
                continue;
            }

            // 除以三角形总面积后，三个权重之和为 1，可以插值顶点属性。
            const float alpha = weight0 / signedArea;
            const float beta = weight1 / signedArea;
            const float gamma = weight2 / signedArea;

            const Color color{
                interpolateChannel(
                    alpha * vertex0.color.red,
                    beta * vertex1.color.red,
                    gamma * vertex2.color.red),
                interpolateChannel(
                    alpha * vertex0.color.green,
                    beta * vertex1.color.green,
                    gamma * vertex2.color.green),
                interpolateChannel(
                    alpha * vertex0.color.blue,
                    beta * vertex1.color.blue,
                    gamma * vertex2.color.blue)};
            // x/y 已由与 Framebuffer 相交后的包围盒限制在合法范围内。
            (void)framebuffer.setPixel(x, y, color);
        }
    }
}

int main(int argc, char* argv[])
{
    (void)argc;

    Framebuffer framebuffer(512, 512, Color{20, 24, 32});

    const Vertex2D top{{256.0F, 40.0F}, {255, 70, 70}};
    const Vertex2D bottomLeft{{48.0F, 464.0F}, {70, 130, 255}};
    const Vertex2D bottomRight{{464.0F, 464.0F}, {70, 255, 130}};
    rasterizeTriangle(framebuffer, top, bottomLeft, bottomRight);

    const auto outputPath = outputPathBesideExecutable(argv[0], "triangle.ppm");
    if (!framebuffer.savePpm(outputPath)) {
        std::cerr << "Failed to save triangle sample: " << outputPath << '\n';
        return 1;
    }

    std::cout << "Triangle sample saved to: " << outputPath << '\n';
    return 0;
}
