#pragma once

#include "framebuffer.h"
#include "sr_math.h"

#include <array>
#include <filesystem>
#include <memory>
#include <vector>

namespace emberframe::software {

class SoftwareDepthBuffer;

struct MeshVertex
{
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};

struct MeshTriangle
{
    std::array<MeshVertex, 3> vertices;
    Color albedo {190, 190, 190};
    bool checkerboard {false};
};

struct RenderSettings
{
    Vec3 cameraPosition {4.2F, 3.1F, 6.0F};
    Vec3 cameraTarget {0.0F, -0.2F, 0.0F};
    Vec3 lightPosition {-3.5F, 5.0F, 2.5F};
    Color clearColor {20, 24, 32};
};

struct RenderStats
{
    std::size_t submittedTriangles {0};
    std::size_t visibleTriangles {0};
    std::size_t shadedFragments {0};
    std::size_t shadowTests {0};
};

// A8 把前七课已经验证过的步骤收束为一个可复用 CPU Renderer。
// 它仍是教学实现：优先让数据流清楚，而不是追求并行化和极致性能。
class CpuRenderer
{
public:
    CpuRenderer(int width, int height, int shadowResolution = 512);
    ~CpuRenderer();

    CpuRenderer(const CpuRenderer&) = delete;
    CpuRenderer& operator=(const CpuRenderer&) = delete;

    RenderStats render(
        const std::vector<MeshTriangle>& triangles,
        const RenderSettings& settings);

    [[nodiscard]] const Framebuffer& framebuffer() const noexcept;
    [[nodiscard]] bool saveColor(const std::filesystem::path& outputPath) const;
    [[nodiscard]] bool saveShadowPreview(const std::filesystem::path& outputPath) const;

private:
    int width_;
    int height_;
    Framebuffer framebuffer_;
    std::unique_ptr<SoftwareDepthBuffer> cameraDepth_;
    std::unique_ptr<SoftwareDepthBuffer> shadowMap_;
};

std::vector<MeshTriangle> transformMesh(
    const std::vector<MeshTriangle>& source,
    const Mat4& modelMatrix);

std::vector<MeshTriangle> makeGroundPlane(float halfExtent = 3.5F);

} // namespace emberframe::software
