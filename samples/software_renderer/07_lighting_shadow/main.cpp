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

struct SurfaceVertex
{
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};

struct SurfaceTriangle
{
    std::array<SurfaceVertex, 3> vertices;
    Color albedo;
    bool checkerboard;
};

struct RasterVertex
{
    Vec2 screen;
    float depth;
    float inverseW;
    Vec3 worldPosition;
    Vec3 normal;
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
        if (!contains(x, y)) {
            return false;
        }
        float& storedDepth = values_[indexOf(x, y)];
        if (depth >= storedDepth) {
            return false;
        }
        storedDepth = depth;
        return true;
    }

    [[nodiscard]] float at(int x, int y) const
    {
        if (!contains(x, y)) {
            return std::numeric_limits<float>::infinity();
        }
        return values_[indexOf(x, y)];
    }

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

private:
    [[nodiscard]] bool contains(int x, int y) const
    {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }

    [[nodiscard]] std::size_t indexOf(int x, int y) const
    {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
               static_cast<std::size_t>(x);
    }

    int width_;
    int height_;
    std::vector<float> values_;
};

float edgeFunction(Vec2 start, Vec2 end, Vec2 point)
{
    return (end.x - start.x) * (point.y - start.y) -
           (end.y - start.y) * (point.x - start.x);
}

RasterVertex projectVertex(
    const SurfaceVertex& vertex,
    const Mat4& viewProjection,
    int width,
    int height)
{
    const Vec4 clip = viewProjection * Vec4{
        vertex.position.x, vertex.position.y, vertex.position.z, 1.0F};
    const float inverseW = 1.0F / clip.w;
    const Vec3 ndc{clip.x * inverseW, clip.y * inverseW, clip.z * inverseW};
    return {
        {(ndc.x * 0.5F + 0.5F) * static_cast<float>(width - 1),
         (1.0F - (ndc.y * 0.5F + 0.5F)) * static_cast<float>(height - 1)},
        ndc.z * 0.5F + 0.5F,
        inverseW,
        vertex.position,
        vertex.normal,
        vertex.uv};
}

std::array<int, 4> triangleBounds(
    const std::array<RasterVertex, 3>& vertices,
    int width,
    int height)
{
    return {
        std::max(0, static_cast<int>(std::floor(std::min(
            {vertices[0].screen.x, vertices[1].screen.x, vertices[2].screen.x})))),
        std::min(width - 1, static_cast<int>(std::ceil(std::max(
            {vertices[0].screen.x, vertices[1].screen.x, vertices[2].screen.x})))),
        std::max(0, static_cast<int>(std::floor(std::min(
            {vertices[0].screen.y, vertices[1].screen.y, vertices[2].screen.y})))),
        std::min(height - 1, static_cast<int>(std::ceil(std::max(
            {vertices[0].screen.y, vertices[1].screen.y, vertices[2].screen.y}))))};
}

bool barycentricAt(
    const std::array<RasterVertex, 3>& vertices,
    float signedArea,
    Vec2 sample,
    float& alpha,
    float& beta,
    float& gamma)
{
    const float weight0 = edgeFunction(vertices[1].screen, vertices[2].screen, sample);
    const float weight1 = edgeFunction(vertices[2].screen, vertices[0].screen, sample);
    const float weight2 = edgeFunction(vertices[0].screen, vertices[1].screen, sample);
    const bool insidePositive = weight0 >= 0.0F && weight1 >= 0.0F && weight2 >= 0.0F;
    const bool insideNegative = weight0 <= 0.0F && weight1 <= 0.0F && weight2 <= 0.0F;
    if (!insidePositive && !insideNegative) {
        return false;
    }
    alpha = weight0 / signedArea;
    beta = weight1 / signedArea;
    gamma = weight2 / signedArea;
    return true;
}

void rasterizeShadowDepth(
    DepthBuffer& shadowMap,
    const SurfaceTriangle& triangle,
    const Mat4& lightViewProjection)
{
    const std::array<RasterVertex, 3> vertices{{
        projectVertex(triangle.vertices[0], lightViewProjection, shadowMap.width(), shadowMap.height()),
        projectVertex(triangle.vertices[1], lightViewProjection, shadowMap.width(), shadowMap.height()),
        projectVertex(triangle.vertices[2], lightViewProjection, shadowMap.width(), shadowMap.height())}};
    const float signedArea = edgeFunction(vertices[0].screen, vertices[1].screen, vertices[2].screen);
    if (std::abs(signedArea) < 0.0001F) {
        return;
    }
    const auto bounds = triangleBounds(vertices, shadowMap.width(), shadowMap.height());
    for (int y = bounds[2]; y <= bounds[3]; ++y) {
        for (int x = bounds[0]; x <= bounds[1]; ++x) {
            float alpha = 0.0F;
            float beta = 0.0F;
            float gamma = 0.0F;
            if (!barycentricAt(
                    vertices,
                    signedArea,
                    {static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F},
                    alpha,
                    beta,
                    gamma)) {
                continue;
            }
            const float depth =
                alpha * vertices[0].depth + beta * vertices[1].depth + gamma * vertices[2].depth;
            (void)shadowMap.testAndWrite(x, y, depth);
        }
    }
}

float shadowVisibility(
    Vec3 worldPosition,
    Vec3 normal,
    Vec3 pointToLight,
    const Mat4& lightViewProjection,
    const DepthBuffer& shadowMap)
{
    const Vec4 lightClip = lightViewProjection * Vec4{
        worldPosition.x, worldPosition.y, worldPosition.z, 1.0F};
    const Vec3 lightNdc{
        lightClip.x / lightClip.w,
        lightClip.y / lightClip.w,
        lightClip.z / lightClip.w};
    const int shadowX = static_cast<int>(
        (lightNdc.x * 0.5F + 0.5F) * static_cast<float>(shadowMap.width() - 1));
    const int shadowY = static_cast<int>(
        (1.0F - (lightNdc.y * 0.5F + 0.5F)) * static_cast<float>(shadowMap.height() - 1));
    const float currentDepth = lightNdc.z * 0.5F + 0.5F;

    if (shadowX < 0 || shadowY < 0 || shadowX >= shadowMap.width() || shadowY >= shadowMap.height()) {
        return 1.0F;
    }

    // 斜向表面使用稍大的偏移，减少有限精度引起的自阴影条纹。
    const float bias = std::max(0.004F * (1.0F - emberframe::software::dot(normal, pointToLight)), 0.0008F);
    return currentDepth - bias > shadowMap.at(shadowX, shadowY) ? 0.28F : 1.0F;
}

std::uint8_t toChannel(float value)
{
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 255.0F)));
}

Color shadeFragment(
    const SurfaceTriangle& triangle,
    Vec3 worldPosition,
    Vec3 normal,
    Vec2 uv,
    Vec3 cameraPosition,
    Vec3 lightPosition,
    const Mat4& lightViewProjection,
    const DepthBuffer& shadowMap)
{
    normal = emberframe::software::normalize(normal);
    const Vec3 pointToLight = emberframe::software::normalize(lightPosition - worldPosition);
    const Vec3 pointToCamera = emberframe::software::normalize(cameraPosition - worldPosition);
    const Vec3 halfVector = emberframe::software::normalize(pointToLight + pointToCamera);

    const float diffuse = std::max(emberframe::software::dot(normal, pointToLight), 0.0F);
    const float specular = std::pow(
        std::max(emberframe::software::dot(normal, halfVector), 0.0F), 32.0F);
    const float visibility = shadowVisibility(
        worldPosition, normal, pointToLight, lightViewProjection, shadowMap);

    float checkerFactor = 1.0F;
    if (triangle.checkerboard) {
        const int tileX = static_cast<int>(std::floor(uv.x * 2.0F));
        const int tileY = static_cast<int>(std::floor(uv.y * 2.0F));
        checkerFactor = (tileX + tileY) % 2 == 0 ? 1.0F : 0.62F;
    }

    constexpr float ambient = 0.16F;
    const float diffuseFactor = ambient + visibility * 0.78F * diffuse;
    const float specularFactor = visibility * 0.32F * specular;
    return {
        toChannel(static_cast<float>(triangle.albedo.red) * checkerFactor * diffuseFactor + 255.0F * specularFactor),
        toChannel(static_cast<float>(triangle.albedo.green) * checkerFactor * diffuseFactor + 255.0F * specularFactor),
        toChannel(static_cast<float>(triangle.albedo.blue) * checkerFactor * diffuseFactor + 255.0F * specularFactor)};
}

void rasterizeLitTriangle(
    Framebuffer& framebuffer,
    DepthBuffer& cameraDepth,
    const SurfaceTriangle& triangle,
    const Mat4& cameraViewProjection,
    Vec3 cameraPosition,
    Vec3 lightPosition,
    const Mat4& lightViewProjection,
    const DepthBuffer& shadowMap)
{
    const Vec3 faceNormal = emberframe::software::normalize(emberframe::software::cross(
        triangle.vertices[1].position - triangle.vertices[0].position,
        triangle.vertices[2].position - triangle.vertices[0].position));
    if (emberframe::software::dot(
            faceNormal,
            emberframe::software::normalize(cameraPosition - triangle.vertices[0].position)) <= 0.0F) {
        return;
    }

    const std::array<RasterVertex, 3> vertices{{
        projectVertex(triangle.vertices[0], cameraViewProjection, framebuffer.width(), framebuffer.height()),
        projectVertex(triangle.vertices[1], cameraViewProjection, framebuffer.width(), framebuffer.height()),
        projectVertex(triangle.vertices[2], cameraViewProjection, framebuffer.width(), framebuffer.height())}};
    const float signedArea = edgeFunction(vertices[0].screen, vertices[1].screen, vertices[2].screen);
    if (std::abs(signedArea) < 0.0001F) {
        return;
    }
    const auto bounds = triangleBounds(vertices, framebuffer.width(), framebuffer.height());

    for (int y = bounds[2]; y <= bounds[3]; ++y) {
        for (int x = bounds[0]; x <= bounds[1]; ++x) {
            float alpha = 0.0F;
            float beta = 0.0F;
            float gamma = 0.0F;
            if (!barycentricAt(
                    vertices,
                    signedArea,
                    {static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F},
                    alpha,
                    beta,
                    gamma)) {
                continue;
            }
            const float depth =
                alpha * vertices[0].depth + beta * vertices[1].depth + gamma * vertices[2].depth;
            if (!cameraDepth.testAndWrite(x, y, depth)) {
                continue;
            }

            const float denominator =
                alpha * vertices[0].inverseW +
                beta * vertices[1].inverseW +
                gamma * vertices[2].inverseW;
            const auto interpolateScalar = [&](float value0, float value1, float value2) {
                return (alpha * value0 * vertices[0].inverseW +
                        beta * value1 * vertices[1].inverseW +
                        gamma * value2 * vertices[2].inverseW) /
                       denominator;
            };
            const Vec3 worldPosition{
                interpolateScalar(vertices[0].worldPosition.x, vertices[1].worldPosition.x, vertices[2].worldPosition.x),
                interpolateScalar(vertices[0].worldPosition.y, vertices[1].worldPosition.y, vertices[2].worldPosition.y),
                interpolateScalar(vertices[0].worldPosition.z, vertices[1].worldPosition.z, vertices[2].worldPosition.z)};
            const Vec3 normal{
                interpolateScalar(vertices[0].normal.x, vertices[1].normal.x, vertices[2].normal.x),
                interpolateScalar(vertices[0].normal.y, vertices[1].normal.y, vertices[2].normal.y),
                interpolateScalar(vertices[0].normal.z, vertices[1].normal.z, vertices[2].normal.z)};
            const Vec2 uv{
                interpolateScalar(vertices[0].uv.x, vertices[1].uv.x, vertices[2].uv.x),
                interpolateScalar(vertices[0].uv.y, vertices[1].uv.y, vertices[2].uv.y)};

            (void)framebuffer.setPixel(
                x,
                y,
                shadeFragment(
                    triangle,
                    worldPosition,
                    normal,
                    uv,
                    cameraPosition,
                    lightPosition,
                    lightViewProjection,
                    shadowMap));
        }
    }
}

void addQuad(
    std::vector<SurfaceTriangle>& scene,
    const std::array<Vec3, 4>& positions,
    Vec3 normal,
    Color albedo,
    bool checkerboard = false)
{
    const std::array<Vec2, 4> uvs{{{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}}};
    scene.push_back({{{
        {positions[0], normal, uvs[0]},
        {positions[1], normal, uvs[1]},
        {positions[2], normal, uvs[2]}}}, albedo, checkerboard});
    scene.push_back({{{
        {positions[0], normal, uvs[0]},
        {positions[2], normal, uvs[2]},
        {positions[3], normal, uvs[3]}}}, albedo, checkerboard});
}

std::vector<SurfaceTriangle> makeScene()
{
    std::vector<SurfaceTriangle> scene;
    addQuad(
        scene,
        {{{-3.5F, -1.0F, -3.5F}, {-3.5F, -1.0F, 3.5F},
          {3.5F, -1.0F, 3.5F}, {3.5F, -1.0F, -3.5F}}},
        {0.0F, 1.0F, 0.0F},
        {185, 190, 200},
        true);

    const std::array<Vec3, 8> p{{
        {-1.0F, -1.0F, -1.0F}, {1.0F, -1.0F, -1.0F},
        {1.0F, 1.0F, -1.0F}, {-1.0F, 1.0F, -1.0F},
        {-1.0F, -1.0F, 1.0F}, {1.0F, -1.0F, 1.0F},
        {1.0F, 1.0F, 1.0F}, {-1.0F, 1.0F, 1.0F}}};
    addQuad(scene, {{p[0], p[3], p[2], p[1]}}, {0.0F, 0.0F, -1.0F}, {85, 145, 235});
    addQuad(scene, {{p[4], p[5], p[6], p[7]}}, {0.0F, 0.0F, 1.0F}, {85, 145, 235});
    addQuad(scene, {{p[0], p[4], p[7], p[3]}}, {-1.0F, 0.0F, 0.0F}, {85, 145, 235});
    addQuad(scene, {{p[1], p[2], p[6], p[5]}}, {1.0F, 0.0F, 0.0F}, {85, 145, 235});
    addQuad(scene, {{p[0], p[1], p[5], p[4]}}, {0.0F, -1.0F, 0.0F}, {85, 145, 235});
    addQuad(scene, {{p[3], p[7], p[6], p[2]}}, {0.0F, 1.0F, 0.0F}, {85, 145, 235});
    return scene;
}

void saveShadowMapPreview(
    const DepthBuffer& shadowMap,
    const std::filesystem::path& outputPath)
{
    Framebuffer preview(shadowMap.width(), shadowMap.height(), Color{20, 24, 32});
    for (int y = 0; y < shadowMap.height(); ++y) {
        for (int x = 0; x < shadowMap.width(); ++x) {
            const float depth = shadowMap.at(x, y);
            if (!std::isfinite(depth)) {
                continue;
            }
            const std::uint8_t value = toChannel((1.0F - depth) * 255.0F);
            (void)preview.setPixel(x, y, {value, value, value});
        }
    }
    if (!preview.savePpm(outputPath)) {
        throw std::runtime_error("Failed to save shadow map preview.");
    }
}

} // namespace

int main(int argc, char* argv[])
{
    (void)argc;

    constexpr int width = 640;
    constexpr int height = 640;
    constexpr int shadowResolution = 512;
    constexpr float pi = 3.1415926535F;
    const auto scene = makeScene();

    const Vec3 lightPosition{-3.5F, 5.0F, 2.5F};
    const Mat4 lightView = emberframe::software::lookAt(
        lightPosition, {0.0F, -0.2F, 0.0F}, {0.0F, 1.0F, 0.0F});
    const Mat4 lightProjection = emberframe::software::orthographic(
        -5.0F, 5.0F, -5.0F, 5.0F, 0.1F, 20.0F);
    const Mat4 lightViewProjection = lightProjection * lightView;

    // Pass 1：只从光源视角保存最近深度，不计算颜色。
    DepthBuffer shadowMap(shadowResolution, shadowResolution);
    for (const auto& triangle : scene) {
        rasterizeShadowDepth(shadowMap, triangle, lightViewProjection);
    }

    const Vec3 cameraPosition{4.2F, 3.1F, 6.0F};
    const Mat4 cameraView = emberframe::software::lookAt(
        cameraPosition, {0.0F, -0.2F, 0.0F}, {0.0F, 1.0F, 0.0F});
    const Mat4 cameraProjection = emberframe::software::perspective(
        55.0F * pi / 180.0F,
        static_cast<float>(width) / static_cast<float>(height),
        0.1F,
        100.0F);

    // Pass 2：从相机视角着色，并查询 Pass 1 的深度判断光是否被遮挡。
    Framebuffer framebuffer(width, height, Color{20, 24, 32});
    DepthBuffer cameraDepth(width, height);
    for (const auto& triangle : scene) {
        rasterizeLitTriangle(
            framebuffer,
            cameraDepth,
            triangle,
            cameraProjection * cameraView,
            cameraPosition,
            lightPosition,
            lightViewProjection,
            shadowMap);
    }

    const auto imagePath = outputPathBesideExecutable(argv[0], "lighting_shadow.ppm");
    const auto shadowPath = outputPathBesideExecutable(argv[0], "shadow_map.ppm");
    if (!framebuffer.savePpm(imagePath)) {
        std::cerr << "Failed to save lighting and shadow sample: " << imagePath << '\n';
        return 1;
    }
    saveShadowMapPreview(shadowMap, shadowPath);

    std::cout << "Lighting and shadow sample saved to: " << imagePath << '\n';
    std::cout << "Shadow map preview saved to: " << shadowPath << '\n';
    return 0;
}
