#include "mesh_lod.h"
#include <cmath>
#include <set>
#include <sstream>

namespace emberframe::lab {
namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void near(double actual,double expected,double tolerance,const char* message) {
    if(!std::isfinite(actual) || std::abs(actual-expected)>tolerance) {
        std::ostringstream text; text<<message<<": actual="<<actual<<", expected="<<expected;
        throw std::runtime_error(text.str());
    }
}
template<class F> void rejects(F&& action,const char* message) {
    bool rejected=false; try { action(); } catch(const std::invalid_argument&) { rejected=true; }
    require(rejected,message);
}
Mesh grid_mesh(bool material_seam=false,bool uv_seam=false,bool curved=false) {
    Mesh mesh; mesh.name="QEM analytic grid";
    for(int y=0;y<=8;++y) for(int x=0;x<=8;++x) {
        Vertex v; v.position={float(x),float(y),curved?0.3f*std::sin(x*0.45f)*std::sin(y*0.45f):0};
        v.normal={0,0,1}; v.uv={float(x)/8,float(y)/8}; v.tangent={1,0,0,1}; v.color={v.uv,0.5f,1};
        v.baked_irradiance={v.uv,.25f}; // 可检验的线性烘焙属性，折叠后必须沿同一对应关系插值。
        mesh.vertices.push_back(v);
    }
    std::array<std::uint32_t,9> split{};
    if(uv_seam) for(int y=0;y<=8;++y) {
        auto v=mesh.vertices[std::size_t(y*9+4)]; v.uv.x+=1;
        split[std::size_t(y)]=std::uint32_t(mesh.vertices.size()); mesh.vertices.push_back(v);
    }
    for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
        auto index=[&](int vx,int vy) { return uv_seam && x>=4 && vx==4?split[std::size_t(vy)]:std::uint32_t(vy*9+vx); };
        const auto a=index(x,y),b=index(x+1,y),c=index(x+1,y+1),d=index(x,y+1);
        const auto first=std::uint32_t(mesh.indices.size());
        mesh.indices.insert(mesh.indices.end(),{a,b,c,a,c,d});
        mesh.primitives.push_back({first,6,material_seam && x>=4?1u:0u});
    }
    return mesh;
}
Mesh octahedron() {
    Mesh mesh; mesh.name="Octahedron";
    for(const auto p:{glm::vec3(1,0,0),glm::vec3(0,1,0),glm::vec3(-1,0,0),glm::vec3(0,-1,0),glm::vec3(0,0,1),glm::vec3(0,0,-1)}) {
        Vertex v; v.position=p; v.normal=p; mesh.vertices.push_back(v);
    }
    for(std::uint32_t i=0;i<4;++i) mesh.indices.insert(mesh.indices.end(),{4u,i,(i+1)%4,5u,(i+1)%4,i});
    return mesh;
}
void check_positive_grid_faces(const Mesh& mesh) {
    require(mesh.indices.size()%3==0,"Output index count is not triangular");
    std::set<std::array<std::uint32_t,3>> faces;
    for(std::size_t i=0;i<mesh.indices.size();i+=3) {
        std::array<std::uint32_t,3> ids{mesh.indices[i],mesh.indices[i+1],mesh.indices[i+2]};
        for(const auto id:ids) require(id<mesh.vertices.size(),"Output index out of bounds");
        const auto cross=glm::cross(mesh.vertices[ids[1]].position-mesh.vertices[ids[0]].position,
                                    mesh.vertices[ids[2]].position-mesh.vertices[ids[0]].position);
        require(cross.z>0 && std::isfinite(cross.z),"QEM emitted a flipped/degenerate grid triangle");
        std::sort(ids.begin(),ids.end()); require(faces.insert(ids).second,"QEM emitted duplicate faces");
    }
}
Camera lod_camera(float z) {
    Camera camera; camera.position={4,4,z}; camera.target={4,4,0}; camera.fov=60;
    camera.near_plane=0.1f; camera.far_plane=10000; return camera;
}
} // namespace

TestResults test_mesh_lod() {
    TestResults results;
    auto run=[&](const char* name,auto&& action) {
        try { results.push_back({name,true,action()}); }
        catch(const std::exception& error) { results.push_back({name,false,error.what()}); }
        catch(...) { results.push_back({name,false,"Unknown exception"}); }
    };
    run("QEM performs deterministic plane-quadric edge collapses",[]() -> std::string {
        const auto input=grid_mesh(); const auto result=simplify_qem(input,{72});
        require(result.target_reached && result.output_triangles<=72 && result.collapses>0,"QEM did not actually reduce geometry");
        require(result.input_triangles==128 && result.output_triangles==result.mesh.indices.size()/3,"QEM statistics do not describe actual output");
        require(result.input_triangles-result.output_triangles==result.collapses*2,"Interior edge collapse should remove two faces");
        near(result.max_collapse_error,0,1e-14,"Planar QEM plane error");
        require(result.geometric_error>0,"Zero plane error incorrectly claimed zero displacement bound");
        check_positive_grid_faces(result.mesh);
        double area=0;
        for(std::size_t i=0;i<result.mesh.indices.size();i+=3) {
            const auto a=result.mesh.vertices[result.mesh.indices[i]].position,b=result.mesh.vertices[result.mesh.indices[i+1]].position,c=result.mesh.vertices[result.mesh.indices[i+2]].position;
            area+=0.5*glm::cross(b-a,c-a).z;
        }
        near(area,64,1e-5,"Locked grid boundary changed covered area");
        for(const auto& original:input.vertices) if(original.position.x==0 || original.position.x==8 || original.position.y==0 || original.position.y==8) {
            bool found=false; for(const auto& v:result.mesh.vertices) if(v.position==original.position && v.uv==original.uv) found=true;
            require(found,"QEM changed boundary vertex or UV");
        }
        for(const auto& v:result.mesh.vertices) {
            near(v.uv.x,v.position.x/8,2e-6,"Planar UV x interpolation"); near(v.uv.y,v.position.y/8,2e-6,"Planar UV y interpolation");
            near(v.color.x,v.uv.x,2e-6,"Color interpolation");
            near(v.baked_irradiance.x,v.uv.x,2e-6,"Baked irradiance x interpolation");
            near(v.baked_irradiance.y,v.uv.y,2e-6,"Baked irradiance y interpolation");
            near(v.baked_irradiance.z,.25,2e-6,"Baked irradiance constant interpolation");
        }
        const auto repeated=simplify_qem(input,{72}); require(repeated.mesh.indices==result.mesh.indices,"QEM is not deterministic");
        require(repeated.mesh.vertices.size()==result.mesh.vertices.size(),"Nondeterministic vertex count");
        for(std::size_t i=0;i<result.mesh.vertices.size();++i) require(result.mesh.vertices[i].position==repeated.mesh.vertices[i].position,"Nondeterministic collapse position");
        std::ostringstream detail; detail<<"128 -> "<<result.output_triangles<<" triangles; "<<result.collapses<<" collapses; conservative displacement="<<result.geometric_error;
        return detail.str();
    });
    run("QEM protects material boundaries and split UV seams",[]() -> std::string {
        for(bool uv:{false,true}) {
            const auto input=grid_mesh(!uv,uv); const auto result=simplify_qem(input,{88});
            require(result.collapses>0,"Seam protection prevented every interior collapse");
            check_positive_grid_faces(result.mesh);
            for(const auto& original:input.vertices) if(original.position.x==4) {
                bool found=false;
                for(const auto& v:result.mesh.vertices) if(v.position==original.position && v.uv==original.uv) found=true;
                require(found,"Material or split-UV seam vertex changed/disappeared");
            }
            if(!uv) for(const auto& primitive:result.mesh.primitives) for(std::uint32_t i=primitive.first_index;i<primitive.first_index+primitive.index_count;++i) {
                const auto x=result.mesh.vertices[result.mesh.indices[i]].position.x;
                require(primitive.material==0?x<=4:x>=4,"Face crossed material seam");
            }
        }
        return "Interior reduction succeeds on both sides; every material/UV seam position and attribute survives";
    });
    run("QEM curved meshes, normal checks and explicit work/error budgets",[]() -> std::string {
        const auto curved=simplify_qem(grid_mesh(false,false,true),{92});
        require(curved.collapses>0 && curved.output_triangles<128,"Curved mesh was not simplified");
        require(curved.max_collapse_error>0 && curved.geometric_error>0,"Curved simplification reported fake zero error");
        check_positive_grid_faces(curved.mesh);
        // 重新验证输出的一环连通性、流形边与绕序，不能只检查面数。
        (void)simplify_qem(curved.mesh,{curved.output_triangles,0});
        const auto input=octahedron();
        const auto zero_error=simplify_qem(input,{4,4096,0});
        require(zero_error.collapses==0 && !zero_error.target_reached && zero_error.stop_reason==QemStopReason::error_budget,"Strict error budget was not enforced/reported");
        const auto no_work=simplify_qem(input,{4,0});
        require(no_work.collapses==0 && no_work.stop_reason==QemStopReason::collapse_budget,"Zero collapse budget ignored");
        const auto one=simplify_qem(input,{4,1});
        require(one.collapses==1 && one.output_triangles==6 && !one.target_reached,"Bounded closed-mesh collapse not executed");
        require(one.stop_reason==QemStopReason::collapse_budget,"Partial simplification reason missing");
        (void)simplify_qem(one.mesh,{one.output_triangles,0});
        std::ostringstream detail; detail<<"Curved 128 -> "<<curved.output_triangles<<", max QEM error="<<curved.max_collapse_error<<"; closed octahedron 8 -> 6 with budget 1";
        return detail.str();
    });
    run("QEM refuses unsafe topology and honestly reports no reduction",[]() -> std::string {
        Mesh one; one.vertices.resize(3); one.vertices[0].position={0,0,0}; one.vertices[1].position={1,0,0}; one.vertices[2].position={0,1,0}; one.indices={0,1,2};
        const auto locked=simplify_qem(one,{0});
        require(locked.collapses==0 && locked.output_triangles==1 && !locked.target_reached,"Boundary-only mesh reported a fake reduction");
        require(locked.stop_reason==QemStopReason::topology_or_seams && !locked.reason.empty(),"No-reduction reason missing");
        Mesh duplicate=one; duplicate.indices.insert(duplicate.indices.end(),{0,1,2});
        rejects([&]{ (void)simplify_qem(duplicate,{0}); },"Duplicate faces accepted");
        Mesh winding=one; Vertex fourth; fourth.position={1,1,0}; winding.vertices.push_back(fourth); winding.indices.insert(winding.indices.end(),{1,2,3});
        rejects([&]{ (void)simplify_qem(winding,{0}); },"Inconsistent shared-edge winding accepted");
        Mesh zero=one; zero.vertices[2].position={2,0,0}; rejects([&]{ (void)simplify_qem(zero,{0}); },"Zero-area face accepted");
        Mesh invalid=one; invalid.indices[2]=99; rejects([&]{ (void)simplify_qem(invalid,{0}); },"Invalid index accepted");
        const auto empty=simplify_qem(Mesh{},{0}); require(empty.target_reached && empty.output_triangles==0 && empty.collapses==0,"Empty mesh result");
        return "Boundary-only input remains unchanged with reason; invalid winding, duplicate/zero faces and invalid indices rejected";
    });
    run("LOD chain contains genuine reductions with monotone error bounds",[]() -> std::string {
        LodBuildOptions options; options.triangle_ratios={0.75f,0.5f,0.375f};
        const auto chain=build_lod_chain(grid_mesh(),options);
        require(chain.levels.size()>=3 && chain.levels.front().triangles==128,"Missing real LOD levels");
        for(std::size_t i=1;i<chain.levels.size();++i) {
            require(chain.levels[i].triangles<chain.levels[i-1].triangles,"Duplicate LOD pretending to reduce geometry");
            require(chain.levels[i].geometric_error>=chain.levels[i-1].geometric_error,"Nonconservative decreasing LOD error");
            require(chain.levels[i].triangles==chain.levels[i].mesh.indices.size()/3,"LOD count does not match mesh");
        }
        auto disabled=options; disabled.max_collapses_per_level=0;
        const auto unchanged=build_lod_chain(grid_mesh(),disabled);
        require(unchanged.levels.size()==1 && unchanged.attempts.size()==3,"No-work LOD invented duplicate levels");
        for(const auto& attempt:unchanged.attempts) require(!attempt.appended && attempt.stop_reason==QemStopReason::collapse_budget && !attempt.reason.empty(),"Missing failed-LOD reason");
        options.triangle_ratios={0.5f,0.75f}; rejects([&]{ (void)build_lod_chain(grid_mesh(),options); },"Unsorted LOD ratios accepted");
        std::ostringstream detail; detail<<"Actual LOD triangles:"; for(const auto& level:chain.levels) detail<<' '<<level.triangles;
        return detail.str();
    });
    run("LOD screen-space selection respects distance, scale and near plane",[]() -> std::string {
        LodBuildOptions options; options.triangle_ratios={0.75f,0.5f}; const auto chain=build_lod_chain(grid_mesh(),options);
        require(chain.levels.size()==3,"Screen selection test requires three real levels");
        const auto near_camera=lod_camera(10),far_camera=lod_camera(500);
        const double near_error=projected_lod_error_pixels(chain.levels.back().geometric_error,chain.bounds,glm::mat4(1),near_camera,1080);
        const double far_error=projected_lod_error_pixels(chain.levels.back().geometric_error,chain.bounds,glm::mat4(1),far_camera,1080);
        require(far_error<near_error,"Distance did not reduce projected error");
        const double budget=far_error*1.01;
        require(select_lod(chain,glm::mat4(1),far_camera,1080,budget)==chain.levels.size()-1,"Far view did not select eligible coarse level");
        require(select_lod(chain,glm::mat4(1),near_camera,1080,budget)<chain.levels.size()-1,"Near view incorrectly selected same coarse level");
        require(select_lod(chain,glm::mat4(1),far_camera,1080,0)==0,"Zero pixel error budget did not select full detail");
        const auto scale=glm::scale(glm::mat4(1),{4,1,2});
        require(projected_lod_error_pixels(0.1,chain.bounds,scale,far_camera,1080)>projected_lod_error_pixels(0.1,chain.bounds,glm::mat4(1),far_camera,1080),"Nonuniform scale not reflected in screen error");
        const auto close_camera=lod_camera(0.11f);
        require(std::isinf(projected_lod_error_pixels(0.1,chain.bounds,glm::mat4(1),close_camera,1080)),"Near-plane error expansion not rejected");
        require(select_lod(chain,glm::mat4(1),close_camera,1080,1e8)==0,"Near-plane crossing selected reduced geometry");
        return "Distance selects coarser geometry; nonuniform scale raises error; near-plane crossing and zero budget use level 0";
    });
    run("LOD projection error bound covers analytic displaced points",[]() -> std::string {
        const Aabb box{{-1,-1,-1},{1,1,1}};
        auto camera=lod_camera(20); camera.position={0,0,20}; camera.target={0,0,0};
        auto world=glm::translate(glm::mat4(1),{3,1,0})*glm::scale(glm::mat4(1),{2,0.5f,1}); world[1][0]+=0.35f;
        const double bound=projected_lod_error_pixels(0.1,box,world,camera,1080);
        const glm::dmat4 vp=glm::dmat4(camera.projection(16.0f/9.0f))*glm::dmat4(camera.view())*glm::dmat4(world);
        for(int corner=0;corner<8;++corner) for(int axis=0;axis<3;++axis) for(double sign:{-1.0,1.0}) {
            const glm::dvec3 p((corner&1)?1:-1,(corner&2)?1:-1,(corner&4)?1:-1);
            auto q=p; q[axis]+=sign*0.1;
            const auto a=vp*glm::dvec4(p,1),b=vp*glm::dvec4(q,1);
            const glm::dvec2 delta=(glm::dvec2(b)/b.w-glm::dvec2(a)/a.w)*glm::dvec2(960,-540);
            require(glm::length(delta)<=bound+1e-8,"Conservative pixel bound underestimates analytic projection");
        }
        return "48 displaced corner samples with nonuniform scale, shear and off-axis perspective fit the bound";
    });
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_MESH_LOD_TEST_MAIN
#include <iostream>
int main() {
    const auto tests=emberframe::lab::test_mesh_lod(); std::size_t failed=0;
    for(const auto& test:tests) { std::cout<<(test.passed?"PASS ":"FAIL ")<<test.name<<": "<<test.detail<<'\n'; if(!test.passed) ++failed; }
    std::cout<<tests.size()-failed<<'/'<<tests.size()<<" mesh LOD tests passed\n";
    return failed?1:0;
}
#endif
