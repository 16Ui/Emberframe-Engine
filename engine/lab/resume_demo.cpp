#include "resume_demo.h"
#include "systems.h"
#include "geometry.h"
#include <chrono>

namespace emberframe::lab {
namespace {
Mesh curved_grid() {
    Mesh mesh;mesh.name="LOD curved surface";constexpr int side=16;
    for(int y=0;y<=side;++y)for(int x=0;x<=side;++x) {
        const float px=float(x)/side*4-2,py=float(y)/side*4-2;
        const float z=.35f*std::sin(px*1.4f)*std::cos(py*1.1f);
        Vertex v;v.position={px,py,z};v.uv={float(x)/side,float(y)/side};
        v.normal=safe_normalize({-.49f*std::cos(px*1.4f)*std::cos(py*1.1f),.385f*std::sin(px*1.4f)*std::sin(py*1.1f),1});
        v.tangent=glm::vec4(safe_normalize({1,0,.49f*std::cos(px*1.4f)*std::cos(py*1.1f)}),1);
        mesh.vertices.push_back(v);
    }
    for(int y=0;y<side;++y)for(int x=0;x<side;++x) {
        const auto a=std::uint32_t(y*(side+1)+x),b=a+1,c=b+side+1,d=a+side+1;
        mesh.indices.insert(mesh.indices.end(),{a,b,c,a,c,d});
    }
    mesh.primitives.push_back({0,std::uint32_t(mesh.indices.size()),0});return mesh;
}
}
ResumeDemo make_resume_demo(std::string_view name) {
    ResumeDemo d;auto& s=d.settings;s.render_width=640;s.render_height=360;
    s.bloom=false;s.energy_compensation=false;s.bake_samples=32;s.shadow_resolution=1024;
    if(name=="pbr"||name=="kc") {
        s.shadow_resolution=1024;
        d.scene=make_demo_scene(0);d.camera.position={4,3,6};d.camera.target={0,.55f,0};d.camera.fov=42;
        if(name=="kc") {
            // 白炉的唯一变量是 KC 开关：没有直接灯、Bloom 或额外曝光。
            // 选真实材质球网格，白色粗糙金属、单位常量环境可揭示单次散射能量损失。
            Mesh sphere=d.scene.meshes.at(1);for(auto& p:sphere.primitives)p.material=0;
            d.scene={};d.scene.name="KC white furnace";d.scene.meshes.push_back(std::move(sphere));
            Material metal;metal.name="White rough metal";metal.base_color=glm::vec4(1);metal.metallic=1;metal.roughness=1;
            d.scene.materials.push_back(metal);Node n;n.name="Furnace sphere";n.mesh=0;d.scene.nodes.push_back(n);
            d.scene.sky_top=d.scene.sky_bottom=glm::vec3(1);d.camera.position={0,0,2.4f};d.camera.target={0,0,0};d.camera.fov=36;
        }
    }else if(name=="lights") {
        d.scene=make_demo_scene(3);d.camera.position={6,5,8};d.camera.target={0,.3f,0};s.path=RenderPath::deferred;
        // 加入真实的视锥外实例，让“候选/可见对象”有明确差异，不只展示灯表颜色。
        for(int i=0;i<12;++i){Node n=d.scene.nodes.at(1);n.name="Offscreen instance "+std::to_string(i);n.parent=-1;
            n.local=glm::translate(glm::mat4(1),glm::vec3(20+2*i,.5f,-2))*glm::scale(glm::mat4(1),glm::vec3(.9f));d.scene.nodes.push_back(n);}
    }else if(name=="shadows") {
        d.scene=make_demo_scene(2);d.camera.position={5,4,8};d.camera.target={0,.5f,0};d.camera.far_plane=45;
        s.shadows=ShadowMode::pcss;s.light_size=.2f;
    }else if(name=="temporal") {
        d.scene=make_demo_scene(1);d.camera.position={0,1.55f,6.8f};d.camera.target={0,1.4f,0};d.camera.far_plane=40;
        s.path=RenderPath::deferred;s.gi=GiMode::ssgi;s.samples=1;
    }else if(name=="geometry") {
        d.scene={};d.scene.name="QEM shared-mesh instances";d.scene.meshes.push_back(curved_grid());
        Material m;m.name="Curved surface";m.base_color={.18f,.48f,.8f,1};m.roughness=.55f;d.scene.materials.push_back(m);
        for(int i=0;i<3;++i){Node n;n.name="Shared mesh "+std::to_string(i+1);n.mesh=0;
            n.local=glm::translate(glm::mat4(1),glm::vec3((i-1)*4.5f,0,-i*6.f));d.scene.nodes.push_back(n);}
        Light light;light.direction={-.3f,-.6f,-1};d.scene.lights.push_back(light);
        d.camera.position={0,1,12};d.camera.target={0,0,-3};d.camera.far_plane=1000;s.lod_error_pixels=16;
        s.debug=DebugView::normal;s.gi=GiMode::none; // 几何对照显示法线，避免光照/阴影掩盖减面形状。
    }else throw std::invalid_argument("Unknown resume demo; use pbr,kc,lights,shadows,temporal,geometry");
    return d;
}
Camera replay_demo_camera(const Camera& origin,std::string_view motion,std::size_t frame,std::size_t frames) {
    if(frames<2||frame>=frames)throw std::invalid_argument("Camera replay frame outside bounds");
    Camera c=origin;const float t=float(frame)/float(frames-1);
    if(motion=="static")return c;
    if(motion=="pan") {
        // 小幅连续平移，保留相机方向；同一回放用于关闭/开启 TAA 或 PCF/CSM 对照。
        const auto right=safe_normalize(glm::cross(origin.target-origin.position,{0,1,0}));
        const auto shift=right*(.18f*std::sin(2*pi*t));c.position+=shift;c.target+=shift;
    }else if(motion=="orbit") {
        const auto offset=origin.position-origin.target;const float a=.35f*std::sin(2*pi*t);
        c.position=origin.target+glm::vec3(offset.x*std::cos(a)+offset.z*std::sin(a),offset.y,-offset.x*std::sin(a)+offset.z*std::cos(a));
    }else if(motion=="dolly") {
        // 平滑拉远再返回，源模型不变；运行时 LOD 选择是唯一几何变量。
        const float scale=1+5*std::sin(pi*t)*std::sin(pi*t);c.position=origin.target+(origin.position-origin.target)*scale;
    }else throw std::invalid_argument("Unknown camera replay; use static,pan,orbit,dolly");
    return c;
}
DemoRayProbe probe_demo_bvh(const Scene& scene,const Camera& camera) {
    using Clock=std::chrono::steady_clock;DemoRayProbe p;const auto triangles=flatten_scene(scene);if(triangles.empty())return p;
    const auto start=Clock::now();Bvh bvh(triangles);p.build_ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    std::vector<Ray> rays;const auto forward=safe_normalize(camera.target-camera.position),right=safe_normalize(glm::cross(forward,{0,1,0})),up=glm::cross(right,forward);
    for(int y=0;y<12;++y)for(int x=0;x<16;++x)rays.push_back({camera.position,safe_normalize(forward+right*((x-7.5f)*.045f)+up*((y-5.5f)*.045f)),.001f,camera.far_plane});
    std::vector<Hit> brute;brute.reserve(rays.size());p.rays=rays.size();TraversalStatistics a,b;
    // 求交 API 每次重置计数；必须逐射线累加，不能把最后一条漏射线的 0 当总量。
    auto begin=Clock::now();for(const auto& ray:rays){brute.push_back(brute_force_intersect(triangles,ray,&a));p.brute_triangle_tests+=a.triangle_tests;}p.brute_ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
    begin=Clock::now();for(std::size_t i=0;i<rays.size();++i){const auto hit=bvh.intersect(rays[i],&b);p.bvh_triangle_tests+=b.triangle_tests;const bool found=hit.triangle>=0,expected=brute[i].triangle>=0;p.hits+=found;
        p.matches&=found==expected&&(!found||std::abs(hit.t-brute[i].t)<=.0001f*std::max(1.f,brute[i].t));}
    p.bvh_ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();return p;
}
}
