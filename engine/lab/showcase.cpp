#include "showcase.h"
#include "environment.h"
#include "editor_lights.h"
#include "geometry.h"
#include <cmath>

namespace emberframe::lab {
namespace {
constexpr std::array<ShowcaseInfo,3> catalog{{
    {ShowcaseKind::materials,"materials","材质 · Atelier","观察金属/粗糙度、法线细节、清漆与环境高光；切换 KC 比较粗糙金属的能量。"},
    {ShowcaseKind::interior,"interior","室内 · Warm Gallery","观察面积光接触阴影与彩色墙面反弹；对照关闭 GI / LPV，保持相机和曝光不变。"},
    {ShowcaseKind::many_objects,"many-objects","复杂场景 · Light Arcade","观察共享网格实例、近远层次和 32 盏局部灯；对照 All / Tiled / Clustered 的 GPU 成本。"}
}};
glm::vec3 direction(float u,float v) {
    const float theta=pi*v,phi=2*pi*u;return {std::sin(theta)*std::cos(phi),std::cos(theta),std::sin(theta)*std::sin(phi)};
}
std::shared_ptr<const EnvironmentMap> studio_environment() {
    // 固定软箱直接写入真实线性 HDR；解析天空保留为可选后备，没有网络资源依赖。
    static const auto map=[] {
        Image<glm::vec3> image(256,128);
        const glm::vec3 key=safe_normalize(glm::vec3(-.8f,.5f,.2f)),rim=safe_normalize(glm::vec3(.65f,.2f,-.7f));
        for(int y=0;y<image.height;++y)for(int x=0;x<image.width;++x) {
            const auto d=direction((x+.5f)/image.width,(y+.5f)/image.height);
            const float horizon=std::pow(std::max(0.f,1-std::abs(d.y)),4.f);
            const float a=std::pow(std::max(0.f,glm::dot(d,key)),96.f),b=std::pow(std::max(0.f,glm::dot(d,rim)),48.f);
            image.at(x,y)=glm::mix(glm::vec3(.035f,.045f,.06f),glm::vec3(.16f,.2f,.28f),d.y*.5f+.5f)
                +glm::vec3(.08f,.065f,.05f)*horizon+glm::vec3(22,18,13)*a+glm::vec3(5,9,16)*b;
        }
        return make_environment_map(std::move(image),"EmberFrame · procedural softbox HDR");
    }();return map;
}
int material(Scene& scene,std::string name,glm::vec3 color,float roughness=.5f,float metallic=0) {
    Material m;m.name=std::move(name);m.base_color=glm::vec4(color,1);m.roughness=roughness;m.metallic=metallic;
    const int index=int(scene.materials.size());scene.materials.push_back(std::move(m));return index;
}
enum class Pattern { marble,wood,fabric };
void texture_material(Scene& scene,int index,Pattern pattern) {
    Texture color,mr,normal;color.name="程序化 · 色彩";color.srgb=true;mr.name="程序化 · 金属粗糙度";normal.name="程序化 · 法线";
    color.levels.emplace_back(128,128);mr.levels.emplace_back(128,128);normal.levels.emplace_back(128,128);
    for(int y=0;y<128;++y)for(int x=0;x<128;++x) {
        const float u=(x+.5f)/128,v=(y+.5f)/128;
        float shade=1,rough=1,du=0,dv=0;
        if(pattern==Pattern::marble) {
            const float phase=2*pi*(u*3+v*2)+.65f*std::sin(2*pi*v*4),vein=std::pow(.5f+.5f*std::sin(phase),12.f);
            shade=1-.25f*vein;rough=.75f+.2f*vein;du=.08f*std::cos(phase);dv=.06f*std::cos(phase);
        }else if(pattern==Pattern::wood) {
            const float phase=2*pi*(v*12)+.5f*std::sin(2*pi*u*2);
            shade=.8f+.2f*std::sin(phase);rough=.8f+.15f*std::cos(phase);du=.025f*std::cos(2*pi*u*2);dv=.12f*std::cos(phase);
        }else {
            const float a=std::sin(2*pi*u*16),b=std::sin(2*pi*v*16);
            shade=.93f+.07f*a*b;rough=.94f;du=.09f*std::cos(2*pi*u*16)*b;dv=.09f*a*std::cos(2*pi*v*16);
        }
        // 色彩先按线性值生成，再编码到 sRGB；MR/法线保持线性，不能混用解释。
        const glm::vec3 c(shade);color.levels[0].at(x,y)={linear_to_srgb(c.r),linear_to_srgb(c.g),linear_to_srgb(c.b),1};
        mr.levels[0].at(x,y)={1,rough,1,1};normal.levels[0].at(x,y)=glm::vec4(safe_normalize(glm::vec3(-du,-dv,1),{0,0,1})*.5f+.5f,1);
    }
    build_mips(color);build_mips(mr);build_normal_mips(normal);
    auto& m=scene.materials.at(index);m.base_texture=int(scene.textures.size());scene.textures.push_back(std::move(color));
    m.mr_texture=int(scene.textures.size());scene.textures.push_back(std::move(mr));
    m.normal_texture=int(scene.textures.size());scene.textures.push_back(std::move(normal));m.normal_scale=.65f;
}
Mesh rounded_box(std::string name,int mat,int divisions=10,float bevel=.075f) {
    Mesh mesh;mesh.name=std::move(name);
    const glm::vec3 normals[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for(auto n:normals) {
        const auto u=safe_normalize(glm::cross(std::abs(n.y)>.5f?glm::vec3(0,0,1):glm::vec3(0,1,0),n));
        const auto v=glm::cross(n,u);const auto first=std::uint32_t(mesh.vertices.size());
        auto position=[&](float a,float b) {
            const auto p=n*.5f+u*(a-.5f)+v*(b-.5f),inner=glm::clamp(p,glm::vec3(-.5f+bevel),glm::vec3(.5f-bevel));
            return inner+safe_normalize(p-inner,n)*bevel;
        };
        for(int y=0;y<=divisions;++y)for(int x=0;x<=divisions;++x) {
            const float a=float(x)/divisions,b=float(y)/divisions;
            const auto p=n*.5f+u*(a-.5f)+v*(b-.5f),inner=glm::clamp(p,glm::vec3(-.5f+bevel),glm::vec3(.5f-bevel));
            Vertex vertex;vertex.position=position(a,b);vertex.normal=safe_normalize(p-inner,n);vertex.uv={a,b};
            const auto tangent=position(std::min(1.f,a+.001f),b)-position(std::max(0.f,a-.001f),b);
            vertex.tangent=glm::vec4(safe_normalize(tangent-vertex.normal*glm::dot(vertex.normal,tangent),u),1);
            mesh.vertices.push_back(vertex);
        }
        for(int y=0;y<divisions;++y)for(int x=0;x<divisions;++x) {
            const auto a=first+std::uint32_t(y*(divisions+1)+x),b=a+divisions+1;
            mesh.indices.insert(mesh.indices.end(),{a,a+1,b,a+1,b+1,b});
        }
    }
    mesh.primitives.push_back({0,std::uint32_t(mesh.indices.size()),std::uint32_t(mat)});return mesh;
}
Mesh sphere(std::string name,int mat,int rows=48,int columns=72) {
    Mesh mesh;mesh.name=std::move(name);
    for(int y=1;y<rows;++y)for(int x=0;x<=columns;++x) {
        const double theta=double(pi)*y/rows,phi=x==columns?0.:double(2*pi)*x/columns;
        Vertex v;v.normal=glm::vec3(std::sin(theta)*std::cos(phi),std::cos(theta),std::sin(theta)*std::sin(phi));
        v.position=v.normal*.5f;v.uv={float(x)/columns,float(y)/rows};v.tangent={float(-std::sin(phi)),0,float(std::cos(phi)),1};mesh.vertices.push_back(v);
    }
    for(int y=0;y<rows-2;++y)for(int x=0;x<columns;++x) {
        const auto a=std::uint32_t(y*(columns+1)+x),b=a+columns+1;mesh.indices.insert(mesh.indices.end(),{a,a+1,b,a+1,b+1,b});
    }
    for(int pole=0;pole<2;++pole)for(int x=0;x<columns;++x) {
        const float u=(x+.5f)/columns,phi=2*pi*u;Vertex v;v.normal={0,pole?-1.f:1.f,0};v.position=v.normal*.5f;
        v.uv={u,float(pole)};v.tangent={-std::sin(phi),0,std::cos(phi),1};const auto tip=std::uint32_t(mesh.vertices.size());mesh.vertices.push_back(v);
        const auto ring=std::uint32_t((pole?rows-2:0)*(columns+1)+x);
        if(pole)mesh.indices.insert(mesh.indices.end(),{ring,ring+1,tip});else mesh.indices.insert(mesh.indices.end(),{tip,ring+1,ring});
    }
    mesh.primitives.push_back({0,std::uint32_t(mesh.indices.size()),std::uint32_t(mat)});return mesh;
}
Mesh vessel(int mat) {
    Mesh mesh;mesh.name="细分陶瓷花器";constexpr int columns=80;
    std::vector<glm::vec2> profile{{0,0},{.35f,0}};
    for(int y=0;y<=48;++y) {
        const float t=float(y)/48,r=.35f+.2f*std::sin(pi*t)-.14f*std::exp(-std::pow((t-.78f)*6,2.f));
        profile.push_back({r,t*1.8f+.015f});
    }
    profile.push_back({0,1.815f});
    for(std::size_t y=0;y<profile.size();++y)for(int x=0;x<=columns;++x) {
        const auto delta=profile[std::min(y+1,profile.size()-1)]-profile[y?y-1:0];const double phi=x==columns?0.:double(2*pi)*x/columns;
        const float c=float(std::cos(phi)),s=float(std::sin(phi));Vertex v;v.position={profile[y].x*c,profile[y].y,profile[y].x*s};
        v.normal=safe_normalize(glm::vec3(delta.y*c,-delta.x,delta.y*s));v.tangent={-s,0,c,-1};
        v.uv={float(x)/columns,float(y)/(profile.size()-1)};mesh.vertices.push_back(v);
    }
    for(std::size_t y=0;y+1<profile.size();++y)for(int x=0;x<columns;++x) {
        const auto a=std::uint32_t(y*(columns+1)+x),b=a+columns+1;
        if(profile[y].x>0)mesh.indices.insert(mesh.indices.end(),{a,b,a+1});
        if(profile[y+1].x>0)mesh.indices.insert(mesh.indices.end(),{a+1,b,b+1});
    }
    mesh.primitives.push_back({0,std::uint32_t(mesh.indices.size()),std::uint32_t(mat)});return mesh;
}
Mesh torus(int mat) {
    Mesh mesh;mesh.name="拉丝金属环";constexpr int columns=96,rows=24;
    for(int y=0;y<=rows;++y)for(int x=0;x<=columns;++x) {
        const float a=x==columns?0.f:2*pi*x/columns,b=y==rows?0.f:2*pi*y/rows;
        Vertex v;v.normal={std::cos(b)*std::cos(a),std::sin(b),std::cos(b)*std::sin(a)};
        v.position={(.65f+.14f*std::cos(b))*std::cos(a),.14f*std::sin(b),(.65f+.14f*std::cos(b))*std::sin(a)};
        v.uv={float(x)/columns,float(y)/rows};v.tangent={-std::sin(a),0,std::cos(a),-1};mesh.vertices.push_back(v);
    }
    for(int y=0;y<rows;++y)for(int x=0;x<columns;++x) {
        const auto a=std::uint32_t(y*(columns+1)+x),b=a+columns+1;mesh.indices.insert(mesh.indices.end(),{a,b,a+1,a+1,b,b+1});
    }
    mesh.primitives.push_back({0,std::uint32_t(mesh.indices.size()),std::uint32_t(mat)});return mesh;
}
int mesh(Scene& scene,Mesh value) {const int index=int(scene.meshes.size());scene.meshes.push_back(std::move(value));return index;}
int instance(Scene& scene,int resource,std::string name,glm::vec3 position,glm::vec3 scale={1,1,1},float rotation=0,int parent=-1) {
    Node node;node.name=std::move(name);node.mesh=resource;node.parent=parent;
    node.local=glm::translate(glm::mat4(1),position)*glm::rotate(glm::mat4(1),rotation,{0,1,0})*glm::scale(glm::mat4(1),scale);
    const int index=int(scene.nodes.size());scene.nodes.push_back(std::move(node));return index;
}
void area(Scene& scene,glm::vec3 position,glm::vec3 direction,glm::vec2 size,glm::vec3 color,float intensity) {
    const int index=add_editor_light(scene,LightKind::rectangle);auto light=scene.lights.at(index);
    light.position=position;light.direction=direction;light.size=size;light.color=color;light.intensity=intensity;update_editor_light(scene,index,light);
}
void materials(ProjectDocument& project) {
    auto& scene=project.scene;scene.name="Atelier · material showcase";scene.environment_map=studio_environment();scene.environment_rotation=24;
    const int stone=material(scene,"白色石材",{.55f,.58f,.62f},.45f),dark=material(scene,"深色背景",{.025f,.03f,.045f},.7f);
    texture_material(scene,stone,Pattern::marble);
    instance(scene,mesh(scene,rounded_box("石材台面",stone)),"展台",{0,-.2f,0},{8,.35f,5});
    instance(scene,mesh(scene,rounded_box("背景板",dark)),"背景",{0,2,-2.8f},{9,5,.25f});
    const int pedestal=mesh(scene,rounded_box("共享圆角展座",dark));
    for(int row=0;row<2;++row)for(int col=0;col<4;++col) {
        const int mat=material(scene,row?"金属 · 粗糙度阶梯":"非金属 · 粗糙度阶梯",row?glm::vec3(.92f,.55f,.23f):glm::vec3(.035f,.3f,.5f),.1f+col*.24f,float(row));
        const glm::vec3 p{(col-1.5f)*1.55f,.68f,row?-.8f:.8f};
        instance(scene,mesh(scene,sphere("高细分材质球",mat)),"材质球 "+std::to_string(row*4+col+1),p,{1.05f,1.05f,1.05f});
        instance(scene,pedestal,"展座",{p.x,.065f,p.z},{1.25f,.18f,1.25f});
    }
    const int ceramic=material(scene,"釉面陶瓷",{.62f,.08f,.035f},.24f);scene.materials[ceramic].clearcoat=.8f;scene.materials[ceramic].clearcoat_roughness=.12f;
    instance(scene,mesh(scene,vessel(ceramic)),"釉面花器",{-3.1f,0,.05f},{.8f,.8f,.8f});
    const int brass=material(scene,"黄铜",{.83f,.59f,.22f},.2f,1);
    instance(scene,mesh(scene,torus(brass)),"金属环",{3.1f,.4f,.2f},{.85f,.85f,.85f});
    area(scene,{-3,4,3},safe_normalize(glm::vec3(3,-4,-3)),{3,2},{1,.85f,.68f},4);
    project.camera.position={6.9f,3.9f,8.8f};project.camera.target={0,.65f,0};project.camera.fov=42;
    project.settings.gi=GiMode::environment;project.settings.shading=ShadingMode::disney;project.settings.exposure=.85f;
}
void interior(ProjectDocument& project) {
    auto& scene=project.scene;scene.name="Warm Gallery · interior showcase";scene.sky_top=scene.sky_bottom=glm::vec3(0);
    const int plaster=material(scene,"暖白灰泥",{.68f,.62f,.52f},.8f),blue=material(scene,"蓝色墙面",{.035f,.16f,.34f},.8f);
    const int clay=material(scene,"赭红墙面",{.53f,.12f,.045f},.75f),wood=material(scene,"木质地板",{.36f,.18f,.075f},.55f);
    texture_material(scene,wood,Pattern::wood);
    instance(scene,mesh(scene,rounded_box("木地板",wood,8,.01f)),"地板",{0,-.12f,0},{7,.2f,7});
    instance(scene,mesh(scene,rounded_box("蓝墙",blue,6,.01f)),"左墙",{-3.4f,1.7f,-.2f},{.16f,3.6f,6.6f});
    instance(scene,mesh(scene,rounded_box("红墙",clay,6,.01f)),"右墙",{3.4f,1.7f,-.2f},{.16f,3.6f,6.6f});
    const int white_box=mesh(scene,rounded_box("灰泥构件",plaster,8,.025f));
    instance(scene,white_box,"背墙",{0,1.7f,-3.4f},{7,3.6f,.16f});
    instance(scene,white_box,"顶部梁",{0,3.4f,-.3f},{7,.22f,6.5f});
    instance(scene,white_box,"低台",{-.95f,.22f,-.3f},{2.1f,.45f,2},.12f);
    const int ceramic=material(scene,"象牙陶瓷",{.75f,.68f,.53f},.33f);scene.materials[ceramic].clearcoat=.35f;
    instance(scene,mesh(scene,vessel(ceramic)),"花器",{-1.15f,.46f,-.4f},{.9f,.9f,.9f},.3f);
    const int fabric=material(scene,"编织蓝布",{.09f,.22f,.32f},.86f);scene.materials[fabric].sheen=.35f;texture_material(scene,fabric,Pattern::fabric);
    instance(scene,mesh(scene,rounded_box("圆角布艺座",fabric,14,.16f)),"布艺座",{1.15f,.5f,.4f},{1.7f,1,1.6f},-.18f);
    const int metal=material(scene,"银色球",{.82f,.85f,.9f},.18f,1);
    instance(scene,mesh(scene,sphere("银色细分球",metal)),"银球",{.5f,.55f,-1.6f},{1.1f,1.1f,1.1f});
    area(scene,{-1.2f,3.15f,.15f},{0,-1,0},{2.4f,1.6f},{1,.82f,.61f},9);
    area(scene,{2.85f,2.2f,-1.6f},safe_normalize(glm::vec3(-1,-.1f,.3f)),{1.5f,1},{.53f,.73f,1},2);
    project.camera.position={.5f,1.85f,7.2f};project.camera.target={0,1.35f,-.75f};project.camera.fov=49;
    project.settings.gi=GiMode::lpv;project.settings.propagation_steps=5;project.settings.voxel_resolution=32;
    project.settings.shading=ShadingMode::disney;project.settings.exposure=.72f;
}
void many_objects(ProjectDocument& project) {
    auto& scene=project.scene;scene.name="Light Arcade · instance showcase";scene.environment_map=studio_environment();scene.environment_intensity=.35f;
    const int floor=material(scene,"暗色石材",{.09f,.105f,.13f},.55f);texture_material(scene,floor,Pattern::marble);
    instance(scene,mesh(scene,rounded_box("广场",floor,8,.02f)),"广场地面",{0,-.22f,0},{27,.4f,26});
    const int arch=material(scene,"建筑构件",{.18f,.2f,.24f},.5f),pillar=mesh(scene,rounded_box("共享圆角立柱",arch));
    for(int side:{-1,1})for(int z=0;z<6;++z) {
        instance(scene,pillar,"回廊立柱",{side*10.8f,2.5f,float(z)*3.5f-8.5f},{.5f,5,.6f});
        instance(scene,pillar,"横梁",{side*10.8f,5,float(z)*3.5f-6.75f},{.55f,.45f,4});
    }
    std::array<int,6> spheres{},boxes{};
    const glm::vec3 colors[]={{.03f,.24f,.35f},{.48f,.16f,.055f},{.19f,.34f,.2f},{.65f,.43f,.12f},{.4f,.08f,.18f},{.31f,.36f,.45f}};
    for(int i=0;i<6;++i) {
        const int mat=material(scene,"共享材质 "+std::to_string(i+1),colors[i],.15f+.12f*i,i%3==0?.9f:0);
        spheres[i]=mesh(scene,sphere("共享细分球",mat,24,36));boxes[i]=mesh(scene,rounded_box("共享圆角物体",mat,8,.1f));
    }
    // 层级只影响实例变换，资源仍由大量节点共享；初始 previous_world 在最后统一计算。
    for(int row=0;row<10;++row) {
        const int group=instance(scene,-1,"实例组 "+std::to_string(row+1),{0,0,(row-4.5f)*1.85f});
        for(int col=0;col<10;++col) {
            const int id=(row*3+col)%6;const float height=.9f+.25f*((row+col)%4);
            const bool ball=(row+col)%3==0;
            instance(scene,ball?spheres[id]:boxes[id],"实例 "+std::to_string(row*10+col+1),
                     {(col-4.5f)*1.85f,height*.5f,0},{1.15f,height,1.15f},.12f*(row+col),group);
        }
    }
    Light sun;sun.direction=safe_normalize(glm::vec3(-.4f,-1,-.2f));sun.intensity=.7f;sun.color={1,.92f,.8f};scene.lights.push_back(sun);
    for(int i=0;i<32;++i) {
        const float a=2*pi*i/32;Light light;light.kind=LightKind::point;
        light.position={9*std::cos(a),1.8f+(i%3)*.6f,9*std::sin(a)};light.range=5.8f;light.intensity=4;
        light.color=glm::mix(glm::vec3(1,.63f,.32f),glm::vec3(.24f,.55f,1),float(i%4)/3);scene.lights.push_back(light);
    }
    project.camera.position={17.8f,13.8f,20.5f};project.camera.target={0,1,0};project.camera.fov=47;project.camera.far_plane=120;
    project.settings.path=RenderPath::deferred;project.settings.gi=GiMode::environment;
    project.settings.culling=LightCulling::clustered;project.settings.exposure=.95f;
}
}
std::span<const ShowcaseInfo> showcase_catalog() noexcept {return catalog;}
ShowcaseKind parse_showcase(std::string_view id) {
    for(const auto& entry:catalog)if(entry.id==id)return entry.kind;
    throw std::invalid_argument("Showcase must be materials, interior or many-objects");
}
ProjectDocument make_showcase(ShowcaseKind kind) {
    ProjectDocument result;auto& settings=result.settings;
    settings.render_width=1280;settings.render_height=720;settings.filter=FilterMode::anisotropic;
    settings.shadows=ShadowMode::pcf;settings.shadow_resolution=1024;settings.shadow_bias=.001f;
    settings.ao=AoMode::gtao;settings.ao_radius=.4f;settings.ao_strength=.65f;
    settings.energy_compensation=true;settings.bloom=true;settings.bloom_strength=.04f;
    // 时间域接入尚由主程序负责；默认不开不完整历史，截图和首帧表现保持确定性。
    settings.taa=false;settings.samples=16;
    switch(kind) {
        case ShowcaseKind::materials:materials(result);break;
        case ShowcaseKind::interior:interior(result);break;
        case ShowcaseKind::many_objects:many_objects(result);break;
        default:throw std::invalid_argument("Invalid showcase kind");
    }
    const auto world=resolve_world_transforms(result.scene);
    for(std::size_t i=0;i<world.size();++i)result.scene.nodes[i].previous_world=world[i];
    result.scene.asset_revision=1;return result;
}
ProjectDocument make_showcase(std::string_view id) {return make_showcase(parse_showcase(id));}
} // namespace emberframe::lab
