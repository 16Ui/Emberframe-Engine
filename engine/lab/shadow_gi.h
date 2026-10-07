#pragma once
#include "types.h"
#include <memory>
#include <span>

namespace emberframe::lab {
class Bvh;

// CPU reference algorithms D19-D23/D26-D30. All colors are linear HDR.
// Surface maps use linear LIGHT depth increasing away from the light, unrelated
// to camera reversed-Z. All returned GI colors already include receiver material.
class MomentSummedArea {
public:
    MomentSummedArea() = default;
    explicit MomentSummedArea(const Image<float>& depth);
    // Half-open rectangle [x0,x1) x [y0,y1), clipped to the image; empty -> zero.
    glm::dvec4 sum(int x0,int y0,int x1,int y1) const;
    glm::dvec4 mean(int x0,int y0,int x1,int y1) const;
    int width() const { return width_; }
    int height() const { return height_; }
private:
    int width_=0,height_=0;
    std::vector<glm::dvec4> prefix_;
};
float cantelli_visibility(glm::dvec2 moments,double receiver_depth,
                         double minimum_variance=1e-7,float bleed_reduction=0);
// Four raw moments -> regularized Hamburger moment problem / Gauss-Radau solve.
// Bias blends toward uniform [0,1] moments. Singular inputs fall back to VSM;
// this is not the quantized 16-bit optimized-moment or Hausdorff MSM variant.
// Algorithm reference: Peters/Klein, https://momentsingraphics.de/I3D2015.html .
float msm_visibility(glm::dvec4 moments,double receiver_depth,
                     double moment_bias=1e-5,double depth_bias=0);
struct BlockerEstimate { float depth=0,fraction=0; bool valid=false; };
// VSSM closure approximates the mean of non-blockers by receiver_depth.
// Multi-modal distributions can still leak; this is an estimate, not an exact CDF.
BlockerEstimate conditional_blocker_mean(glm::dvec2 moments,float receiver_depth);
class MomentShadowMap {
public:
    MomentShadowMap() = default;
    explicit MomentShadowMap(Image<float> depth);
    const Image<float>& depth() const { return depth_; }
    glm::dvec4 moments(glm::vec2 uv,int radius) const;
    glm::dvec4 filtered_moments(glm::vec2 uv,float radius) const;
    float hard(glm::vec2 uv,float receiver_depth) const;
    float pcf(glm::vec2 uv,float receiver_depth,int radius,int sample_count=32) const;
    // Continuous-radius, bilinear comparison filtering; same 64-point disk as GPU PCSS.
    float soft_pcf(glm::vec2 uv,float receiver_depth,float radius) const;
    BlockerEstimate blockers(glm::vec2 uv,float receiver_depth,int radius,int sample_count=64) const;
    // penumbra_scale converts normalized blocker/receiver separation to texels.
    float pcss(glm::vec2 uv,float receiver_depth,int search_radius,float penumbra_scale) const;
    float vssm(glm::vec2 uv,float receiver_depth,int search_radius,float penumbra_scale) const;
private:
    Image<float> depth_;
    MomentSummedArea sat_;
};

struct RsmSample {
    glm::vec3 position{0},normal{0,1,0};
    glm::vec3 flux{0}; // reflected radiant power; includes sample area / MC weight
    float area=0;
};
class ReflectiveShadowMap {
public:
    // Directional: nearest light-orthographic samples. Point: spherical rays.
    // Rectangle: cosine emission rays; size is full width/height, intensity is Le.
    // Constant base material, opaque geometry; texture/alpha masking is not sampled.
    void build(const Scene&,const std::vector<Triangle>&,const Bvh&,const Light&,int resolution=32);
    void set_samples(std::vector<RsmSample> samples);
    const std::vector<RsmSample>& samples() const { return samples_; }
    // Exact sum when max_samples >= sample count. Bounded deterministic subset
    // otherwise. Optional secondary visibility removes classic RSM wall leakage.
    glm::vec3 gather(glm::vec3 position,glm::vec3 normal,glm::vec3 diffuse_albedo,
                     int max_samples=64,float min_distance=0.02f,
                     const std::function<bool(const Ray&)>& occluded={}) const;
private:
    std::vector<RsmSample> samples_;
};

using ShRgb=std::array<glm::vec3,4>; // order: Y00, Y1x, Y1y, Y1z; ray travel direction
class LightPropagationVolume {
public:
    LightPropagationVolume(glm::vec3 minimum,glm::vec3 maximum,int resolution=16);
    void clear();
    void inject(std::span<const RsmSample>);
    // Cell occupancy in x + n*(y+n*z) order. Empty removes geometry blocking.
    void set_occupancy(std::vector<float> occupancy);
    // Ping-pong frontier + accumulated field. Six-neighbor, first-order SH;
    // propagation is attenuated transport, NOT a count of physical bounces.
    void propagate(int iterations,float attenuation=0.8f);
    glm::vec3 irradiance(glm::vec3 position,glm::vec3 normal) const;
    glm::vec3 frontier_energy() const;
    int resolution() const { return resolution_; }
private:
    glm::vec3 minimum_,maximum_,cell_size_;
    int resolution_;
    std::vector<ShRgb> frontier_,accumulated_;
    std::vector<float> occupancy_;
};

struct ConeTraceResult { glm::vec3 radiance{0}; float opacity=0; int steps=0; };
class VoxelRadianceVolume {
public:
    // Cubic bounds; resolution is rounded up to a power of two, clamped [4,64].
    VoxelRadianceVolume(glm::vec3 minimum,glm::vec3 maximum,int resolution=32);
    // Conservative triangle/AABB SAT; overlapping surfaces average their isotropic
    // outgoing radiance. Thin surfaces occupy a whole cell (documented dilation).
    void voxelize(const std::vector<Triangle>&,
        const std::function<glm::vec3(glm::vec3,glm::vec3,int)>& outgoing_radiance);
    void set_voxel(int x,int y,int z,glm::vec3 radiance,float opacity=1);
    void build_mips();
    // Premultiplied radiance RGB + opacity A. Mips scale optical depth to their
    // doubled step length; isotropic opacity cannot preserve directional thin walls.
    glm::vec4 sample(glm::vec3 position,float lod=0) const;
    ConeTraceResult trace(glm::vec3 origin,glm::vec3 direction,float half_angle,
                          float max_distance=100,int max_steps=96) const;
    float cell_size() const { return cell_size_; }
    int resolution() const { return resolution_; }
    int mip_count() const { return int(levels_.size()); }
    glm::vec3 minimum() const { return minimum_; }
    glm::vec3 maximum() const { return maximum_; }
private:
    struct Level { int size=0; std::vector<glm::vec4> cells; };
    glm::vec3 minimum_,maximum_;
    int resolution_;
    float cell_size_;
    std::vector<Level> levels_;
};

using DistanceFunction=std::function<float(glm::vec3)>;
float sdf_sphere(glm::vec3 position,glm::vec3 center,float radius);
float sdf_box(glm::vec3 position,glm::vec3 center,glm::vec3 half_extent);
struct SdfTraceResult {
    // distance is the Ray parameter t; world distance is t * length(direction).
    bool hit=false; float distance=0; glm::vec3 position{0},normal{0,1,0}; int steps=0;
};
// Requires a conservative Lipschitz bound for the supplied field. Supports an
// origin inside a solid by tracing abs(distance) to the exit. No transform is assumed.
SdfTraceResult sphere_trace(const DistanceFunction&,const Ray&,float epsilon=1e-3f,
                            int max_steps=256,float lipschitz_bound=1);
// Empirical penumbra estimate, not an area-light visibility integral.
float sdf_soft_shadow(const DistanceFunction&,const Ray&,float softness=12,
                      float epsilon=1e-3f,int max_steps=128,float lipschitz_bound=1);
class BakedSdfGrid {
public:
    BakedSdfGrid(glm::vec3 minimum,glm::vec3 maximum,int resolution=24);
    void bake(const DistanceFunction&);
    // Exact point/triangle unsigned distance + ray parity sign. Closed, consistently
    // modeled solids only; open/non-manifold/intersecting soups have ambiguous sign.
    // O(resolution^3 * triangle_count), bounded CPU reference, not a fast mesh baker.
    void bake(const std::vector<Triangle>&);
    float sample(glm::vec3 position) const;
    SdfTraceResult trace(const Ray&,float epsilon=1e-3f,int max_steps=512) const;
    float lipschitz_bound() const { return lipschitz_; }
    float geometric_error_bound() const; // sampling bound for an exact 1-Lipschitz SDF
private:
    glm::vec3 minimum_,maximum_,cell_size_;
    int resolution_;
    std::vector<float> values_;
    float lipschitz_=1;
};

struct ScreenTraceSettings {
    float thickness=0.08f,max_distance=20;
    int max_steps=96,refinement_steps=7;
};
struct ScreenSpaceHit {
    bool valid=false; glm::ivec2 pixel{-1}; glm::vec3 position{0};
    float distance=0,confidence=0; int steps=0;
};
// Top-left image origin. Uses world positions and camera-linear depth, so reversed-Z
// storage does not alter tracing. Only first visible layers are represented.
ScreenSpaceHit trace_screen_space(const GBuffer&,const Camera&,const Ray&,
                                  const ScreenTraceSettings& options={});

class ShadowGi {
public:
    // Builds BVH, light maps and selected GI resources ONCE. Reconstruct after scene,
    // settings or camera changes; subsequent queries are const and thread safe.
    ShadowGi(const Scene&,const std::vector<Triangle>&,const Camera&,const Settings&);
    float visibility(const SurfaceSample&,const Light&) const;
    // Final outgoing indirect radiance (includes receiver albedo/BRDF). environment,
    // ssr, ssgi and none return zero: environment is owned by the renderer; screen
    // modes use screen_space_indirect below. RSM/LPV/VCT are distinct implementations.
    glm::vec3 indirect(const SurfaceSample&,glm::vec3 view_direction) const;
    // Directional lights: all shadow dropdown modes incl. 3 frustum CSM cascades.
    // shadow_resolution is bounded [16,512]; light_size is angular radius in radians
    // for directional PCSS/VSSM (clamped [0,.75]), shadow_bias is in world units.
    // Point/rectangle: bounded BVH visibility sampling, not cube moment maps/CSM.
    // RSM/LPV/VCT: one diffuse bounce, opaque base materials, no alpha/texture baking.
    // LPV surface lookup is offset 1.5 cells to escape conservatively occupied cells;
    // this trades near-surface spatial bias for avoiding a falsely black receiver.
    // SDF is an independent query API; no SDF UI mode exists in shared Settings.
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
// Source MUST exclude this invocation's indirect term. Returns a separate image of
// final outgoing radiance: SSR Fresnel-weighted reflection or cosine-sampled SSGI.
// Misses/low-confidence hits blend with environment once. Source view dependence,
// off-screen geometry, hidden layers and temporal denoising remain outside this API.
Image<glm::vec3> screen_space_indirect(const GBuffer&,const Image<glm::vec3>& source,
                                      const Scene&,const Camera&,const Settings&);
TestResults test_shadow_gi();
}
