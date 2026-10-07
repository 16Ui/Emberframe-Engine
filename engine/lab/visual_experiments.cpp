#include "visual_experiments.h"
#include "geometry.h"
#include "shading.h"
#include "shadow_gi.h"
#include <chrono>
#include <cmath>
#include <utility>

namespace emberframe::lab {
namespace {
bool cancelled(std::atomic<bool>* flag) { return flag&&flag->load(std::memory_order_relaxed); }
float sat(float value) { return std::clamp(value,0.0f,1.0f); }
bool finite(glm::vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
struct CameraFrame {
    Camera camera;
    glm::vec3 forward,right,up;
    CameraFrame(Camera c):camera(c) {
        forward=safe_normalize(c.target-c.position,{0,0,-1});
        right=safe_normalize(glm::cross(forward,{0,1,0}),{1,0,0});up=glm::cross(right,forward);
    }
    Ray ray(int x,int y,int width,int height) const {
        const float tangent=std::tan(glm::radians(camera.fov)*0.5f);
        const auto d=safe_normalize(forward+right*((2*(x+0.5f)/width-1)*tangent*width/height)
            +up*((1-2*(y+0.5f)/height)*tangent));
        const float cosine=std::max(1e-5f,glm::dot(d,forward));
        return {camera.position,d,camera.near_plane/cosine,camera.far_plane/cosine};
    }
    glm::mat4 vp(float aspect,bool reversed) const {
        return camera.projection(aspect,reversed)*glm::lookAtRH(camera.position,camera.target,up);
    }
};
Camera demo_camera(Camera camera) {
    glm::vec3 direction=safe_normalize(camera.position-camera.target,{0.6f,0.35f,0.7f});
    // 内置实验跟随轨道方向，自动对准示例中心，避免导入场景的远处相机看不到物体。
    if(std::abs(direction.y)>0.97f)direction=safe_normalize(glm::vec3(direction.x,0.8f,direction.z+0.3f));
    camera.target={0,0.5f,0};camera.position=camera.target+direction*std::clamp(glm::length(camera.position-camera.target),5.5f,10.0f);
    camera.fov=std::clamp(camera.fov,35.0f,75.0f);camera.near_plane=std::min(camera.near_plane,0.1f);camera.far_plane=std::max(camera.far_plane,25.0f);
    return camera;
}
RenderOutput cleared_frame(const Settings& settings) {
    RenderOutput output;const int w=settings.render_width,h=settings.render_height;
    output.color.reset(w,h,glm::vec3(0));output.indirect.reset(w,h,glm::vec3(0));
    output.ao.reset(w,h,1);output.shadow.reset(w,h,1);output.variance.reset(w,h,0);
    SurfaceSample clear;clear.depth=settings.reversed_z?0.f:1.f;output.surfaces.reset(w,h,clear);return output;
}
void append_quad(Mesh& mesh,glm::vec3 a,glm::vec3 b,glm::vec3 c,glm::vec3 d,int material) {
    const auto first=std::uint32_t(mesh.indices.size()),base=std::uint32_t(mesh.vertices.size());
    const auto normal=safe_normalize(glm::cross(b-a,c-a));
    for(const auto p:{a,b,c,d}) { Vertex v;v.position=p;v.normal=normal;mesh.vertices.push_back(v); }
    for(const auto i:{0u,1u,2u,0u,2u,3u})mesh.indices.push_back(base+i);
    mesh.primitives.push_back({first,6,std::uint32_t(material)});
}
void append_box(Mesh& mesh,glm::vec3 lo,glm::vec3 hi,int material) {
    append_quad(mesh,{lo.x,lo.y,lo.z},{lo.x,hi.y,lo.z},{hi.x,hi.y,lo.z},{hi.x,lo.y,lo.z},material);
    append_quad(mesh,{lo.x,lo.y,hi.z},{hi.x,lo.y,hi.z},{hi.x,hi.y,hi.z},{lo.x,hi.y,hi.z},material);
    append_quad(mesh,{lo.x,lo.y,lo.z},{lo.x,lo.y,hi.z},{lo.x,hi.y,hi.z},{lo.x,hi.y,lo.z},material);
    append_quad(mesh,{hi.x,lo.y,lo.z},{hi.x,hi.y,lo.z},{hi.x,hi.y,hi.z},{hi.x,lo.y,hi.z},material);
    append_quad(mesh,{lo.x,lo.y,lo.z},{hi.x,lo.y,lo.z},{hi.x,lo.y,hi.z},{lo.x,lo.y,hi.z},material);
    append_quad(mesh,{lo.x,hi.y,lo.z},{lo.x,hi.y,hi.z},{hi.x,hi.y,hi.z},{hi.x,hi.y,lo.z},material);
}
Scene fallback_scene(const Scene& original) {
    Scene scene;scene.sky_top=original.sky_top;scene.sky_bottom=original.sky_bottom;
    scene.materials.resize(3);scene.materials[0].base_color={0.68f,0.69f,0.7f,1};
    scene.materials[1].base_color={0.68f,0.19f,0.08f,1};scene.materials[2].base_color={0.07f,0.38f,0.58f,1};
    Mesh mesh;mesh.name="SH PRT comparison stage";
    append_quad(mesh,{-3,0,-3},{-3,0,3},{3,0,3},{3,0,-3},0);
    append_box(mesh,{-1.1f,0,-0.7f},{-0.2f,1.6f,0.5f},1);
    append_box(mesh,{0.25f,0,-0.3f},{1.1f,0.8f,0.65f},2);
    scene.meshes.push_back(std::move(mesh));return scene;
}
glm::vec3 geometric_normal(const Triangle& t) {
    return safe_normalize(glm::cross(t.vertices[1].position-t.vertices[0].position,t.vertices[2].position-t.vertices[0].position));
}
glm::vec3 interpolated_normal(const Triangle& t,glm::vec3 bary,float orientation) {
    auto n=safe_normalize(t.vertices[0].normal*bary.x+t.vertices[1].normal*bary.y+t.vertices[2].normal*bary.z,geometric_normal(t));
    if(glm::dot(n,geometric_normal(t))<0)n=-n;
    return n*orientation;
}
void fill_depth(SurfaceSample& surface,const CameraFrame& frame,const glm::mat4& vp) {
    const auto clip=vp*glm::vec4(surface.position,1);surface.depth=clip.z/clip.w;
    surface.linear_depth=glm::dot(surface.position-frame.camera.position,frame.forward);
}
glm::vec3 debug_color(const RenderOutput& output,int x,int y,const Settings& settings,glm::vec3 radiance) {
    const auto& surface=output.surfaces.at(x,y);
    switch(settings.debug) {
    case DebugView::albedo:return surface.albedo;
    case DebugView::normal:return surface.normal*0.5f+glm::vec3(0.5f);
    case DebugView::depth:return glm::vec3(surface.depth);
    case DebugView::roughness:return glm::vec3(surface.roughness);
    case DebugView::metallic:return glm::vec3(surface.metallic);
    case DebugView::ao:return glm::vec3(output.ao.at(x,y));
    case DebugView::shadow:return glm::vec3(output.shadow.at(x,y));
    case DebugView::indirect:return output.indirect.at(x,y);
    case DebugView::motion:return glm::vec3(0);
    case DebugView::variance:return glm::vec3(output.variance.at(x,y));
    case DebugView::light_count:return glm::vec3(0);
    default:return radiance;
    }
}
// Tiny dependency-free bitmap captions: the full Chinese guide is owned by the app.
std::array<unsigned,7> glyph(char c) {
    switch(c) {
    case 'A':return {14,17,17,31,17,17,17};case 'B':return {30,17,17,30,17,17,30};
    case 'C':return {14,17,16,16,16,17,14};case 'D':return {30,17,17,17,17,17,30};
    case 'E':return {31,16,16,30,16,16,31};case 'F':return {31,16,16,30,16,16,16};
    case 'G':return {14,17,16,23,17,17,15};case 'H':return {17,17,17,31,17,17,17};
    case 'I':return {14,4,4,4,4,4,14};case 'K':return {17,18,20,24,20,18,17};case 'L':return {16,16,16,16,16,16,31};
    case 'M':return {17,27,21,21,17,17,17};case 'N':return {17,25,25,21,19,19,17};
    case 'O':return {14,17,17,17,17,17,14};case 'P':return {30,17,17,30,16,16,16};
    case 'R':return {30,17,17,30,20,18,17};case 'S':return {15,16,16,14,1,1,30};
    case 'T':return {31,4,4,4,4,4,4};case 'U':return {17,17,17,17,17,17,14};
    case 'V':return {17,17,17,17,17,10,4};case 'Y':return {17,17,10,4,4,4,4};
    case '0':return {14,17,19,21,25,17,14};case '1':return {4,12,4,4,4,4,14};
    case '2':return {14,17,1,2,4,8,31};case '3':return {30,1,1,14,1,1,30};
    case '4':return {2,6,10,18,31,2,2};case '5':return {31,16,16,30,1,1,30};
    case '6':return {14,16,16,30,17,17,14};case '7':return {31,1,2,4,8,8,8};
    case '8':return {14,17,17,14,17,17,14};case '9':return {14,17,17,15,1,1,14};
    case '.':return {0,0,0,0,0,4,4};case '/':return {1,1,2,4,8,16,16};
    case '=':return {0,0,31,0,31,0,0};default:return {};
    }
}
void caption(Image<glm::vec3>& image,int x,int y,int width,const std::string& text) {
    const int scale=image.width>=960?2:1,height=9*scale;
    for(int yy=y;yy<std::min(y+height,image.height);++yy)for(int xx=x;xx<std::min(x+width,image.width);++xx)
        image.at(xx,yy)=glm::vec3(0.012f,0.018f,0.025f);
    int pen=x+2*scale;
    for(char c:text) {
        if(pen+5*scale>=x+width)break;
        const auto rows=glyph(c);
        for(int gy=0;gy<7;++gy)for(int gx=0;gx<5;++gx)if(rows[gy]&(1u<<(4-gx)))
            for(int dy=0;dy<scale;++dy)for(int dx=0;dx<scale;++dx) {
                const int px=pen+gx*scale+dx,py=y+scale+gy*scale+dy;
                if(px>=0&&py>=0&&px<image.width&&py<image.height)image.at(px,py)={0.72f,0.86f,0.96f};
            }
        pen+=6*scale;
    }
}
struct PrtLattice {
    int divisions=1;float orientation=1;
    std::vector<DiffusePrt> transfer;
    std::size_t index(int i,int j) const { return std::size_t(i*(divisions+1)-i*(i-1)/2+j); }
    DiffusePrt sample(glm::vec3 bary) const {
        const float u=sat(bary.y)*divisions,v=sat(bary.z)*divisions;
        int i=std::min(int(u),divisions),j=std::min(int(v),divisions-i);
        if(i+j>=divisions)return transfer[index(i,j)];
        const float a=u-i,b=v-j;std::array<std::size_t,3> ids;glm::vec3 weights;
        if(a+b<=1||i+j>=divisions-1) { ids={index(i,j),index(i+1,j),index(i,j+1)};weights={1-a-b,a,b}; }
        else { ids={index(i+1,j),index(i+1,j+1),index(i,j+1)};weights={1-b,a+b-1,1-a}; }
        DiffusePrt result;for(int k=0;k<9;++k)result.transfer[k]=transfer[ids[0]].transfer[k]*weights.x
            +transfer[ids[1]].transfer[k]*weights.y+transfer[ids[2]].transfer[k]*weights.z;return result;
    }
};

void render_sh_prt(RenderOutput& output,int topic,const Scene& original,Camera camera,const Settings& settings,std::atomic<bool>* cancel) {
    Scene fallback;const Scene* scene=&original;auto triangles=flatten_scene(original);
    if(triangles.empty()) { fallback=fallback_scene(original);scene=&fallback;triangles=flatten_scene(*scene);camera=demo_camera(camera); }
    if(triangles.size()>100000)throw std::invalid_argument("SH/PRT visual reference supports at most 100000 triangles");
    const Bvh bvh(triangles);output.triangles=triangles.size();const CameraFrame frame(camera);
    const int samples=std::clamp(settings.samples,1,32)*16;
    const auto env=[&](glm::vec3 d){return environment(*scene,d);};
    const auto coefficients=project_sh9(env,std::clamp(samples*8,256,4096));
    glm::vec3 lo(1e30f),hi(-1e30f);for(const auto& t:triangles)for(const auto& v:t.vertices){lo=glm::min(lo,v.position);hi=glm::max(hi,v.position);}
    const float spacing=std::max(0.01f,glm::length(hi-lo)/12),bias=std::max(1e-4f,settings.shadow_bias);
    std::vector<PrtLattice> prt;
    if(topic==11) {
        prt.resize(triangles.size());
        for(std::size_t ti=0;ti<triangles.size();++ti) {
            if(cancelled(cancel))return;
            const auto& t=triangles[ti];auto& lattice=prt[ti];
            float edge=0;for(int k=0;k<3;++k)edge=std::max(edge,glm::length(t.vertices[k].position-t.vertices[(k+1)%3].position));
            lattice.divisions=std::clamp(int(std::ceil(edge/spacing)),1,triangles.size()>2000?1:8);
            lattice.orientation=glm::dot(geometric_normal(t),camera.position-t.vertices[0].position)>=0?1.0f:-1.0f;
            const int n=lattice.divisions;lattice.transfer.resize(std::size_t(n+1)*(n+2)/2);
            for(int i=0;i<=n;++i)for(int j=0;j<=n-i;++j) {
                if(cancelled(cancel))return;
                const glm::vec3 bary(1-float(i+j)/n,float(i)/n,float(j)/n);
                const auto p=t.vertices[0].position*bary.x+t.vertices[1].position*bary.y+t.vertices[2].position*bary.z;
                // 传输先在三角形细分采样点烘焙一次；逐像素只插值 9 个系数。
                // 大三角形若只在三个角烘焙，会完全错过地面中央的接触遮挡。
                const auto visibility=[&](glm::vec3 origin,glm::vec3 direction) {
                    if(cancelled(cancel))return 0.0f;
                    TraversalStatistics stats;++output.rays;
                    const bool blocked=bvh.occluded({origin,direction,bias,1e30f},&stats);output.node_tests+=stats.box_tests;return blocked?0.0f:1.0f;
                };
                lattice.transfer[lattice.index(i,j)]=bake_diffuse_prt(p,interpolated_normal(t,bary,lattice.orientation),visibility,{std::clamp(samples,32,256),bias});
            }
        }
    }
    const int w=output.color.width,h=output.color.height;
    for(int panel=0;panel<2;++panel) {
        const int first=panel*w/2,last=(panel+1)*w/2,pw=last-first;if(pw==0)continue;
        const auto vp=frame.vp(float(pw)/h,settings.reversed_z);
        for(int y=0;y<h;++y) {
            if(cancelled(cancel))return;
            for(int x=first;x<last;++x) {
                const auto ray=frame.ray(x-first,y,pw,h);TraversalStatistics stats;++output.rays;
                const auto hit=bvh.intersect(ray,&stats);output.node_tests+=stats.box_tests;
                if(hit.triangle<0){output.color.at(x,y)=env(ray.direction);continue;}
                const auto& t=triangles[std::size_t(hit.triangle)];auto& surface=output.surfaces.at(x,y);
                surface.valid=true;surface.position=ray.origin+ray.direction*hit.t;surface.material=t.material;surface.object=t.object;
                const float orientation=glm::dot(geometric_normal(t),-ray.direction)>=0?1.0f:-1.0f;
                surface.normal=interpolated_normal(t,hit.barycentric,orientation);
                surface.uv=t.vertices[0].uv*hit.barycentric.x+t.vertices[1].uv*hit.barycentric.y+t.vertices[2].uv*hit.barycentric.z;
                surface.vertex_color=t.vertices[0].color*hit.barycentric.x+t.vertices[1].color*hit.barycentric.y+t.vertices[2].color*hit.barycentric.z;
                surface.tangent=t.vertices[0].tangent*hit.barycentric.x+t.vertices[1].tangent*hit.barycentric.y+t.vertices[2].tangent*hit.barycentric.z;
                const auto material=sample_material(*scene,surface,settings);surface.albedo=glm::vec3(material.base_color);
                surface.roughness=material.roughness;surface.metallic=material.metallic;fill_depth(surface,frame,vp);
                const glm::vec3 albedo=glm::clamp(surface.albedo,glm::vec3(0),glm::vec3(1))*(1-sat(surface.metallic));glm::vec3 indirect(0);
                if(topic==10&&panel==0) {
                    const auto basis=shading_frame(surface.normal);
                    for(int i=0;i<samples;++i)indirect+=env(basis.world(sample_cosine_hemisphere(hammersley(std::uint32_t(i),std::uint32_t(samples))).direction));
                    indirect*=albedo/float(samples);
                } else if(topic==11&&panel==1) {
                    const auto transfer=prt[std::size_t(hit.triangle)].sample(hit.barycentric);
                    indirect=evaluate_diffuse_prt(transfer,coefficients,albedo);
                    output.shadow.at(x,y)=output.ao.at(x,y)=sat(transfer.transfer[0].x/(pi*0.2820947918f));
                } else indirect=evaluate_sh9_irradiance(coefficients,surface.normal)*albedo/pi;
                output.indirect.at(x,y)=glm::max(indirect,glm::vec3(0));
                output.color.at(x,y)=debug_color(output,x,y,settings,output.indirect.at(x,y)+material.emissive);
            }
        }
    }
    caption(output.color,0,0,w/2,topic==10?"ENV INTEGRAL":"SH NO OCCLUSION");
    caption(output.color,w/2,0,w-w/2,topic==10?"SH9":"PRT BVH VISIBILITY");
}

glm::vec3 latlong_direction(float u,float v) {
    const float theta=pi*v,phi=2*pi*u;return {std::sin(theta)*std::cos(phi),std::cos(theta),std::sin(theta)*std::sin(phi)};
}
void render_ibl_atlas(RenderOutput& output,const Scene& scene,const Settings& settings,std::atomic<bool>* cancel) {
    const auto key=safe_normalize(glm::vec3(-0.3f,0.65f,0.4f)),fill=safe_normalize(glm::vec3(0.8f,0.15f,-0.4f));
    const auto env=[&](glm::vec3 direction) {
        if(cancelled(cancel))return glm::vec3(0);
        // 梯度天光没有高频，难以观察预过滤。只在 D13 叠加已声明的解析 HDR 亮斑。
        return environment(scene,direction)+glm::vec3(12,9,5)*std::pow(std::max(0.0f,glm::dot(direction,key)),128.0f)
            +glm::vec3(0.4f,1,4)*std::pow(std::max(0.0f,glm::dot(direction,fill)),32.0f);
    };
    IblOptions options;options.width=64;options.height=32;options.roughness_levels=6;options.lut_resolution=32;
    options.samples=std::clamp(settings.samples,8,32)*32;
    const auto ibl=precompute_ibl(env,options);if(cancelled(cancel))return;
    const int w=output.color.width,h=output.color.height;
    const std::array<std::string,9> labels={"ENV HDR","DIFFUSE / PI","BRDF A / B","R=0.0","R=0.2","R=0.4","R=0.6","R=0.8","R=1.0"};
    for(int tile=0;tile<9;++tile) {
        const int x0=(tile%3)*w/3,x1=(tile%3+1)*w/3,y0=(tile/3)*h/3,y1=(tile/3+1)*h/3;
        for(int y=y0;y<y1;++y) {
            if(cancelled(cancel))return;
            for(int x=x0;x<x1;++x) {
                const float u=float(x-x0+0.5f)/std::max(1,x1-x0),v=float(y-y0+0.5f)/std::max(1,y1-y0);
                const auto direction=latlong_direction(u,v);glm::vec3 color;
                if(tile==0)color=env(direction);
                else if(tile==1)color=sample_ibl_diffuse(ibl,direction)/pi;
                else if(tile==2) { const auto ab=sample_brdf_lut(ibl,u,v);color={ab.x,ab.y,0}; }
                else color=sample_ibl_specular(ibl,direction,float(tile-3)/5);
                output.color.at(x,y)=output.indirect.at(x,y)=glm::max(color,glm::vec3(0));
            }
        }
        caption(output.color,x0,y0,x1-x0,labels[std::size_t(tile)]);
    }
}

float demo_sdf(glm::vec3 p) {
    const float sphere=sdf_sphere(p,{-0.65f,0.58f,0},0.58f);
    const float box=sdf_box(p,{0.72f,0.5f,0.05f},{0.48f,0.5f,0.48f});
    const float floor=sdf_box(p,{0,-0.08f,0},{2.8f,0.08f,2.8f});return std::min({sphere,box,floor});
}
std::pair<int,glm::vec3> sdf_material(glm::vec3 p) {
    const float sphere=std::abs(sdf_sphere(p,{-0.65f,0.58f,0},0.58f));
    const float box=std::abs(sdf_box(p,{0.72f,0.5f,0.05f},{0.48f,0.5f,0.48f}));
    const float floor=std::abs(sdf_box(p,{0,-0.08f,0},{2.8f,0.08f,2.8f}));
    if(sphere<std::min(box,floor))return {1,{0.08f,0.43f,0.63f}};
    if(box<floor)return {2,{0.73f,0.24f,0.06f}};
    const int checker=int(std::floor(p.x*1.6f))+int(std::floor(p.z*1.6f));
    return {0,glm::vec3((checker&1)?0.46f:0.69f)};
}
void render_sdf(RenderOutput& output,const Scene& scene,Camera camera,const Settings& settings,std::atomic<bool>* cancel) {
    camera=demo_camera(camera);const CameraFrame frame(camera);
    BakedSdfGrid grid({-3.2f,-0.3f,-3.2f},{3.2f,2,3.2f},std::clamp(settings.voxel_resolution,16,48));
    grid.bake([&](glm::vec3 p){return cancelled(cancel)?10.0f:demo_sdf(p);});if(cancelled(cancel))return;
    const DistanceFunction analytic=demo_sdf,discrete=[&](glm::vec3 p){return grid.sample(p);};
    Light light;if(scene.lights.empty()){light.direction={-0.6f,-1,-0.4f};light.intensity=3.5f;light.color={1,0.91f,0.79f};}
    else light=scene.lights.front();
    const auto env=[&](glm::vec3 d){return environment(scene,d);};const auto env_sh=project_sh9(env,512);
    const int w=output.color.width,h=output.color.height;const float epsilon=0.0008f,bias=std::max(0.003f,settings.shadow_bias);
    for(int panel=0;panel<2;++panel) {
        const int first=panel*w/2,last=(panel+1)*w/2,pw=last-first;if(pw==0)continue;
        const auto vp=frame.vp(float(pw)/h,settings.reversed_z);
        const DistanceFunction& field=panel?discrete:analytic;
        // 网格在域外以边界值加距离延拓；L+1 保守覆盖此延拓的梯度。
        const float bound=panel?grid.lipschitz_bound()+1:1;
        for(int y=0;y<h;++y) {
            if(cancelled(cancel))return;
            for(int x=first;x<last;++x) {
                const auto ray=frame.ray(x-first,y,pw,h);++output.rays;
                const auto hit=panel?grid.trace(ray,epsilon,512):sphere_trace(field,ray,epsilon,256,1);
                if(!hit.hit){output.color.at(x,y)=env(ray.direction);continue;}
                auto& surface=output.surfaces.at(x,y);surface.valid=true;surface.position=hit.position;surface.normal=hit.normal;
                const auto material=sdf_material(hit.position);surface.object=surface.material=material.first;surface.albedo=material.second;
                surface.roughness=material.first==1?0.24f:0.65f;fill_depth(surface,frame,vp);
                glm::vec3 direct(0);float visibility=1;
                const int count=light.kind==LightKind::rectangle?8:1;float visibility_sum=0;
                for(int sample=0;sample<count;++sample) {
                    glm::vec3 l;float distance=30;glm::vec3 incident=glm::max(light.color,glm::vec3(0))*std::max(0.0f,light.intensity);
                    if(light.kind==LightKind::directional)l=-safe_normalize(light.direction);
                    else {
                        glm::vec3 emitter=light.position;
                        if(light.kind==LightKind::rectangle) {
                            const auto basis=shading_frame(light.direction);const auto u=hammersley(std::uint32_t(sample),std::uint32_t(count));
                            emitter+=basis.t*((u.x-0.5f)*light.size.x)+basis.b*((u.y-0.5f)*light.size.y);
                        }
                        const auto delta=emitter-hit.position;distance=glm::length(delta);l=delta/std::max(distance,1e-5f);
                        if(distance>light.range)incident=glm::vec3(0);else incident/=std::max(distance*distance,1e-4f);
                        if(light.kind==LightKind::rectangle)incident*=std::abs(light.size.x*light.size.y)*std::max(0.0f,glm::dot(safe_normalize(light.direction),-l));
                    }
                    ++output.rays;const float softness=settings.shadows==ShadowMode::hard?100000.0f:1/std::max(0.01f,settings.light_size);
                    visibility=sdf_soft_shadow(field,{hit.position+hit.normal*bias,l,bias,std::max(bias,distance-bias)},softness,epsilon,256,bound);
                    visibility_sum+=visibility;const float cosine=std::max(0.0f,glm::dot(hit.normal,l));
                    direct+=surface.albedo*incident*(cosine*visibility/(pi*count));
                }
                output.shadow.at(x,y)=visibility_sum/count;
                output.indirect.at(x,y)=glm::max(evaluate_sh9_irradiance(env_sh,hit.normal),glm::vec3(0))*surface.albedo/pi;
                output.color.at(x,y)=debug_color(output,x,y,settings,direct+output.indirect.at(x,y));
            }
        }
    }
    caption(output.color,0,0,w/2,"ANALYTIC SDF");caption(output.color,w/2,0,w-w/2,"BAKED GRID SDF");
}
}

bool supports_visual_experiment(int topic) { return topic==10||topic==11||topic==12||topic==29; }
std::string visual_experiment_guide(int topic) {
    switch(topic) {
    case 10:return "D11 球谐环境光：左侧为余弦采样积分，右侧为 9 系数 SH 余弦卷积；同一相机、几何与材质，无几何遮挡。调整天光颜色和 samples 比较近似误差。SH 是低频表示，不能恢复尖锐高光。空场景自动使用内置演示。";
    case 11:return "D12 漫反射 PRT：左侧为无遮挡 SH，右侧为 BVH 可见性烘焙的 9 系数传输。大三角形细分采样后逐像素插值；shadow/ao 视图显示余弦加权可见率。调整天光可重新照明，几何变动需重烘焙。这里只含静态漫反射直达传输，不含光泽或多次反弹。";
    case 12:return "D13 IBL 九宫格：上排依次为环境、漫反射辐照度/pi、BRDF LUT（红=A，绿=B；横轴 N·V，纵轴向下粗糙度增加）。下两排为粗糙度 0/0.2/0.4/0.6/0.8/1 的 GGX 预过滤。天光叠加两枚内置 HDR 亮斑，用于观察高频被展宽；非普通颜色 Mip。samples 控制积分质量，输出仍为线性 HDR。";
    case 29:return "D30 SDF：左侧解析球/盒/地板，右侧同场景离散 SDF；两侧均使用真正的 sphere tracing、梯度法线和距离场阴影。相机自动对准 (0,0.5,0) 并保留轨道方向；使用第一盏场景灯，无灯时补方向光。voxel_resolution 控制烘焙网格，shadow 显示阴影，normal/depth 显示命中数据。离散表面有插值偏差，软阴影为经验近似。";
    default:throw std::invalid_argument("No visual experiment for zero-based topic "+std::to_string(topic)+"; supported: 10,11,12,29");
    }
}
RenderOutput run_visual_experiment(int topic,const Scene& scene,Camera camera,Settings settings,std::atomic<bool>* cancel) {
    if(!supports_visual_experiment(topic))throw std::invalid_argument("No visual experiment for zero-based topic "+std::to_string(topic)+"; supported: 10,11,12,29");
    if(settings.render_width<=0||settings.render_height<=0||std::uint64_t(settings.render_width)*std::uint64_t(settings.render_height)>4194304ull)
        throw std::invalid_argument("Visual experiment dimensions must be positive and <=4M pixels");
    if(!finite(camera.position)||!finite(camera.target)||!std::isfinite(camera.fov)||camera.fov<=1||camera.fov>=175||
        !std::isfinite(camera.near_plane)||!std::isfinite(camera.far_plane)||camera.near_plane<=0||camera.far_plane<=camera.near_plane)
        throw std::invalid_argument("Invalid visual experiment camera");
    const auto started=std::chrono::steady_clock::now();auto output=cleared_frame(settings);
    if(!cancelled(cancel)) {
        if(topic==10||topic==11)render_sh_prt(output,topic,scene,camera,settings,cancel);
        else if(topic==12)render_ibl_atlas(output,scene,settings,cancel);
        else render_sdf(output,scene,camera,settings,cancel);
    }
    output.cpu_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();return output;
}
} // namespace emberframe::lab
