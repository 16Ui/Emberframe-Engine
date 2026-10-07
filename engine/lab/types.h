#pragma once
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // Vendored GLM anonymous union members only.
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace emberframe::lab {
constexpr float pi = 3.14159265358979323846f;
inline glm::vec3 safe_normalize(glm::vec3 v, glm::vec3 fallback={0,1,0}) {
    const float q=glm::dot(v,v); return q>1e-20f ? v/std::sqrt(q) : fallback;
}
template<class T> struct Image {
    int width=0,height=0;
    std::vector<T> pixels;
    Image()=default;
    Image(int w,int h,T value={}) { reset(w,h,value); }
    void reset(int w,int h,T value={}) {
        if(w<0||h<0||std::uint64_t(w)*std::uint64_t(h)>268435456ull) throw std::invalid_argument("Invalid image size");
        width=w;height=h;pixels.assign(std::size_t(w)*h,value);
    }
    T& at(int x,int y) { return pixels.at(std::size_t(y)*width+x); }
    const T& at(int x,int y) const { return pixels.at(std::size_t(y)*width+x); }
    const T& clamped(int x,int y) const { return at(std::clamp(x,0,width-1),std::clamp(y,0,height-1)); }
    bool empty() const { return width==0||height==0; }
};
struct Vertex {
    glm::vec3 position{0}, normal{0,1,0};
    glm::vec2 uv{0};
    glm::vec4 tangent{1,0,0,1}, color{1};
    // 场景 PRT 的世界空间辐照度；这是烘焙结果，不替代顶点颜色/材质。
    glm::vec3 baked_irradiance{0};
};
struct Texture {
    std::string name;
    // 原始归一化通道；sample_texture 根据 srgb 标志对 RGB 解码，alpha 始终线性。
    bool srgb=false;
    std::vector<Image<glm::vec4>> levels;
    // glTF 采样器枚举保留标准数值；追加字段不改变 name/srgb/levels 的聚合初始化顺序。
    enum class Wrap:int { repeat=10497,clamp_to_edge=33071,mirrored_repeat=33648 };
    enum class Filter:int { nearest=9728,linear=9729,nearest_mipmap_nearest=9984,
                            linear_mipmap_nearest=9985,nearest_mipmap_linear=9986,linear_mipmap_linear=9987 };
    Wrap wrap_s=Wrap::repeat,wrap_t=Wrap::repeat;
    Filter min_filter=Filter::linear_mipmap_linear,mag_filter=Filter::linear;
};
struct Material {
    std::string name="Material";
    glm::vec4 base_color{0.7f,0.7f,0.7f,1};
    glm::vec3 emissive{0};
    float metallic=0,roughness=0.5f,normal_scale=1,ao_strength=1;
    float clearcoat=0,clearcoat_roughness=0.15f,anisotropy=0,sheen=0;
    int base_texture=-1,mr_texture=-1,normal_texture=-1,ao_texture=-1,emissive_texture=-1;
    int alpha_mode=0; // 0 opaque, 1 mask, 2 blend
    float alpha_cutoff=0.5f;
    bool double_sided=false;
};
struct Primitive { std::uint32_t first_index=0,index_count=0,material=0; };
struct MeshLod {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Primitive> primitives;
    double geometric_error=0;
};
struct Mesh {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Primitive> primitives;
    // LOD0 始终是上面的原网格；派生级别独立保存，避免简化覆盖原始资源。
    std::vector<MeshLod> lods;
};
struct Node {
    std::string name;
    int parent=-1,mesh=-1;
    glm::mat4 local{1},previous_world{1};
};
enum class LightKind:int { directional,point,rectangle };
struct Light {
    LightKind kind=LightKind::directional;
    glm::vec3 position{3,5,2},direction{-0.4f,-1,-0.3f},color{1};
    float intensity=3,range=15;
    glm::vec2 size{2,2};
    // Editor-only ownership of a rectangle emitter; GPU light records stay explicit.
    int linked_node=-1;
};
struct SceneBakeResources;
struct Scene {
    std::string name="Material studio";
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Texture> textures;
    std::vector<Node> nodes;
    std::vector<Light> lights;
    glm::vec3 sky_top{0.25f,0.35f,0.55f},sky_bottom{0.03f,0.03f,0.03f};
    std::uint64_t revision=1;
    // 编辑器选择性启用：网格/索引/材质/纹理/拓扑变更必须更新这个版本；纯变换与灯光不必。
    // 0 表示未提供该契约，渲染器仍按 revision 走完整、安全的资源上传。此字段不持久化。
    std::uint64_t asset_revision=0;
    // 不可变场景烘焙快照。几何/变换改变时必须核对指纹，不能只看 revision。
    std::shared_ptr<const SceneBakeResources> baked_resources;
};
struct Camera {
    glm::vec3 position{5,3,6},target{0,0.5f,0};
    float fov=50,near_plane=0.05f,far_plane=100;
    glm::mat4 view() const { return glm::lookAtRH(position,target,glm::vec3(0,1,0)); }
    glm::mat4 projection(float aspect,bool reversed=false) const {
        if(aspect<=0||near_plane<=0||far_plane<=near_plane) throw std::invalid_argument("Invalid camera");
        glm::mat4 p(0);const float f=1/std::tan(glm::radians(fov)*0.5f);
        p[0][0]=f/aspect;p[1][1]=f;p[2][3]=-1;
        if(reversed){p[2][2]=near_plane/(far_plane-near_plane);p[3][2]=far_plane*near_plane/(far_plane-near_plane);}
        else{p[2][2]=far_plane/(near_plane-far_plane);p[3][2]=far_plane*near_plane/(near_plane-far_plane);}
        return p;
    }
};
enum class RenderPath:int { forward,deferred,cpu_raster,path_trace };
enum class ShadowMode:int { hard,pcf,pcss,vsm,vssm,msm,csm };
enum class GiMode:int { environment,ssr,ssgi,rsm,lpv,voxel,none };
enum class AoMode:int { none,ssao,gtao };
enum class ShadingMode:int { pbr,blinn_phong,disney,toon };
enum class LightCulling:int { all,tiled,clustered };
enum class FilterMode:int { nearest,bilinear,trilinear,anisotropic };
enum class DebugView:int { final_color,albedo,normal,depth,roughness,metallic,ao,shadow,indirect,motion,variance,light_count };
enum class EnvironmentDiffuse:int { ibl,sh,prt };
enum class SpatialStructure:int { bvh,octree };
struct Settings {
    RenderPath path=RenderPath::forward;
    ShadowMode shadows=ShadowMode::pcf;
    GiMode gi=GiMode::environment;
    AoMode ao=AoMode::none;
    ShadingMode shading=ShadingMode::pbr;
    LightCulling culling=LightCulling::all;
    FilterMode filter=FilterMode::trilinear;
    DebugView debug=DebugView::final_color;
    EnvironmentDiffuse environment_diffuse=EnvironmentDiffuse::ibl;
    SpatialStructure spatial_structure=SpatialStructure::bvh;
    bool sdf_shadows=false,auto_lod=false,sparse_voxels=true;
    float lod_error_pixels=1,sdf_softness=12;
    int bake_samples=64,sdf_resolution=16;
    bool reversed_z=true,energy_compensation=true,bloom=true,taa=false,denoise=false,svgf=false,outline=false,hatching=false;
    float exposure=1,bloom_strength=0.08f,bloom_threshold=1,ao_radius=0.5f,ao_strength=1;
    float shadow_bias=0.002f,light_size=0.15f,temporal_weight=0.1f;
    int samples=8,max_bounces=5,shadow_resolution=256,voxel_resolution=32,propagation_steps=5;
    int render_width=640,render_height=360;
    std::uint32_t seed=42;
};
struct SurfaceSample {
    glm::vec3 position{0},normal{0,1,0},albedo{0};
    glm::vec2 uv{0},motion{0};
    glm::vec4 tangent{1,0,0,1},vertex_color{1};
    glm::vec3 baked_irradiance{0};
    float depth=1,linear_depth=0,roughness=0.5f,metallic=0;
    int material=-1,object=-1;
    bool valid=false;
};
using GBuffer=Image<SurfaceSample>;
struct RenderOutput {
    Image<glm::vec3> color,indirect;
    Image<float> ao,shadow,variance;
    GBuffer surfaces;
    std::uint64_t triangles=0,rays=0,node_tests=0;
    double cpu_ms=0,gpu_ms=0;
};
struct Triangle {
    std::array<Vertex,3> vertices;
    int material=0,object=0;
};
struct Ray { glm::vec3 origin{0},direction{0,0,-1}; float t_min=1e-4f,t_max=1e30f; };
struct Hit { float t=1e30f; glm::vec3 barycentric{0}; int triangle=-1; };
struct TestResult { std::string name; bool passed=false; std::string detail; };
using TestResults=std::vector<TestResult>;
}

