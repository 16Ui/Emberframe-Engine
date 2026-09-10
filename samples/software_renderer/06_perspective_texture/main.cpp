#include "framebuffer.h"
#include "sr_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

using emberframe::software::Mat4;
using emberframe::software::Vec2;
using emberframe::software::Vec3;
using emberframe::software::Vec4;

namespace {

struct Vertex
{
    Vec3 position;
    Vec2 uv;
};

struct RasterVertex
{
    Vec2 screen;
    float depth;
    float inverseW;
    Vec2 uv;
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
        const std::size_t index =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
            static_cast<std::size_t>(x);
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

float edgeFunction(Vec2 start, Vec2 end, Vec2 point)
{
    return (end.x - start.x) * (point.y - start.y) -
           (end.y - start.y) * (point.x - start.x);
}

Color sampleCheckerboard(Vec2 uv)
{
    // UV 表示纹理上的连续坐标；采样函数才把它转换为离散颜色。
    const float u = std::clamp(uv.x, 0.0F, 0.9999F);
    const float v = std::clamp(uv.y, 0.0F, 0.9999F);
    constexpr int tileCount = 10;
    const int tileX = static_cast<int>(std::floor(u * static_cast<float>(tileCount)));
    const int tileY = static_cast<int>(std::floor(v * static_cast<float>(tileCount)));
    if ((tileX + tileY) % 2 == 0) {
        return {238, 238, 224};
    }
    return {45, 105, 190};
}

RasterVertex projectVertex(
    const Vertex& vertex,
    const Mat4& projection,
    int viewportOffsetX,
    int viewportWidth,
    int viewportHeight)
{
    const Vec4 clip = projection * Vec4{
        vertex.position.x, vertex.position.y, vertex.position.z, 1.0F};
    const float inverseW = 1.0F / clip.w;
    const Vec3 ndc{clip.x * inverseW, clip.y * inverseW, clip.z * inverseW};
    return {
        {static_cast<float>(viewportOffsetX) +
             (ndc.x * 0.5F + 0.5F) * static_cast<float>(viewportWidth - 1),
         (1.0F - (ndc.y * 0.5F + 0.5F)) * static_cast<float>(viewportHeight - 1)},
        ndc.z * 0.5F + 0.5F,
        inverseW,
        vertex.uv};
}

void rasterizeTexturedTriangle(
    Framebuffer& framebuffer,
    DepthBuffer& depthBuffer,
    const std::array<RasterVertex, 3>& vertices,
    bool perspectiveCorrect)
{
    const float signedArea = edgeFunction(vertices[0].screen, vertices[1].screen, vertices[2].screen);
    if (std::abs(signedArea) < 0.0001F) {
        return;
    }

    const int minX = std::max(0, static_cast<int>(std::floor(std::min(
        {vertices[0].screen.x, vertices[1].screen.x, vertices[2].screen.x}))));
    const int maxX = std::min(framebuffer.width() - 1, static_cast<int>(std::ceil(std::max(
        {vertices[0].screen.x, vertices[1].screen.x, vertices[2].screen.x}))));
    const int minY = std::max(0, static_cast<int>(std::floor(std::min(
        {vertices[0].screen.y, vertices[1].screen.y, vertices[2].screen.y}))));
    const int maxY = std::min(framebuffer.height() - 1, static_cast<int>(std::ceil(std::max(
        {vertices[0].screen.y, vertices[1].screen.y, vertices[2].screen.y}))));

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const Vec2 sample{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F};
            const float weight0 = edgeFunction(vertices[1].screen, vertices[2].screen, sample);
            const float weight1 = edgeFunction(vertices[2].screen, vertices[0].screen, sample);
            const float weight2 = edgeFunction(vertices[0].screen, vertices[1].screen, sample);
            const bool insidePositive = weight0 >= 0.0F && weight1 >= 0.0F && weight2 >= 0.0F;
            const bool insideNegative = weight0 <= 0.0F && weight1 <= 0.0F && weight2 <= 0.0F;
            if (!insidePositive && !insideNegative) {
                continue;
            }

            const float alpha = weight0 / signedArea;
            const float beta = weight1 / signedArea;
            const float gamma = weight2 / signedArea;
            const float depth =
                alpha * vertices[0].depth + beta * vertices[1].depth + gamma * vertices[2].depth;
            if (!depthBuffer.testAndWrite(x, y, depth)) {
                continue;
            }

            Vec2 uv;
            if (perspectiveCorrect) {
                // 先插值 uv/w 和 1/w，再相除恢复属性。
                // 这样插值发生在投影前的三维关系中，而不是把投影后的屏幕当作平面。
                const float denominator =
                    alpha * vertices[0].inverseW +
                    beta * vertices[1].inverseW +
                    gamma * vertices[2].inverseW;
                uv.x =
                    (alpha * vertices[0].uv.x * vertices[0].inverseW +
                     beta * vertices[1].uv.x * vertices[1].inverseW +
                     gamma * vertices[2].uv.x * vertices[2].inverseW) /
                    denominator;
                uv.y =
                    (alpha * vertices[0].uv.y * vertices[0].inverseW +
                     beta * vertices[1].uv.y * vertices[1].inverseW +
                     gamma * vertices[2].uv.y * vertices[2].inverseW) /
                    denominator;
            } else {
                // 左图故意保留错误方法，作为失败对照。
                uv.x = alpha * vertices[0].uv.x + beta * vertices[1].uv.x + gamma * vertices[2].uv.x;
                uv.y = alpha * vertices[0].uv.y + beta * vertices[1].uv.y + gamma * vertices[2].uv.y;
            }

            (void)framebuffer.setPixel(x, y, sampleCheckerboard(uv));
        }
    }
}

void drawPanel(
    Framebuffer& framebuffer,
    DepthBuffer& depthBuffer,
    const std::array<Vertex, 4>& quad,
    const Mat4& projection,
    int viewportOffsetX,
    int viewportWidth,
    int viewportHeight,
    bool perspectiveCorrect)
{
    const std::array<RasterVertex, 4> projected{{
        projectVertex(quad[0], projection, viewportOffsetX, viewportWidth, viewportHeight),
        projectVertex(quad[1], projection, viewportOffsetX, viewportWidth, viewportHeight),
        projectVertex(quad[2], projection, viewportOffsetX, viewportWidth, viewportHeight),
        projectVertex(quad[3], projection, viewportOffsetX, viewportWidth, viewportHeight)}};
    rasterizeTexturedTriangle(
        framebuffer, depthBuffer, {projected[0], projected[1], projected[2]}, perspectiveCorrect);
    rasterizeTexturedTriangle(
        framebuffer, depthBuffer, {projected[0], projected[2], projected[3]}, perspectiveCorrect);
}

} // namespace

int main(int argc, char* argv[])
{
    (void)argc;

    constexpr int panelWidth = 512;
    constexpr int height = 512;
    constexpr int width = panelWidth * 2;
    constexpr float pi = 3.1415926535F;
    Framebuffer framebuffer(width, height, Color{20, 24, 32});
    DepthBuffer depthBuffer(width, height);

    // 下边离相机近，上边离相机远。不同 w 正是透视插值问题出现的条件。
    const std::array<Vertex, 4> quad{{
        {{-1.15F, -0.9F, -2.0F}, {0.0F, 0.0F}},
        {{1.15F, -0.9F, -2.0F}, {1.0F, 0.0F}},
        {{1.15F, 1.1F, -5.0F}, {1.0F, 1.0F}},
        {{-1.15F, 1.1F, -5.0F}, {0.0F, 1.0F}}}};
    const Mat4 projection = emberframe::software::perspective(
        60.0F * pi / 180.0F, 1.0F, 0.1F, 100.0F);

    // 左侧故意使用仿射插值，右侧使用透视正确插值。
    drawPanel(framebuffer, depthBuffer, quad, projection, 0, panelWidth, height, false);
    drawPanel(framebuffer, depthBuffer, quad, projection, panelWidth, panelWidth, height, true);

    for (int y = 0; y < height; ++y) {
        (void)framebuffer.setPixel(panelWidth - 1, y, Color{235, 88, 88});
        (void)framebuffer.setPixel(panelWidth, y, Color{235, 88, 88});
    }

    const auto outputPath = outputPathBesideExecutable(argv[0], "perspective_texture.ppm");
    if (!framebuffer.savePpm(outputPath)) {
        std::cerr << "Failed to save perspective texture sample: " << outputPath << '\n';
        return 1;
    }

    std::cout << "Perspective texture sample saved to: " << outputPath << '\n';
    std::cout << "Left: affine UV interpolation (wrong). Right: perspective-correct UV interpolation.\n";
    return 0;
}
