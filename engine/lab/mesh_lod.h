#pragma once
#include "geometry.h"
#include <optional>

namespace emberframe::lab {
struct QemOptions {
    std::size_t target_triangles=0;
    std::uint32_t max_collapses=4096;
    double max_error=std::numeric_limits<double>::infinity(); // Sum of squared world-plane distances per collapse.
};
enum class QemStopReason { target_reached, topology_or_seams, error_budget, collapse_budget };
struct QemResult {
    Mesh mesh;
    std::size_t input_triangles=0,output_triangles=0,collapses=0;
    std::size_t topology_rejections=0,flip_rejections=0,locked_edge_tests=0;
    double max_collapse_error=0;
    // Conservative bound on ORIGINAL vertex displacement under the collapse
    // correspondence (world/local mesh units), not sqrt(QEM) or a Hausdorff fit.
    double geometric_error=0;
    bool target_reached=false;
    QemStopReason stop_reason=QemStopReason::topology_or_seams;
    std::string reason;
};
// Bounded reference QEM: <=4096 input triangles, exhaustive candidate search,
// no global self-intersection test. Rejects degenerate/nonmanifold or wrongly
// wound input. Locks boundary, material seam and coincident split UV/normal
// vertices; never implicitly welds. Preserves manifold links and rejects
// duplicate faces, flips and changes of face normal beyond 60 degrees.
// Quadrics retain original planes. UV/color use the candidate's projected,
// clamped edge fraction; texture distortion remains possible. Reports a reason
// when constraints prevent reduction; it never substitutes a copy as success.
QemResult simplify_qem(const Mesh& mesh,QemOptions options);

struct LodLevel {
    Mesh mesh;
    std::size_t triangles=0;
    double geometric_error=0; // Accumulated conservative displacement from level 0.
};
struct LodAttempt {
    std::size_t target_triangles=0,achieved_triangles=0,collapses=0;
    bool appended=false;
    QemStopReason stop_reason=QemStopReason::topology_or_seams;
    std::string reason;
};
struct LodChain {
    Aabb bounds;
    std::vector<LodLevel> levels; // Level 0 is full detail; only real reductions are appended.
    std::vector<LodAttempt> attempts;
};
struct LodBuildOptions {
    // Relative to level 0, strictly decreasing, in (0,1), at most eight requests.
    std::vector<float> triangle_ratios{0.5f,0.25f,0.125f};
    std::uint32_t max_collapses_per_level=4096;
    double max_qem_error=std::numeric_limits<double>::infinity();
};
LodChain build_lod_chain(const Mesh& mesh,LodBuildOptions options={});

struct MeshLodBuildReport {
    std::size_t mesh=0; // build_scene_lods 中的源 mesh 编号。
    std::size_t source_triangles=0; // primitive 实际引用的 LOD0 三角形数。
    std::size_t stored_levels=0; // 保存的减面级别数，不包含 LOD0。
    bool reduced=false; // 至少保存一级真实减面时才为 true。
    std::vector<LodAttempt> attempts; // 每个目标的实际达成数、折叠数、停止原因。
    std::string reason; // UI 可直接显示的中文预算/接缝/成功说明。
};
// 原网格保持 LOD0；lods[0] 存 LOD1，仅保存真实减面结果与累计几何误差。
// 有界穷举 QEM 最多接受 4096 三角形。边界/材质/UV/法线接缝锁定可能阻止减面，
// 这种情况和输入超预算均在报告中解释，不把复制网格记为成功。
Mesh build_mesh_lods(Mesh mesh,LodBuildOptions options={},MeshLodBuildReport* report=nullptr);
std::vector<MeshLodBuildReport> build_scene_lods(Scene& scene,
    std::optional<std::size_t> mesh_index=std::nullopt,LodBuildOptions options={});

// Conservative perspective pixel displacement bound, including off-axis extent
// and a matrix-norm upper bound for nonuniform scale/shear. May overestimate.
// An error-expanded bound crossing the near plane returns infinity. Assumes
// square pixels, a perspective Camera and a non-jittered projection.
double projected_lod_error_pixels(double object_error,const Aabb& object_bounds,
                                 const glm::mat4& world,const Camera& camera,int viewport_height);
// Coarsest available level within the pixel budget. Near-plane crossing uses
// level 0. No temporal hysteresis or geomorphing; caller owns transition policy.
std::size_t select_lod(const LodChain& chain,const glm::mat4& world,const Camera& camera,
                       int viewport_height,double max_error_pixels=1);
TestResults test_mesh_lod();
} // namespace emberframe::lab
