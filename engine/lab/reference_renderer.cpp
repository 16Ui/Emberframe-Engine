#include "reference_renderer.h"
#include "geometry.h"
#include "shading.h"
#include "shadow_gi.h"
#include "postprocess.h"
#include "systems.h"
#include "scene_resources.h"
#include "scene_geometry.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#ifdef EMBERFRAME_REFERENCE_TEST_MAIN
#include <iostream>
#endif

namespace emberframe::lab {
namespace {
struct Random {
    std::uint32_t state;
    float next() { state=state*747796405u+2891336453u;std::uint32_t v=((state>>((state>>28u)+4u))^state)*277803737u;v=(v>>22u)^v;return float(v>>8)*(1.0f/16777216.0f); }
};
void basis(glm::vec3 n,glm::vec3& t,glm::vec3& b) {
    // 矩形的宽高轴必须与 ShadowGi::Basis 使用同一阈值；否则接近竖直的
    // 非正方形灯会出现 LTC 发光面与可见率采样面旋转约 90 度的错配。
    t=safe_normalize(glm::cross(std::abs(n.y)<0.95f?glm::vec3(0,1,0):glm::vec3(1,0,0),n));
    b=glm::cross(n,t);
}
bool finite(glm::vec3 x) {return std::isfinite(x.x)&&std::isfinite(x.y)&&std::isfinite(x.z);}
bool finite(glm::vec2 x) {return std::isfinite(x.x)&&std::isfinite(x.y);}
bool finite(const glm::mat4& m) {
    for(int c=0;c<4;++c)for(int r=0;r<4;++r)if(!std::isfinite(m[c][r]))return false;
    return true;
}
void validate_camera(const Camera& c) {
    if(!finite(c.position)||!finite(c.target)||!std::isfinite(c.fov)||c.fov<=0||c.fov>=179.9f||
       !std::isfinite(c.near_plane)||!std::isfinite(c.far_plane)||c.near_plane<=0||c.far_plane<=c.near_plane||
       glm::length(c.target-c.position)<1e-8f||glm::length(glm::cross(c.target-c.position,glm::vec3(0,1,0)))<1e-8f)
        throw std::invalid_argument("Invalid reference camera");
    if(!finite(c.view()))throw std::invalid_argument("Degenerate reference camera");
}
void validate_inputs(const Scene& scene,const Camera& camera,const Settings& s,const ReferenceFrameInput& frame) {
    if(s.render_width<1||s.render_height<1||s.render_width>(1<<20)||s.render_height>(1<<20)||
       std::uint64_t(s.render_width)*std::uint64_t(s.render_height)>16ull*1024*1024)
        throw std::invalid_argument("Reference image must contain 1..16M pixels");
    if(s.samples<1||s.samples>65536||s.max_bounces<1||s.max_bounces>64)
        throw std::invalid_argument("Reference sample/bounce budget exceeded");
    validate_camera(camera);if(frame.previous_camera)validate_camera(*frame.previous_camera);
    if(!finite(frame.jitter_ndc)||std::abs(frame.jitter_ndc.x)>1||std::abs(frame.jitter_ndc.y)>1)
        throw std::invalid_argument("Reference jitter must be finite NDC in [-1,1]");
    if(!finite(scene.sky_top)||!finite(scene.sky_bottom))throw std::invalid_argument("Non-finite environment");
    for(const auto& light:scene.lights) {
        if(!finite(light.position)||!finite(light.direction)||!finite(light.color)||!std::isfinite(light.intensity)||
           light.intensity<0||glm::any(glm::lessThan(light.color,glm::vec3(0)))||!std::isfinite(light.range)||light.range<0||
           !finite(light.size)||(light.kind==LightKind::rectangle&&(light.size.x<=0||light.size.y<=0))||
           (light.kind!=LightKind::point&&glm::length(light.direction)<1e-8f))
            throw std::invalid_argument("Invalid reference light");
    }
}
struct EnvironmentCacheEntry {
    glm::vec3 top,bottom;std::uint64_t revision;
    std::shared_ptr<const IblData> data;
};
std::shared_ptr<const IblData> reference_ibl(const Scene& scene) {
    static std::mutex mutex;
    static std::vector<EnvironmentCacheEntry> entries;
    std::lock_guard lock(mutex);
    for(const auto& entry:entries)
        if(entry.revision==scene.revision&&entry.top==scene.sky_top&&entry.bottom==scene.sky_bottom)return entry.data;
    // 不以 Scene 地址或 revision 单独作键，避免不同场景同版本、地址复用时误取旧环境。
    Scene sky;sky.sky_top=scene.sky_top;sky.sky_bottom=scene.sky_bottom;
    IblOptions options;options.width=64;options.height=32;options.roughness_levels=7;options.lut_resolution=48;options.samples=512;
    auto data=std::make_shared<const IblData>(precompute_ibl([&](glm::vec3 d){return environment(sky,d);},options));
    if(entries.size()==4)entries.erase(entries.begin());
    entries.push_back({scene.sky_top,scene.sky_bottom,scene.revision,data});return data;
}
const LtcLut& reference_ltc() {
    // C++ 局部静态初始化有同步保证；只有第一次需要拟合，随后无锁只读。
    static const LtcLut lut=precompute_ltc({12,12,512,40});return lut;
}
glm::vec3 ibl_lighting(const IblData& ibl,const Material& material,const SurfaceSample& surface,
                       glm::vec3 view,const Settings& settings,const SceneBakeResources* resources=nullptr) {
    const auto base=glm::clamp(glm::vec3(material.base_color),glm::vec3(0),glm::vec3(1));
    glm::vec3 irradiance;
    // 三种环境漫反射互斥，只替换 diffuse；下面的镜面始终保留预过滤 IBL。
    if(settings.environment_diffuse==EnvironmentDiffuse::ibl)irradiance=sample_ibl_diffuse(ibl,surface.normal);
    else if(settings.environment_diffuse==EnvironmentDiffuse::prt)irradiance=surface.baked_irradiance;
    else {
        if(!resources)throw std::invalid_argument("Missing scene SH resource");
        irradiance=evaluate_environment_sh(resources->environment_sh9,surface.normal);
    }
    if(settings.shading==ShadingMode::toon)return irradiance*base/pi;
    const float metal=std::clamp(material.metallic,0.f,1.f);
    const auto f0=glm::mix(glm::vec3(.04f),base,metal);
    // split-sum 与漫反射分别读取预卷积数据；Fresnel 分配采用视角近似，
    // 并非直接积分完整 Disney/KC 模型，限制在公开 API 中说明。
    const auto fresnel=schlick_fresnel(f0,std::max(0.f,glm::dot(surface.normal,view)));
    const auto diffuse=base*(1-metal)*(glm::vec3(1)-fresnel)*irradiance/pi;
    return diffuse+evaluate_ibl_specular(ibl,surface.normal,view,f0,material.roughness);
}
SurfaceSample surface_at(const Triangle& tri,const Hit& hit) {
    SurfaceSample s;s.valid=true;s.material=tri.material;s.object=tri.object;
    glm::vec3 normal(0);glm::vec4 tangent(0);s.position=glm::vec3(0);s.uv=glm::vec2(0);s.vertex_color=glm::vec4(0);
    for(int j=0;j<3;++j) {
        const auto& v=tri.vertices[j];const float w=hit.barycentric[j];
        s.position+=w*v.position;normal+=w*v.normal;s.uv+=w*v.uv;
        tangent+=w*v.tangent;s.vertex_color+=w*v.color;
        // 射线命中的顶点照度与光栅路径使用同一三角形重心属性。
        s.baked_irradiance+=w*v.baked_irradiance;
    }
    s.normal=safe_normalize(normal);s.tangent=glm::vec4(safe_normalize(glm::vec3(tangent)),tangent.w<0?-1.f:1.f);
    return s;
}
Ray camera_ray(const Camera& camera,int x,int y,int w,int h,float ox,float oy,glm::vec2 jitter={0,0}) {
    const auto forward=safe_normalize(camera.target-camera.position);
    const auto right=safe_normalize(glm::cross(forward,glm::vec3(0,1,0)),{1,0,0});
    const auto up=glm::cross(right,forward);const float f=std::tan(glm::radians(camera.fov)*0.5f);
    // 投影加 jitter，反投影必须减 jitter；Y-down 像素和 Y-up NDC 分开换算。
    auto direction=safe_normalize(forward+right*((2*(x+ox)/w-1-jitter.x)*float(w)/h*f)+up*((1-2*(y+oy)/h-jitter.y)*f));
    const float cosine=glm::dot(direction,forward);
    return {camera.position,direction,camera.near_plane/cosine,camera.far_plane/cosine};
}
void rect_axes(const Light& l,glm::vec3& normal,glm::vec3& u,glm::vec3& v) {
    normal=safe_normalize(l.direction,{0,-1,0});basis(normal,u,v);
}
float rect_hit(const Light& light,const Ray& ray) {
    glm::vec3 n,u,v;rect_axes(light,n,u,v);
    const float dn=glm::dot(ray.direction,n);
    if(dn>=-1e-7f)return 1e30f;
    const float t=glm::dot(light.position-ray.origin,n)/dn;
    if(t<ray.t_min||t>ray.t_max)return 1e30f;
    const auto p=ray.origin+ray.direction*t-light.position;
    return std::abs(glm::dot(p,u))<=light.size.x*.5f&&std::abs(glm::dot(p,v))<=light.size.y*.5f?t:1e30f;
}
RectangleLight rectangle_light(const Light& light) {
    glm::vec3 n,u,v;rect_axes(light,n,u,v);
    return {light.position,u*(light.size.x*.5f),v*(light.size.y*.5f),light.color*light.intensity,false};
}
float sdf_visibility(const Scene& scene,const SurfaceSample& surface,glm::vec3 direction,
                     float distance,const Settings& settings) {
    if(!settings.sdf_shadows||!scene.baked_resources||!scene.baked_resources->sdf)return 1;
    return scene_sdf_visibility(*scene.baked_resources->sdf,surface.position,surface.normal,direction,
                                distance,settings.sdf_softness,std::max(0.f,settings.shadow_bias));
}
float sdf_light_visibility(const Scene& scene,const SurfaceSample& surface,const Light& light,const Settings& settings) {
    const auto delta=light.position-surface.position;const float distance=glm::length(delta);
    if(light.kind!=LightKind::directional&&distance<1e-6f)return 1;
    // 矩形光的 SDF 因子沿灯心方向估计；原有面积光可见率仍由 ShadowGi 采样。
    return sdf_visibility(scene,surface,light.kind==LightKind::directional?-safe_normalize(light.direction):delta/distance,
                          light.kind==LightKind::directional?1e30f:distance,settings);
}
glm::vec3 direct_sample(const Scene& scene,const Material& m,const SurfaceSample& s,glm::vec3 v,
    const Settings& settings,Random& rng,const std::function<bool(const Ray&)>& occluded,bool use_mis) {
    glm::vec3 out(0);
    for(const auto& light:scene.lights) {
        glm::vec3 l,li;float distance=1e30f,pdf=1;bool area=false;
        if(light.kind==LightKind::directional) {l=-safe_normalize(light.direction);li=light.color*light.intensity;}
        else {
            glm::vec3 target=light.position;
            glm::vec3 ln,u,b;
            if(light.kind==LightKind::rectangle) {
                rect_axes(light,ln,u,b);
                target+=u*((rng.next()-.5f)*light.size.x)+b*((rng.next()-.5f)*light.size.y);area=true;
            }
            const auto delta=target-s.position;const float d2=glm::dot(delta,delta);
            if(d2<1e-10f)continue;
            distance=std::sqrt(d2);l=delta/distance;
            if(area) {
                const float cosine=std::max(0.f,glm::dot(ln,-l));
                if(cosine<=0||light.size.x*light.size.y<=0)continue;
                pdf=d2/(cosine*light.size.x*light.size.y);li=light.color*light.intensity;
            }else{
                if(distance>light.range)continue;
                li=light.color*(light.intensity/std::max(d2,1e-5f));
            }
        }
        const float nl=std::max(0.f,glm::dot(s.normal,l));
        if(nl<=0||occluded({s.position+s.normal*0.0003f,l,0.0001f,distance-0.0006f}))continue;
        const float weight=area&&use_mis?mis_power(pdf,brdf_pdf(m,s.normal,v,l,s.tangent,settings)):1.f;
        // SDF 只乘直接光可见率，不修改 BRDF 或重复增加环境项。
        out+=evaluate_brdf(m,s.normal,v,l,s.tangent,settings)*li*(nl*weight/pdf*sdf_visibility(scene,s,l,distance,settings));
    }
    return out;
}
template<class Spatial>
glm::vec3 trace(const Scene& scene,const std::vector<Triangle>& triangles,const Spatial& bvh,Ray ray,
               const Settings& settings,Random& rng,std::uint64_t& rays,std::uint64_t* node_tests=nullptr) {
    glm::vec3 result(0),weight(1);float previous_pdf=0;
    for(int bounce=0;bounce<std::max(1,settings.max_bounces);++bounce) {
        ++rays;TraversalStatistics hit_stats;const auto hit=bvh.intersect(ray,&hit_stats);
        if(node_tests)*node_tests+=hit_stats.box_tests;
        float nearest=hit.triangle>=0?hit.t:1e30f;const Light* emitter=nullptr;
        for(const auto& light:scene.lights) if(light.kind==LightKind::rectangle) {
            const float t=rect_hit(light,ray);
            if(t<nearest) {nearest=t;emitter=&light;}
        }
        if(emitter) {
            float w=1;
            if(bounce>0) {
                const float nl=std::max(1e-6f,glm::dot(safe_normalize(emitter->direction),-ray.direction));
                const float light_pdf=nearest*nearest/(nl*emitter->size.x*emitter->size.y);
                w=mis_power(previous_pdf,light_pdf);
            }
            result+=weight*emitter->color*(emitter->intensity*w);break;
        }
        if(hit.triangle<0) {result+=weight*environment(scene,ray.direction);break;}
        auto s=surface_at(triangles.at(hit.triangle),hit);
        if(glm::dot(s.normal,ray.direction)>0)s.normal=-s.normal;
        const auto m=sample_material(scene,s,settings);
        const auto geometric_normal=s.normal;
        s.normal=sample_normal(scene,s,settings);
        if(glm::dot(s.normal,-ray.direction)<=1e-5f)s.normal=geometric_normal;
        result+=weight*m.emissive;
        auto visible=[&](const Ray& shadow) {
            ++rays;TraversalStatistics shadow_stats;const bool blocked=bvh.occluded(shadow,&shadow_stats);
            if(node_tests)*node_tests+=shadow_stats.box_tests;
            return blocked;
        };
        const bool continue_path=bounce+1<settings.max_bounces;
        // 最后一跳没有互补的 BSDF 灯命中估计器，面积采样必须使用完整权重。
        result+=weight*direct_sample(scene,m,s,-ray.direction,settings,rng,visible,continue_path);
        if(!continue_path)break;
        const auto proposal=sample_brdf(m,s.normal,-ray.direction,s.tangent,settings,{rng.next(),rng.next(),rng.next()});
        if(!proposal.valid)break;
        const auto next=proposal.direction;
        const float nl=glm::dot(s.normal,next);
        if(nl<=0)break;
        previous_pdf=proposal.pdf;
        if(previous_pdf<=1e-12f)break;
        // 贡献除以实际混合采样密度；MIS 只在同时被灯采样覆盖的矩形发光体上应用。
        weight*=proposal.value*(nl/previous_pdf);
        if(!finite(weight))throw std::runtime_error("Non-finite path throughput");
        if(bounce>=2) {
            const float survive=std::clamp(std::max({weight.r,weight.g,weight.b}),0.05f,0.95f);
            if(rng.next()>survive)break;
            weight/=survive;
        }
        ray={s.position+s.normal*0.0003f,next,0.0001f,1e30f};
    }
    return result;
}
template<class F> void parallel_rows(int count,F f,std::atomic<bool>* cancel) {
    // 真正把参考渲染行任务交给引擎 JobSystem，异常在所有工作结束后传回调用者。
    JobSystem jobs(std::max(1u,std::min(8u,std::thread::hardware_concurrency())));
    jobs.parallel_for(std::size_t(count),[&](std::size_t y){if(!cancel||!cancel->load())f(int(y));},2);
}
struct ReferenceSpatial {
    const SceneSpatialIndex& index;
    Hit intersect(const Ray& ray,TraversalStatistics* stats) const { return index.pick(ray,stats).hit; }
    bool occluded(const Ray& ray,TraversalStatistics* stats) const { return index.occluded(ray,stats); }
};
}
void prepare_reference_lighting(const Scene& scene,const Settings& settings) {
    if(settings.path==RenderPath::path_trace)return;
    if(!finite(scene.sky_top)||!finite(scene.sky_bottom))throw std::invalid_argument("Non-finite environment");
    if(settings.gi==GiMode::environment)(void)reference_ibl(scene);
    if(std::any_of(scene.lights.begin(),scene.lights.end(),[](const Light& l){return l.kind==LightKind::rectangle;}))
        (void)reference_ltc();
}
RenderOutput render_reference(const Scene& scene,const Camera& camera,const Settings& settings,std::atomic<bool>* cancel) {
    return render_reference(scene,camera,settings,ReferenceFrameInput{},cancel);
}
RenderOutput render_reference(const Scene& source,const Camera& camera,const Settings& settings,
                              const ReferenceFrameInput& frame,std::atomic<bool>* cancel) {
    validate_inputs(source,camera,settings,frame);
    const auto start=std::chrono::steady_clock::now();
    const int w=settings.render_width,h=settings.render_height;
    RenderOutput output;output.color.reset(w,h);
    output.indirect.reset(w,h);output.shadow.reset(w,h,1);output.ao.reset(w,h,1);
    if(cancel&&cancel->load()) {
        SurfaceSample clear;clear.depth=settings.reversed_z?0.f:1.f;output.surfaces.reset(w,h,clear);
        if(frame.previous_world_positions)frame.previous_world_positions->reset(w,h);
        return output;
    }
    // prepare 只选择已保存的真实 LOD；没有链时保持 LOD0，不隐式启动 QEM。
    static thread_local SceneGeometryRuntime runtime;
    const Scene& geometry=runtime.prepare(source,camera,settings);
    std::unique_ptr<Scene> baked_scene;
    Settings bake_settings=settings;
    // PT 保留数值 BSDF/环境积分；SH/PRT 是光栅环境 diffuse 的近似选项。
    if(settings.path==RenderPath::path_trace||settings.gi!=GiMode::environment)
        bake_settings.environment_diffuse=EnvironmentDiffuse::ibl;
    if(bake_settings.environment_diffuse!=EnvironmentDiffuse::ibl||settings.sdf_shadows) {
        const auto resources=prepare_scene_resources(geometry,bake_settings,cancel);
        if(!resources) {
            SurfaceSample clear;clear.depth=settings.reversed_z?0.f:1.f;output.surfaces.reset(w,h,clear);
            if(frame.previous_world_positions)frame.previous_world_positions->reset(w,h);
            return output;
        }
        // 对真正提交的选级快照烘焙，派生顶点不会读取 LOD0 的零照度属性。
        // 同一选级/几何复用不可变缓存；切换到不同几何级别时首次创建对应 transfer。
        baked_scene=std::make_unique<Scene>(apply_scene_resources(geometry,resources,bake_settings));
    }
    // 索引使用相同几何；写入 baked 属性不改变三角形编号、节点层级或空间命中。
    const Scene& scene=baked_scene?*baked_scene:geometry;
    const auto triangles=flatten_scene(scene);const ReferenceSpatial bvh{runtime.spatial_index()};output.triangles=triangles.size();
    Image<glm::vec2> uv_dx,uv_dy;RasterOptions raster_options;
    raster_options.jitter_ndc=frame.jitter_ndc;raster_options.previous_camera=frame.previous_camera;
    raster_options.previous_world_positions=frame.previous_world_positions;raster_options.uv_dx=&uv_dx;raster_options.uv_dy=&uv_dy;
    output.surfaces=rasterize(scene,camera,settings,raster_options);
    // 两条路径都提供采样后的材质/法线引导；AO 与时域拒绝看到同一表面。
    parallel_rows(h,[&](int y) {
        for(int x=0;x<w;++x) {
            auto& s=output.surfaces.at(x,y);if(!s.valid)continue;
            const auto v=safe_normalize(camera.position-s.position);
            if(glm::dot(s.normal,v)<0)s.normal=-s.normal;
            const auto dx=uv_dx.at(x,y),dy=uv_dy.at(x,y);
            const auto m=sample_material(scene,s,settings,dx,dy);const auto geometric_normal=s.normal;
            s.normal=sample_normal(scene,s,settings,dx,dy);
            if(glm::dot(s.normal,v)<=1e-5f)s.normal=geometric_normal;
            s.albedo=glm::vec3(m.base_color);s.roughness=m.roughness;s.metallic=m.metallic;
            output.ao.at(x,y)=sample_occlusion(scene,s,settings,dx,dy);
        }
    },cancel);
    std::atomic<std::uint64_t> ray_count{0};
    std::atomic<std::uint64_t> node_count{0};
    if(settings.path==RenderPath::path_trace) {
        parallel_rows(h,[&](int y) {
            for(int x=0;x<w;++x){
                Random rng{settings.seed+std::uint32_t((y*w+x)*9781u+1u)};glm::vec3 c(0);std::uint64_t rays=0,nodes=0;
                const int samples=settings.samples;int completed=0;
                for(int s=0;s<samples;++s) {
                    if(cancel&&cancel->load())break;
                    c+=trace(scene,triangles,bvh,camera_ray(camera,x,y,w,h,rng.next(),rng.next(),frame.jitter_ndc),settings,rng,rays,&nodes);
                    ++completed;
                }
                // 取消发生在像素中途时，仅按已完成的有效样本平均，不能除原计划数。
                output.color.at(x,y)=completed?c/float(completed):glm::vec3(0);ray_count+=rays;node_count+=nodes;
            }
        },cancel);
    } else {
        const auto ibl=settings.gi==GiMode::environment?reference_ibl(scene):std::shared_ptr<const IblData>{};
        const bool has_rectangle=std::any_of(scene.lights.begin(),scene.lights.end(),[](const Light& l){return l.kind==LightKind::rectangle;});
        const LtcLut* ltc=has_rectangle?&reference_ltc():nullptr;
        const ShadowGi shadow_gi(scene,triangles,camera,settings);
        const auto screen_ao=ambient_occlusion(output.surfaces,camera,settings);
        for(std::size_t i=0;i<output.ao.pixels.size();++i)output.ao.pixels[i]*=screen_ao.pixels[i];
        parallel_rows(h,[&](int y) {
            for(int x=0;x<w;++x){
                auto& s=output.surfaces.at(x,y);
                if(!s.valid){output.color.at(x,y)=environment(scene,camera_ray(camera,x,y,w,h,.5f,.5f,frame.jitter_ndc).direction);continue;}
                const auto material=sample_material(scene,s,settings,uv_dx.at(x,y),uv_dy.at(x,y));
                const auto v=safe_normalize(camera.position-s.position);
                glm::vec3 direct(0);
                for(const auto& light:scene.lights) {
                    if(light.kind==LightKind::rectangle){
                        const float visibility=shadow_gi.visibility(s,light)*sdf_light_visibility(scene,s,light,settings);
                        output.shadow.at(x,y)=std::min(output.shadow.at(x,y),visibility);
                        const auto rectangle=rectangle_light(light);
                        direct+=visibility*evaluate_rectangle_light(*ltc,rectangle,s.position,s.normal,v,s.tangent,material,settings);
                        continue;
                    }
                    const auto delta=light.position-s.position;
                    const float d2=glm::dot(delta,delta);
                    const auto l=light.kind==LightKind::directional?-safe_normalize(light.direction):safe_normalize(delta);
                    if(light.kind==LightKind::point&&d2>light.range*light.range)continue;
                    const auto li=light.color*(light.intensity/(light.kind==LightKind::point?std::max(d2,1e-5f):1.f));
                    const float visibility=shadow_gi.visibility(s,light)*sdf_light_visibility(scene,s,light,settings);
                    output.shadow.at(x,y)=std::min(output.shadow.at(x,y),visibility);
                    direct+=evaluate_brdf(material,s.normal,v,l,s.tangent,settings)*li*(std::max(0.f,glm::dot(s.normal,l))*visibility);
                }
                glm::vec3 indirect(0);
                if(settings.gi==GiMode::environment) {
                    indirect=ibl_lighting(*ibl,material,s,v,settings,scene.baked_resources.get());
                }else if(settings.gi==GiMode::rsm||settings.gi==GiMode::lpv||settings.gi==GiMode::voxel)
                    indirect=shadow_gi.indirect(s,v);
                output.indirect.at(x,y)=indirect*output.ao.at(x,y);
                output.color.at(x,y)=material.emissive+direct+output.indirect.at(x,y);
            }
        },cancel);
        if(settings.gi==GiMode::ssr||settings.gi==GiMode::ssgi) {
            output.indirect=screen_space_indirect(output.surfaces,output.color,scene,camera,settings);
            for(std::size_t i=0;i<output.color.pixels.size();++i){output.indirect.pixels[i]*=output.ao.pixels[i];output.color.pixels[i]+=output.indirect.pixels[i];}
        }
    }
    if(frame.apply_postprocess&&(settings.outline||settings.hatching||settings.shading==ShadingMode::toon))output.color=apply_npr(output.color,output.surfaces,settings);
    if(frame.apply_postprocess&&settings.bloom){const auto glow=bloom(output.color,settings);for(std::size_t i=0;i<output.color.pixels.size();++i)output.color.pixels[i]+=glow.pixels[i];}
    for(std::size_t i=0;i<output.color.pixels.size();++i) {
        const auto& s=output.surfaces.pixels[i];glm::vec3 c=output.color.pixels[i];
        switch(frame.apply_postprocess?settings.debug:DebugView::final_color){
        case DebugView::albedo:c=s.albedo;break;
        case DebugView::normal:c=s.valid?s.normal*.5f+.5f:glm::vec3(0);break;
        case DebugView::depth:c=glm::vec3(s.valid?s.linear_depth/camera.far_plane:0);break;
        case DebugView::roughness:c=glm::vec3(s.roughness);break;
        case DebugView::metallic:c=glm::vec3(s.metallic);break;
        case DebugView::ao:c=glm::vec3(output.ao.pixels[i]);break;
        case DebugView::shadow:c=glm::vec3(output.shadow.pixels[i]);break;
        case DebugView::indirect:c=output.indirect.pixels[i];break;
        case DebugView::motion:c=glm::vec3(s.motion*.05f+glm::vec2(.5f),0);break;
        default:break;
        }
        if(!finite(c))throw std::runtime_error("Non-finite radiance in reference renderer");
        output.color.pixels[i]=glm::max(c,glm::vec3(0));
    }
    output.rays=ray_count;output.node_tests=node_count;
    output.cpu_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    return output;
}
void save_bmp(const std::filesystem::path& path,const Image<glm::vec3>& hdr,float exposure){
    if(hdr.empty())throw std::invalid_argument("Cannot save empty image");
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot open image output");
    const std::uint32_t stride=(hdr.width*3+3)&~3,bytes=stride*hdr.height;
    auto u16=[&](std::uint16_t v){char b[2]{char(v),char(v>>8)};f.write(b,2);};
    auto u32=[&](std::uint32_t v){char b[4]{char(v),char(v>>8),char(v>>16),char(v>>24)};f.write(b,4);};
    f.write("BM",2);u32(54+bytes);u32(0);u32(54);u32(40);u32(hdr.width);u32(hdr.height);u16(1);u16(24);u32(0);u32(bytes);u32(2835);u32(2835);u32(0);u32(0);
    std::vector<unsigned char> row(stride,0);
    for(int y=hdr.height-1;y>=0;--y){
        for(int x=0;x<hdr.width;++x){const auto c=tone_map(hdr.at(x,y),exposure);for(int j=0;j<3;++j)row[x*3+2-j]=static_cast<unsigned char>(std::clamp(c[j],0.f,1.f)*255+.5f);}
        f.write(reinterpret_cast<const char*>(row.data()),row.size());
    }if(!f)throw std::runtime_error("Image output failed");
}
void save_pfm(const std::filesystem::path& path,const Image<glm::vec3>& hdr){
    if(hdr.empty())throw std::invalid_argument("Cannot save empty HDR image");
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path,std::ios::binary);f<<"PF\n"<<hdr.width<<" "<<hdr.height<<"\n-1.0\n";
    for(int y=hdr.height-1;y>=0;--y)for(int x=0;x<hdr.width;++x){auto c=hdr.at(x,y);f.write(reinterpret_cast<const char*>(&c.x),3*sizeof(float));}
    if(!f)throw std::runtime_error("HDR output failed");
}
TestResults test_reference(){
    TestResults tests;
    auto check=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    auto add=[&](const char* name,const std::function<std::string()>& body){
        try{tests.push_back({name,true,body()});}catch(const std::exception& e){tests.push_back({name,false,e.what()});}
    };
    auto plane=[] {
        Scene scene;scene.materials.emplace_back();scene.materials[0].base_color={.7f,.4f,.2f,1};
        scene.sky_top={.8f,.6f,.4f};scene.sky_bottom={.1f,.2f,.3f};Mesh mesh;
        for(const auto p:{glm::vec3(-2,-2,0),glm::vec3(2,-2,0),glm::vec3(2,2,0),glm::vec3(-2,2,0)}) {
            Vertex v;v.position=p;v.normal={0,0,1};v.tangent={1,0,0,1};mesh.vertices.push_back(v);
        }
        mesh.indices={0,1,2,0,2,3};scene.meshes.push_back(mesh);return scene;
    };
    auto camera=[] {Camera c;c.position={0,0,3};c.target={0,0,0};c.fov=60;c.near_plane=.1f;c.far_plane=20;return c;};
    auto options=[] {Settings s;s.path=RenderPath::cpu_raster;s.render_width=12;s.render_height=12;s.samples=4;s.bloom=false;s.shadow_resolution=16;return s;};
    auto error=[](glm::vec3 a,glm::vec3 b){auto d=glm::abs(a-b);return std::max({d.x,d.y,d.z});};
    add("reference linear HDR empty environment and deterministic path seed",[&] {
        Scene scene;scene.sky_top=scene.sky_bottom={2,3,4};auto s=options();s.path=RenderPath::path_trace;
        const auto a=render_reference(scene,camera(),s),b=render_reference(scene,camera(),s);
        for(std::size_t i=0;i<a.color.pixels.size();++i){check(error(a.color.pixels[i],{2,3,4})<1e-6f,"HDR clipped/encoded");check(a.color.pixels[i]==b.color.pixels[i],"Nondeterministic PT");}
        check(a.rays==std::uint64_t(s.render_width*s.render_height*s.samples),"Primary ray count incorrect");return "constant (2,3,4), exact repeat, complete primary ray count";
    });
    add("reference precomputed IBL is independent of per-pixel seed and samples",[&] {
        auto scene=plane();auto s=options();scene.materials[0].metallic=.6f;
        const auto a=render_reference(scene,camera(),s);s.seed=777;s.samples=29;const auto b=render_reference(scene,camera(),s);
        float difference=0;int count=0;
        for(std::size_t i=0;i<a.color.pixels.size();++i)if(a.surfaces.pixels[i].valid){difference=std::max(difference,error(a.color.pixels[i],b.color.pixels[i]));++count;}
        check(count>20&&difference==0,"Raster environment still depends on per-pixel stochastic samples");
        const auto& surface=a.surfaces.at(6,6);const auto m=sample_material(scene,surface,s);const auto view=safe_normalize(camera().position-surface.position);
        const auto expected=ibl_lighting(*reference_ibl(scene),m,surface,view,s);
        check(error(a.indirect.at(6,6),expected)<1e-6f,"Raster did not consume split-sum precomputation");return "exact seed/sample invariance; prefilter + BRDF-LUT result matched";
    });
    add("reference environment cache isolates values/revisions and concurrent callers",[&] {
        Scene a,b;a.revision=b.revision=9983;a.sky_top=a.sky_bottom={.13f,.27f,.41f};b.sky_top=b.sky_bottom={.71f,.39f,.17f};
        auto first=std::async(std::launch::async,[&]{return reference_ibl(a);});auto second=std::async(std::launch::async,[&]{return reference_ibl(a);});
        const auto aa=first.get(),same=second.get(),bb=reference_ibl(b);check(aa==same&&aa!=bb,"Cache raced or aliased scenes with equal revisions");
        check(error(sample_ibl_diffuse(*aa,{0,1,0}),a.sky_top*pi)<2e-6f,"First environment data stale");
        check(error(sample_ibl_diffuse(*bb,{0,1,0}),b.sky_top*pi)<2e-6f,"Second environment data stale");
        ++a.revision;const auto changed=reference_ibl(a);check(changed!=aa,"Revision did not invalidate precomputation");
        check(error(sample_ibl_diffuse(*aa,{0,1,0}),glm::vec3(.13f,.27f,.41f)*pi)<2e-6f,"Rebuild mutated reader snapshot");return "shared immutable same-key snapshot; distinct value/revision keys";
    });
    add("reference rectangle uses fitted LTC and deterministic visibility",[&] {
        auto scene=plane();scene.sky_top=scene.sky_bottom=glm::vec3(0);Light light;light.kind=LightKind::rectangle;light.position={.5f,.3f,2};light.direction={0,0,-1};light.size={1.2f,.8f};light.intensity=3;scene.lights={light};
        scene.materials[0].metallic=1;scene.materials[0].roughness=.7f;auto s=options();s.gi=GiMode::none;s.energy_compensation=false;
        const auto a=render_reference(scene,camera(),s);s.seed=498;s.samples=31;const auto b=render_reference(scene,camera(),s);
        const auto& surface=a.surfaces.at(6,6);const auto view=safe_normalize(camera().position-surface.position);const auto rectangle=rectangle_light(light);
        const auto expected=evaluate_ltc_rectangle(reference_ltc(),rectangle,surface.position,surface.normal,view,glm::vec3(0),glm::vec3(scene.materials[0].base_color),.7f);
        check(error(a.color.at(6,6),expected)<1e-6f&&glm::length(expected)>.01f,"Raster rectangle did not use fitted LTC");
        for(std::size_t i=0;i<a.color.pixels.size();++i)check(a.color.pixels[i]==b.color.pixels[i],"Rectangle lighting depends on RNG");
        const auto numeric=integrate_rectangle_reference(rectangle,surface.position,surface.normal,view,surface.tangent,scene.materials[0],s,16384);
        const float relative=glm::length(expected-numeric)/glm::length(numeric);check(relative<.22f,"LTC coarse-table approximation exceeded reference tolerance");
        std::ostringstream detail;detail<<"LTC/area-quadrature relative error="<<relative;return detail.str();
    });
    add("reference path-trace terminal bounce light sampling keeps full energy",[&] {
        auto scene=plane();scene.sky_top=scene.sky_bottom=glm::vec3(0);scene.materials[0].base_color=glm::vec4(1);
        Light light;light.kind=LightKind::rectangle;light.position={0,0,2};light.direction={0,0,-1};light.size={2,2};light.intensity=1;scene.lights={light};
        auto s=options();s.path=RenderPath::path_trace;s.shading=ShadingMode::toon;s.max_bounces=1;
        const auto triangles=flatten_scene(scene);const Bvh bvh(triangles);Random random{1937};glm::dvec3 total(0);std::uint64_t rays=0;
        constexpr int count=32768;
        for(int i=0;i<count;++i)total+=glm::dvec3(trace(scene,triangles,bvh,{{0,0,.1f},{0,0,-1},.001f,10},s,random,rays));
        const auto reference=integrate_rectangle_reference(rectangle_light(light),{0,0,0},{0,0,1},{0,0,1},{1,0,0,1},scene.materials[0],s,count);
        const float relative=error(glm::vec3(total/double(count)),reference)/reference.r;
        check(relative<.005f,"Terminal direct estimate lost energy to nonexistent BSDF estimator");
        s.max_bounces=2;random.state=1937;glm::dvec3 mis_total(0);
        for(int i=0;i<count;++i)mis_total+=glm::dvec3(trace(scene,triangles,bvh,{{0,0,.1f},{0,0,-1},.001f,10},s,random,rays));
        const float mis_relative=error(glm::vec3(mis_total/double(count)),reference)/reference.r;
        check(mis_relative<.015f,"Area+BSDF MIS energy disagrees with quadrature");
        std::ostringstream detail;detail<<"terminal relative error="<<relative<<", two-estimator MIS="<<mis_relative;return detail.str();
    });
    add("reference true jitter projection, inverse ray and unjittered motion",[&] {
        auto scene=plane();auto s=options();const auto c=camera();ReferenceFrameInput frame;frame.jitter_ndc={1.f/s.render_width,-1.f/s.render_height};frame.apply_postprocess=false;
        const auto shifted=render_reference(scene,c,s,frame),plain=render_reference(scene,c,s);
        const auto& surface=shifted.surfaces.at(6,6);check(surface.valid&&plain.surfaces.at(6,6).valid,"Missing plane fixture");
        check(glm::length(surface.position-plain.surfaces.at(6,6).position)>.05f,"Jitter did not affect raster samples");
        auto clip=c.projection(1,s.reversed_z)*c.view()*glm::vec4(surface.position,1);const auto ndc=glm::vec2(clip)/clip.w+frame.jitter_ndc;
        const auto pixel=(ndc*glm::vec2(.5f,-.5f)+.5f)*glm::vec2(s.render_width,s.render_height);
        check(glm::length(pixel-glm::vec2(6.5f))<.005f,"clip += jitter*w convention mismatch");
        const auto ray=camera_ray(c,6,6,s.render_width,s.render_height,.5f,.5f,frame.jitter_ndc);
        const auto hit=ray.origin+ray.direction*(-ray.origin.z/ray.direction.z);check(glm::length(hit-surface.position)<.002f,"Ray/raster jitter disagree");
        for(const auto& p:shifted.surfaces.pixels)if(p.valid)check(glm::length(p.motion)<1e-5f,"Jitter leaked into motion vectors");
        return "half-pixel jitter changes samples; projected centers/rays agree; static motion zero";
    });
    add("reference previous camera and moving-object position correspondence",[&] {
        auto scene=plane();Node node;node.mesh=0;node.local=glm::translate(glm::mat4(1),{.1f,0,0});node.previous_world=glm::translate(glm::mat4(1),{-.2f,0,.3f});scene.nodes={node};
        auto c=camera(),old=c;old.position.x=.3f;old.target.x=.3f;auto s=options();ReferenceFrameInput frame;frame.previous_camera=&old;frame.jitter_ndc={.03f,-.02f};Image<glm::vec3> positions;frame.previous_world_positions=&positions;
        const auto rendered=render_reference(scene,c,s,frame);const auto& p=rendered.surfaces.at(6,6);const auto previous=p.position+glm::vec3(-.3f,0,.3f);
        check(error(positions.at(6,6),previous)<1e-6f,"Previous world transform not preserved");
        auto now_clip=c.projection(1,s.reversed_z)*c.view()*glm::vec4(p.position,1),old_clip=old.projection(1,s.reversed_z)*old.view()*glm::vec4(previous,1);
        const auto expected=(glm::vec2(now_clip)/now_clip.w-glm::vec2(old_clip)/old_clip.w)*glm::vec2(s.render_width*.5f,-s.render_height*.5f);
        check(glm::length(p.motion-expected)<1e-5f&&glm::length(p.motion)>.1f,"Combined camera/object motion incorrect");
        scene.nodes[0].previous_world=glm::translate(glm::mat4(1),{0,0,10});const auto behind=render_reference(scene,c,s,frame);
        check(behind.surfaces.at(6,6).motion.x>s.render_width,"Unprojectable history was not rejected");return "camera+object motion includes depth; previous world output; behind-camera rejection";
    });
    add("reference uses shared geometry coverage and camera depth convention",[&] {
        auto scene=plane();auto c=camera();auto s=options();ReferenceFrameInput frame;frame.jitter_ndc={.02f,-.03f};RasterOptions raster_options;raster_options.jitter_ndc=frame.jitter_ndc;
        const auto baseline=rasterize(scene,c,s,raster_options);const auto rendered=render_reference(scene,c,s,frame);int covered=0;
        for(std::size_t i=0;i<baseline.pixels.size();++i){const auto& a=baseline.pixels[i];const auto& b=rendered.surfaces.pixels[i];check(a.valid==b.valid,"Coverage changed");if(a.valid){++covered;check(std::abs(a.depth-b.depth)<1e-6f&&error(a.position,b.position)<2e-6f,"Depth/interpolation changed");}}
        check(covered>20,"No raster coverage tested");auto p=c.projection(1,true);auto near=p*glm::vec4(0,0,-c.near_plane,1),far=p*glm::vec4(0,0,-c.far_plane,1);
        check(std::abs(near.z/near.w-1)<1e-5&&std::abs(far.z/far.w)<1e-5,"Reverse-Z endpoints");
        const auto ray=camera_ray(c,0,0,12,12,.5f,.5f,{.01f,-.02f});
        check(std::abs(-(c.view()*glm::vec4(ray.origin+ray.direction*ray.t_min,1)).z-c.near_plane)<1e-5f,"Primary near clip incorrect");
        check(std::abs(-(c.view()*glm::vec4(ray.origin+ray.direction*ray.t_max,1)).z-c.far_plane)<1e-4f,"Primary far clip incorrect");return "jittered geometry/raster guides agree; primary ray uses camera near/far planes";
    });
    add("reference deferred postprocess keeps raw toon HDR and complete diagnostics",[&] {
        auto scene=plane();scene.materials[0].emissive={2,3,4};auto s=options();s.gi=GiMode::none;s.shading=ShadingMode::toon;s.bloom=true;s.outline=true;s.hatching=true;s.debug=DebugView::normal;
        ReferenceFrameInput frame;frame.apply_postprocess=false;const auto raw=render_reference(scene,camera(),s,frame);
        check(error(raw.color.at(6,6),{2,3,4})<1e-6f,"Toon/NPR/bloom/debug ran despite deferred postprocess");
        check(error(raw.surfaces.at(6,6).albedo,{.7f,.4f,.2f})<1e-6f,"G-buffer material factors missing");
        s.path=RenderPath::path_trace;const auto pt=render_reference(scene,camera(),s,frame);check(error(pt.surfaces.at(6,6).albedo,{.7f,.4f,.2f})<1e-6f,"Path-trace guide missing material data");return "apply_postprocess=false bypasses NPR/bloom/debug; guides remain populated";
    });
    add("reference consumes analytic UV gradients for material and alpha mask",[&] {
        auto scene=plane();scene.sky_top=scene.sky_bottom=glm::vec3(0);auto s=options();s.gi=GiMode::none;s.filter=FilterMode::trilinear;
        for(auto& vertex:scene.meshes[0].vertices)vertex.uv=glm::vec2(vertex.position)*16.f;
        Texture checker;checker.srgb=true;checker.levels.emplace_back(64,64);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x)checker.levels[0].at(x,y)=glm::vec4(glm::vec3(float((x+y)%2)),1);
        build_mips(checker);scene.textures.push_back(checker);scene.materials[0].base_color=glm::vec4(1);scene.materials[0].base_texture=0;scene.materials[0].emissive_texture=0;scene.materials[0].emissive=glm::vec3(1);
        scene.materials[0].mr_texture=0;scene.materials[0].metallic=.8f;scene.materials[0].roughness=.8f;scene.materials[0].ao_texture=0;
        ReferenceFrameInput frame;frame.jitter_ndc={.02f,-.03f};frame.apply_postprocess=false;const auto filtered=render_reference(scene,camera(),s,frame);
        check(error(filtered.color.at(6,6),glm::vec3(.5f))<1e-5f,"Emissive ignored UV footprint");
        check(error(filtered.surfaces.at(6,6).albedo,glm::vec3(.5f))<1e-5f,"Base texture ignored UV footprint");
        check(std::abs(filtered.surfaces.at(6,6).roughness-.4f)<1e-5f&&std::abs(filtered.surfaces.at(6,6).metallic-.4f)<1e-5f,"MR texture ignored UV footprint");
        check(std::abs(filtered.ao.at(6,6)-.5f)<1e-5f,"Occlusion texture ignored UV footprint");
        Material masked;masked.base_color=glm::vec4(1);masked.base_texture=1;masked.alpha_mode=1;masked.alpha_cutoff=.55f;masked.emissive=glm::vec3(10);scene.materials.push_back(masked);
        Texture alpha;alpha.levels.emplace_back(64,64);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x)alpha.levels[0].at(x,y)={1,1,1,float((x+y)%2)};
        build_mips(alpha);scene.textures.push_back(alpha);Mesh front=scene.meshes[0];for(auto& v:front.vertices)v.position.z=.5f;front.primitives={{0,6,1}};scene.meshes.push_back(front);
        const auto cutout=render_reference(scene,camera(),s,frame);
        check(cutout.surfaces.at(6,6).material==0&&error(cutout.color.at(6,6),glm::vec3(.5f))<1e-5f,"Filtered alpha mask wrote foreground depth");
        return "minified checker gives linear 0.5; alpha=0.5 discarded before depth at cutoff=0.55";
    });
    add("reference cancellation and invalid input boundaries",[&] {
        auto scene=plane();auto s=options();std::atomic<bool> cancel{true};const auto cancelled=render_reference(scene,camera(),s,&cancel);
        check(cancelled.rays==0&&cancelled.color.width==s.render_width,"Pre-cancel performed ray work or lost shape");
        for(auto c:cancelled.color.pixels)check(c==glm::vec3(0),"Pre-cancel produced nonzero samples");
        auto rejected=[&](const Settings& settings,const Camera& c,const ReferenceFrameInput& frame=ReferenceFrameInput{}){try{(void)render_reference(scene,c,settings,frame);return false;}catch(const std::invalid_argument&){return true;}};
        auto bad=s;bad.samples=0;check(rejected(bad,camera()),"Zero sample budget accepted");bad=s;bad.max_bounces=65;check(rejected(bad,camera()),"Unbounded bounces");
        bad=s;bad.render_width=100000;bad.render_height=100000;check(rejected(bad,camera()),"Unbounded frame allocation");
        auto c=camera();c.target=c.position;check(rejected(s,c),"Degenerate camera accepted");ReferenceFrameInput frame;frame.jitter_ndc.x=std::numeric_limits<float>::quiet_NaN();check(rejected(s,camera(),frame),"NaN jitter accepted");
        Light invalid;invalid.kind=LightKind::rectangle;invalid.size.x=0;scene.lights={invalid};check(rejected(s,camera()),"Degenerate rectangle accepted");return "cancellation returns shaped zero frame; invalid budgets/camera/jitter/light rejected";
    });
    return tests;
}
}

#ifdef EMBERFRAME_REFERENCE_TEST_MAIN
int main(){int failed=0;const auto tests=emberframe::lab::test_reference();for(const auto& t:tests){std::cout<<(t.passed?"PASS ":"FAIL ")<<t.name<<": "<<t.detail<<'\n';failed+=!t.passed;}std::cout<<tests.size()-failed<<'/'<<tests.size()<<" passed\n";return failed?1:0;}
#endif

