#include "shadow_gi.h"
#include "geometry.h"
#include <cmath>
#include <sstream>
#ifdef EMBERFRAME_SHADOW_GI_TEST_MAIN
#include <iostream>
#endif

namespace emberframe::lab {
namespace {
void require(bool condition,const std::string& reason) { if(!condition)throw std::runtime_error(reason); }
void near(double value,double expected,double tolerance,const std::string& reason) {
    if(!std::isfinite(value)||std::abs(value-expected)>tolerance) {
        std::ostringstream message;message<<reason<<": expected "<<expected<<" +/- "<<tolerance<<", got "<<value;
        throw std::runtime_error(message.str());
    }
}
void near_vec(glm::vec3 value,glm::vec3 expected,float tolerance,const std::string& reason) {
    for(int k=0;k<3;++k)near(value[k],expected[k],tolerance,reason);
}
Triangle triangle(glm::vec3 a,glm::vec3 b,glm::vec3 c,int material=0) {
    Triangle t;t.material=material;const auto n=safe_normalize(glm::cross(b-a,c-a));
    t.vertices[0].position=a;t.vertices[1].position=b;t.vertices[2].position=c;
    for(auto& v:t.vertices)v.normal=n;
    return t;
}
void quad(std::vector<Triangle>& ts,glm::vec3 a,glm::vec3 b,glm::vec3 c,glm::vec3 d,int material=0) {
    ts.push_back(triangle(a,b,c,material));ts.push_back(triangle(a,c,d,material));
}
std::vector<Triangle> cube() {
    std::vector<Triangle> ts;
    quad(ts,{-1,-1,-1},{-1,1,-1},{1,1,-1},{1,-1,-1});
    quad(ts,{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1});
    quad(ts,{-1,-1,-1},{-1,-1,1},{-1,1,1},{-1,1,-1});
    quad(ts,{1,-1,-1},{1,1,-1},{1,1,1},{1,-1,1});
    quad(ts,{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1});
    quad(ts,{-1,1,-1},{-1,1,1},{1,1,1},{1,1,-1});return ts;
}
glm::dvec4 raw_moments(const std::vector<double>& values) {
    glm::dvec4 m(0);for(double z:values)m+=glm::dvec4(z,z*z,z*z*z,z*z*z*z);return m/double(values.size());
}
GBuffer plane_gbuffer(const Camera& camera,int size=48) {
    GBuffer gbuffer(size,size);const float tangent=std::tan(glm::radians(camera.fov)*0.5f);
    for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
        auto& s=gbuffer.at(x,y);s.valid=true;s.normal={0,0,1};s.albedo={0.5f,0.25f,0.125f};
        s.position={(2*(x+0.5f)/size-1)*3*tangent,(1-2*(y+0.5f)/size)*3*tangent,-3};s.linear_depth=3;
    }
    return gbuffer;
}
}
TestResults test_shadow_gi() {
    TestResults results;
    const auto run=[&](const char* name,const std::function<void()>& test) {
        try { test();results.push_back({name,true,"All numerical invariants passed"}); }
        catch(const std::exception& e) { results.push_back({name,false,e.what()}); }
    };
    run("D19 SAT agrees with brute force for clipped/empty rectangles",[] {
        Image<float> depth(7,5);for(int y=0;y<5;++y)for(int x=0;x<7;++x)depth.at(x,y)=float((x*17+y*13)%29)/29;
        MomentSummedArea sat(depth);
        for(int x0=-1;x0<8;++x0)for(int y0=-1;y0<6;++y0)for(int x1=x0;x1<9;++x1)for(int y1=y0;y1<7;++y1) {
            glm::dvec4 expected(0);int count=0;
            for(int y=std::max(0,y0);y<std::min(5,y1);++y)for(int x=std::max(0,x0);x<std::min(7,x1);++x) {
                const double z=depth.at(x,y);expected+=glm::dvec4(z,z*z,z*z*z,z*z*z*z);++count;
            }
            const auto sum=sat.sum(x0,y0,x1,y1),mean=sat.mean(x0,y0,x1,y1);
            for(int k=0;k<4;++k){near(sum[k],expected[k],1e-10,"rectangle sum");near(mean[k],count?expected[k]/count:0,1e-10,"rectangle mean");}
        }
    });
    run("D19 Cantelli is an upper bound, not exact PCF",[] {
        const auto m=raw_moments({0.2,0.2,0.4,0.8});
        near(cantelli_visibility({m.x,m.y},0.7,0),0.4,1e-6,"Cantelli documented example");
        require(cantelli_visibility({m.x,m.y},0.7,0)>0.25f,"VSM should retain its known leakage");
        near(cantelli_visibility({0.4,0.16},0.8,0),0,1e-6,"zero variance shadow");
        near(cantelli_visibility({0.4,0.16},0.2,0),1,1e-6,"front receiver");
    });
    run("D21 MSM separates two depth modes that VSM cannot",[] {
        const auto m=raw_moments({0.2,0.8});
        near(msm_visibility(m,0.5,1e-7),0.5,0.002,"four-moment two-point distribution");
        near(cantelli_visibility({m.x,m.y},0.5),1,1e-6,"same first-two moments");
        near(msm_visibility(raw_moments({0.4,0.4}),0.41),0,1e-6,"degenerate shadow");
        near(msm_visibility(raw_moments({0.4,0.4}),0.41,1e-5,0.02),1,1e-6,"depth bias");
    });
    run("D21 MSM bounded and conservative on deterministic multi-modal distributions",[] {
        for(int seed=0;seed<16;++seed) {
            std::vector<double> depths;for(int i=0;i<11;++i)depths.push_back(0.02+0.96*((seed*19+i*37)%101)/101.0);
            const auto m=raw_moments(depths);
            for(int i=0;i<=40;++i) {
                const double z=i/40.0;int lit=0;for(double d:depths)lit+=d>=z;
                const float p=msm_visibility(m,z,1e-7);
                require(std::isfinite(p)&&p>=0&&p<=1,"invalid moment visibility");
                require(p+0.001>=double(lit)/depths.size(),"MSM violated four-moment visibility upper bound");
            }
        }
    });
    run("D20 VSSM conditional blocker mean and exact search",[] {
        auto b=conditional_blocker_mean({0.25,0.0625},0.7f);require(b.valid,"all blockers must be found");
        near(b.depth,0.25,1e-6,"all-blocker mean");near(b.fraction,1,1e-6,"all-blocker fraction");
        b=conditional_blocker_mean({0.4,0.22},0.7f);near(b.depth,0.2,1e-5,"VSSM conditional closure");
        require(!conditional_blocker_mean({1,1},0.7f).valid,"clear search must stay lit");
        Image<float> depth(3,3,1);depth.at(0,0)=0.2f;depth.at(1,1)=0.4f;MomentShadowMap map(depth);
        b=map.blockers({0.5f,0.5f},0.7f,1);near(b.depth,0.3,1e-6,"exact blocker mean");near(b.fraction,2.0/9,1e-6,"exact fraction");
        near(map.pcf({0.5f,0.5f},0.7f,1),7.0/9,1e-6,"PCF comparisons");
    });
    run("D20 bounded VSSM never samples outside blocker estimate support",[] {
        Image<float> a(32,32,1),b(32,32,.01f);
        for(int y=13;y<=19;++y)for(int x=13;x<=19;++x)a.at(x,y)=b.at(x,y)=(x+y)%2?.9f:.3f;
        const glm::vec2 uv(16.5f/32);
        const float first=MomentShadowMap(a).vssm(uv,.7f,3,100),second=MomentShadowMap(b).vssm(uv,.7f,3,100);
        near(first,second,1e-6,"Unrelated remote depth must not change a local unstable estimate");
        require(first>.1f&&first<.99f,"Bounded VSSM retains actual partial visibility, not forced lit/dark");
    });
    run("D20 PCSS/VSSM lit, shadowed and variable-penumbra cases",[] {
        MomentShadowMap clear(Image<float>(16,16,1)),blocked(Image<float>(16,16,0.2f));
        near(clear.pcss({0.5f,0.5f},0.7f,6,12),1,1e-6,"PCSS clear");
        near(blocked.pcss({0.5f,0.5f},0.7f,6,12),0,1e-6,"PCSS shadow");
        near(clear.vssm({0.5f,0.5f},0.7f,6,12),1,1e-6,"VSSM clear");
        require(blocked.vssm({0.5f,0.5f},0.7f,6,12)<1e-4,"VSSM shadow");
        Image<float> edge(32,32,1);for(int y=0;y<32;++y)for(int x=0;x<16;++x)edge.at(x,y)=0.2f;
        MomentShadowMap map(edge);const float narrow=map.pcss({0.57f,0.5f},0.8f,10,1),wide=map.pcss({0.57f,0.5f},0.8f,10,16);
        require(narrow>wide&&wide>0,"light radius must change penumbra");
    });
    run("PCSS fractional radius and subtexel filtering remain continuous",[] {
        Image<float> edge(64,64,1);for(int y=0;y<64;++y)for(int x=0;x<32;++x)edge.at(x,y)=.2f;
        MomentShadowMap map(edge);
        // 同一个半影点只微调半径，不应像原 ceil(radius) 那样突然换一整圈 tap。
        const auto uv=glm::vec2(.53f,.5f);
        const float a=map.soft_pcf(uv,.7f,3.999f),b=map.soft_pcf(uv,.7f,4.001f);
        require(a>0&&a<1&&std::abs(a-b)<.002f,"integer-radius boundary produced a visible jump");
        const float left=map.pcf({.5f-1e-5f,.5f},.7f,1),right=map.pcf({.5f+1e-5f,.5f},.7f,1);
        require(std::abs(left-right)<.002f,"PCF texel boundary should interpolate comparison results");
        // 不能以抬高 Bias 或全白阴影来通过平滑检查。
        near(map.soft_pcf({.2f,.5f},.7f,4),0,1e-6,"opaque shadow retained");
        near(map.soft_pcf({.8f,.5f},.7f,4),1,1e-6,"lit exterior retained");
        const auto ma=map.filtered_moments(uv,3.999f),mb=map.filtered_moments(uv,4.001f);
        require(glm::length(ma-mb)<.002,"moment filter radius should not jump at integers");
        for(auto corner:{glm::vec2(0),glm::vec2(.99999f)}) {
            const auto m=map.filtered_moments(corner,0);
            for(int i=0;i<4;++i)require(std::isfinite(m[i]),"subtexel map border produced a non-finite moment");
        }
    });
    run("D26 RSM flux, receiver material, inverse-square and secondary visibility",[] {
        ReflectiveShadowMap rsm;rsm.set_samples({{{0,0,0},{0,0,1},glm::vec3(4*pi*pi),1}});
        const glm::vec3 albedo(0.5f,0.25f,0.125f);
        near_vec(rsm.gather({0,0,2},{0,0,-1},albedo),albedo,1e-6f,"two Lambert factors");
        near_vec(rsm.gather({0,0,4},{0,0,-1},albedo),albedo*0.25f,1e-6f,"inverse square");
        near_vec(rsm.gather({0,0,2},{0,0,1},albedo),glm::vec3(0),1e-6f,"receiver backface");
        near_vec(rsm.gather({0,0,2},{0,0,-1},albedo,64,0.02f,[](const Ray&){return true;}),glm::vec3(0),1e-6f,"secondary occlusion");
        rsm.set_samples(std::vector<RsmSample>(4,{{0,0,0},{0,0,1},glm::vec3(pi*pi),0.25f}));
        near_vec(rsm.gather({0,0,2},{0,0,-1},albedo),albedo,1e-6f,"subdivided samples conserve flux");
    });
    run("D26 directional RSM light direction and resolution-independent power",[] {
        Scene scene;scene.materials.push_back(Material{});scene.materials[0].base_color={0.5f,0.25f,0.125f,1};
        std::vector<Triangle> ts;quad(ts,{-1,0,-1},{-1,0,1},{1,0,1},{1,0,-1});Bvh bvh(ts);
        Light light;light.direction={0,-1,0};light.intensity=2;
        ReflectiveShadowMap a,b;a.build(scene,ts,bvh,light,32);b.build(scene,ts,bvh,light,64);
        const auto total=[](const ReflectiveShadowMap& r){glm::vec3 total(0);for(auto& s:r.samples())total+=s.flux;return total;};
        const auto pa=total(a),pb=total(b);
        // Point samples have boundary quadrature error; both 32 and 64 should
        // approximate the same physical power, not scale with their texel count.
        near_vec(pa,glm::vec3(4,2,1),0.25f,"RSM integrated power");near_vec(pa,pb,0.25f,"resolution normalization");
        light.direction={0,1,0};a.build(scene,ts,bvh,light,16);require(a.samples().empty(),"backfaces may not reflect a one-sided light");
    });
    run("D26 rectangle RSM uses emitted radiance times area and pi",[] {
        Scene scene;scene.materials.push_back(Material{});scene.materials[0].base_color=glm::vec4(1);
        std::vector<Triangle> ts;quad(ts,{-50,0,-50},{-50,0,50},{50,0,50},{50,0,-50});Bvh bvh(ts);
        Light light;light.kind=LightKind::rectangle;light.position={0,1,0};light.direction={0,-1,0};light.size={2,3};light.intensity=2;light.range=200;
        ReflectiveShadowMap rsm;rsm.build(scene,ts,bvh,light,32);glm::vec3 sum(0);for(const auto& s:rsm.samples())sum+=s.flux;
        near(sum.x,12*pi,12*pi*0.04,"cosine emitter packets integrate Le*A*pi");
    });
    run("D27 LPV SH injection and dissipative ping-pong transport",[] {
        LightPropagationVolume lpv(glm::vec3(-4),glm::vec3(4),8);
        const std::vector<RsmSample> samples={{{0,0,0},{1,0,0},{4,2,1},1}};lpv.inject(samples);
        near_vec(lpv.frontier_energy(),{4,2,1},1e-5f,"injected SH integral");
        lpv.propagate(1,0.8f);near_vec(lpv.frontier_energy(),{3.2f,1.6f,0.8f},1e-5f,"one transport step");
        const auto toward=lpv.irradiance({1.5f,0.5f,0.5f},{-1,0,0}),away=lpv.irradiance({1.5f,0.5f,0.5f},{1,0,0});
        require(toward.x>away.x&&toward.x>0,"SH must retain travel direction");
        lpv.propagate(2,0.8f);require(lpv.frontier_energy().x<3.2f,"transport cannot manufacture power");
        lpv.clear();near_vec(lpv.frontier_energy(),glm::vec3(0),1e-6f,"clear removes history");
    });
    run("D27 LPV geometry blocks propagation",[] {
        LightPropagationVolume lpv(glm::vec3(-4),glm::vec3(4),8);
        lpv.inject(std::vector<RsmSample>{{{0,0,0},{1,0,0},{1,1,1},1}});
        lpv.set_occupancy(std::vector<float>(8*8*8,1));lpv.propagate(1);
        near_vec(lpv.frontier_energy(),glm::vec3(0),1e-6f,"fully blocked neighbors");
    });
    run("D28 radiance mip preserves color and rescales optical depth",[] {
        VoxelRadianceVolume volume(glm::vec3(-1),glm::vec3(1),4);const glm::vec3 color(2,0.4f,0.1f);
        for(int z=0;z<4;++z)for(int y=0;y<4;++y)for(int x=0;x<4;++x)volume.set_voxel(x,y,z,color,0.2f);
        volume.build_mips();require(volume.mip_count()==3,"4^3 volume needs 3 levels");
        const auto value=volume.sample({0,0,0},2);near(value.a,1-std::pow(0.8,4),1e-6,"coarse optical depth");
        near_vec(glm::vec3(value)/value.a,color,1e-5f,"premultiplied radiance color");
        const auto traced=volume.trace({-2,0,0},{1,0,0},0,6);
        near(traced.opacity,1-std::pow(0.8,4),2e-5,"actual step-length compositing");
        near_vec(traced.radiance,color*traced.opacity,1e-5f,"radiance weighted by transmittance");
    });
    run("D28 conservative voxelization covers triangle interior, not just vertices",[] {
        VoxelRadianceVolume volume(glm::vec3(-1),glm::vec3(1),16);
        const std::vector<Triangle> ts={triangle({-0.9f,0,-0.9f},{0,0,0.9f},{0.9f,0,-0.9f})};
        volume.voxelize(ts,[](glm::vec3,glm::vec3,int){return glm::vec3(1,0,0);});
        require(volume.sample({0,0,0}).a>0.99f,"triangle interior missing");
        near(volume.sample({0.85f,0,0.85f}).a,0,1e-6,"SAT must reject separated cell");
        volume.voxelize({},{});near(volume.sample({0,0,0}).a,0,1e-6,"revoxelization clears old geometry");
    });
    run("D29 cone opacity stops hidden radiance and empty volume stays empty",[] {
        VoxelRadianceVolume volume(glm::vec3(-1),glm::vec3(1),4);
        auto empty=volume.trace({-2,0,0},{1,0,0},0,6);near(empty.opacity,0,1e-6,"empty opacity");
        for(int z=0;z<4;++z)for(int y=0;y<4;++y){volume.set_voxel(0,y,z,{2,0,0});volume.set_voxel(3,y,z,{0,0,100});}
        volume.build_mips();const auto hit=volume.trace({-2,0,0},{1,0,0},0,6);
        near(hit.opacity,1,1e-6,"opaque foreground");near_vec(hit.radiance,{2,0,0},1e-6f,"hidden blue must not leak through exact level");
    });
    run("D30 analytic sphere tracing hits, misses and inside exits",[] {
        const DistanceFunction field=[](glm::vec3 p){return sdf_sphere(p,{0,0,0},1);};
        auto hit=sphere_trace(field,{{0,0,3},{0,0,-1},0,10},1e-5f);require(hit.hit,"sphere miss");
        near(hit.distance,2,2e-5,"sphere distance");near_vec(hit.normal,{0,0,1},1e-3f,"gradient normal");
        require(!sphere_trace(field,{{2,0,3},{0,0,-1},0,10}).hit,"false positive outside sphere");
        hit=sphere_trace(field,{{0,0,0},{1,0,0},0,10},1e-5f);require(hit.hit,"inside exit missing");near(hit.distance,1,2e-5,"inside exit distance");
        hit=sphere_trace(field,{{0,0,3},{0,0,-2},0,10},1e-5f);near(hit.distance,1,2e-5,"unnormalized ray parameter");
    });
    run("D30 box distance, Lipschitz scaling and soft shadows",[] {
        near(sdf_box({0,0,0},{0,0,0},{1,2,3}),-1,1e-6,"inside box");
        near(sdf_box({2,3,3},{0,0,0},{1,2,3}),std::sqrt(2.0),1e-6,"outside box corner");
        const DistanceFunction field=[](glm::vec3 p){return sdf_sphere(p,{0,0,0},1);};
        const auto hit=sphere_trace([&](glm::vec3 p){return field(p)*3;},{{0,0,3},{0,0,-1},0,10},1e-5f,256,3);
        require(hit.hit,"scaled field miss");near(hit.distance,2,2e-5,"Lipschitz correction");
        near(sdf_soft_shadow(field,{{0,0,3},{0,0,-1},0.01f,6}),0,1e-6,"SDF blocked light");
        require(sdf_soft_shadow(field,{{3,0,3},{0,0,-1},0.01f,6})>0.99f,"SDF clear light");
    });
    run("D30 analytic-to-grid SDF interpolation and safe discrete tracing",[] {
        BakedSdfGrid grid(glm::vec3(-2),glm::vec3(2),24);grid.bake([](glm::vec3 p){return sdf_sphere(p,{0,0,0},1);});
        near(grid.sample({0,0,0}),-1,1e-6,"grid signed center");require(grid.lipschitz_bound()>=1,"grid derivative bound");
        const auto hit=grid.trace({{0.2f,0.1f,3},{0,0,-1},0,10},1e-4f);
        require(hit.hit,"baked sphere miss");near(hit.distance,3-std::sqrt(0.95),grid.geometric_error_bound(),"baked surface error");
        require(!grid.trace({{3,0,3},{0,0,-1},0,10}).hit,"domain miss must stay miss");
    });
    run("D30 triangle-grid bake has closed-solid sign and surface distance",[] {
        BakedSdfGrid grid(glm::vec3(-1.5f),glm::vec3(1.5f),16);grid.bake(cube());
        near(grid.sample({0,0,0}),-1,1e-5,"closed mesh interior sign");
        near(grid.sample({1.3f,0,0}),0.3,1e-5,"closed mesh exterior distance");
        const auto hit=grid.trace({{0.17f,0.23f,3},{0,0,-1},0,10},1e-4f);
        require(hit.hit,"mesh SDF miss");near(hit.distance,2,0.002,"mesh SDF hit distance");
    });
    run("D22 screen trace validates crossing and ignores stored reversed depth",[] {
        Camera camera;camera.position={0,0,0};camera.target={0,0,-1};camera.fov=70;
        auto gbuffer=plane_gbuffer(camera);ScreenTraceSettings options;options.max_distance=8;options.thickness=0.1f;
        const Ray ray{{0,0,-1},safe_normalize(glm::vec3(0.25f,0,-1)),0.01f,8};
        const auto hit=trace_screen_space(gbuffer,camera,ray,options);
        require(hit.valid&&hit.confidence>0,"visible front-facing plane not hit");near(hit.position.z,-3,1e-6,"hit layer");
        near(hit.distance,2*std::sqrt(1.0625),0.02,"screen ray intersection distance");
        for(auto& s:gbuffer.pixels)s.depth=1-s.depth;
        const auto reversed=trace_screen_space(gbuffer,camera,ray,options);
        require(reversed.valid&&reversed.pixel==hit.pixel,"reversed-Z storage changed world-space trace");
    });
    run("D22 screen miss, invalid background and backfaces are not fake hits",[] {
        Camera camera;camera.position={0,0,0};camera.target={0,0,-1};auto gbuffer=plane_gbuffer(camera);
        const Ray ray{{0,0,-1},{0,0,-1},0.01f,10};
        for(auto& s:gbuffer.pixels)s.normal={0,0,-1};
        require(!trace_screen_space(gbuffer,camera,ray).valid,"backface accepted");
        for(auto& s:gbuffer.pixels)s.valid=false;
        require(!trace_screen_space(gbuffer,camera,ray).valid,"empty background accepted");
        require(!trace_screen_space(gbuffer,camera,{{0,0,-1},{1,0,0},0.01f,10}).valid,"offscreen ray accepted");
    });
    run("D22-D23 screen fallback contains exactly one receiver material factor",[] {
        Scene scene;scene.sky_top=scene.sky_bottom=glm::vec3(1);Camera camera;camera.position={0,0,0};camera.target={0,0,-1};
        GBuffer gbuffer(1,1);auto& surface=gbuffer.at(0,0);surface.valid=true;surface.position={0,0,-2};surface.normal={0,0,1};surface.albedo={0.5f,0.25f,0.125f};
        Image<glm::vec3> source(1,1,glm::vec3(100));Settings settings;settings.gi=GiMode::ssr;
        auto result=screen_space_indirect(gbuffer,source,scene,camera,settings);near_vec(result.at(0,0),glm::vec3(0.04f),1e-5f,"SSR environment Fresnel fallback");
        settings.gi=GiMode::ssgi;settings.samples=8;result=screen_space_indirect(gbuffer,source,scene,camera,settings);
        near_vec(result.at(0,0),surface.albedo,1e-5f,"SSGI cosine PDF and receiver albedo");
        settings.gi=GiMode::none;result=screen_space_indirect(gbuffer,source,scene,camera,settings);near_vec(result.at(0,0),glm::vec3(0),1e-6f,"disabled mode");
    });
    run("D22 SSR valid hit blends source color with IBL by confidence",[] {
        Scene scene;scene.sky_top=scene.sky_bottom={0,0,1};Camera camera;camera.position={0,0,0};camera.target={0,0,-1};camera.fov=70;
        auto gbuffer=plane_gbuffer(camera,32);auto& receiver=gbuffer.at(16,16);receiver.position={0,0,-1};receiver.normal=safe_normalize(glm::vec3(1,0,0.1f));receiver.roughness=0;
        Image<glm::vec3> source(32,32,glm::vec3(4,0,0));Settings settings;settings.gi=GiMode::ssr;
        const auto result=screen_space_indirect(gbuffer,source,scene,camera,settings);
        require(result.at(16,16).x>0.05f&&result.at(16,16).z<0.65f,"SSR did not consume a valid source hit");
    });
    run("Integrated directional projection and all seven shadow modes",[] {
        Scene scene;Light light;light.direction={0,-1,0};scene.lights={light};scene.materials.push_back(Material{});
        std::vector<Triangle> ts;quad(ts,{-1,1,-1},{-1,1,1},{1,1,1},{1,1,-1});quad(ts,{-4,0,-4},{-4,0,4},{4,0,4},{4,0,-4});
        Camera camera;camera.position={5,4,6};camera.target={0,0,0};camera.far_plane=30;
        Settings settings;settings.shadow_resolution=64;settings.light_size=0.01f;settings.gi=GiMode::none;
        SurfaceSample shadow,lit;shadow.valid=lit.valid=true;shadow.position={0,0,0};lit.position={3,0,0};
        for(const auto mode:{ShadowMode::hard,ShadowMode::pcf,ShadowMode::pcss,ShadowMode::vsm,ShadowMode::vssm,ShadowMode::msm,ShadowMode::csm}) {
            settings.shadows=mode;ShadowGi gi(scene,ts,camera,settings);
            require(gi.visibility(shadow,light)<0.15f,"mode "+std::to_string(int(mode))+" missed occluder");
            require(gi.visibility(lit,light)>0.9f,"mode "+std::to_string(int(mode))+" shadowed clear receiver");
        }
        scene.lights[0].direction={0.5f,-1,0};light=scene.lights[0];settings.shadows=ShadowMode::hard;settings.shadow_bias=0.03f;
        ShadowGi rotated(scene,ts,camera,settings);shadow.position={0.5f,0,0};lit.position={-2,0,0};
        require(rotated.visibility(shadow,light)<0.1f&&rotated.visibility(lit,light)>0.9f,"light projection has inconsistent direction");
    });
    run("Integrated RSM, LPV, VCT color bounce and environment ownership",[] {
        Scene scene;scene.materials.resize(2);scene.materials[0].base_color=glm::vec4(1);scene.materials[1].base_color={1,0,0,1};
        Light light;light.direction={-1,-1,0};light.intensity=3;scene.lights={light};
        std::vector<Triangle> ts;quad(ts,{-2,0,-2},{-2,0,2},{2,0,2},{2,0,-2},0);
        quad(ts,{-1,0,-2},{-1,2,-2},{-1,2,2},{-1,0,2},1);
        Camera camera;Settings settings;settings.shadow_resolution=32;settings.voxel_resolution=16;settings.samples=16;
        SurfaceSample surface;surface.valid=true;surface.position={0,0,0};surface.normal={0,1,0};surface.albedo=glm::vec3(0.5f);
        for(const auto mode:{GiMode::rsm,GiMode::lpv,GiMode::voxel}) {
            settings.gi=mode;ShadowGi gi(scene,ts,camera,settings);const auto color=gi.indirect(surface,{0,1,0});
            require(std::isfinite(color.x)&&color.x>1e-5f,"mode "+std::to_string(int(mode))+" has no color bounce; red="+std::to_string(color.x));
            require(color.x>color.y*1.1f&&color.x>color.z*1.1f,"mode "+std::to_string(int(mode))+" lost red wall contribution");
        }
        settings.gi=GiMode::environment;ShadowGi gi(scene,ts,camera,settings);
        near_vec(gi.indirect(surface,{0,1,0}),glm::vec3(0),1e-6f,"main owns environment GI");
    });
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_SHADOW_GI_TEST_MAIN
int main() {
    const auto results=emberframe::lab::test_shadow_gi();int failed=0;
    for(const auto& result:results) { std::cout<<(result.passed?"PASS ":"FAIL ")<<result.name<<": "<<result.detail<<'\n';failed+=!result.passed; }
    std::cout<<results.size()-std::size_t(failed)<<'/'<<results.size()<<" passed\n";return failed?1:0;
}
#endif
