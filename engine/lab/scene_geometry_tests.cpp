#include "scene_geometry.h"
#include "environment.h"
#include <sstream>

namespace emberframe::lab {
namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& action,const char* message) {
    bool rejected=false; try { action(); } catch(const std::invalid_argument&) { rejected=true; }
    require(rejected,message);
}
Mesh grid(int side=8,bool curved=true) {
    Mesh mesh; mesh.name="运行时曲面测试";
    for(int y=0;y<=side;++y) for(int x=0;x<=side;++x) {
        Vertex v; const float px=float(x)/side*8-4,py=float(y)/side*8-4;
        v.position={px,py,curved?0.6f*std::sin(px*.7f)*std::cos(py*.5f):0};
        v.normal={0,0,1}; v.uv={float(x)/side,float(y)/side};
        v.baked_irradiance={v.uv.x,v.uv.y,.25f}; mesh.vertices.push_back(v);
    }
    for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
        const auto a=std::uint32_t(y*(side+1)+x),b=a+1,c=b+std::uint32_t(side+1),d=a+std::uint32_t(side+1);
        mesh.indices.insert(mesh.indices.end(),{a,b,c,a,c,d});
    }
    mesh.primitives.push_back({0,std::uint32_t(mesh.indices.size()),0}); return mesh;
}
Camera camera_at(float z) {
    Camera camera; camera.position={0,0,z}; camera.target={0,0,0}; camera.fov=60;
    camera.near_plane=.1f; camera.far_plane=10000; return camera;
}
Settings settings() {
    Settings s; s.auto_lod=true; s.render_width=192; s.render_height=160; s.lod_error_pixels=100; return s;
}
Scene scene_with_grid() {
    Scene scene; scene.meshes.push_back(grid()); scene.materials.push_back({});
    Node node; node.mesh=0; scene.nodes.push_back(node); return scene;
}
bool same_base(const Mesh& a,const Mesh& b) {
    if(a.name!=b.name || a.indices!=b.indices || a.vertices.size()!=b.vertices.size() || a.primitives.size()!=b.primitives.size()) return false;
    for(std::size_t i=0;i<a.vertices.size();++i) {
        const auto& x=a.vertices[i]; const auto& y=b.vertices[i];
        if(x.position!=y.position || x.normal!=y.normal || x.uv!=y.uv || x.tangent!=y.tangent ||
           x.color!=y.color || x.baked_irradiance!=y.baked_irradiance) return false;
    }
    for(std::size_t i=0;i<a.primitives.size();++i) {
        const auto& x=a.primitives[i]; const auto& y=b.primitives[i];
        if(x.first_index!=y.first_index || x.index_count!=y.index_count || x.material!=y.material) return false;
    }
    return true;
}
void same_matrix(const glm::mat4& a,const glm::mat4& b,const char* message) {
    for(int i=0;i<4;++i) for(int j=0;j<4;++j) require(a[i][j]==b[i][j],message);
}
} // namespace

TestResults test_scene_geometry() {
    TestResults results;
    auto run=[&](const char* name,auto&& action) {
        try { results.push_back({name,true,action()}); }
        catch(const std::exception& e) { results.push_back({name,false,e.what()}); }
        catch(...) { results.push_back({name,false,"Unknown exception"}); }
    };
    run("Scene LOD builder preserves LOD0 and persists actual reductions",[]() -> std::string {
        auto source=scene_with_grid(); source.meshes.push_back(grid(4,false)); const auto original=source;
        const auto reports=build_scene_lods(source,0);
        require(reports.size()==1 && reports[0].mesh==0 && reports[0].reduced,"Targeted scene LOD build did not reduce geometry");
        require(same_base(source.meshes[0],original.meshes[0]),"Builder overwrote original mesh");
        require(same_base(source.meshes[1],original.meshes[1]) && source.meshes[1].lods.empty(),"Targeted builder changed another mesh");
        require(source.revision==original.revision+1,"Stored resources did not invalidate source once");
        std::size_t triangles=128; double error=0;
        for(const auto& lod:source.meshes[0].lods) {
            require(!lod.indices.empty() && lod.indices.size()/3<triangles,"Stored level is not a real reduction");
            require(std::isfinite(lod.geometric_error) && lod.geometric_error>0 && lod.geometric_error>=error,"Invalid stored geometric error");
            require(!lod.primitives.empty() && lod.primitives[0].material==0,"Stored level lost material primitives");
            for(const auto id:lod.indices) require(id<lod.vertices.size(),"Stored level index out of range");
            triangles=lod.indices.size()/3; error=lod.geometric_error;
        }
        SceneGeometryRuntime runtime; auto s=settings(); runtime.prepare(source,camera_at(200),s);
        require(runtime.stats().lod_builds==0 && runtime.stats().selected_triangles<128,"Runtime ignored persistent LOD resources");
        rejects([&]{build_scene_lods(source,99);},"Invalid mesh selection accepted");
        return "持久化级别有真实索引、primitive 和累计误差；LOD0/其他网格不变，加载无需再次 QEM";
    });
    run("Automatic LOD changes rasterized geometry and restores full detail near camera",[]() -> std::string {
        auto source=scene_with_grid(); build_scene_lods(source); const auto original=source;
        SceneGeometryRuntime runtime; auto s=settings(); const auto camera=camera_at(20);
        const Scene coarse=runtime.prepare(source,camera,s);
        require(runtime.stats().selected_triangles<runtime.stats().source_triangles,"Automatic LOD did not reduce submitted triangles");
        require(coarse.nodes[0].mesh!=source.nodes[0].mesh,"Snapshot did not select a geometry variant");
        const auto coarse_image=rasterize(coarse,camera,s),full_image=rasterize(source,camera,s);
        std::size_t changed_pixels=0;
        for(std::size_t i=0;i<coarse_image.pixels.size();++i) {
            const auto& a=coarse_image.pixels[i]; const auto& b=full_image.pixels[i];
            if(a.valid!=b.valid || (a.valid && b.valid && std::abs(a.linear_depth-b.linear_depth)>1e-4f)) ++changed_pixels;
        }
        require(changed_pixels>0,"Reported reduction did not change rasterized image geometry");
        const auto far_revision=coarse.revision;
        const auto& near=runtime.prepare(source,camera_at(.11f),s);
        require(runtime.stats().current_levels[0]==0 && near.nodes[0].mesh==0,"Near-plane crossing did not restore LOD0");
        require(runtime.stats().selected_triangles==128 && near.revision>far_revision,"Near restoration did not invalidate snapshot");
        require(source.revision==original.revision && same_base(source.meshes[0],original.meshes[0]) && source.meshes[0].lods.size()==original.meshes[0].lods.size(),"Runtime mutated source scene");
        return "实际 raster GBuffer 深度改变像素="+std::to_string(changed_pixels)+"；靠近相机恢复 LOD0，源 Scene 未改写";
    });
    run("Shared mesh instances select independent levels and preserve hierarchy/history",[]() -> std::string {
        auto source=scene_with_grid(); build_scene_lods(source);
        source.nodes.clear();
        Node close; close.mesh=0; close.parent=2; close.previous_world=glm::translate(glm::mat4(1),{1,2,3});
        Node far=close; far.local=glm::translate(glm::mat4(1),{0,0,-400}); far.previous_world=glm::scale(glm::mat4(1),{2,3,4});
        Node parent; parent.local=glm::translate(glm::mat4(1),{.2f,0,0});
        source.nodes={close,far,parent}; const auto worlds=resolve_world_transforms(source);
        const auto& lod=source.meshes[0].lods.back(); const auto camera=camera_at(8); auto s=settings();
        Aabb bounds; for(const auto& v:source.meshes[0].vertices) bounds.expand(v.position);
        s.lod_error_pixels=float(projected_lod_error_pixels(lod.geometric_error,bounds,worlds[1],camera,s.render_height)*1.01);
        SceneGeometryRuntime runtime; const auto& snapshot=runtime.prepare(source,camera,s);
        const auto& levels=runtime.stats().current_levels;
        require(levels.size()==3 && levels[0]==0 && levels[1]>0 && levels[2]==0,"Shared mesh was not selected independently per instance");
        require(snapshot.nodes[0].mesh==0 && snapshot.nodes[1].mesh!=0,"Near/far nodes do not reference separate variants");
        const auto selected_worlds=resolve_world_transforms(snapshot);
        for(std::size_t i=0;i<3;++i) {
            require(snapshot.nodes[i].parent==source.nodes[i].parent,"Hierarchy parent changed");
            same_matrix(worlds[i],selected_worlds[i],"World transform changed");
            same_matrix(snapshot.nodes[i].previous_world,source.nodes[i].previous_world,"Previous world transform changed");
        }
        require(runtime.stats().source_triangles==256 && runtime.stats().selected_triangles<256,"Instance triangle totals wrong");
        require(runtime.selectedTriangles()==runtime.stats().selected_triangles && !runtime.status().empty(),"UI status API unavailable");
        return "共享 mesh 的近实例用 LOD0，远实例用派生 mesh，父节点可晚于子节点，世界/历史矩阵不变";
    });
    run("Stable LOD ignores unrelated revision and sky does not rebuild geometry",[]() -> std::string {
        auto source=scene_with_grid(); build_scene_lods(source); SceneGeometryRuntime runtime; auto s=settings(); const auto camera=camera_at(100);
        const auto& first=runtime.prepare(source,camera,s); const auto revision=first.revision;
        const auto* meshes=first.meshes.data(); const auto lod_builds=runtime.stats().lod_builds,trees=runtime.stats().spatial_rebuilds;
        source.revision+=100;
        const auto& stable=runtime.prepare(source,camera,s);
        require(stable.revision==revision && stable.meshes.data()==meshes,"Unrelated global revision copied/invalidated snapshot");
        source.nodes[0].previous_world=glm::translate(glm::mat4(1),{2,0,0});
        const auto& history=runtime.prepare(source,camera,s);
        same_matrix(history.nodes[0].previous_world,source.nodes[0].previous_world,"History sidecar did not update");
        require(history.revision==revision,"History updates incremented scene revision");
        source.sky_top={.7f,.6f,.5f}; ++source.revision;
        const auto& sky=runtime.prepare(source,camera,s);
        require(sky.sky_top==source.sky_top && sky.revision>revision,"Sky edit did not refresh visible snapshot");
        require(sky.meshes.data()==meshes,"Sky edit copied unchanged mesh resources");
        require(runtime.stats().lod_builds==lod_builds && runtime.stats().spatial_rebuilds==trees,"Sky edit rebuilt QEM or spatial hierarchy");
        const auto sky_revision=sky.revision; runtime.prepare(source,camera_at(101),s);
        require(runtime.stats().current_levels[0]>0 && sky.revision==sky_revision,"Camera movement with stable choice invalidated revision");
        return "无关 revision 不复制快照；稳定相机/运动历史不涨 revision；sky 刷新光照但不重算 QEM/树";
    });
    run("In-place geometry edits invalidate fingerprints including stale persistent LOD",[]() -> std::string {
        auto source=scene_with_grid(); build_scene_lods(source); const auto original_lod=source.meshes[0].lods[0].indices;
        SceneGeometryRuntime runtime; auto s=settings(); const auto camera=camera_at(100);
        const auto revision=runtime.prepare(source,camera,s).revision; const auto trees=runtime.stats().spatial_rebuilds;
        require(runtime.stats().lod_builds==0,"Initial persistent LOD was not reused");
        source.meshes[0].vertices[40].position.z+=.23f;
        const auto& edited=runtime.prepare(source,camera,s);
        require(edited.revision>revision && runtime.stats().lod_builds==0 && runtime.stats().current_levels[0]==0 && runtime.stats().spatial_rebuilds>trees,"Geometry edit without revision reused stale geometry or blocked on QEM");
        require(source.meshes[0].lods[0].indices==original_lod,"Runtime overwrote persistent source LOD");
        const auto count=runtime.stats().lod_builds; runtime.prepare(source,camera,s);
        require(runtime.stats().lod_builds==count && runtime.stats().current_levels[0]==0,"Unchanged edited mesh repeatedly ran QEM or reaccepted stale LOD");
        source.nodes[0].local=glm::scale(glm::mat4(1),{2,.5f,1}); runtime.prepare(source,camera,s);
        require(runtime.stats().lod_builds==count,"Transform edit regenerated object-space QEM chain");
        build_scene_lods(source); runtime.prepare(source,camera,s);
        require(runtime.stats().current_levels[0]>0 && runtime.stats().lod_builds==0,"Explicit rebuilt chain was not consumed without blocking QEM");
        return "同 revision 原地顶点编辑使旧链失效并回退 LOD0；后台生成完成后恢复选级，prepare 始终不运行 QEM";
    });
    run("Automatic selection respects viewport, FOV, scale and disabled/zero budget",[]() -> std::string {
        auto source=scene_with_grid(); build_scene_lods(source); SceneGeometryRuntime runtime;
        auto s=settings(); auto camera=camera_at(70); Aabb bounds;
        for(const auto& v:source.meshes[0].vertices) bounds.expand(v.position);
        const auto& last=source.meshes[0].lods.back();
        s.lod_error_pixels=float(projected_lod_error_pixels(last.geometric_error,bounds,glm::mat4(1),camera,s.render_height)*1.01);
        runtime.prepare(source,camera,s); const auto far_level=runtime.stats().current_levels[0]; require(far_level==source.meshes[0].lods.size(),"Eligible coarse level was not selected");
        s.render_height*=8; runtime.prepare(source,camera,s); require(runtime.stats().current_levels[0]<far_level,"Higher viewport did not increase detail");
        s.render_height/=8; camera.fov=10; runtime.prepare(source,camera,s); require(runtime.stats().current_levels[0]<far_level,"Narrow FOV did not increase detail");
        camera.fov=60; source.nodes[0].local=glm::scale(glm::mat4(1),{4,.5f,2}); runtime.prepare(source,camera,s);
        require(runtime.stats().current_levels[0]<far_level,"Nonuniform scale was not reflected in selection");
        s.lod_error_pixels=0; runtime.prepare(source,camera,s); require(runtime.stats().current_levels[0]==0,"Zero error budget selected reduction");
        s.lod_error_pixels=100; s.auto_lod=false; const auto& full=runtime.prepare(source,camera,s);
        require(runtime.stats().current_levels[0]==0 && full.nodes[0].mesh==0,"Disabled automatic LOD retained coarse geometry");
        return "分辨率升高、FOV 缩小、非均匀缩放使选级更精细；零预算和关闭自动 LOD 使用原网格";
    });
    run("QEM budget and seam failures never count copies as successful LOD",[]() -> std::string {
        MeshLodBuildReport seam_report,budget_report;
        const auto seam=build_mesh_lods(grid(1,false),{},&seam_report);
        const auto huge=grid(46,false); const auto bounded=build_mesh_lods(huge,{},&budget_report);
        require(seam.lods.empty() && !seam_report.reduced && seam_report.stored_levels==0 && !seam_report.reason.empty(),"Seam-locked copy counted as reduction");
        require(bounded.lods.empty() && same_base(huge,bounded) && !budget_report.reduced && budget_report.reason.find("4096")!=std::string::npos,"4096-triangle budget not explicitly reported");
        Scene source; source.meshes={seam}; const auto revision=source.revision;
        const auto reports=build_scene_lods(source);
        require(reports.size()==1 && !reports[0].reduced && source.revision==revision,"No reduction incorrectly invalidated source");
        auto fake=seam; fake.lods.push_back({fake.vertices,fake.indices,fake.primitives,1}); source.meshes={fake};
        SceneGeometryRuntime runtime; auto s=settings(); runtime.prepare(source,camera_at(100),s);
        require(runtime.stats().current_levels[0]==0 && runtime.stats().reduced_meshes==0 && runtime.stats().selected_triangles==2,"Duplicate stored geometry counted as LOD success");
        const auto builds=runtime.stats().lod_builds; runtime.prepare(source,camera_at(100),s);
        require(runtime.stats().lod_builds==builds,"Seam failure repeatedly retried every frame");
        return "边界锁定和 4232>4096 输入均明确报告失败；复制级别被拒绝，不计成功、不逐帧重试";
    });
    run("Current BVH and Octree ray picking match transformed scene brute force",[]() -> std::string {
        auto source=scene_with_grid(); source.meshes[0].primitives[0].material=7;
        source.nodes[0].parent=1; source.nodes[0].local=glm::scale(glm::mat4(1),{-2,.5f,1.5f});
        Node parent; parent.local=glm::translate(glm::mat4(1),{3,2,-4}); source.nodes.push_back(parent);
        Node other; other.mesh=0; other.local=glm::translate(glm::mat4(1),{-12,0,-12}); source.nodes.push_back(other);
        const auto triangles=flatten_scene(source); SceneSpatialIndex index; std::size_t matched=0;
        for(const auto structure:{SpatialStructure::bvh,SpatialStructure::octree}) {
            index.prepare(source,structure); require(index.statistics().nodes>0,"Spatial structure was not built");
            for(int y=-3;y<=3;++y) for(int x=-7;x<=7;++x) {
                Ray ray; ray.origin={float(x)+3,float(y)*.5f+2,20}; ray.direction={0,0,-2};
                TraversalStatistics traversal; const auto actual=index.pick(ray,&traversal); const auto expected=brute_force_intersect(triangles,ray);
                const auto fast=index.intersect(ray);
                require(fast.triangle==expected.triangle && fast.t==expected.t,"Render-only intersect API differs from picking");
                require(actual.hit.triangle==expected.triangle,"Tree picking differs from original triangle order/tie policy");
                require(bool(actual)==(expected.triangle>=0),"Pick success flag differs from actual hit");
                if(actual) {
                    require(actual.hit.t==expected.t && actual.node==triangles[std::size_t(expected.triangle)].object && actual.mesh==0 && actual.material==7,"Pick distance/object/mesh/material mapping wrong");
                    require(glm::length(actual.position-(ray.origin+ray.direction*expected.t))<1e-5f,"Non-normalized ray parameter used as distance");
                    require(std::abs(glm::length(actual.normal)-1)<1e-5f,"World normal not normalized"); ++matched;
                }
                require(traversal.box_tests>0,"Pick did not traverse selected hierarchy");
                require(index.occluded(ray)==bool(actual),"Occlusion query disagrees with picking");
            }
        }
        require(matched>0 && index.rebuilds()==2,"Both BVH and Octree were not exercised");
        const auto builds=index.rebuilds(); source.sky_top={1,0,0}; ++source.revision; index.prepare(source,SpatialStructure::octree);
        require(index.rebuilds()==builds,"Sky/global revision rebuilt query hierarchy");
        source.nodes[1].local[3].x+=2; index.prepare(source,SpatialStructure::octree);
        require(index.rebuilds()==builds+1,"World transform edit did not rebuild current hierarchy");
        return "210 条非单位方向射线分别遍历 BVH/Octree，对照真实世界三角形，含反射/缩放/父子变换";
    });
    run("Bounds and frustum queries preserve offscreen shadow geometry",[]() -> std::string {
        Scene source; source.meshes.push_back(grid(1,false)); source.materials.push_back({});
        Node visible_node; visible_node.mesh=0;
        Node behind=visible_node; behind.local=glm::translate(glm::mat4(1),{0,0,20});
        Node outside=visible_node; outside.local=glm::translate(glm::mat4(1),{500,0,0});
        source.nodes={visible_node,behind,outside}; SceneGeometryRuntime runtime; auto s=settings(); s.auto_lod=false;
        const auto camera=camera_at(10); const auto& snapshot=runtime.prepare(source,camera,s);
        const auto& index=runtime.spatial_index(); const auto candidates=index.visible_objects(camera,s.render_width,s.render_height);
        require(candidates==std::vector<int>{0} && runtime.stats().candidates==1,"Frustum candidates contain offscreen objects");
        require(snapshot.nodes.size()==3 && flatten_scene(snapshot).size()==6 && runtime.stats().selected_triangles==6,"Visibility query deleted offscreen shadow casters");
        require(index.query_bounds({{-1,-1,-.1f},{1,1,.1f}})==std::vector<int>{0},"Bounds query selected wrong world object");
        require(index.query_bounds({{496,-4,0},{496,-4,0}})==std::vector<int>{2},"Closed zero-thickness bounds query missed touching corner");
        require(index.query_bounds({{-1,-1,19},{1,1,21}})==std::vector<int>{1},"Bounds query lost geometry behind camera");
        auto crossing=source; crossing.nodes.resize(1); crossing.nodes[0].local=glm::rotate(glm::mat4(1),.5f,{0,1,0});
        SceneSpatialIndex near_index; near_index.prepare(crossing,SpatialStructure::octree); auto close=camera_at(1); close.near_plane=1;
        require(near_index.visible_objects(close,192,160)==std::vector<int>{0},"Near-plane intersecting bounds were incorrectly culled");
        return "视锥只报告可见候选，背后/视野外对象仍进入渲染快照供阴影使用；闭区间 bounds/近平面相交正确";
    });
    run("Implicit mesh instances do not render duplicated variant meshes",[]() -> std::string {
        auto source=scene_with_grid(); build_scene_lods(source); source.nodes.clear(); SceneGeometryRuntime runtime; auto s=settings();
        const auto& snapshot=runtime.prepare(source,camera_at(100),s);
        require(snapshot.nodes.empty() && snapshot.meshes.size()==source.meshes.size(),"Implicit instances duplicated appended variant meshes");
        require(runtime.stats().current_levels.size()==1 && runtime.stats().current_levels[0]>0 && runtime.stats().selected_triangles<128,"Implicit instance did not reduce");
        require(flatten_scene(snapshot).size()==runtime.stats().selected_triangles,"Implicit snapshot stats do not match actually rendered geometry");
        const Ray ray{{0,0,100},{0,0,-1},0,200}; const auto pick=runtime.spatial_index().pick(ray);
        require(bool(pick) && pick.node==0 && pick.mesh==0,"Implicit instance picking id differs from renderer object id");
        return "无节点场景以替换快照 mesh 的方式选级，避免追加变体导致重复绘制/阴影；拾取编号与渲染一致";
    });
    run("Missing LOD resources keep interactive prepare free of implicit QEM",[]() -> std::string {
        Scene source;
        for(int i=0;i<6;++i) source.meshes.push_back(grid(26));
        const auto original=source; SceneGeometryRuntime runtime; auto s=settings();
        const auto& snapshot=runtime.prepare(source,camera_at(100),s);
        require(runtime.stats().lod_builds==0 && runtime.stats().lod_cache_loads==0,"Prepare performed implicit QEM");
        require(runtime.stats().source_triangles==8112 && runtime.stats().selected_triangles==8112,"Missing resources incorrectly reduced geometry");
        for(const auto level:runtime.stats().current_levels) require(level==0,"Missing chain selected nonexistent level");
        require(snapshot.meshes.size()==6 && runtime.stats().lod_reports.size()==6,"Missing-chain scene was not staged in full");
        for(std::size_t i=0;i<6;++i) {
            require(same_base(source.meshes[i],original.meshes[i]) && source.meshes[i].lods.empty(),"Prepare implicitly populated source LOD cache");
            require(runtime.stats().lod_reports[i].reason.find("后台")!=std::string::npos,"Missing-chain UI status does not explain background generation");
        }
        return "6 个 1352 三角形网格缺链时保持全部 8112 三角形；prepare 不运行 QEM，逐网格提示后台生成";
    });
    run("Texture changes refresh snapshot only through source revision contract",[]() -> std::string {
        auto source=scene_with_grid(); build_scene_lods(source);
        Texture texture; texture.levels.emplace_back(2,2,glm::vec4(1)); source.textures.push_back(texture);
        SceneGeometryRuntime runtime; auto s=settings(); const auto camera=camera_at(100);
        const auto& first=runtime.prepare(source,camera,s); const auto* pixels=first.textures[0].levels[0].pixels.data();
        const auto revision=first.revision,trees=runtime.stats().spatial_rebuilds;
        ++source.revision; const auto& same=runtime.prepare(source,camera,s);
        require(same.revision==revision && same.textures[0].levels[0].pixels.data()==pixels,"Unrelated revision copied unchanged texture");
        source.sky_bottom={.4f,.5f,.6f}; ++source.revision; const auto& sky=runtime.prepare(source,camera,s);
        require(sky.textures[0].levels[0].pixels.data()==pixels,"Sky edit copied unchanged texture data");
        source.textures[0].levels[0].pixels[2]={1,0,0,1}; ++source.revision; const auto& edited=runtime.prepare(source,camera,s);
        require(edited.textures[0].levels[0].pixels[2]==glm::vec4(1,0,0,1),"Texture payload edit did not reach snapshot");
        require(runtime.stats().spatial_rebuilds==trees && runtime.stats().lod_builds==0,"Texture edit rebuilt geometry");
        return "纹理原地编辑遵守 source revision；无关 revision 与 sky 编辑不复制相同像素资源，不重建几何";
    });
    run("Editor pose revisions keep resident CPU mesh and texture storage",[]() -> std::string {
        auto source=scene_with_grid();source.asset_revision=1;Texture texture;texture.levels.emplace_back(257,129,glm::vec4(1));source.textures.push_back(std::move(texture));
        SceneGeometryRuntime runtime;auto s=settings();s.auto_lod=false;const auto camera=camera_at(100);
        const auto& first=runtime.prepare(source,camera,s);const auto* pixels=first.textures[0].levels[0].pixels.data();const auto* vertices=first.meshes[0].vertices.data();
        for(int i=0;i<8;++i){source.nodes[0].local[3].x=float(i+1);++source.revision;const auto& moved=runtime.prepare(source,camera,s);
            require(moved.nodes[0].local[3].x==float(i+1),"Pose matrix did not update");
            require(moved.textures[0].levels[0].pixels.data()==pixels&&moved.meshes[0].vertices.data()==vertices,"Pose edit deep-copied static asset storage");}
        source.textures[0].levels[0].pixels[2]={1,0,0,1};++source.asset_revision;++source.revision;
        require(runtime.prepare(source,camera,s).textures[0].levels[0].pixels[2]==glm::vec4(1,0,0,1),"Static texture edit ignored asset revision");
        s.auto_lod=true;require(runtime.prepare(source,camera,s).asset_revision==0,"Automatic LOD incorrectly reused static asset contract");
        return "连续 8 次变换复用像素与顶点存储；资源变更仍更新纹理，自动 LOD 保留完整上传路径";
    });
    run("Scene geometry propagates HDR replacement and sliders without copying immutable pixels",[]() -> std::string {
        auto source=scene_with_grid();source.asset_revision=1;Settings s;s.auto_lod=false;
        SceneGeometryRuntime runtime;auto camera=camera_at(10);(void)runtime.prepare(source,camera,s);
        source.environment_map=make_environment_map(Image<glm::vec3>(4,2,{2,1,.5f}));++source.revision;
        const auto& imported=runtime.prepare(source,camera,s);
        require(imported.environment_map==source.environment_map,"HDR replacement lost at scene preparation boundary");
        source.environment_intensity=.4f;source.environment_rotation=82;++source.revision;
        const auto& adjusted=runtime.prepare(source,camera,s);
        require(adjusted.environment_intensity==.4f&&adjusted.environment_rotation==82,"HDR sliders ignored by prepared scene");
        require(adjusted.environment_map==source.environment_map,"Immutable HDR pixels copied instead of shared");
        return "Renderer consumes the updated HDR identity, rotation and intensity";
    });
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_SCENE_GEOMETRY_TEST_MAIN
#include <iostream>
int main() {
    const auto tests=emberframe::lab::test_scene_geometry(); std::size_t failed=0;
    for(const auto& test:tests) {
        std::cout<<(test.passed?"PASS ":"FAIL ")<<test.name<<": "<<test.detail<<'\n'; if(!test.passed) ++failed;
    }
    std::cout<<tests.size()-failed<<'/'<<tests.size()<<" scene geometry tests passed\n"; return failed?1:0;
}
#endif
