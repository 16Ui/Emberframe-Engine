#include "framebuffer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

struct Point2f
{
    float x;
    float y;
};

struct Vertex3D
{
    Point2f position;
    float depth;
    Color color;
};

struct Triangle3D
{
    std::array<Vertex3D, 3> vertices;
    const char* name;
};

class DepthBuffer
{
public:
    DepthBuffer(int width, int height)
        : width_(width),
          height_(height),
          values_(
              static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
              std::numeric_limits<float>::infinity())
    {
    }

    bool testAndWrite(int x, int y, float depth)
    {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) {
            return false;
        }

        const std::size_t index =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
            static_cast<std::size_t>(x);

        // 本课约定深度越小越靠近观察者。只有更近的片元才能覆盖旧结果。
        if (depth >= values_[index]) {
            return false;
        }

        values_[index] = depth;
        return true;
    }

private:
    int width_;
    int height_;
    std::vector<float> values_;
};

float edgeFunction(Point2f start, Point2f end, Point2f point)
{
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
    DepthBuffer* depthBuffer,
    const Vertex3D& vertex0,
    const Vertex3D& vertex1,
    const Vertex3D& vertex2)
{
    const float signedArea =
        edgeFunction(vertex0.position, vertex1.position, vertex2.position);
    if (std::abs(signedArea) < 0.0001F) {
        return;
    }

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

            const float alpha = weight0 / signedArea;
            const float beta = weight1 / signedArea;
            const float gamma = weight2 / signedArea;
            const float depth =
                alpha * vertex0.depth + beta * vertex1.depth + gamma * vertex2.depth;

            // 深度测试失败表示当前位置已经有更近的三角形，不再覆盖颜色。
            if (depthBuffer != nullptr && !depthBuffer->testAndWrite(x, y, depth)) {
                continue;
            }

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
            // 深度测试坐标来自已裁剪的包围盒，因此写入不会越界。
            (void)framebuffer.setPixel(x, y, color);
        }
    }
}

bool sampleTriangleDepth(
    const Triangle3D& triangle,
    int pixelX,
    int pixelY,
    float& depth)
{
    const auto& vertex0 = triangle.vertices[0];
    const auto& vertex1 = triangle.vertices[1];
    const auto& vertex2 = triangle.vertices[2];
    const float signedArea = edgeFunction(vertex0.position, vertex1.position, vertex2.position);
    if (std::abs(signedArea) < 0.0001F) {
        return false;
    }

    const Point2f sample{
        static_cast<float>(pixelX) + 0.5F,
        static_cast<float>(pixelY) + 0.5F};
    const float weight0 = edgeFunction(vertex1.position, vertex2.position, sample);
    const float weight1 = edgeFunction(vertex2.position, vertex0.position, sample);
    const float weight2 = edgeFunction(vertex0.position, vertex1.position, sample);
    const bool allNonNegative = weight0 >= 0.0F && weight1 >= 0.0F && weight2 >= 0.0F;
    const bool allNonPositive = weight0 <= 0.0F && weight1 <= 0.0F && weight2 <= 0.0F;
    if (!allNonNegative && !allNonPositive) {
        return false;
    }

    const float alpha = weight0 / signedArea;
    const float beta = weight1 / signedArea;
    const float gamma = weight2 / signedArea;
    depth =
        alpha * vertex0.depth + beta * vertex1.depth + gamma * vertex2.depth;
    return true;
}

void printDepthProbe(
    int pixelX,
    int pixelY,
    const Triangle3D& nearTriangle,
    const Triangle3D& farTriangle)
{
    float nearDepth = 0.0F;
    float farDepth = 0.0F;
    const bool insideNear = sampleTriangleDepth(nearTriangle, pixelX, pixelY, nearDepth);
    const bool insideFar = sampleTriangleDepth(farTriangle, pixelX, pixelY, farDepth);
    std::cout << "pixel=(" << pixelX << ',' << pixelY << ")\n";
    std::cout << nearTriangle.name << ": inside=" << (insideNear ? "yes" : "no");
    if (insideNear) {
        std::cout << ", depth=" << nearDepth;
    }
    std::cout << '\n';
    std::cout << farTriangle.name << ": inside=" << (insideFar ? "yes" : "no");
    if (insideFar) {
        std::cout << ", depth=" << farDepth;
    }
    std::cout << '\n';

    if (insideNear && insideFar) {
        std::cout << "winner="
                  << (nearDepth < farDepth ? nearTriangle.name : farTriangle.name)
                  << " (smaller depth)\n";
    } else if (insideNear) {
        std::cout << "winner=" << nearTriangle.name << " (only candidate)\n";
    } else if (insideFar) {
        std::cout << "winner=" << farTriangle.name << " (only candidate)\n";
    } else {
        std::cout << "winner=background (no triangle covers this pixel)\n";
    }
}

int main(int argc, char* argv[])
{
    constexpr int width = 512;
    constexpr int height = 512;
    Framebuffer framebuffer(width, height, Color{20, 24, 32});
    DepthBuffer depthBuffer(width, height);

    const Color nearColor{80, 150, 255};
    const Color farColor{255, 155, 60};
    const Triangle3D nearTriangle{{{
        {{128.0F, 128.0F}, 0.2F, nearColor},
        {{464.0F, 224.0F}, 0.2F, nearColor},
        {{224.0F, 464.0F}, 0.2F, nearColor}}}, "near-blue"};
    const Triangle3D farTriangle{{{
        {{64.0F, 64.0F}, 0.8F, farColor},
        {{448.0F, 96.0F}, 0.8F, farColor},
        {{320.0F, 448.0F}, 0.8F, farColor}}}, "far-orange"};

    bool reverseOrder = false;
    bool disableDepth = false;
    if (argc == 2 && std::string(argv[1]) == "--reverse-order") {
        reverseOrder = true;
    } else if (argc == 2 && std::string(argv[1]) == "--disable-depth") {
        disableDepth = true;
    } else if (argc == 4 && std::string(argv[1]) == "--probe") {
        try {
            printDepthProbe(
                std::stoi(argv[2]),
                std::stoi(argv[3]),
                nearTriangle,
                farTriangle);
        } catch (const std::exception& error) {
            std::cerr << "Invalid probe coordinate: " << error.what() << '\n';
            return 1;
        }
    } else if (argc != 1) {
        std::cerr << "Usage: " << argv[0]
                  << " [--probe x y | --reverse-order | --disable-depth]\n";
        return 1;
    }

    DepthBuffer* activeDepthBuffer = disableDepth ? nullptr : &depthBuffer;
    const auto drawTriangle = [&](const Triangle3D& triangle) {
        rasterizeTriangle(
            framebuffer,
            activeDepthBuffer,
            triangle.vertices[0],
            triangle.vertices[1],
            triangle.vertices[2]);
    };

    // 默认故意先提交近处，再提交远处；反转模式用于证明结果不依赖顺序。
    if (reverseOrder) {
        drawTriangle(farTriangle);
        drawTriangle(nearTriangle);
    } else {
        drawTriangle(nearTriangle);
        drawTriangle(farTriangle);
    }

    const char* outputName = disableDepth
        ? "depth_disabled.ppm"
        : (reverseOrder ? "depth_reverse.ppm" : "depth.ppm");
    const auto outputPath = outputPathBesideExecutable(argv[0], outputName);
    if (!framebuffer.savePpm(outputPath)) {
        std::cerr << "Failed to save depth sample: " << outputPath << '\n';
        return 1;
    }

    std::cout << "Depth sample saved to: " << outputPath << '\n';
    return 0;
}
