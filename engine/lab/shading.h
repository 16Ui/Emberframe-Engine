#pragma once
#include "types.h"
#include <optional>

namespace emberframe::lab {
// Independently implemented equations/reference conventions:
// Heitz VNDF: https://jcgt.org/published/0007/04/01/
// Kulla/Conty: https://fpsunflower.github.io/ckulla/data/s2017_pbs_imageworks_slides_v2.pdf
// Burley: https://disneyanimation.com/publications/physically-based-shading-at-disney/
// LTC: https://eheitzresearch.wordpress.com/415-2/ (no external LUT/code copied).
// All radiance, BRDF values and precomputed data are linear. Directions point away
// from the surface. BRDFs exclude N.L. Roughness is perceptual, alpha=roughness^2.
float srgb_to_linear(float value);
float linear_to_srgb(float value);
// Exposure is a nonnegative linear multiplier; fitted ACES curve, then sRGB encoding.
glm::vec3 tone_map(glm::vec3 color,float exposure);
glm::vec3 environment(const Scene&,glm::vec3 direction);
// 可选 HDR 与解析天空共用此入口；返回线性辐亮度，包含场景环境旋转/强度。

// Repeat addressing. Decode sRGB BEFORE interpolation; alpha is always linear.
// Derivatives are UV/pixel; anisotropic filtering uses the ellipse's principal
// axis, capped at 16 taps (an approximation, not an EWA reconstruction).
glm::vec4 sample_texture(const Texture&,glm::vec2 uv,FilterMode,float lod=0,
                         glm::vec2 dudx={},glm::vec2 dudy={});
// Area-weighted NPOT mip reduction, stored back in the texture's encoding.
void build_mips(Texture&);
// Normal data must be linear; renormalizes tangent-space vectors at each level.
void build_normal_mips(Texture&);
Material sample_material(const Scene&,const SurfaceSample&,const Settings&);
// Analytic screen-space UV/pixel derivatives from RasterOptions sidecars.
// Legacy three-argument calls use a zero footprint (e.g. rays without differentials).
Material sample_material(const Scene&,const SurfaceSample&,const Settings&,glm::vec2 dudx,glm::vec2 dudy);
glm::vec3 sample_normal(const Scene&,const SurfaceSample&,const Settings&);
glm::vec3 sample_normal(const Scene&,const SurfaceSample&,const Settings&,glm::vec2 dudx,glm::vec2 dudy);
inline glm::vec3 sampled_normal(const Scene& scene,const SurfaceSample& surface,const Settings& settings) {
    return sample_normal(scene,surface,settings);
}
// Returns occlusion for INDIRECT light only. It does not darken base color.
float sample_occlusion(const Scene&,const SurfaceSample&,const Settings&);
float sample_occlusion(const Scene&,const SurfaceSample&,const Settings&,glm::vec2 dudx,glm::vec2 dudy);

struct ShadingFrame {
    glm::vec3 t{1,0,0},b{0,1,0},n{0,0,1};
    glm::vec3 local(glm::vec3 w) const;
    glm::vec3 world(glm::vec3 w) const;
};
ShadingFrame shading_frame(glm::vec3 n,glm::vec4 tangent={0,0,0,1});
glm::vec2 hammersley(std::uint32_t index,std::uint32_t count);
struct DirectionSample { glm::vec3 direction{0,0,1}; float pdf=0; bool valid=false; };
DirectionSample sample_uniform_sphere(glm::vec2 u);
DirectionSample sample_uniform_hemisphere(glm::vec2 u);
DirectionSample sample_cosine_hemisphere(glm::vec2 u);
float cosine_hemisphere_pdf(float cosine);
float ggx_distribution(glm::vec3 h,float alpha_x,float alpha_y);
float smith_g1(glm::vec3 w,float alpha_x,float alpha_y);
float smith_g2(glm::vec3 v,glm::vec3 l,float alpha_x,float alpha_y);
glm::vec3 schlick_fresnel(glm::vec3 f0,float cosine);
// Local +Z normal, Heitz visible-normal sampling. Reflections below the surface
// are null events, NOT resampled. pdf is with respect to solid angle on the sphere.
DirectionSample sample_ggx(glm::vec3 v,float alpha_x,float alpha_y,glm::vec2 u);
float ggx_pdf(glm::vec3 v,glm::vec3 l,float alpha_x,float alpha_y);
float mis_balance(float pdf_a,float pdf_b);
float mis_power(float pdf_a,float pdf_b);
float area_to_solid_angle_pdf(float area_pdf,float distance_squared,float light_cosine);

struct EnergyLutOptions { int cosine_resolution=32,roughness_resolution=24,samples=1024; };
struct EnergyLut {
    // x=N.V in [0,1], y=perceptual roughness in [0,1], endpoint grids.
    Image<float> directional;
    std::vector<float> average;
};
EnergyLut precompute_energy_lut(EnergyLutOptions options={});
float directional_albedo(const EnergyLut&,float cosine,float roughness);
float average_albedo(const EnergyLut&,float roughness);
// Default bounded LUT is integrated once at translation-unit startup. Calling
// this explicitly during renderer initialization makes the startup cost visible.
// evaluate_brdf does table lookups only; no per-pixel integration or mutable cache.
const EnergyLut& default_energy_lut();
glm::vec3 kulla_conty(const EnergyLut&,glm::vec3 f0,float nv,float nl,float roughness);
glm::vec3 evaluate_brdf(const Material&,glm::vec3 n,glm::vec3 v,glm::vec3 l,
                        glm::vec4 tangent,const Settings&);
glm::vec3 evaluate_brdf(const Material&,glm::vec3 n,glm::vec3 v,glm::vec3 l,
                        glm::vec4 tangent,const Settings&,const EnergyLut&);
// PBR: correlated Smith GGX + energy-partitioned Lambert. Disney: Burley 2012
// diffuse/GTR1 clearcoat/aniso GGX/sheen, with reciprocal coat attenuation.
// Not the full Disney BSDF: no transmission, subsurface or thin-film model.
// KC is isotropic: deliberately disabled for anisotropic Disney lobes. Disney
// artistic diffuse/sheen and the Blinn mode do not promise white-furnace unity.
struct BrdfSample { glm::vec3 direction{0},value{0}; float pdf=0; bool valid=false; };
// 50/50 cosine + GGX proposal; includes all evaluated lobes in value. Disney
// clearcoat/Blinn use this full-support proposal, not exact lobe importance sampling.
BrdfSample sample_brdf(const Material&,glm::vec3 n,glm::vec3 v,glm::vec4 tangent,
                       const Settings&,glm::vec3 u);
float brdf_pdf(const Material&,glm::vec3 n,glm::vec3 v,glm::vec3 l,
               glm::vec4 tangent,const Settings&);

using RadianceFunction=std::function<glm::vec3(glm::vec3)>;
using SH9=std::array<glm::vec3,9>;
// Orthonormal real SH, world XYZ, ordering: 1,y,z,x,xy,yz,3z²-1,xz,x²-y².
std::array<float,9> sh9_basis(glm::vec3 direction);
SH9 project_sh9(const RadianceFunction&,int samples=2048);
glm::vec3 evaluate_sh9(const SH9&,glm::vec3 direction);
// Irradiance includes the cosine convolution (pi,2pi/3,pi/4), not albedo/pi.
// Raw SH reconstruction may be negative; no hidden clamp that conceals ringing.
glm::vec3 evaluate_sh9_irradiance(const SH9&,glm::vec3 normal);

using VisibilityFunction=std::function<float(glm::vec3 origin,glm::vec3 direction)>;
struct PrtOptions { int samples=1024; float ray_offset=1e-4f; };
struct DiffusePrt { std::array<glm::vec3,9> transfer{}; };
// Stores integral Y_i(w)*visibility*cos(theta) dw, excluding first-surface albedo/pi.
// Callback returns [0,1] visibility; absent callback means unoccluded. Static only.
DiffusePrt bake_diffuse_prt(glm::vec3 position,glm::vec3 normal,
                           const VisibilityFunction& visibility={},PrtOptions options={});
glm::vec3 evaluate_diffuse_prt(const DiffusePrt&,const SH9& environment_sh,glm::vec3 albedo);
struct PrtHit { glm::vec3 position{0},normal{0,1,0},albedo{1}; };
using PrtTraceFunction=std::function<std::optional<PrtHit>(const Ray&)>;
// Diffuse interreflection paths terminate at the environment; max_bounces [0,8]
// counts secondary surfaces. Depth truncation loses energy and is explicit.
DiffusePrt bake_interreflection_prt(glm::vec3 position,glm::vec3 normal,
    const PrtTraceFunction&,int max_bounces=2,PrtOptions options={});
struct GlossyPrt { std::array<std::array<glm::vec3,9>,9> transfer{}; };
// 9x9 incident-radiance -> outgoing-radiance SH matrix. Low order blurs highlights;
// world-space static geometry and fixed material, no interreflection in this API.
GlossyPrt bake_glossy_prt(glm::vec3 position,glm::vec3 normal,glm::vec4 tangent,
    const Material&,const Settings&,const VisibilityFunction& visibility={},
    int outgoing_samples=64,int incoming_samples=128);
SH9 apply_glossy_prt(const GlossyPrt&,const SH9& environment_sh);

struct IblOptions { int width=32,height=16,roughness_levels=6,lut_resolution=24,samples=256; };
struct IblData {
    // Linear lat-long maps, +Y north, phi=atan2(z,x). Diffuse stores irradiance.
    Image<glm::vec3> diffuse;
    std::vector<Image<glm::vec3>> specular;
    Image<glm::vec2> brdf;
    // Scene IBL 在环境坐标中烘焙；查表时统一逆旋转并乘强度，与 GPU 完全同序。
    // 缓存视图只引用不可变预过滤数组，旋转/强度编辑不重新积分或复制纹理。
    float environment_intensity=1,environment_rotation=0;
    std::shared_ptr<const IblData> shared_maps;
};
IblData precompute_ibl(const RadianceFunction&,IblOptions options={});
glm::vec3 sample_ibl_diffuse(const IblData&,glm::vec3 normal);
glm::vec3 sample_ibl_specular(const IblData&,glm::vec3 reflection,float roughness);
glm::vec2 sample_brdf_lut(const IblData&,float nv,float roughness);
// Isotropic split-sum, single-scattering GGX: prefilter(R,r)*(F0*A+B).
// This approximation omits visibility, anisotropy and KC; do not add it twice.
glm::vec3 evaluate_ibl_specular(const IblData&,glm::vec3 n,glm::vec3 v,
                               glm::vec3 f0,float roughness);

struct RectangleLight {
    glm::vec3 center{0,2,0},half_u{1,0,0},half_v{0,0,1},radiance{1};
    bool two_sided=false; // Emitting normal is cross(half_u,half_v).
};
struct LtcOptions { int cosine_resolution=8,roughness_resolution=8,samples=256,iterations=28; };
struct LtcEntry {
    glm::mat3 inverse{1};
    glm::vec2 fresnel_integral{0}; // GGX Schlick split A,B; F0*A+B is total energy.
    float fit_error=0; // Measured squared Hellinger fitting objective.
};
struct LtcLut { Image<LtcEntry> entries; };
// Self-fitted four-parameter LTCs using deterministic numerical GGX fitting, not
// author LUT data. Bounded coarse tables approximate isotropic single-scatter GGX;
// Fresnel uses integrated A/B amplitudes (not a separately fitted colored shape).
// No visibility/soft shadows. Roughness >=0.08, N.V>=0.02 in the fit.
// Lowest roughness/grazing cells can have appreciable shape error; fit_error is
// retained for diagnostics. Tables trade startup work/memory against accuracy.
LtcLut precompute_ltc(LtcOptions options={});
LtcEntry sample_ltc(const LtcLut&,float nv,float roughness);
// Analytic clipped spherical-polygon integral of an LTC. Clips BOTH the physical
// shading hemisphere and transformed cosine hemisphere. Absolute winding is used.
float integrate_ltc_rectangle(const RectangleLight&,glm::vec3 position,
    glm::vec3 normal,glm::vec3 view,const glm::mat3& inverse);
glm::vec3 evaluate_ltc_rectangle(const LtcLut&,const RectangleLight&,glm::vec3 position,
    glm::vec3 normal,glm::vec3 view,glm::vec3 diffuse_albedo,glm::vec3 f0,float roughness);
// Raster/GPU parity: retain calibrated PBR LTC, integrate only its isotropic KC
// addition; Blinn/Disney integrate the selected BRDF with fixed 8x8 Gauss nodes.
// Toon remains analytic Lambert. No visibility; narrow lobes/near lights can be
// undersampled. Anisotropic Disney and Blinn do not receive isotropic GGX KC.
glm::vec3 evaluate_rectangle_light(const LtcLut&,const RectangleLight&,glm::vec3 position,
    glm::vec3 normal,glm::vec3 view,glm::vec4 tangent,const Material&,const Settings&);
// Supply the uploaded energy table explicitly for independent GPU diagnostics.
glm::vec3 evaluate_rectangle_light(const LtcLut&,const RectangleLight&,glm::vec3 position,
    glm::vec3 normal,glm::vec3 view,glm::vec4 tangent,const Material&,const Settings&,const EnergyLut&);
// Independent area-domain numerical reference, no visibility; includes exact BRDF.
glm::vec3 integrate_rectangle_reference(const RectangleLight&,glm::vec3 position,
    glm::vec3 normal,glm::vec3 view,glm::vec4 tangent,const Material&,const Settings&,
    int samples=4096);

TestResults test_shading();
}
