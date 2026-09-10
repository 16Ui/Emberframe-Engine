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

struct ScreenVertex
{
    Vec2 position;
    float depth;
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

ScreenVertex projectToScreen(Vec3 modelPosition, const Mat4& mvp, int width, int height)
{
    // Projection 输出齐次裁剪坐标。除以 w 后才得到范围约为 [-1, 1] 的 NDC。
    const Vec4 clip = mvp * Vec4{modelPosition.x, modelPosition.y, modelPosition.z, 1.0F};
    const float inverseW = 1.0F / clip.w;
    const Vec3 ndc{clip.x * inverseW, clip.y * inverseW, clip.z * inverseW};

    // Viewport 把 NDC 映射到像素坐标；图片 y 轴向下，所以这里翻转 y。
    return {
        {(ndc.x * 0.5F + 0.5F) * static_cast<float>(width - 1),
         (1.0F - (ndc.y * 0.5F + 0.5F)) * static_cast<float>(height - 1)},
        ndc.z * 0.5F + 0.5F};
}

void rasterizeTriangle(
    Framebuffer& framebuffer,
    DepthBuffer& depthBuffer,
    const std::array<ScreenVertex, 3>& vertices,
    Color color)
{
    const float signedArea =
        edgeFunction(vertices[0].position, vertices[1].position, vertices[2].position);
    if (std::abs(signedArea) < 0.0001F) {
        return;
    }

    const int minX = std::max(0, static_cast<int>(std::floor(std::min(
        {vertices[0].position.x, vertices[1].position.x, vertices[2].position.x}))));
    const int maxX = std::min(framebuffer.width() - 1, static_cast<int>(std::ceil(std::max(
        {vertices[0].position.x, vertices[1].position.x, vertices[2].position.x}))));
    const int minY = std::max(0, static_cast<int>(std::floor(std::min(
        {vertices[0].position.y, vertices[1].position.y, vertices[2].position.y}))));
    const int maxY = std::min(framebuffer.height() - 1, static_cast<int>(std::ceil(std::max(
        {vertices[0].position.y, vertices[1].position.y, vertices[2].position.y}))));

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const Vec2 sample{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F};
            const float weight0 = edgeFunction(vertices[1].position, vertices[2].position, sample);
            const float weight1 = edgeFunction(vertices[2].position, vertices[0].position, sample);
            const float weight2 = edgeFunction(vertices[0].position, vertices[1].position, sample);
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
            if (depthBuffer.testAndWrite(x, y, depth)) {
                (void)framebuffer.setPixel(x, y, color);
            }
        }
    }
}

} // namespace

int main(int argc, char* argv[])
{
    (void)argc;

    constexpr int width = 512;
    constexpr int height = 512;
    constexpr float pi = 3.1415926535F;
    Framebuffer framebuffer(width, height, Color{20, 24, 32});
    DepthBuffer depthBuffer(width, height);

    const std::array<Vec3, 8> positions{{
        {-1.0F, -1.0F, -1.0F}, {1.0F, -1.0F, -1.0F},
        {1.0F, 1.0F, -1.0F}, {-1.0F, 1.0F, -1.0F},
        {-1.0F, -1.0F, 1.0F}, {1.0F, -1.0F, 1.0F},
        {1.0F, 1.0F, 1.0F}, {-1.0F, 1.0F, 1.0F}}};

    // 每一面两个三角形，顶点按从物体外部观察时的逆时针顺序排列。
    const std::array<std::array<int, 3>, 12> triangles{{
        {0, 3, 2}, {0, 2, 1}, {4, 5, 6}, {4, 6, 7},
        {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5},
        {0, 1, 5}, {0, 5, 4}, {3, 7, 6}, {3, 6, 2}}};
    const std::array<Color, 6> faceColors{{
        {235, 88, 88}, {80, 150, 255}, {95, 210, 145},
        {255, 183, 77}, {174, 120, 235}, {70, 205, 220}}};

    const Mat4 model =
        emberframe::software::rotationY(-0.35F) *
        emberframe::software::rotationX(0.18F);
    const Vec3 cameraPosition{3.2F, 2.4F, 4.5F};
    const Mat4 view = emberframe::software::lookAt(
        cameraPosition, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    const Mat4 projection = emberframe::software::perspective(
        60.0F * pi / 180.0F,
        static_cast<float>(width) / static_cast<float>(height),
        0.1F,
        100.0F);
    const Mat4 mvp = projection * view * model;

    for (std::size_t triangleIndex = 0; triangleIndex < triangles.size(); ++triangleIndex) {
        const auto& triangle = triangles[triangleIndex];
        const Vec3 world0 = emberframe::software::transformPoint(model, positions[triangle[0]]);
        const Vec3 world1 = emberframe::software::transformPoint(model, positions[triangle[1]]);
        const Vec3 world2 = emberframe::software::transformPoint(model, positions[triangle[2]]);

        // 法线朝向观察者时点积为正。反面不会贡献可见像素，因此提前剔除。
        const Vec3 faceNormal = emberframe::software::normalize(
            emberframe::software::cross(world1 - world0, world2 - world0));
        const Vec3 towardCamera = emberframe::software::normalize(cameraPosition - world0);
        if (emberframe::software::dot(faceNormal, towardCamera) <= 0.0F) {
            continue;
        }

        const std::array<ScreenVertex, 3> screenVertices{{
            projectToScreen(positions[triangle[0]], mvp, width, height),
            projectToScreen(positions[triangle[1]], mvp, width, height),
            projectToScreen(positions[triangle[2]], mvp, width, height)}};
        rasterizeTriangle(
            framebuffer,
            depthBuffer,
            screenVertices,
            faceColors[(triangleIndex / 2) % faceColors.size()]);
    }

    const auto outputPath = outputPathBesideExecutable(argv[0], "transforms.ppm");
    if (!framebuffer.savePpm(outputPath)) {
        std::cerr << "Failed to save transform sample: " << outputPath << '\n';
        return 1;
    }

    std::cout << "Transform sample saved to: " << outputPath << '\n';
    return 0;
}
