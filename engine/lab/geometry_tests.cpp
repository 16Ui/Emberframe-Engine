#include "geometry.h"
#include "shading.h"
#include <cmath>
#include <sstream>

namespace emberframe::lab {
namespace {
void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
void near(double actual,double expected,double tolerance,const char* message) {
    if(!std::isfinite(actual) || std::abs(actual-expected)>tolerance) {
        std::ostringstream out; out<<message<<": actual="<<actual<<", expected="<<expected<<", tolerance="<<tolerance;
        throw std::runtime_error(out.str());
    }
}
template<glm::length_t N>
void near_vector(const glm::vec<N,float>& actual,const glm::vec<N,float>& expected,float tolerance,const char* message) {
    for(glm::length_t i=0;i<N;++i) near(actual[i],expected[i],tolerance,message);
}
template<class F> void rejects(F&& f,const char* message) {
    bool rejected=false; try { f(); } catch(const std::invalid_argument&) { rejected=true; }
    require(rejected,message);
}
Triangle triangle(glm::vec3 a,glm::vec3 b,glm::vec3 c) {
    Triangle t; t.vertices[0].position=a; t.vertices[1].position=b; t.vertices[2].position=c;
    for(auto& v:t.vertices) { v.normal={0,0,1}; v.tangent={1,0,0,1}; }
    return t;
}
Camera test_camera() {
    Camera camera; camera.position={0,0,0}; camera.target={0,0,-1}; camera.fov=90;
    camera.near_plane=0.5f; camera.far_plane=16; return camera;
}
Settings test_settings(bool reversed=false) {
    Settings settings; settings.render_width=32; settings.render_height=32; settings.reversed_z=reversed; return settings;
}
glm::vec3 screen_point(glm::vec2 p,float depth=2) { return {(p.x/16-1)*depth,(1-p.y/16)*depth,-depth}; }
Scene make_scene(const std::vector<Triangle>& triangles) {
    Scene scene; Mesh mesh;
    for(const auto& t:triangles) {
        const auto first=std::uint32_t(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(),t.vertices.begin(),t.vertices.end());
        mesh.primitives.push_back({first,3,std::uint32_t(std::max(0,t.material))});
    }
    scene.meshes.push_back(std::move(mesh));
    return scene;
}
std::array<Triangle,2> quad(const std::array<glm::vec2,4>& p,bool other_diagonal) {
    const auto a=screen_point(p[0]),b=screen_point(p[1]),c=screen_point(p[2]),d=screen_point(p[3]);
    if(other_diagonal) return {triangle(a,b,d),triangle(b,c,d)};
    return {triangle(a,b,c),triangle(a,c,d)};
}
void same_hit(const Hit& actual,const Hit& expected,const char* message) {
    if(actual.triangle!=expected.triangle) {
        std::ostringstream out; out<<message<<": triangle="<<actual.triangle<<", expected="<<expected.triangle;
        throw std::runtime_error(out.str());
    }
    if(expected.triangle>=0) {
        near(actual.t,expected.t,1e-5*std::max(1.0f,std::abs(expected.t)),message);
        near_vector(actual.barycentric,expected.barycentric,2e-5f,message);
    }
}
struct Random {
    std::uint32_t value=0x9e3779b9u;
    float unit() { value=value*1664525u+1013904223u; return float(value>>8)*(1.0f/16777216.0f); }
    float signed_unit() { return 2*unit()-1; }
    glm::vec3 vector() { const float x=signed_unit(),y=signed_unit(),z=signed_unit(); return {x,y,z}; }
};
std::vector<Triangle> scattered_triangles() {
    Random random; std::vector<Triangle> triangles;
    for(int i=0;i<240;++i) {
        const auto center=random.vector()*8.0f;
        const auto a=random.vector()*0.9f,b=random.vector()*0.9f;
        triangles.push_back(triangle(center-a,center+a,center+b));
    }
    // 大跨块三角形、薄长三角形、退化图元以及重复面用于检验父节点驻留和编号决胜。
    triangles.push_back(triangle({-10,-10,0},{10,-10,0},{0,10,0}));
    triangles.push_back(triangle({-20,0,3},{20,0,3},{0,0.0001f,3}));
    triangles.push_back(triangle({0,0,0},{0,0,0},{0,0,0}));
    triangles.push_back(triangles[0]); return triangles;
}
ClipVertex clip_vertex(glm::vec4 p) {
    ClipVertex v; v.clip=p;
    v.attributes.position={2*p.x+3*p.w,p.y-p.z,0.25f*p.z};
    v.attributes.uv={p.x+2*p.w,p.y-p.z};
    v.attributes.normal={p.x,p.y,p.z};
    v.attributes.tangent={p.y,p.z,p.w,p.x};
    v.attributes.color={p.x,p.y,p.z,p.w};
    v.previous_position=glm::vec3(p)*0.5f; return v;
}
void check_clip_vertex(const ClipVertex& v) {
    const auto p=v.clip;
    require(p.x>=-p.w-2e-6f && p.x<=p.w+2e-6f && p.y>=-p.w-2e-6f &&
            p.y<=p.w+2e-6f && p.z>=-2e-6f && p.z<=p.w+2e-6f,"Clip vertex outside Vulkan volume");
    const auto expected=clip_vertex(p);
    near_vector(v.attributes.position,expected.attributes.position,3e-6f,"Clipped world attribute");
    near_vector(v.attributes.uv,expected.attributes.uv,3e-6f,"Clipped UV attribute");
    near_vector(v.attributes.normal,expected.attributes.normal,3e-6f,"Clipping must not normalize normals");
    near_vector(v.attributes.tangent,expected.attributes.tangent,3e-6f,"Clipped tangent attribute");
    near_vector(v.attributes.color,expected.attributes.color,3e-6f,"Clipped color attribute");
    near_vector(v.previous_position,expected.previous_position,3e-6f,"Clipped previous position");
}
double projected_area(const std::vector<ClippedTriangle>& triangles) {
    double sum=0;
    for(const auto& t:triangles) {
        if(t[0].clip.w<=0 || t[1].clip.w<=0 || t[2].clip.w<=0) continue;
        const glm::dvec2 a=glm::dvec2(t[0].clip)/double(t[0].clip.w),
                         b=glm::dvec2(t[1].clip)/double(t[1].clip.w),
                         c=glm::dvec2(t[2].clip)/double(t[2].clip.w);
        sum+=std::abs((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x))*0.5;
    }
    return sum;
}
} // namespace

TestResults test_geometry() {
    TestResults results;
    auto run=[&](const char* name,auto&& test) {
        try { const std::string detail=test(); results.push_back({name,true,detail}); }
        catch(const std::exception& e) { results.push_back({name,false,e.what()}); }
        catch(...) { results.push_back({name,false,"Unknown exception"}); }
    };
    run("D01 six homogeneous planes and all attributes",[]() -> std::string {
        for(int plane=0;plane<6;++plane) {
            ClippedTriangle t{clip_vertex({-0.4f,-0.4f,0.4f,1}),clip_vertex({0.4f,-0.4f,0.4f,1}),clip_vertex({0,0.4f,0.4f,1})};
            auto p=t[2].clip;
            if(plane<4) p[plane/2]=plane%2?2.0f:-2.0f;
            else p.z=plane==4?-1.0f:2.0f;
            t[2]=clip_vertex(p);
            const auto clipped=clip_triangle(t);
            require(!clipped.empty(),"Crossing triangle discarded");
            for(const auto& piece:clipped) for(const auto& v:piece) check_clip_vertex(v);
            std::swap(t[0],t[2]);
            const auto reversed=clip_triangle(t);
            near(projected_area(reversed),projected_area(clipped),2e-6,"Winding changed clipped area");
        }
        return "All six planes; UV/world/normal/tangent/color/history linear invariants; both windings";
    });
    run("D01 outside, behind eye, plane endpoints and invalid inputs",[]() -> std::string {
        for(int plane=0;plane<6;++plane) {
            ClippedTriangle t;
            for(int k=0;k<3;++k) {
                glm::vec4 p{float(k)*0.2f,float(k)*0.1f,0.5f,1};
                if(plane<4) p[plane/2]=plane%2?2.0f:-2.0f; else p.z=plane==4?-1.0f:2.0f;
                t[k]=clip_vertex(p);
            }
            require(clip_triangle(t).empty(),"Completely outside triangle survived");
        }
        ClippedTriangle behind{clip_vertex({-0.6f,-0.5f,0.4f,1}),clip_vertex({0.6f,-0.5f,0.4f,1}),clip_vertex({0,0.7f,-0.5f,-0.3f})};
        const auto clipped=clip_triangle(behind); require(!clipped.empty(),"Eye crossing lost visible portion");
        for(const auto& t:clipped) for(const auto& v:t) check_clip_vertex(v);
        ClippedTriangle on_plane{clip_vertex({-0.5f,-0.5f,0,1}),clip_vertex({0.5f,-0.5f,0,1}),clip_vertex({0,0.5f,0,1})};
        require(clip_triangle(on_plane).size()==1,"On-near-plane triangle changed");
        on_plane[2]=on_plane[0]; require(clip_triangle(on_plane).empty(),"Duplicate degenerate vertices survived");
        on_plane[0].clip.x=std::numeric_limits<float>::quiet_NaN();
        rejects([&]{ (void)clip_triangle(on_plane); },"Non-finite clip input accepted");
        return "Outside rejection, negative w, exact near plane, repeated vertices, NaN rejection";
    });
    run("D01 shared clipped edge is orientation independent",[]() -> std::string {
        const auto a=clip_vertex({-2,-0.3f,0.5f,1}),b=clip_vertex({0.5f,0.7f,0.5f,1});
        const auto first=clip_triangle({a,b,clip_vertex({0,-0.8f,0.5f,1})});
        const auto second=clip_triangle({b,a,clip_vertex({0,0.9f,0.5f,1})});
        bool found=false;
        for(const auto& ta:first) for(const auto& va:ta)
            if(va.clip.x==-1 && std::abs(va.clip.y-0.1f)<1e-6f)
                for(const auto& tb:second) for(const auto& vb:tb) if(va.clip==vb.clip) {
                    require(va.attributes.uv==vb.attributes.uv,"Reversed shared edge UV differs"); found=true;
                }
        require(found,"No identical shared clipping intersection"); return "Bit-identical shared intersection in both edge directions";
    });
    run("Scene hierarchy, reflected scale, inverse-transpose and tangents",[]() -> std::string {
        auto t=triangle({0,0,0},{1,-1,0},{1,0,-1});
        for(auto& v:t.vertices) { v.normal=glm::normalize(glm::vec3(1,1,1)); v.tangent={glm::normalize(glm::vec3(1,-1,0)),1}; }
        auto scene=make_scene({t}); scene.nodes.resize(3);
        scene.nodes[0].parent=2; scene.nodes[0].mesh=0; scene.nodes[0].local=glm::translate(glm::mat4(1),{1,2,3});
        scene.nodes[1].parent=0;
        scene.nodes[2].local=glm::rotate(glm::mat4(1),0.31f,glm::vec3(0,0,1))*glm::scale(glm::mat4(1),{-2,3,0.5f});
        const auto world=resolve_world_transforms(scene); const auto flat=flatten_scene(scene);
        require(flat.size()==1 && flat[0].object==0,"Mesh instance mapping changed");
        const auto expected=scene.nodes[2].local*scene.nodes[0].local;
        const auto expected_normal=glm::normalize(glm::transpose(glm::inverse(glm::mat3(expected)))*t.vertices[0].normal);
        for(int k=0;k<3;++k) {
            const auto& v=flat[0].vertices[k];
            near_vector(v.position,glm::vec3(expected*glm::vec4(t.vertices[k].position,1)),2e-6f,"Parent order position");
            near_vector(v.normal,expected_normal,2e-6f,"Inverse-transpose normal");
            near(glm::dot(v.normal,glm::vec3(v.tangent)),0,2e-6,"Normal and tangent not perpendicular");
            near(v.tangent.w,-1,0,"Reflected tangent handedness");
        }
        const auto edge=flat[0].vertices[1].position-flat[0].vertices[0].position;
        near(glm::dot(flat[0].vertices[0].normal,edge),0,2e-6,"Normal is not perpendicular to transformed edge");
        near_vector(glm::vec3(world[1][3]),glm::vec3(expected[3]),2e-6f,"Grandchild resolved incorrectly");
        return "Arbitrary parent order; negative determinant; nonuniform scale; normal/tangent orthogonality";
    });
    run("Parallel and zero tangent fallback stays perpendicular through rasterization",[]() -> std::string {
        const auto n=glm::normalize(glm::vec3(1,1,1));
        for(auto direction:{n,glm::vec3(0)})for(float sign:{-1.f,1.f}) {
            auto t=triangle({-1,-1,-2},{1,-1,-2},{0,1,-2});
            for(auto& v:t.vertices){v.normal=n;v.tangent=glm::vec4(direction,sign);}
            const auto scene=make_scene({t});const auto flat=flatten_scene(scene);
            for(const auto& v:flat.at(0).vertices) {
                near(glm::length(glm::vec3(v.tangent)),1,2e-6,"Fallback tangent length");
                near(glm::dot(v.normal,glm::vec3(v.tangent)),0,2e-6,"Parallel input became a normal-aligned tangent");
                near(v.tangent.w,sign,0,"Fallback lost handedness");
            }
            const auto image=rasterize(scene,test_camera(),test_settings());std::size_t samples=0;
            for(const auto& s:image.pixels)if(s.valid) {
                ++samples;near(glm::length(glm::vec3(s.tangent)),1,2e-6,"Raster fallback length");
                near(glm::dot(s.normal,glm::vec3(s.tangent)),0,2e-6,"Raster frame is not perpendicular");
                near(s.tangent.w,sign,0,"Raster fallback handedness");
            }
            require(samples>0,"Fallback regression rasterized no samples");
        }
        return "Non-axis unit normals, parallel/zero tangents and both signs remain orthonormal in flatten/raster";
    });
    run("Scene graph validation, nonindexed meshes and deep hierarchy",[]() -> std::string {
        auto scene=make_scene({triangle({0,0,0},{1,0,0},{0,1,0})});
        scene.nodes.resize(3); scene.nodes[0].mesh=0; scene.nodes[1].parent=2; scene.nodes[2].parent=1;
        rejects([&]{ (void)flatten_scene(scene); },"Disconnected cycle accepted");
        scene.nodes[1].parent=-1; scene.nodes[2].parent=9;
        rejects([&]{ (void)flatten_scene(scene); },"Out-of-range parent accepted");
        scene.nodes[2].parent=-1; scene.nodes[2].local=glm::scale(glm::mat4(1),{1,0,1});
        rejects([&]{ (void)flatten_scene(scene); },"Singular node accepted");
        scene.nodes.clear(); scene.meshes[0].primitives.clear();
        require(flatten_scene(scene).size()==1,"Nonindexed implicit primitive failed");
        scene.meshes[0].indices={0,1,9};
        rejects([&]{ (void)flatten_scene(scene); },"Invalid mesh index accepted");
        scene.meshes[0].indices={0,1,2}; scene.nodes.resize(12000);
        for(int i=0;i<11999;++i) scene.nodes[std::size_t(i)].parent=i+1;
        scene.nodes[0].mesh=0;
        scene.nodes.back().local=glm::translate(glm::mat4(1),{2,0,0});
        near(flatten_scene(scene)[0].vertices[0].position.x,2,0,"Deep hierarchy translation");
        return "Disconnected cycle, invalid references, singular matrix, malformed index and 12000-node chain";
    });
    run("D02 top-left rectangle coverage and submission invariance",[]() -> std::string {
        std::size_t cases=0;
        for(float shift:{0.0f,0.25f,0.5f,0.75f}) for(bool diagonal:{false,true}) for(bool reverse:{false,true}) {
            const float lo=4+shift,hi=28+shift;
            auto pair=quad({glm::vec2(lo,lo),{hi,lo},{hi,hi},{lo,hi}},diagonal);
            if(reverse) for(auto& t:pair) std::swap(t.vertices[0],t.vertices[2]);
            pair[0].vertices[0].color=pair[0].vertices[1].color=pair[0].vertices[2].color={1,0,0,1};
            pair[1].vertices[0].color=pair[1].vertices[1].color=pair[1].vertices[2].color={0,1,0,1};
            const auto a=rasterize(make_scene({pair[0]}),test_camera(),test_settings());
            const auto b=rasterize(make_scene({pair[1]}),test_camera(),test_settings());
            const auto forward=rasterize(make_scene({pair[0],pair[1]}),test_camera(),test_settings());
            const auto backward=rasterize(make_scene({pair[1],pair[0]}),test_camera(),test_settings());
            for(int y=0;y<32;++y) for(int x=0;x<32;++x) {
                const int expected=x+0.5f>=lo && x+0.5f<hi && y+0.5f>=lo && y+0.5f<hi ? 1:0;
                require(int(a.at(x,y).valid)+int(b.at(x,y).valid)==expected,"Shared edge hole or double coverage");
                require(forward.at(x,y).valid==backward.at(x,y).valid,"Draw order changed mask");
                if(expected) near_vector(forward.at(x,y).vertex_color,backward.at(x,y).vertex_color,0,"Draw order changed shared-edge owner");
            }
            ++cases;
        }
        return std::to_string(cases)+" cases; separately rasterized masks prove exactly-one coverage";
    });
    run("D02 rotated quad and clipped shared edges",[]() -> std::string {
        std::array<glm::vec2,4> p{glm::vec2(-10,-10),{10,-10},{10,10},{-10,10}};
        for(auto& v:p) v=glm::vec2(std::cos(0.27f)*v.x-std::sin(0.27f)*v.y,std::sin(0.27f)*v.x+std::cos(0.27f)*v.y)+glm::vec2(16);
        std::vector<int> reference(1024,0);
        for(bool diagonal:{false,true}) {
            const auto pair=quad(p,diagonal);
            const auto a=rasterize(make_scene({pair[0]}),test_camera(),test_settings());
            const auto b=rasterize(make_scene({pair[1]}),test_camera(),test_settings());
            for(std::size_t i=0;i<reference.size();++i) {
                const int count=int(a.pixels[i].valid)+int(b.pixels[i].valid);
                require(count<=1,"Rotated diagonal double-covered");
                if(!diagonal) reference[i]=count; else require(reference[i]==count,"Changing diagonal changed quad coverage");
            }
        }
        for(bool diagonal:{false,true}) {
            const auto pair=quad({glm::vec2(-8,4),{40,4},{40,28},{-8,28}},diagonal);
            const auto a=rasterize(make_scene({pair[0]}),test_camera(),test_settings());
            const auto b=rasterize(make_scene({pair[1]}),test_camera(),test_settings());
            for(int y=0;y<32;++y) for(int x=0;x<32;++x)
                require(int(a.at(x,y).valid)+int(b.at(x,y).valid)==(y>=4 && y<28?1:0),"Clipped shared edge coverage failure");
        }
        return "Rotation, both diagonals, and shared-edge intersections across left/right clip planes";
    });
    run("D01-D05 camera-plane crossing matches analytic rays",[]() -> std::string {
        const auto t=triangle({-0.7f,-0.7f,-2},{0.7f,-0.7f,-2},{0,0.7f,0.2f});
        auto camera=test_camera(); camera.near_plane=0.25f;
        std::size_t compared=0,boundary_samples=0;
        for(bool reverse:{false,true}) {
            const auto g=rasterize(make_scene({t}),camera,test_settings(reverse));
            for(int y=0;y<32;++y) for(int x=0;x<32;++x) {
                const Ray ray{{0,0,0},{(x+0.5f)/16-1,1-(y+0.5f)/16,-1},camera.near_plane,camera.far_plane};
                const auto h=intersect_triangle(ray,t);
                if(h.triangle>=0 && h.barycentric.x>0.002f && h.barycentric.y>0.002f && h.barycentric.z>0.002f && h.t>camera.near_plane+0.002f) {
                    require(g.at(x,y).valid,"Interior analytic hit missing after near/eye clipping");
                    near_vector(g.at(x,y).position,ray.direction*h.t,8e-4f,"Clipped surface differs from analytic ray"); ++compared;
                }
                if(g.at(x,y).valid) {
                    const auto& sample=g.at(x,y);
                    const auto projected=camera.projection(1,reverse)*glm::vec4(sample.position,1);
                    // 定点顶点最多移动半个子像素；插值得到的世界点重投影也必须遵守此界。
                    near((projected.x/projected.w*0.5+0.5)*32,x+0.5,1.0/512+2e-5,"Subpixel reprojection X bound");
                    near((0.5-projected.y/projected.w*0.5)*32,y+0.5,1.0/512+2e-5,"Subpixel reprojection Y bound");
                    if(h.triangle<0) {
                        const glm::dvec3 e1=glm::dvec3(t.vertices[1].position)-glm::dvec3(t.vertices[0].position);
                        const glm::dvec3 e2=glm::dvec3(t.vertices[2].position)-glm::dvec3(t.vertices[0].position);
                        const glm::dvec3 q=glm::dvec3(sample.position)-glm::dvec3(t.vertices[0].position);
                        const double a=glm::dot(e1,e1),b=glm::dot(e1,e2),c=glm::dot(e2,e2),u=glm::dot(q,e1),v=glm::dot(q,e2);
                        const double beta=(c*u-b*v)/(a*c-b*b),gamma=(a*v-b*u)/(a*c-b*b);
                        const double smallest=std::min({beta,gamma,1-beta-gamma});
                        require(smallest>=-2e-6 && smallest<1e-4,"Analytic miss is not a quantized triangle edge");
                        ++boundary_samples;
                    }
                    require(g.at(x,y).depth>=0 && g.at(x,y).depth<=1,"Invalid clipped depth");
                }
            }
        }
        require(compared>100,"Too few analytic comparisons");
        return std::to_string(compared)+" interior comparisons; "+std::to_string(boundary_samples)+" edge samples bounded by 1/512 pixel";
    });
    run("D03 reciprocal-w attributes and affine projected depth",[]() -> std::string {
        auto t=triangle(screen_point({4,4},1),screen_point({28,4},2),screen_point({4,28},4));
        t.vertices[0].uv={0,0}; t.vertices[1].uv={1,0}; t.vertices[2].uv={0,1};
        t.vertices[0].normal=glm::normalize(glm::vec3(1,0,2));
        t.vertices[1].normal=glm::normalize(glm::vec3(0,1,2));
        t.vertices[2].normal={0,0,1};
        t.vertices[0].color={1,0,0,1}; t.vertices[1].color={0,1,0,1}; t.vertices[2].color={0,0,1,1};
        const auto scene=make_scene({t}); const auto transformed=flatten_scene(scene)[0];
        const glm::dvec3 lambda{11.0/24,6.5/24,6.5/24};
        glm::dvec3 weight=lambda/glm::dvec3(1,2,4); weight/=weight.x+weight.y+weight.z;
        glm::vec3 expected_position(0),expected_normal(0),expected_tangent(0); glm::vec4 expected_color(0);
        for(int k=0;k<3;++k) {
            expected_position+=float(weight[k])*t.vertices[k].position;
            expected_normal+=float(weight[k])*t.vertices[k].normal;
            expected_color+=float(weight[k])*t.vertices[k].color;
            expected_tangent+=float(weight[k])*glm::vec3(transformed.vertices[k].tangent);
        }
        expected_normal=glm::normalize(expected_normal);
        expected_tangent=glm::normalize(expected_tangent-expected_normal*glm::dot(expected_tangent,expected_normal));
        for(bool reverse:{false,true}) {
            const auto camera=test_camera(); const auto g=rasterize(scene,camera,test_settings(reverse)); const auto& s=g.at(10,10);
            require(s.valid,"Analytic perspective sample not covered");
            near_vector(s.uv,{float(weight.y),float(weight.z)},2e-6f,"UV is not reciprocal-w interpolated");
            require(std::abs(s.uv.x-float(lambda.y))>0.05f,"Test fails to distinguish affine UV");
            near_vector(s.position,expected_position,3e-6f,"Perspective world position");
            near_vector(s.normal,expected_normal,3e-6f,"Perspective normal");
            near_vector(glm::vec3(s.tangent),expected_tangent,3e-6f,"Perspective tangent");
            near_vector(s.vertex_color,expected_color,3e-6f,"Perspective vertex color");
            near(s.linear_depth,-expected_position.z,3e-6,"Linear view depth");
            const auto projection=camera.projection(1,reverse); double expected_depth=0;
            for(int k=0;k<3;++k) { const auto clip=projection*glm::vec4(t.vertices[k].position,1); expected_depth+=lambda[k]*clip.z/clip.w; }
            near(s.depth,expected_depth,2e-6,"Depth incorrectly perspective corrected twice");
            near_vector(s.motion,{0,0},0,"Static mesh motion");
        }
        return "Analytic barycentrics at (10.5,10.5), unequal w=(1,2,4), all attributes and both depth modes";
    });
    run("D04 projection endpoints, clear values, occlusion and far plane",[]() -> std::string {
        auto camera=test_camera();
        for(bool reverse:{false,true}) {
            const auto p=camera.projection(1,reverse);
            const auto n=p*glm::vec4(0,0,-camera.near_plane,1),f=p*glm::vec4(0,0,-camera.far_plane,1);
            near(n.z/n.w,reverse?1:0,1e-6,"Near endpoint"); near(f.z/f.w,reverse?0:1,1e-6,"Far endpoint");
            auto near_t=triangle(screen_point({-4,-4},1),screen_point({36,-4},1),screen_point({-4,36},1)); near_t.material=1;
            auto far_t=triangle(screen_point({-4,-4},5),screen_point({36,-4},5),screen_point({-4,36},5)); far_t.material=2;
            for(bool order:{false,true}) {
                const auto scene=order?make_scene({near_t,far_t}):make_scene({far_t,near_t});
                const auto g=rasterize(scene,camera,test_settings(reverse));
                require(g.at(8,8).valid && g.at(8,8).material==1,"Wrong depth comparison");
                require(!g.at(31,31).valid,"Clear pixel unexpectedly covered");
                near(g.at(31,31).depth,reverse?0:1,0,"Wrong clear depth");
            }
            auto exact_camera=camera; exact_camera.near_plane=1; exact_camera.far_plane=2;
            const auto far_plane=triangle(screen_point({4,4}),screen_point({28,4}),screen_point({4,28}));
            const auto g=rasterize(make_scene({far_plane}),exact_camera,test_settings(reverse));
            require(g.at(8,8).valid,"Far-plane geometry confused with clear depth");
            near(g.at(8,8).depth,reverse?0:1,0,"Exact far-plane depth");
        }
        return "Near/far projection, GREATER/LESS visibility, clear mask and inclusive far clip surface";
    });
    run("GBuffer material factors, preserved UV/color and pixel motion",[]() -> std::string {
        auto t=triangle(screen_point({4,4}),screen_point({28,4}),screen_point({4,28}));
        for(auto& v:t.vertices) { v.color={0.2f,0.4f,0.8f,0.6f}; v.uv={0.3f,0.7f}; }
        auto scene=make_scene({t}); scene.materials.resize(1); scene.materials[0].base_color={0.5f,0.25f,0.125f,0.2f};
        scene.materials[0].metallic=0.75f; scene.materials[0].roughness=0.2f;
        scene.nodes.resize(1); scene.nodes[0].mesh=0;
        scene.nodes[0].local=glm::translate(glm::mat4(1),{0.25f,0.125f,0});
        auto g=rasterize(scene,test_camera(),test_settings()); const auto& s=g.at(10,10);
        require(s.valid,"Moved test triangle missing");
        near_vector(s.motion,{2,-1},2e-6f,"Motion must be current minus previous in pixels");
        near_vector(s.vertex_color,t.vertices[0].color,1e-6f,"Material modified vertex color");
        near_vector(s.uv,t.vertices[0].uv,1e-6f,"Material modified UV");
        near_vector(s.albedo,{0.1f,0.1f,0.1f},1e-6f,"Base factor multiply");
        near(s.metallic,0.75,0,"Metallic factor"); near(s.roughness,0.2,1e-7,"Roughness factor");
        scene.nodes[0].previous_world=scene.nodes[0].local;
        g=rasterize(scene,test_camera(),test_settings()); near_vector(g.at(10,10).motion,{0,0},0,"Static transformed object motion");
        return "UV/color retained for sample_material; object motion=(2,-1) pixels; static history=0";
    });
    run("Raster jitter and previous camera/object motion share one projection",[]() -> std::string {
        const auto t=triangle(screen_point({4,4}),screen_point({28,4}),screen_point({4,28}));
        auto scene=make_scene({t}); const auto camera=test_camera();
        const auto plain=rasterize(scene,camera,test_settings());
        RasterOptions options; options.jitter_ndc={1.0f/16,-1.0f/16};
        auto jittered=rasterize(scene,camera,test_settings(),options);
        for(int y=1;y<32;++y) for(int x=1;x<32;++x) {
            require(jittered.at(x,y).valid==plain.at(x-1,y-1).valid,"Actual jitter did not shift coverage by one pixel");
            if(jittered.at(x,y).valid) {
                near_vector(jittered.at(x,y).position,plain.at(x-1,y-1).position,1e-6f,"Jitter surface correspondence");
                near_vector(jittered.at(x,y).motion,{0,0},0,"Projection jitter leaked into motion");
            }
        }
        auto old_camera=camera; old_camera.position.x=0.25f; old_camera.target.x=0.25f;
        options.previous_camera=&old_camera;
        Image<glm::vec3> previous; options.previous_world_positions=&previous;
        scene.nodes.resize(1); scene.nodes[0].mesh=0;
        scene.nodes[0].local=glm::translate(glm::mat4(1),{0.25f,0.125f,0});
        jittered=rasterize(scene,camera,test_settings(),options);
        const auto& sample=jittered.at(10,10); require(sample.valid,"Camera/object motion test not covered");
        near_vector(sample.motion,{4,-1},3e-6f,"Camera and object motion must combine in unjittered pixels");
        near_vector(previous.at(10,10),sample.position-glm::vec3(0.25f,0.125f,0),2e-6f,"Previous world sidecar");
        scene.nodes[0].previous_world=glm::translate(glm::mat4(1),{0,0,4});
        jittered=rasterize(scene,camera,test_settings(),options);
        require(jittered.at(10,10).motion.x>=64 && jittered.at(10,10).motion.y>=64,"Behind-camera history did not become out of bounds");
        return "Actual one-pixel jitter, jitter-free motion, total camera+object=(4,-1) pixels, previous-world sidecar and invalid-history sentinel";
    });
    run("D03 analytic perspective UV derivatives are exported",[]() -> std::string {
        auto t=triangle(screen_point({4,4},1),screen_point({28,4},2),screen_point({4,28},4));
        t.vertices[0].uv={0,0}; t.vertices[1].uv={1,0}; t.vertices[2].uv={0,1};
        Image<glm::vec2> dx,dy; RasterOptions options; options.uv_dx=&dx; options.uv_dy=&dy;
        const auto g=rasterize(make_scene({t}),test_camera(),test_settings(),options); require(g.at(10,10).valid,"Derivative sample missing");
        auto analytic_uv=[](double x,double y) {
            const double b=(x-4)/24,c=(y-4)/24,a=1-b-c,d=a+b/2+c/4;
            return glm::dvec2(b/2,c/4)/d;
        };
        const double step=0.001;
        const auto expected_dx=(analytic_uv(10.5+step,10.5)-analytic_uv(10.5-step,10.5))/(2*step);
        const auto expected_dy=(analytic_uv(10.5,10.5+step)-analytic_uv(10.5,10.5-step))/(2*step);
        near_vector(dx.at(10,10),glm::vec2(expected_dx),2e-7f,"Perspective UV d/dx");
        near_vector(dy.at(10,10),glm::vec2(expected_dy),2e-7f,"Perspective UV d/dy");
        near_vector(dx.at(31,31),{0,0},0,"Background dx not cleared"); near_vector(dy.at(31,31),{0,0},0,"Background dy not cleared");
        options.uv_dy=&dx; rejects([&]{ (void)rasterize(make_scene({t}),test_camera(),test_settings(),options); },"Aliased derivative outputs accepted");
        return "Quotient-rule derivatives agree with independent analytic finite differences; background sidecars zero";
    });
    run("D03 raster texture footprints drive real material Mip and mask",[]() -> std::string {
        auto t=triangle(screen_point({4,4}),screen_point({28,4}),screen_point({4,28}));
        t.vertices[0].uv={0,0}; t.vertices[1].uv={24,0}; t.vertices[2].uv={0,24};
        auto scene=make_scene({t}); scene.materials.resize(1); auto& material=scene.materials[0];
        material.base_color={1,1,1,1}; material.base_texture=0; material.alpha_mode=1; material.alpha_cutoff=0.5f;
        Texture texture; texture.name="Analytic Mip levels";
        for(int size=64;size>=1;size/=2) texture.levels.emplace_back(size,size,size==1?glm::vec4(0,0,1,1):glm::vec4(1,0,0,0));
        scene.textures.push_back(texture);
        Image<glm::vec2> dx,dy; RasterOptions options; options.uv_dx=&dx; options.uv_dy=&dy;
        auto settings=test_settings(); settings.filter=FilterMode::trilinear;
        const auto g=rasterize(scene,test_camera(),settings,options); const auto& s=g.at(10,10);
        require(s.valid,"Alpha mask sampled level 0 instead of the minified footprint");
        near_vector(s.albedo,{0,0,1},2e-6f,"Raster material did not sample Mip 6");
        near_vector(dx.at(10,10),{1,0},2e-6f,"Unit UV-per-pixel footprint x");
        near_vector(dy.at(10,10),{0,1},2e-6f,"Unit UV-per-pixel footprint y");
        const auto isolated=sample_texture(texture,s.uv,settings.filter);
        near_vector(isolated,{1,0,0,0},0,"Test cannot distinguish missing derivatives");
        near_vector(s.vertex_color,{1,1,1,1},0,"Texture sample overwrote vertex color");
        return "64 texels/pixel chooses opaque blue Mip 6; zero-derivative lookup is transparent red Mip 0";
    });
    run("Alpha mask discards before depth and preserves behind surfaces",[]() -> std::string {
        auto front=triangle(screen_point({4,4},1),screen_point({28,4},1),screen_point({4,28},1));
        front.vertices[0].uv={0,0}; front.vertices[1].uv={1,0}; front.vertices[2].uv={0,1};
        auto back=triangle(screen_point({4,4},2),screen_point({28,4},2),screen_point({4,28},2)); back.material=1;
        for(bool reverse:{false,true}) for(bool order:{false,true}) {
            auto scene=order?make_scene({front,back}):make_scene({back,front}); scene.materials.resize(2);
            scene.materials[0].base_color={1,1,1,1}; scene.materials[0].alpha_mode=1; scene.materials[0].alpha_cutoff=0.5f; scene.materials[0].base_texture=0;
            Texture mask; mask.levels.emplace_back(2,1,glm::vec4(1)); mask.levels[0].at(0,0).a=0; scene.textures.push_back(mask);
            auto settings=test_settings(reverse); settings.filter=FilterMode::nearest;
            Image<glm::vec3> previous; RasterOptions options; options.previous_world_positions=&previous;
            auto g=rasterize(scene,test_camera(),settings,options);
            require(g.at(8,8).valid && g.at(8,8).material==1,"Cutout texel hid the background");
            require(g.at(18,8).valid && g.at(18,8).material==0,"Opaque texel failed to write foreground depth");
            near(previous.at(8,8).z,-2,1e-6,"Discarded sample wrote history sidecar"); near(previous.at(18,8).z,-1,1e-6,"Foreground history sidecar");
            scene.materials[0].base_color.a=0.4f;
            g=rasterize(scene,test_camera(),settings,options); require(g.at(18,8).material==1,"Material alpha factor ignored");
            scene.materials[0].alpha_cutoff=0.4f;
            g=rasterize(scene,test_camera(),settings,options); require(g.at(18,8).material==0,"Alpha equality should survive cutoff");
            scene.materials[0].alpha_mode=0;
            g=rasterize(scene,test_camera(),settings,options); require(g.at(8,8).material==0,"Opaque material unexpectedly discarded alpha");
        }
        return "Both draw orders and depth conventions; cutout, opaque alpha, cutoff equality, base factor and surviving history data";
    });
    run("D06 AABB slabs, parallel rays, grazing and degenerate bounds",[]() -> std::string {
        const Aabb box{{-1,-1,-1},{1,1,1}}; double enter=0,exit=0;
        require(box.intersect({{0,0,3},{0,0,-2},0,10},enter,exit),"Analytic slab miss");
        near(enter,1,1e-14,"Slab enter"); near(exit,2,1e-14,"Slab exit");
        require(box.intersect({{0,0,0},{-2,0,0},0,10},enter,exit),"Inside ray miss");
        near(enter,0,0,"Inside enter"); near(exit,0.5,1e-14,"Inside exit");
        require(box.intersect({{1,1,3},{-0.0f,0,-1},0,10}),"Grazing corner ray miss");
        require(!box.intersect({{2,0,3},{0,0,-1},0,10}),"Parallel outside ray hit");
        require(!box.intersect({{0,0,3},{0,0,0},0,10}),"Zero direction accepted");
        require(box.intersect({{0,0,3},{0,0,-1},2,2}),"Closed interval endpoint miss");
        require(!box.intersect({{0,0,3},{0,0,-1},0,1.9f}),"Finite ray extent ignored");
        const Aabb flat{{-1,-1,0},{1,1,0}};
        require(flat.intersect({{0,0,1},{0,0,-1},0,2},enter,exit),"Flat box miss"); near(enter,1,1e-14,"Flat enter");
        require(!Aabb{}.intersect({{0,0,1},{0,0,-1},0,2}),"Empty box hit");
        require(box.intersect({{-2,0,0},{1e-30f,0,0},0,2e30f}),"Tiny nonzero direction treated as parallel");
        return "Closed slabs, inside start, signed zero, corner grazing, finite interval and tiny direction";
    });
    run("D06 sphere, OBB and conservative transformed bounds",[]() -> std::string {
        double enter=0,exit=0;
        require(intersect_sphere({{0,0,3},{0,0,-2},0,10},{{0,0,0},1},enter,exit),"Sphere miss");
        near(enter,1,1e-12,"Sphere enter"); near(exit,2,1e-12,"Sphere exit");
        require(intersect_sphere({{1,0,3},{0,0,-2},0,10},{{0,0,0},1},enter,exit),"Tangent sphere miss");
        near(enter,1.5,1e-12,"Sphere tangent");
        require(intersect_sphere({{0,0,0},{0,0,2},0,10},{{0,0,0},1},enter,exit),"Sphere inside miss");
        near(enter,0,0,"Sphere inside enter"); near(exit,0.5,1e-12,"Sphere inside exit");
        Obb obb; obb.axes=glm::mat3(glm::rotate(glm::mat4(1),0.4f,glm::vec3(0,0,1))); obb.half_extent={1,2,1};
        require(intersect_obb({obb.axes*glm::vec3(3,0,0),obb.axes*glm::vec3(-1,0,0),0,10},obb,enter,exit),"Rotated OBB miss");
        near(enter,2,3e-7,"OBB enter"); near(exit,4,3e-7,"OBB exit");
        obb.axes[0]*=2; require(!intersect_obb({{3,0,0},{-1,0,0},0,10},obb,enter,exit),"Nonorthonormal OBB accepted");
        const Aabb local{{-1,-2,-3},{1,2,3}};
        const auto transform=glm::translate(glm::mat4(1),{2,-3,1})*glm::rotate(glm::mat4(1),0.785f,glm::vec3(0,0,1))*glm::scale(glm::mat4(1),{-2,0.5f,3});
        const auto world=transform_aabb(local,transform);
        for(int i=0;i<8;++i) {
            const glm::dvec4 p((i&1)?local.max.x:local.min.x,(i&2)?local.max.y:local.min.y,(i&4)?local.max.z:local.min.z,1);
            const auto q=glm::dmat4(transform)*p;
            for(int a=0;a<3;++a) require(q[a]>=world.min[a] && q[a]<=world.max[a],"Transformed AABB lost corner");
        }
        const auto triangles=scattered_triangles(); const auto sphere=bounding_sphere(triangles);
        for(const auto& t:triangles) for(const auto& v:t.vertices)
            require(glm::length(glm::dvec3(v.position)-glm::dvec3(sphere.center))<=sphere.radius,"Sphere lost vertex");
        return "Analytic sphere roots, rotated orthonormal box, all eight transformed corners and sphere containment";
    });
    run("D06 analytic triangle hits, boundaries and scale robustness",[]() -> std::string {
        auto t=triangle({0,0,0},{1,0,0},{0,1,0});
        auto h=intersect_triangle({{0.25f,0.25f,1},{0,0,-2},0,1},t,7);
        require(h.triangle==7,"Triangle index mapping"); near(h.t,0.5,0,"Analytic t");
        near_vector(h.barycentric,{0.5f,0.25f,0.25f},0,"Analytic barycentrics");
        require(intersect_triangle({{0.25f,0.25f,1},{0,0,-2},0.5f,0.5f},t).triangle==0,"Triangle closed t endpoints");
        require(intersect_triangle({{0.25f,0.25f,1},{0,0,-2},0.5001f,2},t).triangle<0,"Triangle t_min ignored");
        require(intersect_triangle({{0.25f,0.25f,0},{0,0,-1},0,1},t).triangle==0,"Surface origin t=0 rejected");
        require(intersect_triangle({{0.25f,0.25f,1},{1,0,0},0,10},t).triangle<0,"Parallel triangle hit");
        std::swap(t.vertices[1],t.vertices[2]);
        h=intersect_triangle({{0.25f,0.25f,-1},{0,0,2},0,1},t);
        require(h.triangle==0,"Backface hit rejected"); near(h.t,0.5,0,"Backface t");
        for(float scale:{1e-18f,1e-8f,1.0f,1e18f}) {
            const auto scaled=triangle({0,0,0},{scale,0,0},{0,scale,0});
            h=intersect_triangle({{0.25f*scale,0.25f*scale,scale},{0,0,-scale},0,2},scaled);
            require(h.triangle==0,"Triangle scale rejected by epsilon"); near(h.t,1,1e-6,"Scaled t");
            near_vector(h.barycentric,{0.5f,0.25f,0.25f},2e-6f,"Scaled barycentrics");
        }
        const auto degenerate=triangle({0,0,0},{1,0,0},{2,0,0});
        require(intersect_triangle({{0.5f,0,1},{0,0,-1},0,2},degenerate).triangle<0,"Degenerate triangle hit");
        return "Analytic barycentrics/t, two sided, exact endpoints, zero/parallel/degenerate and scales 1e-18..1e18";
    });
    run("D06 shared ray edges and stable original-index ties",[]() -> std::string {
        const std::vector<Triangle> triangles{triangle({0,0,0},{1,0,0},{1,1,0}),triangle({0,0,0},{1,1,0},{0,1,0})};
        const Bvh bvh(triangles,{BvhSplit::binned_sah,1,8,32}); const Octree octree(triangles,{1,10});
        for(int i=0;i<=64;++i) {
            const float x=float(i)/64;
            const Ray ray{{x,x,1},{0,0,-1},0,2};
            const auto expected=brute_force_intersect(triangles,ray);
            require(expected.triangle==0,"Shared edge missing or tie not lowest original index");
            same_hit(bvh.intersect(ray),expected,"BVH shared edge"); same_hit(octree.intersect(ray),expected,"Octree shared edge");
        }
        return "65 shared-edge/vertex rays, inclusive geometry intersections, lowest original index tie";
    });
    run("D07-D08 median/SAH/octree agree with brute force",[]() -> std::string {
        auto input=scattered_triangles(); const auto reference=input;
        const Bvh median(input,{BvhSplit::median,3,16,64}),sah(input,{BvhSplit::binned_sah,3,16,64});
        const Octree octree(input,{3,12});
        input.clear(); // 查询必须依赖层次结构自己的副本，而不是调用方数组的生存期。
        Random random; std::uint64_t brute_tests=0,median_tests=0,sah_tests=0,octree_tests=0; std::size_t hits=0;
        for(int i=0;i<3000;++i) {
            Ray ray; ray.origin=random.vector()*15.0f; ray.direction=random.vector()*2.0f; ray.t_min=0; ray.t_max=i%11?100.0f:0.4f;
            if(i%3==0) {
                const auto& t=reference[std::size_t(i)%reference.size()];
                ray.direction=(t.vertices[0].position+t.vertices[1].position+t.vertices[2].position)/3.0f-ray.origin;
            }
            if(i%7==0) { ray.direction={0,0,-1}; ray.direction[i%3]=i%2?1.0f:-1.0f; }
            TraversalStatistics a,b,c,d;
            const auto expected=brute_force_intersect(reference,ray,&a);
            same_hit(median.intersect(ray,&b),expected,"Median versus brute force");
            same_hit(sah.intersect(ray,&c),expected,"SAH versus brute force");
            same_hit(octree.intersect(ray,&d),expected,"Octree versus brute force");
            require(median.occluded(ray)==(expected.triangle>=0) && sah.occluded(ray)==(expected.triangle>=0) && octree.occluded(ray)==(expected.triangle>=0),"Occlusion differs from nearest query");
            require(b.triangle_tests<=reference.size() && c.triangle_tests<=reference.size() && d.triangle_tests<=reference.size(),"Duplicate triangle tests in one query");
            brute_tests+=a.triangle_tests; median_tests+=b.triangle_tests; sah_tests+=c.triangle_tests; octree_tests+=d.triangle_tests;
            if(expected.triangle>=0) ++hits;
        }
        require(hits>500,"Insufficient hit coverage");
        require(median_tests<brute_tests && sah_tests<brute_tests,"BVH did not prune any work");
        require(octree.statistics().primitive_references==reference.size(),"Octree duplicated primitive storage");
        require(median.statistics().nodes>1 && sah.statistics().nodes>1 && octree.statistics().nodes>1,"Hierarchy never split");
        std::ostringstream out; out<<"3000 deterministic rays, hits="<<hits<<"; triangle tests brute/median/SAH/octree="<<brute_tests<<'/'<<median_tests<<'/'<<sah_tests<<'/'<<octree_tests;
        return out.str();
    });
    run("D07 refit correctness and measurable quality degradation",[]() -> std::string {
        std::vector<Triangle> triangles;
        for(int i=0;i<64;++i) { const float x=float(i)*2; triangles.push_back(triangle({x,0,0},{x+1,0,0},{x,1,0})); }
        Bvh bvh(triangles,{BvhSplit::median,2,16,64}); const auto old_nodes=bvh.statistics().nodes; const double old_cost=bvh.statistics().sah_cost;
        for(std::size_t i=0;i<triangles.size();++i) for(auto& v:triangles[i].vertices) v.position.x+=i%2?1000.0f:-1000.0f;
        bvh.refit(triangles); const Bvh rebuilt(triangles,{BvhSplit::median,2,16,64});
        require(bvh.statistics().nodes==old_nodes && bvh.statistics().refit_nodes==old_nodes,"Refit changed topology or missing stats");
        require(bvh.statistics().sah_cost>old_cost*2 && bvh.statistics().sah_cost>rebuilt.statistics().sah_cost*2,"Refit degradation test failed to stress topology");
        for(const auto& t:triangles) {
            const Ray ray{t.vertices[0].position+glm::vec3(0.25f,0.25f,2),{0,0,-1},0,3};
            const auto expected=brute_force_intersect(triangles,ray);
            same_hit(bvh.intersect(ray),expected,"Refit versus brute"); same_hit(rebuilt.intersect(ray),expected,"Rebuild versus brute");
        }
        auto wrong=triangles; wrong.pop_back(); rejects([&]{ bvh.refit(wrong); },"Refit accepted changed count");
        require(bvh.triangles().size()==triangles.size(),"Failed refit changed data");
        std::ostringstream out; out<<"64 moved primitives; SAH cost initial/refit/rebuild="<<old_cost<<'/'<<bvh.statistics().sah_cost<<'/'<<rebuilt.statistics().sah_cost;
        return out.str();
    });
    run("D08 octree region queries, no duplicates and boundary rebuild",[]() -> std::string {
        auto triangles=scattered_triangles(); Octree octree(triangles,{2,12}); Random random;
        for(int i=0;i<300;++i) {
            const auto center=random.vector()*10.0f; const glm::vec3 half(i%4?1.5f:0.0f);
            const Aabb region{center-half,center+half}; std::vector<std::uint32_t> expected;
            for(std::size_t j=0;j<triangles.size();++j) if(triangle_bounds(triangles[j]).overlaps(region)) expected.push_back(std::uint32_t(j));
            const auto actual=octree.query(region);
            require(actual==expected,"Octree region query missed, duplicated or invented a candidate");
        }
        const auto all=octree.query(octree.bounds()); require(all.size()==triangles.size(),"Root query lost references");
        for(auto& t:triangles) for(auto& v:t.vertices) v.position+=glm::vec3(20,-15,30);
        octree.rebuild(triangles);
        for(std::size_t i=0;i<triangles.size();i+=3) {
            const auto& t=triangles[i]; const auto center=(t.vertices[0].position+t.vertices[1].position+t.vertices[2].position)/3.0f;
            const Ray ray{center+glm::vec3(0,0,3),{0,0,-1},0,6};
            same_hit(octree.intersect(ray),brute_force_intersect(triangles,ray),"Octree boundary rebuild");
        }
        return "300 conservative AABB candidate queries, root enumeration, translated rebuild and ray agreement";
    });
    run("Empty and coincident hierarchies, finite ray intervals and invalid input",[]() -> std::string {
        const Bvh empty({}); const Octree empty_octree({}); const Ray ray{{0,0,1},{0,0,-1},0,2};
        require(empty.intersect(ray).triangle<0 && !empty.occluded(ray) && empty.bounds().empty(),"Empty BVH query");
        require(empty_octree.intersect(ray).triangle<0 && empty_octree.query({{-1,-1,-1},{1,1,1}}).empty(),"Empty octree query");
        std::vector<Triangle> repeated(300,triangle({0,0,0},{0,0,0},{0,0,0}));
        const Bvh degenerate(repeated,{BvhSplit::binned_sah,1,16,64}); const Octree flat(repeated,{1,32});
        require(degenerate.intersect(ray).triangle<0 && flat.intersect(ray).triangle<0,"Degenerate hierarchy hit");
        require(flat.statistics().nodes==1 && flat.query({{0,0,0},{0,0,0}}).size()==300,"Zero-size octree did not terminate/retain primitives");
        const std::vector<Triangle> one{triangle({-1,-1,0},{1,-1,0},{0,1,0})}; const Bvh bvh(one); const Octree octree(one);
        const Ray short_ray{{0,0,1},{0,0,-1},0,0.9f}; require(!bvh.occluded(short_ray) && !octree.occluded(short_ray),"Occlusion ignored light distance");
        const Ray nan_ray{{0,0,1},{std::numeric_limits<float>::quiet_NaN(),0,-1},0,2};
        require(bvh.intersect(nan_ray).triangle<0 && octree.intersect(nan_ray).triangle<0,"Invalid ray accepted");
        repeated[0].vertices[0].position.x=std::numeric_limits<float>::infinity();
        rejects([&]{ Bvh bad(repeated); },"BVH accepted nonfinite geometry"); rejects([&]{ Octree bad(repeated); },"Octree accepted nonfinite geometry");
        rejects([&]{ Bvh bad(one,{BvhSplit::median,0,16,64}); },"Invalid BVH leaf size accepted");
        return "Empty/coincident geometry, bounded subdivision, finite shadow extent, NaN and configuration rejection";
    });
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_GEOMETRY_TEST_MAIN
#include <iostream>
int main() {
    const auto results=emberframe::lab::test_geometry(); std::size_t failed=0;
    for(const auto& result:results) {
        std::cout<<(result.passed?"PASS ":"FAIL ")<<result.name<<": "<<result.detail<<'\n';
        if(!result.passed) ++failed;
    }
    std::cout<<results.size()-failed<<'/'<<results.size()<<" geometry tests passed\n";
    return failed?1:0;
}
#endif
