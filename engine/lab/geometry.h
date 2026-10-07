#pragma once
#include "types.h"
#include <cstddef>

namespace emberframe::lab {

// Vulkan clip volume: -w <= x,y <= w, 0 <= z <= w. attributes.position is
// world space; attributes are NOT divided by w. Zero-w apex vertices are legal
// clip results but cannot themselves be rasterized. No extra epsilon plane.
struct ClipVertex {
    glm::vec4 clip{0};
    Vertex attributes;
    glm::vec3 previous_position{0};
};
using ClippedTriangle = std::array<ClipVertex, 3>;
std::vector<ClippedTriangle> clip_triangle(const ClippedTriangle& triangle);

// Parent order is arbitrary. Invalid parent/mesh indices, cycles, non-affine,
// non-finite or singular world transforms throw invalid_argument. An empty
// node list instantiates every mesh once at identity. Empty primitives means
// the entire mesh is one primitive; non-indexed triangle lists are supported.
std::vector<glm::mat4> resolve_world_transforms(const Scene& scene);
std::vector<Triangle> flatten_scene(const Scene& scene);

struct RasterOptions {
    // Offset added to clip.xy as jitter_ndc*clip.w; +Y NDC points up.
    glm::vec2 jitter_ndc{0};
    const Camera* previous_camera=nullptr; // Null uses the current camera.
    // Optional caller-owned sidecars, resized/cleared by rasterize. UV gradients
    // are analytic UV/pixel at the surviving sample, including perspective.
    Image<glm::vec3>* previous_world_positions=nullptr;
    Image<glm::vec2>* uv_dx=nullptr;
    Image<glm::vec2>* uv_dy=nullptr;
};
// Reference rasterizer: Y-down framebuffer, pixel centers, 8 subpixel bits,
// two-sided triangles, no MSAA or alpha blending. Alpha MASK uses filtered
// material alpha and discards before depth testing/writing. Attributes use
// reciprocal-w interpolation; depth is affine z/w. Dimensions <= 2^20.
// Normals use inverse-transpose; reflected transforms flip tangent handedness.
// motion = unjittered current pixel - unjittered previous pixel, using node
// previous_world and options.previous_camera. It includes camera/object motion
// and excludes projection jitter; valid zero is a total zero displacement.
// An unprojectable previous position yields motion=(2*width,2*height), forcing
// temporal history rejection. Previous-frame image extent is assumed unchanged.
// Equal-depth ties keep the first triangle. Clear depth is 0 reversed / 1 normal;
// valid distinguishes the clear value from a surface on the far clip plane.
GBuffer rasterize(const Scene& scene, const Camera& camera, const Settings& settings);
GBuffer rasterize(const Scene& scene, const Camera& camera, const Settings& settings,
                  const RasterOptions& options);

struct Aabb {
    glm::vec3 min{std::numeric_limits<float>::infinity()};
    glm::vec3 max{-std::numeric_limits<float>::infinity()};
    bool empty() const;
    void expand(glm::vec3 point);
    void expand(const Aabb& box);
    glm::vec3 center() const;
    glm::vec3 extent() const;
    double surface_area() const;
    bool contains(glm::vec3 point) const;
    bool contains(const Aabb& box) const;
    bool overlaps(const Aabb& box) const;
    // Closed slabs, including zero-thickness boxes, parallel rays and t endpoints.
    bool intersect(const Ray& ray, double& enter, double& exit) const;
    bool intersect(const Ray& ray) const;
};
struct Sphere { glm::vec3 center{0}; float radius=0; };
// OBB axes must be orthonormal and half_extent nonnegative. A sheared affine
// box is not an OBB; transform_aabb remains conservative for that case.
struct Obb { glm::vec3 center{0},half_extent{0}; glm::mat3 axes{1}; };
Aabb triangle_bounds(const Triangle& triangle);
Aabb transform_aabb(const Aabb& local, const glm::mat4& transform);
Sphere bounding_sphere(const std::vector<Triangle>& triangles); // Conservative, not minimum.
bool intersect_sphere(const Ray& ray, const Sphere& sphere, double& enter, double& exit);
bool intersect_obb(const Ray& ray, const Obb& box, double& enter, double& exit);

// Rays need not be normalized: t is the parameter of origin+t*direction.
// Closed [t_min,t_max], two-sided, double-precision sheared edge tests. Invalid
// rays/degenerate triangles miss. Hit barycentrics follow vertex order (A,B,C).
// Nearest float-distance ties choose the lowest ORIGINAL triangle index.
Hit intersect_triangle(const Ray& ray, const Triangle& triangle, int index=0);
struct TraversalStatistics {
    std::uint64_t box_tests=0,triangle_tests=0,nodes_visited=0;
};
Hit brute_force_intersect(const std::vector<Triangle>& triangles, const Ray& ray,
                          TraversalStatistics* statistics=nullptr);
struct HierarchyStatistics {
    std::size_t nodes=0,leaves=0,primitive_references=0,bytes=0;
    std::uint32_t max_depth=0;
    double build_ms=0,refit_ms=0,sah_cost=0;
    std::size_t refit_nodes=0;
};
enum class BvhSplit { median, binned_sah };
struct BvhBuildOptions {
    BvhSplit split=BvhSplit::binned_sah;
    std::uint32_t leaf_size=4,bin_count=16,max_depth=64;
};
class Bvh {
public:
    explicit Bvh(const std::vector<Triangle>& triangles, BvhBuildOptions options={});
    Hit intersect(const Ray& ray, TraversalStatistics* statistics=nullptr) const;
    bool occluded(const Ray& ray, TraversalStatistics* statistics=nullptr) const;
    // Same primitive count/order, arbitrary motion; preserves topology. Rebuild
    // after major motion if sah_cost degrades. Count mismatch throws.
    void refit(const std::vector<Triangle>& triangles);
    const HierarchyStatistics& statistics() const { return statistics_; }
    const std::vector<Triangle>& triangles() const { return triangles_; }
    Aabb bounds() const;
private:
    struct Node { Aabb box; std::uint32_t first=0,count=0,left=0,right=0; };
    std::vector<Triangle> triangles_;
    std::vector<std::uint32_t> indices_;
    std::vector<Node> nodes_;
    BvhBuildOptions options_;
    HierarchyStatistics statistics_;
    std::uint32_t build(std::uint32_t first,std::uint32_t count,std::uint32_t depth);
    Hit traverse(const Ray& ray,bool any,TraversalStatistics* statistics) const;
    double sah_cost() const;
};

struct OctreeOptions { std::uint32_t leaf_size=8,max_depth=12; };
class Octree {
public:
    explicit Octree(const std::vector<Triangle>& triangles, OctreeOptions options={});
    Hit intersect(const Ray& ray, TraversalStatistics* statistics=nullptr) const;
    bool occluded(const Ray& ray, TraversalStatistics* statistics=nullptr) const;
    // Conservative triangle-AABB overlap candidates, sorted original indices.
    // A triangle lives in one node only: straddlers remain at the parent.
    // Consequently neither region queries nor ray queries repeat triangles.
    std::vector<std::uint32_t> query(const Aabb& region,
                                   TraversalStatistics* statistics=nullptr) const;
    void rebuild(const std::vector<Triangle>& triangles);
    const HierarchyStatistics& statistics() const { return statistics_; }
    const std::vector<Triangle>& triangles() const { return triangles_; }
    Aabb bounds() const;
private:
    struct Node {
        Aabb box;
        std::vector<std::uint32_t> resident;
        std::array<int,8> children{-1,-1,-1,-1,-1,-1,-1,-1};
    };
    std::vector<Triangle> triangles_;
    std::vector<Node> nodes_;
    OctreeOptions options_;
    HierarchyStatistics statistics_;
    int build(const Aabb& box,std::vector<std::uint32_t> ids,std::uint32_t depth);
    Hit traverse(const Ray& ray,bool any,TraversalStatistics* statistics) const;
};


// Optional standalone runner: compile geometry_tests.cpp with
// EMBERFRAME_GEOMETRY_TEST_MAIN; normal integration calls this function.
TestResults test_geometry();

} // namespace emberframe::lab
