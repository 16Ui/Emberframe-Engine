#include "scene_resources.h"
#include "geometry.h"
#include "reference_renderer.h"
#include "scene_geometry.h"
#include "systems.h"
#include <chrono>
#include <future>
#include <thread>
#ifdef EMBERFRAME_SCENE_RESOURCES_TEST_MAIN
#include <iostream>
#endif

namespace emberframe::lab {
namespace {
void require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
template<class F> bool rejects(F body) {
    try { body(); }catch(const std::invalid_argument&) { return true; }catch(const std::length_error&) { return true; }
    return false;
}
float error(glm::vec3 a,glm::vec3 b) { const auto q=glm::abs(a-b);return std::max({q.x,q.y,q.z}); }
Mesh plane(float half_extent=1,float height=0) {
    Mesh mesh;mesh.name="保留 UV 和颜色的真实网格";
    for(auto p:{glm::vec3(-half_extent,height,-half_extent),glm::vec3(half_extent,height,-half_extent),
                glm::vec3(half_extent,height,half_extent),glm::vec3(-half_extent,height,half_extent)}) {
        Vertex v;v.position=p;v.normal={0,1,0};v.uv={p.x,p.z};v.color={.8f,.7f,.6f,1};mesh.vertices.push_back(v);
    }
    mesh.indices={0,2,1,0,3,2};mesh.primitives={{0,6,0}};return mesh;
}
Scene floor_scene(bool roof=false) {
    Scene scene;scene.sky_top=scene.sky_bottom=glm::vec3(.8f);scene.materials.emplace_back();
    scene.materials[0].base_color=glm::vec4(1);scene.meshes.push_back(plane());
    if(roof)scene.meshes.push_back(plane(20,1));
    return scene;
}
Settings options() {
    Settings s;s.path=RenderPath::cpu_raster;s.environment_diffuse=EnvironmentDiffuse::prt;
    s.bake_samples=64;s.sdf_resolution=16;s.render_width=24;s.render_height=20;s.bloom=false;
    s.shadow_resolution=16;s.samples=4;return s;
}
Camera floor_camera() { Camera c;c.position={0,.65f,2};c.target={0,0,0};c.near_plane=.05f;c.far_plane=30;return c; }
Scene sheet_scene() {
    Scene scene;scene.materials.emplace_back();Mesh mesh;
    for(auto p:{glm::vec3(0,-2,-2),glm::vec3(0,2,-2),glm::vec3(0,0,2)}) {
        Vertex v;v.position=p;v.normal={1,0,0};mesh.vertices.push_back(v);
    }
    mesh.indices={0,1,2};scene.meshes.push_back(mesh);return scene;
}
void put_u64(std::string& bytes,std::size_t offset,std::uint64_t value) {
    for(int i=0;i<8;++i)bytes.at(offset+i)=char(value>>(8*i));
}
// 重算校验和后再注入非法语义字段，验证解析器不只依赖 checksum。
void reseal(std::string& bytes) {
    std::uint64_t hash=14695981039346656037ull;
    for(std::size_t i=24;i<bytes.size();++i)hash=(hash^static_cast<unsigned char>(bytes[i]))*1099511628211ull;
    put_u64(bytes,8,bytes.size()-24);put_u64(bytes,16,hash);
}
}
TestResults test_scene_resources() {
    TestResults tests;
    auto add=[&](const char* name,const std::function<std::string()>& body) {
        try { tests.push_back({name,true,body()}); }catch(const std::exception& e) { tests.push_back({name,false,e.what()}); }
    };
    add("scene resources analytic SH constant and gradient sky",[] {
        auto scene=floor_scene();auto s=options();s.environment_diffuse=EnvironmentDiffuse::sh;
        const auto constant=prepare_scene_resources(scene,s);
        for(auto n:{glm::vec3(1,0,0),glm::vec3(0,1,0),glm::vec3(0,-1,0),safe_normalize(glm::vec3(1,2,3))})
            require(error(evaluate_environment_sh(constant->environment_sh9,n),scene.sky_top*pi)<1e-6f,"Constant sky SH energy wrong");
        scene.sky_top={1,.7f,.4f};scene.sky_bottom={.1f,.2f,.3f};const auto gradient=prepare_scene_resources(scene,s);
        for(auto n:{glm::vec3(0,1,0),glm::vec3(0,-1,0),glm::vec3(1,0,0)}) {
            const auto expected=(scene.sky_top+scene.sky_bottom)*(.5f*pi)+(scene.sky_top-scene.sky_bottom)*(pi/3*n.y);
            require(error(evaluate_environment_sh(gradient->environment_sh9,n),expected)<1e-6f,"Gradient sky projection is noisy or incorrectly convolved");
        }
        require(!gradient->prt&&!gradient->sdf,"SH unnecessarily baked geometry arrays");return std::string("解析低阶投影与精确 cosine 卷积一致");
    });
    add("scene resources real mesh PRT visibility and energy",[] {
        auto open=floor_scene(),covered=floor_scene(true);const auto s=options();
        const auto a=prepare_scene_resources(open,s),b=prepare_scene_resources(covered,s);
        require(a->prt&&b->prt&&a->prt->instances[0].transfer.size()==4,"Missing per-instance vertex transfer");
        for(std::size_t i=0;i<4;++i) {
            const auto lit=evaluate_prt_irradiance(a->environment_sh9,a->prt->instances[0].transfer[i]);
            const auto blocked=evaluate_prt_irradiance(b->environment_sh9,b->prt->instances[0].transfer[i]);
            require(error(lit,open.sky_top*pi)<1e-6f,"Unoccluded PRT cosine energy wrong");
            require(blocked.r<lit.r*.1f,"Actual mesh ceiling did not occlude transfer");
        }
        require(open.meshes[0].vertices[0].baked_irradiance==glm::vec3(0),"Baker mutated source attributes");
        return std::string("真实 BVH ceiling 遮挡，无遮挡环境照度等于 pi*L");
    });
    add("scene resources instance copies preserve hierarchy and repeated apply",[] {
        auto scene=floor_scene();scene.meshes[0]=plane(.4f);scene.meshes.push_back(plane(2,1));
        Node left;left.name="左实例";left.mesh=0;left.local=glm::translate(glm::mat4(1),{-3,0,0});
        Node right=left;right.name="右实例";right.local=glm::translate(glm::mat4(1),{3,0,0});
        Node roof;roof.mesh=1;roof.local=glm::translate(glm::mat4(1),{-3,0,0});
        scene.nodes={left,right,roof};const auto s=options();const auto resources=prepare_scene_resources(scene,s);
        const auto applied=apply_scene_resources(scene,resources,s);
        require(applied.nodes[0].mesh!=applied.nodes[1].mesh,"Shared mesh instances were not separated");
        const auto& a=applied.meshes[std::size_t(applied.nodes[0].mesh)];const auto& b=applied.meshes[std::size_t(applied.nodes[1].mesh)];
        require(a.vertices[0].baked_irradiance.r<b.vertices[0].baked_irradiance.r*.5f,"World-space transfer aliased instances");
        require(a.vertices[0].uv==scene.meshes[0].vertices[0].uv&&a.vertices[0].color==scene.meshes[0].vertices[0].color&&
                a.indices==scene.meshes[0].indices&&a.primitives[0].material==scene.meshes[0].primitives[0].material,"Mesh attributes changed");
        for(std::size_t i=0;i<scene.nodes.size();++i)
            require(applied.nodes[i].parent==scene.nodes[i].parent&&applied.nodes[i].local==scene.nodes[i].local&&
                    applied.nodes[i].previous_world==scene.nodes[i].previous_world,"Node hierarchy/transform/history changed");
        require(geometry_fingerprint(applied)==geometry_fingerprint(scene),"Instancing changed bake fingerprint");
        auto again=applied;
        for(int i=0;i<8;++i)again=apply_scene_resources(again,resources,s);
        require(again.meshes.size()==applied.meshes.size(),"Repeated apply appended meshes indefinitely");
        require(prepare_scene_resources(again,s)==resources,"Applied scene recursively invalidated bake cache");
        return std::string("独立实例照度，保留材料/UV/颜色/层级，重复 apply 不增长");
    });
    add("scene resources data fingerprint ignores presentation and invalidates geometry",[] {
        auto scene=floor_scene();Node node;node.mesh=0;scene.nodes={node};const auto hash=geometry_fingerprint(scene);
        scene.revision+=200;scene.sky_top=glm::vec3(4);scene.materials[0].roughness=.9f;
        scene.meshes[0].vertices[0].baked_irradiance=glm::vec3(7);scene.meshes[0].vertices[0].uv=glm::vec2(5);
        scene.nodes[0].previous_world=glm::translate(glm::mat4(1),{3,0,0});
        require(geometry_fingerprint(scene)==hash,"Presentation/history/baked state leaked into fingerprint");
        auto changed=scene;changed.meshes[0].vertices[0].position.x+=.01f;
        require(geometry_fingerprint(changed)!=hash,"Actual position mutation did not invalidate");
        changed=scene;changed.nodes[0].local=glm::translate(glm::mat4(1),{.01f,0,0});
        require(geometry_fingerprint(changed)!=hash,"Instance transform did not invalidate");
        changed=scene;changed.meshes[0].vertices[0].normal={0,0,1};
        require(geometry_fingerprint(changed)!=hash,"Transfer normal did not invalidate");
        changed=scene;std::swap(changed.meshes[0].indices[1],changed.meshes[0].indices[2]);
        require(geometry_fingerprint(changed)!=hash,"Index mutation did not invalidate");return std::string("实际几何与法线失效；缓存、revision、天空、材质不递归");
    });
    add("scene resources immutable cache reuses relighting and rejects stale apply",[] {
        auto scene=floor_scene();auto s=options();s.sdf_shadows=true;
        const auto first=prepare_scene_resources(scene,s),same=prepare_scene_resources(scene,s);
        require(first==same,"Same immutable cache key was rebuilt");
        scene.revision++;scene.materials[0].base_color={.2f,.3f,.4f,1};
        require(prepare_scene_resources(scene,s)==first,"Material/revision triggered transfer rebuild");
        scene.sky_top=glm::vec3(1.7f);const auto sky=prepare_scene_resources(scene,s);
        require(sky!=first&&sky->prt==first->prt&&sky->sdf==first->sdf,"Sky did not reuse independent geometry arrays");
        require(error(first->environment_sh9[0],same->environment_sh9[0])==0,"Relight mutated reader snapshot");
        const auto relit=apply_scene_resources(scene,first,s);require(relit.baked_resources->prt==first->prt,"Apply rebaked static transfer on sky edit");
        scene.meshes[0].vertices[0].position.x+=.05f;const auto moved=prepare_scene_resources(scene,s);
        require(moved->prt!=first->prt&&moved->sdf!=first->sdf,"Changed geometry reused stale arrays");
        require(rejects([&]{(void)apply_scene_resources(scene,first,s);}),"Stale geometry apply accepted");return std::string("天空只更新 SH 小壳，几何改变拒绝旧快照");
    });
    add("scene resources binary roundtrip and malformed data rejection",[] {
        auto scene=floor_scene();auto s=options();s.sdf_shadows=true;const auto original=prepare_scene_resources(scene,s);
        const auto encoded=serialize_scene_resources(*original);const auto restored=deserialize_scene_resources(encoded);
        require(serialize_scene_resources(*restored)==encoded&&restored->sdf->values==original->sdf->values&&
                restored->prt->instances[0].transfer==original->prt->instances[0].transfer,"Binary roundtrip lost arrays");
        require(rejects([&]{(void)deserialize_scene_resources(encoded.substr(0,encoded.size()-1));}),"Truncated binary accepted");
        auto bad=encoded;bad.back()^=1;require(rejects([&]{(void)deserialize_scene_resources(bad);}),"Checksum mismatch accepted");
        bad=encoded;bad[4]=2;require(rejects([&]{(void)deserialize_scene_resources(bad);}),"Future version accepted");
        bad=encoded;for(int i=0;i<4;++i)bad[40+i]=static_cast<char>(static_cast<unsigned char>(255));reseal(bad);
        require(rejects([&]{(void)deserialize_scene_resources(bad);}),"Unbounded sample count accepted after checksum reseal");
        bad=encoded;for(int i=0;i<4;++i)bad[156+i]=static_cast<char>(static_cast<unsigned char>(255));reseal(bad);
        require(rejects([&]{(void)deserialize_scene_resources(bad);}),"Unbounded PRT allocation accepted");
        bad=encoded;bad.push_back('x');reseal(bad);require(rejects([&]{(void)deserialize_scene_resources(bad);}),"Trailing body accepted");
        SceneBakeResources malformed=*original;auto grid=std::make_shared<SceneSdfGrid>(*original->sdf);grid->values[0]=-1;malformed.sdf=grid;
        require(rejects([&]{(void)serialize_scene_resources(malformed);}),"Invalid negative unsigned SDF serialized");
        malformed=*original;malformed.environment_sh9[0].x=std::numeric_limits<float>::quiet_NaN();
        require(rejects([&]{(void)serialize_scene_resources(malformed);}),"NaN SH serialized");return std::string("逐位 roundtrip，版本/长度/校验和/语义与分配上限均验证");
    });
    add("scene resources cancellation and explicit budget refusal",[] {
        auto scene=floor_scene();auto s=options();std::atomic<bool> cancel{true};
        require(!prepare_scene_resources(scene,s,&cancel),"Pre-cancelled bake produced snapshot");
        auto bad=s;bad.bake_samples=15;require(rejects([&]{(void)prepare_scene_resources(scene,bad);}),"Invalid sample budget accepted");
        bad=s;bad.sdf_resolution=65;require(rejects([&]{(void)prepare_scene_resources(scene,bad);}),"Invalid SDF resolution accepted");
        scene.meshes[0].vertices.resize(5000,scene.meshes[0].vertices[0]);s.bake_samples=4096;
        require(rejects([&]{(void)prepare_scene_resources(scene,s);}),"Oversized PRT silently downgraded or fabricated");
        scene.meshes[0].vertices.resize(1000);scene.meshes[0].vertices[0].position.x-=.137f;
        cancel=false;auto work=std::async(std::launch::async,[&]{return prepare_scene_resources(scene,s,&cancel);});
        std::this_thread::sleep_for(std::chrono::milliseconds(2));cancel=true;
        require(!work.get(),"Cancelled in-flight bake committed partial cache");
        s.bake_samples=16;cancel=false;const auto complete=prepare_scene_resources(scene,s,&cancel);
        require(complete&&complete->prt->instances[0].transfer.size()==1000,"Cancelled bake poisoned future cache");
        return std::string("预算超限明确拒绝；取消不发布半成品，可随后重新准备");
    });
    add("scene resources open mesh exact unsigned SDF and visible shadow rays",[] {
        auto scene=sheet_scene();auto s=options();s.environment_diffuse=EnvironmentDiffuse::sh;s.sdf_shadows=true;s.sdf_resolution=17;
        const auto resources=prepare_scene_resources(scene,s);require(resources->sdf&&resources->sdf->unsigned_distance,"Open mesh SDF missing");
        const auto& g=*resources->sdf;const int n=g.resolution;
        require(g.values[(std::size_t(8)*n+8)*n+8]<1e-6f,"SDF did not bake actual triangle surface");
        const float x=g.min.x+(g.max.x-g.min.x)*14.f/16;
        require(std::abs(g.values[(std::size_t(8)*n+8)*n+14]-std::abs(x))<1e-6f,"Raw field is not exact point-to-triangle distance");
        const float blocked=scene_sdf_visibility(g,{-2,0,0},{-1,0,0},{1,0,0},5,12,.002f);
        const float visible=scene_sdf_visibility(g,{-2,0,0},{-1,0,0},{0,1,0},5,12,.002f);
        require(blocked==0&&visible>.99f,"Open triangle blocked/visible rays failed");
        for(float p:{-.35f,-.1f,.1f,.35f})require(sample_scene_sdf(g,{p,0,0})<=std::abs(p)+1e-6f,"Interpolated distance is not conservative");
        require(sample_scene_sdf(g,{-.2f,0,0})>=0,"Open mesh falsely assigned inside sign");
        return std::string("真实开放三角面 unsigned 距离；遮挡 0、无遮挡 1、插值不越过表面");
    });
    add("scene resources concurrent prepare publishes one immutable snapshot",[] {
        auto scene=floor_scene();scene.meshes[0].vertices[0].position.x-=.0317f;auto s=options();s.sdf_shadows=true;
        auto a=std::async(std::launch::async,[&]{return prepare_scene_resources(scene,s);});
        auto b=std::async(std::launch::async,[&]{return prepare_scene_resources(scene,s);});
        require(a.get()==b.get(),"Concurrent same-key bake published different snapshots");return std::string("同键并发复用单一不可变资源");
    });
    add("scene resources raster perspective and clip interpolate irradiance",[] {
        Scene scene;scene.materials.emplace_back();Mesh mesh;
        for(auto p:{glm::vec3(-1,-1,-1),glm::vec3(1,-1,-3),glm::vec3(0,1,-2)}) {
            Vertex v;v.position=p;v.normal={0,0,1};v.baked_irradiance=p+glm::vec3(4);mesh.vertices.push_back(v);
        }
        mesh.indices={0,1,2};scene.meshes={mesh};Camera c;c.position={0,0,0};c.target={0,0,-1};c.near_plane=1.5f;c.far_plane=20;
        auto s=options();s.render_width=s.render_height=32;const auto buffer=rasterize(scene,c,s);int count=0;
        for(const auto& p:buffer.pixels)if(p.valid) { ++count;require(error(p.baked_irradiance,p.position+glm::vec3(4))<2e-6f,"Baked irradiance lost perspective/near clipping"); }
        require(count>30,"No near-clipped pixels tested");return std::string("穿近平面的三角形保持照度与位置的透视线性关系");
    });
    add("scene resources CPU normal renderer SH PRT image effect and specular retention",[] {
        auto scene=floor_scene(true);auto s=options();s.environment_diffuse=EnvironmentDiffuse::ibl;
        const auto ibl=render_reference(scene,floor_camera(),s);s.environment_diffuse=EnvironmentDiffuse::sh;
        const auto sh=render_reference(scene,floor_camera(),s);s.environment_diffuse=EnvironmentDiffuse::prt;
        const auto prt=render_reference(scene,floor_camera(),s);int count=0;double lit=0,dark=0;
        for(std::size_t i=0;i<ibl.surfaces.pixels.size();++i)if(ibl.surfaces.pixels[i].valid&&ibl.surfaces.pixels[i].object==0) {
            ++count;lit+=ibl.indirect.pixels[i].r;dark+=prt.indirect.pixels[i].r;
            require(error(ibl.indirect.pixels[i],sh.indirect.pixels[i])<2e-5f,"SH double-counted or changed constant diffuse energy");
        }
        require(count>10&&dark<lit*.15,"Normal CPU PRT did not affect occluded scene image");
        scene.materials[0].metallic=1;s.environment_diffuse=EnvironmentDiffuse::ibl;const auto metal_ibl=render_reference(scene,floor_camera(),s);
        s.environment_diffuse=EnvironmentDiffuse::prt;const auto metal_prt=render_reference(scene,floor_camera(),s);
        for(std::size_t i=0;i<metal_ibl.surfaces.pixels.size();++i)if(metal_ibl.surfaces.pixels[i].valid&&metal_ibl.surfaces.pixels[i].object==0)
            require(error(metal_ibl.indirect.pixels[i],metal_prt.indirect.pixels[i])<2e-6f,"PRT replaced or double-counted specular IBL");
        return std::string("正常场景 PRT 明显变暗、SH 保持精确能量、金属镜面 IBL 保留");
    });
    add("scene resources CPU SDF unblocked scene remains lit",[] {
        auto scene=floor_scene();scene.sky_top=scene.sky_bottom=glm::vec3(0);Light light;light.direction={0,-1,0};light.intensity=2;scene.lights={light};
        auto s=options();s.environment_diffuse=EnvironmentDiffuse::ibl;s.gi=GiMode::none;s.sdf_shadows=true;s.shadows=ShadowMode::hard;
        const auto image=render_reference(scene,floor_camera(),s);int count=0;double light_sum=0;
        for(std::size_t i=0;i<image.surfaces.pixels.size();++i)if(image.surfaces.pixels[i].valid) {
            ++count;light_sum+=image.color.pixels[i].r;require(image.shadow.pixels[i]>.99f,"SDF receiver self-shadowed an unobstructed scene");
        }
        require(count>10&&light_sum/count>.1,"SDF integration rendered unblocked scene black");return std::string("网格表面起点偏移避免自阴影；正常 CPU 直接光保持非黑");
    });
    add("scene resources PRT plus generated and serialized LOD remains illuminated",[] {
        auto scene=floor_scene();Mesh grid;constexpr int side=6;
        for(int z=0;z<=side;++z)for(int x=0;x<=side;++x) {
            Vertex v;const float px=float(x)/side*4-2,pz=float(z)/side*4-2;
            v.position={px,.15f*std::sin(px)*std::sin(pz),pz};v.normal={0,1,0};v.uv={float(x)/side,float(z)/side};grid.vertices.push_back(v);
        }
        for(int z=0;z<side;++z)for(int x=0;x<side;++x) {
            const auto a=std::uint32_t(z*(side+1)+x),b=a+1,c=a+side+2,d=a+side+1;
            grid.indices.insert(grid.indices.end(),{a,c,b,a,d,c});
        }
        grid.primitives={{0,std::uint32_t(grid.indices.size()),0}};scene.meshes[0]=grid;
        auto s=options();s.auto_lod=true;s.lod_error_pixels=100;Camera camera;camera.position={0,4,5};camera.target={0,0,0};
        // 先验证缺链时严格回退 LOD0，prepare/render 均不会隐式 QEM。
        SceneGeometryRuntime empty_runtime;
        empty_runtime.prepare(scene,camera,s);
        require(empty_runtime.stats().selected_triangles==grid.indices.size()/3,"Missing chain did not retain LOD0");
        require(render_reference(scene,camera,s).triangles==grid.indices.size()/3,"Reference implicitly generated LOD");
        scene.meshes[0]=build_mesh_lods(grid); // 一次显式、真实 QEM，后续 prepare 仅选择。
        require(!scene.meshes[0].lods.empty(),"LOD fixture failed to reduce actual geometry");
        for(bool loaded:{false,true}) {
            if(loaded) {
                // 真正经工程序列化加载派生链，零 baked 值必须由当前选级几何烘焙填充。
                for(auto& lod:scene.meshes[0].lods)for(auto& v:lod.vertices)v.baked_irradiance=glm::vec3(0);
                ProjectDocument document;document.scene=scene;document.camera=camera;document.settings=s;
                scene=deserialize_project(serialize_project(document)).scene;
                require(!scene.meshes[0].lods.empty(),"Serialized project lost real LOD chain");
            }
            const auto source_resources=prepare_scene_resources(scene,s);scene.baked_resources=source_resources;
            SceneGeometryRuntime runtime;const auto& geometry=runtime.prepare(scene,camera,s);
            require(runtime.stats().selected_triangles<grid.indices.size()/3,"PRT+LOD did not select an actual reduction");
            const auto resources=prepare_scene_resources(geometry,s);
            const auto selected=apply_scene_resources(geometry,resources,s);
            for(const auto& t:flatten_scene(selected))for(const auto& v:t.vertices)
                require(v.baked_irradiance.r>.3f,"Selected LOD vertex lost PRT irradiance");
            const auto image=render_reference(scene,camera,s);int count=0;double indirect=0;
            for(std::size_t i=0;i<image.surfaces.pixels.size();++i)if(image.surfaces.pixels[i].valid) {
                ++count;indirect+=image.indirect.pixels[i].r;
            }
            require(image.triangles<grid.indices.size()/3&&count>20&&indirect/count>.1,"Normal PRT+LOD render became black or ignored LOD");
            require(prepare_scene_resources(geometry,s)==resources,"Stable selected geometry rebuilt PRT");
            require(scene.baked_resources==source_resources,"Reference overwrote full-source persistent bake");
        }
        return std::string("一次显式 QEM 与真正工程加载的零照度链均非黑；缺链保持 LOD0");
    });
    add("scene resources CPU path trace selects real Octree and matches BVH",[] {
        auto scene=floor_scene();Light light;light.direction={-.3f,-1,-.2f};scene.lights={light};
        auto s=options();s.path=RenderPath::path_trace;s.max_bounces=2;s.render_width=12;s.render_height=10;
        s.spatial_structure=SpatialStructure::bvh;const auto bvh=render_reference(scene,floor_camera(),s);
        s.spatial_structure=SpatialStructure::octree;const auto octree=render_reference(scene,floor_camera(),s);
        require(bvh.rays==octree.rays&&bvh.node_tests>0&&octree.node_tests>0,"Spatial traversal was not exercised/counted");
        for(std::size_t i=0;i<bvh.color.pixels.size();++i)require(error(bvh.color.pixels[i],octree.color.pixels[i])<1e-6f,"Octree changed seeded path radiance");
        return std::string("真实 Octree 命中/遮挡查询与 BVH 同种子图像一致");
    });
    return tests;
}
} // namespace emberframe::lab
#ifdef EMBERFRAME_SCENE_RESOURCES_TEST_MAIN
int main() {
    int failed=0;const auto tests=emberframe::lab::test_scene_resources();
    for(const auto& t:tests) { std::cout<<(t.passed?"PASS ":"FAIL ")<<t.name<<": "<<t.detail<<'\n';failed+=!t.passed; }
    std::cout<<tests.size()-failed<<'/'<<tests.size()<<" passed\n";return failed?1:0;
}
#endif
