#include "cpu_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace emberframe::software {

class SoftwareDepthBuffer
{
public:
    SoftwareDepthBuffer(int width, int height)
        : width_(width),
          height_(height),
          values_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
    {
        if (width <= 0 || height <= 0) {
            throw std::invalid_argument("Depth buffer dimensions must be positive.");
        }
        clear();
    }

    void clear()
    {
        std::fill(values_.begin(), values_.end(), std::numeric_limits<float>::infinity());
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

namespace {

struct RasterVertex
{
    Vec2 screen;
    float depth;
    float inverseW;
    Vec3 worldPosition;
    Vec3 normal;
    Vec2 uv;
};

float edgeFunction(Vec2 start, Vec2 end, Vec2 point)
{
    return (end.x - start.x) * (point.y - start.y) -
           (end.y - start.y) * (point.x - start.x);
}

RasterVertex projectVertex(
    const MeshVertex& vertex,
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
    SoftwareDepthBuffer& shadowMap,
    const MeshTriangle& triangle,
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

float visibilityFromShadowMap(
    Vec3 worldPosition,
    Vec3 normal,
    Vec3 pointToLight,
    const Mat4& lightViewProjection,
    const SoftwareDepthBuffer& shadowMap)
{
    const Vec4 lightClip = lightViewProjection * Vec4{
        worldPosition.x, worldPosition.y, worldPosition.z, 1.0F};
    const Vec3 lightNdc{
        lightClip.x / lightClip.w,
        lightClip.y / lightClip.w,
        lightClip.z / lightClip.w};
    const int x = static_cast<int>(
        (lightNdc.x * 0.5F + 0.5F) * static_cast<float>(shadowMap.width() - 1));
    const int y = static_cast<int>(
        (1.0F - (lightNdc.y * 0.5F + 0.5F)) * static_cast<float>(shadowMap.height() - 1));
    if (x < 0 || y < 0 || x >= shadowMap.width() || y >= shadowMap.height()) {
        return 1.0F;
    }

    const float currentDepth = lightNdc.z * 0.5F + 0.5F;
    const float bias = std::max(0.004F * (1.0F - dot(normal, pointToLight)), 0.0008F);
    return currentDepth - bias > shadowMap.at(x, y) ? 0.28F : 1.0F;
}

std::uint8_t toChannel(float value)
{
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 255.0F)));
}

float wrappedChecker(Vec2 uv)
{
    const int tileX = static_cast<int>(std::floor(uv.x * 4.0F));
    const int tileY = static_cast<int>(std::floor(uv.y * 4.0F));
    return (std::abs(tileX + tileY) % 2) == 0 ? 1.0F : 0.58F;
}

Color shade(
    const MeshTriangle& triangle,
    Vec3 worldPosition,
    Vec3 normal,
    Vec2 uv,
    const RenderSettings& settings,
    const Mat4& lightViewProjection,
    const SoftwareDepthBuffer& shadowMap,
    RenderStats& stats)
{
    normal = normalize(normal);
    const Vec3 pointToLight = normalize(settings.lightPosition - worldPosition);
    const Vec3 pointToCamera = normalize(settings.cameraPosition - worldPosition);
    const Vec3 halfVector = normalize(pointToLight + pointToCamera);
    const float diffuse = std::max(dot(normal, pointToLight), 0.0F);
    const float specular = std::pow(std::max(dot(normal, halfVector), 0.0F), 32.0F);
    const float visibility = visibilityFromShadowMap(
        worldPosition, normal, pointToLight, lightViewProjection, shadowMap);
    ++stats.shadowTests;

    const float textureFactor = triangle.checkerboard ? wrappedChecker(uv) : 1.0F;
    constexpr float ambient = 0.16F;
    const float diffuseFactor = ambient + visibility * 0.78F * diffuse;
    const float specularFactor = visibility * 0.32F * specular;
    return {
        toChannel(static_cast<float>(triangle.albedo.red) * textureFactor * diffuseFactor + 255.0F * specularFactor),
        toChannel(static_cast<float>(triangle.albedo.green) * textureFactor * diffuseFactor + 255.0F * specularFactor),
        toChannel(static_cast<float>(triangle.albedo.blue) * textureFactor * diffuseFactor + 255.0F * specularFactor)};
}

void rasterizeColor(
    Framebuffer& framebuffer,
    SoftwareDepthBuffer& cameraDepth,
    const MeshTriangle& triangle,
    const Mat4& cameraViewProjection,
    const RenderSettings& settings,
    const Mat4& lightViewProjection,
    const SoftwareDepthBuffer& shadowMap,
    RenderStats& stats)
{
    const Vec3 faceNormal = normalize(cross(
        triangle.vertices[1].position - triangle.vertices[0].position,
        triangle.vertices[2].position - triangle.vertices[0].position));
    if (dot(faceNormal, normalize(settings.cameraPosition - triangle.vertices[0].position)) <= 0.0F) {
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
    ++stats.visibleTriangles;
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
            const auto interpolate = [&](float value0, float value1, float value2) {
                return (alpha * value0 * vertices[0].inverseW +
                        beta * value1 * vertices[1].inverseW +
                        gamma * value2 * vertices[2].inverseW) /
                       denominator;
            };
            const Vec3 worldPosition{
                interpolate(vertices[0].worldPosition.x, vertices[1].worldPosition.x, vertices[2].worldPosition.x),
                interpolate(vertices[0].worldPosition.y, vertices[1].worldPosition.y, vertices[2].worldPosition.y),
                interpolate(vertices[0].worldPosition.z, vertices[1].worldPosition.z, vertices[2].worldPosition.z)};
            const Vec3 normal{
                interpolate(vertices[0].normal.x, vertices[1].normal.x, vertices[2].normal.x),
                interpolate(vertices[0].normal.y, vertices[1].normal.y, vertices[2].normal.y),
                interpolate(vertices[0].normal.z, vertices[1].normal.z, vertices[2].normal.z)};
            const Vec2 uv{
                interpolate(vertices[0].uv.x, vertices[1].uv.x, vertices[2].uv.x),
                interpolate(vertices[0].uv.y, vertices[1].uv.y, vertices[2].uv.y)};
            (void)framebuffer.setPixel(
                x,
                y,
                shade(
                    triangle,
                    worldPosition,
                    normal,
                    uv,
                    settings,
                    lightViewProjection,
                    shadowMap,
                    stats));
            ++stats.shadedFragments;
        }
    }
}

} // namespace

CpuRenderer::CpuRenderer(int width, int height, int shadowResolution)
    : width_(width),
      height_(height),
      framebuffer_(width, height),
      cameraDepth_(std::make_unique<SoftwareDepthBuffer>(width, height)),
      shadowMap_(std::make_unique<SoftwareDepthBuffer>(shadowResolution, shadowResolution))
{
}

CpuRenderer::~CpuRenderer() = default;

RenderStats CpuRenderer::render(
    const std::vector<MeshTriangle>& triangles,
    const RenderSettings& settings)
{
    constexpr float pi = 3.1415926535F;
    framebuffer_.clear(settings.clearColor);
    cameraDepth_->clear();
    shadowMap_->clear();

    const Mat4 lightView = lookAt(
        settings.lightPosition, {0.0F, -0.2F, 0.0F}, {0.0F, 1.0F, 0.0F});
    const Mat4 lightViewProjection =
        orthographic(-5.0F, 5.0F, -5.0F, 5.0F, 0.1F, 20.0F) * lightView;
    for (const auto& triangle : triangles) {
        rasterizeShadowDepth(*shadowMap_, triangle, lightViewProjection);
    }

    const Mat4 cameraView = lookAt(
        settings.cameraPosition, settings.cameraTarget, {0.0F, 1.0F, 0.0F});
    const Mat4 cameraViewProjection =
        perspective(
            55.0F * pi / 180.0F,
            static_cast<float>(width_) / static_cast<float>(height_),
            0.1F,
            100.0F) *
        cameraView;

    RenderStats stats;
    stats.submittedTriangles = triangles.size();
    for (const auto& triangle : triangles) {
        rasterizeColor(
            framebuffer_,
            *cameraDepth_,
            triangle,
            cameraViewProjection,
            settings,
            lightViewProjection,
            *shadowMap_,
            stats);
    }
    return stats;
}

const Framebuffer& CpuRenderer::framebuffer() const noexcept
{
    return framebuffer_;
}

bool CpuRenderer::saveColor(const std::filesystem::path& outputPath) const
{
    return framebuffer_.savePpm(outputPath);
}

bool CpuRenderer::saveShadowPreview(const std::filesystem::path& outputPath) const
{
    Framebuffer preview(shadowMap_->width(), shadowMap_->height(), Color{20, 24, 32});
    for (int y = 0; y < shadowMap_->height(); ++y) {
        for (int x = 0; x < shadowMap_->width(); ++x) {
            const float depth = shadowMap_->at(x, y);
            if (!std::isfinite(depth)) {
                continue;
            }
            const std::uint8_t value = toChannel((1.0F - depth) * 255.0F);
            (void)preview.setPixel(x, y, {value, value, value});
        }
    }
    return preview.savePpm(outputPath);
}

std::vector<MeshTriangle> transformMesh(
    const std::vector<MeshTriangle>& source,
    const Mat4& modelMatrix)
{
    std::vector<MeshTriangle> transformed = source;
    for (auto& triangle : transformed) {
        for (auto& vertex : triangle.vertices) {
            vertex.position = transformPoint(modelMatrix, vertex.position);
            // A8 只使用旋转矩阵；若加入非均匀缩放，法线应改用逆转置矩阵。
            vertex.normal = normalize(transformDirection(modelMatrix, vertex.normal));
        }
    }
    return transformed;
}

std::vector<MeshTriangle> makeGroundPlane(float halfExtent)
{
    const Vec3 normal{0.0F, 1.0F, 0.0F};
    const std::array<MeshVertex, 4> vertices{{
        {{-halfExtent, -1.0F, -halfExtent}, normal, {0.0F, 0.0F}},
        {{-halfExtent, -1.0F, halfExtent}, normal, {0.0F, 4.0F}},
        {{halfExtent, -1.0F, halfExtent}, normal, {4.0F, 4.0F}},
        {{halfExtent, -1.0F, -halfExtent}, normal, {4.0F, 0.0F}}}};
    return {
        {{vertices[0], vertices[1], vertices[2]}, {185, 190, 200}, true},
        {{vertices[0], vertices[2], vertices[3]}, {185, 190, 200}, true}};
}

} // namespace emberframe::software
