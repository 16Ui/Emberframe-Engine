#include "temporal_session.h"
#include "reference_renderer.h"
#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>
#ifdef EMBERFRAME_TEMPORAL_SESSION_TEST_MAIN
#include <iostream>
#endif

namespace emberframe::lab {
namespace {
void require(bool condition,const std::string& reason) { if (!condition) throw std::runtime_error(reason); }
std::string metric(const char* label,double v) { std::ostringstream s; s<<label<<'='<<v; return s.str(); }
template<class F> void test(TestResults& results,const char* name,F&& f) {
    try { results.push_back({name,true,f()}); }
    catch (const std::exception& e) { results.push_back({name,false,e.what()}); }
    catch (...) { results.push_back({name,false,"unknown exception"}); }
}
Camera view() { Camera c; c.position={0,0,0}; c.target={0,0,-1}; c.fov=60; return c; }
Settings options() {
    Settings s; s.path=RenderPath::path_trace; s.gi=GiMode::none; s.ao=AoMode::none;
    s.bloom=false; s.samples=1; s.max_bounces=2; s.render_width=16; s.render_height=16;
    s.shadow_resolution=16; s.voxel_resolution=4; return s;
}
Scene plane_scene(bool emissive=false) {
    Scene s; s.name="session deterministic plane";
    s.sky_top={2,0.9f,0.3f}; s.sky_bottom={0.04f,0.15f,0.8f};
    Material material; material.base_color={0.7f,0.7f,0.7f,1}; material.roughness=0.8f;
    if (emissive) { material.base_color={0,0,0,1}; material.emissive={0.3f,0.3f,0.3f}; }
    s.materials.push_back(material);
    Mesh m;
    for (const auto& p:std::array<glm::vec3,4>{{{-10,-10,-3},{10,-10,-3},{10,10,-3},{-10,10,-3}}}) {
        Vertex v; v.position=p; v.normal={0,0,1}; v.tangent={1,0,0,1}; m.vertices.push_back(v);
    }
    m.indices={0,1,2,0,2,3}; m.primitives.push_back({0,6,0}); s.meshes.push_back(m);
    Node node; node.mesh=0; s.nodes.push_back(node); return s;
}
double image_error(const Image<glm::vec3>& a,const Image<glm::vec3>& b) {
    require(a.width==b.width && a.height==b.height,"image extent mismatch");
    double sum=0;
    for (std::size_t i=0;i<a.pixels.size();++i) {
        const auto d=a.pixels[i]-b.pixels[i]; sum+=double(glm::dot(d,d))/3;
    }
    return sum/double(std::max(std::size_t(1),a.pixels.size()));
}
float constant_error(const Image<glm::vec3>& a,glm::vec3 expected) {
    float error=0; for (const auto& c:a.pixels) error=std::max(error,glm::length(c-expected)); return error;
}
RenderOutput raw_render(const Scene& scene,const Camera& camera,Settings s,std::uint32_t seed,glm::vec2 jitter={0,0}) {
    s.seed=seed; s.taa=s.denoise=s.svgf=false; s.bloom=s.outline=s.hatching=false; s.debug=DebugView::final_color;
    ReferenceFrameInput input; input.apply_postprocess=false; input.jitter_ndc=jitter;
    return render_reference(scene,camera,s,input);
}
} // namespace

TestResults test_temporal_session() {
    TestResults results;
    test(results,"session/linear_HDR_and_one_bloom",[] {
        Scene scene; scene.sky_top=scene.sky_bottom=glm::vec3(4);
        auto s=options(); s.bloom=true; s.bloom_strength=0.25f; s.bloom_threshold=1; s.exposure=100;
        ReferenceSession session; const auto out=session.render(scene,view(),s);
        const float error=constant_error(out.color,glm::vec3(4.75f));
        require(error<2e-5f,"HDR should be 4 + .25*(4-1), with no tone mapping: "+metric("error",error));
        require(out.variance.width==16 && out.variance.height==16,"variance buffer missing when temporal is off");
        return metric("HDR",out.color.at(8,8).x)+"; one bloom, no exposure/tone-map";
    });
    test(results,"session/one_toon_then_bloom",[] {
        auto scene=plane_scene(true); auto s=options(); s.max_bounces=1; s.shading=ShadingMode::toon;
        s.bloom=true; s.bloom_threshold=0; s.bloom_strength=0.5f;
        ReferenceSession session; const auto out=session.render(scene,view(),s);
        const float error=constant_error(out.color,glm::vec3(0.48f));
        require(error<1e-5f,"emissive .3 -> band .32 -> bloom .48: "+metric("actual",out.color.at(8,8).x));
        return metric("toon + single bloom",out.color.at(8,8).x);
    });
    test(results,"session/copied_snapshots_keep_history_and_seed",[] {
        auto scene=plane_scene(true); auto s=options(); s.taa=true; s.max_bounces=1;
        ReferenceSession session; std::uint32_t first=0; std::uint64_t resets=0;
        for (int i=0;i<5;++i) {
            Scene snapshot=scene; const auto out=session.render(snapshot,view(),s); const auto info=session.info();
            require(info.frames==std::uint64_t(i+1),"snapshot address change restarted sequence");
            require(!out.color.empty(),"missing committed frame");
            if (i==0) { first=info.frame_seed; resets=info.reset_count; require(first==s.seed,"first frame must preserve base seed"); }
            else require(info.frame_seed!=first && info.reset_count==resets,"seed failed to advance or copied scene reset");
        }
        const auto info=session.info();
        require(info.max_history_length>=4 && info.reused_fraction>0.8f,"history did not actually survive calls");
        return metric("history age",info.max_history_length)+", "+metric("coverage",info.reused_fraction);
    });
    test(results,"session/true_raster_projection_jitter",[] {
        const auto scene=plane_scene(true); const auto c=view(); auto s=options(); s.path=RenderPath::cpu_raster; s.taa=true;
        ReferenceSession session;
        const auto a=session.render(scene,c,s); const auto ia=session.info();
        const auto b=session.render(scene,c,s); const auto ib=session.info();
        const auto delta=b.surfaces.at(8,8).position-a.surfaces.at(8,8).position;
        const float scale=3*std::tan(glm::radians(c.fov)*0.5f);
        const glm::vec2 predicted=-(ib.jitter_ndc-ia.jitter_ndc)*scale;
        // 这个等深平面的屏幕->世界映射为仿射。光栅顶点 q_i=p_i+e_i，
        // llround(256*p_i) 保证每轴 |e_i|<=1/512 像素。像素内部 λ_i>=0、
        // Σλ_i=1，因此插值位置误差是 -Σλ_i*e_i 的世界空间映射，同样
        // <=1/512 像素；两帧差分的每轴误差 <=1/256 像素。这里直接检查
        // 该解析界限，不加任意固定 epsilon，也不按实测误差反推容差。
        const glm::vec2 world_per_pixel(2*scale*float(s.render_width)/s.render_height/s.render_width,
                                       2*scale/s.render_height);
        const glm::vec2 axis_bound=world_per_pixel/256.0f;
        const glm::vec2 axis_error=glm::abs(glm::vec2(delta)-predicted);
        require(axis_error.x<=axis_bound.x && axis_error.y<=axis_bound.y,
                "jitter world-sample mismatch: "+metric("error x",axis_error.x)+", "+metric("error y",axis_error.y)
                +", "+metric("bound x",axis_bound.x)+", "+metric("bound y",axis_bound.y));
        require(glm::length(glm::vec2(delta))>0.01f,"jitter only stored as metadata");
        require(glm::length(b.surfaces.at(8,8).motion)<1e-4f,"static surface motion includes projection jitter");
        require(ib.reused_fraction>0.8f,"jitter compensation rejected static history");
        return metric("world-sample displacement",glm::length(delta))+", "+metric("max axis error",std::max(axis_error.x,axis_error.y))
            +", "+metric("1/256-pixel axis bound",axis_bound.x);
    });
    test(results,"session/true_path_ray_jitter",[] {
        Scene scene; scene.sky_top={3,1,0}; scene.sky_bottom={0,0,1}; auto s=options(); s.taa=true;
        ReferenceSession session; const auto out=session.render(scene,view(),s); const auto info=session.info();
        const auto expected=raw_render(scene,view(),s,info.frame_seed,info.jitter_ndc);
        const auto unjittered=raw_render(scene,view(),s,info.frame_seed);
        require(image_error(out.color,expected.color)<1e-12,"session first frame differs from jittered ray reference");
        const double changed=image_error(out.color,unjittered.color);
        require(changed>1e-7,"path rays did not apply projection jitter");
        return metric("jittered/un-jittered ray MSE",changed);
    });
    test(results,"session/continuous_camera_validation",[] {
        const auto scene=plane_scene(true); auto c=view(); auto s=options(); s.taa=true; s.max_bounces=1;
        ReferenceSession session; session.render(scene,c,s); const auto first=session.info();
        for (int i=1;i<=3;++i) {
            c.position.x+=0.025f; c.target.x+=0.025f;
            const auto out=session.render(scene,c,s); const auto info=session.info();
            require(info.frames==std::uint64_t(i+1) && info.reset_count==first.reset_count && !info.history_reset,
                    "continuous camera motion reset the whole session");
            require(info.reused_fraction>0.75f && info.max_history_length>=float(i+1)-1e-4f,"moving camera lost stable plane history");
            require(out.surfaces.at(8,8).motion.x<0,"rightward camera should move geometry left in pixels");
        }
        return metric("moving-camera history age",session.info().max_history_length);
    });
    test(results,"session/stale_previous_world_is_normalized",[] {
        auto scene=plane_scene(true); scene.nodes[0].local=glm::translate(glm::mat4(1),glm::vec3(0.5f,0,0.3f));
        auto s=options(); s.taa=true; s.max_bounces=1; ReferenceSession session;
        session.render(scene,view(),s); const auto out=session.render(scene,view(),s);
        require(scene.nodes[0].previous_world==glm::mat4(1),"session mutated caller's scene");
        require(glm::length(out.surfaces.at(8,8).motion)<1e-4f,"static transformed object reprojects to identity transform");
        require(session.info().reused_fraction>0.8f,"stale previous transforms destroyed static accumulation");
        return metric("history coverage",session.info().reused_fraction);
    });
    test(results,"session/scene_content_revision_and_settings_reset",[] {
        auto scene=plane_scene(true); auto s=options(); s.taa=true; s.max_bounces=1; ReferenceSession session;
        const auto render_twice=[&] { session.render(scene,view(),s); session.render(scene,view(),s); require(session.info().frames==2,"reset frame sequence did not warm up"); };
        render_twice(); const auto initial=session.info();
        ++scene.revision; session.render(scene,view(),s); require(session.info().frames==1,"revision change not detected");
        scene.materials[0].emissive=glm::vec3(0.8f); const auto changed=session.render(scene,view(),s);
        require(session.info().frames==1 && session.info().reused_fraction==0,"unversioned material edit retained history");
        require(constant_error(changed.color,glm::vec3(0.8f))<1e-5f,"material edit contaminated by old lighting");
        scene.meshes[0].vertices[0].position.x-=0.1f; session.render(scene,view(),s); require(session.info().frames==1,"mesh edit not detected");
        scene.sky_top.r+=0.1f; session.render(scene,view(),s); require(session.info().frames==1,"sky edit not detected");
        scene.nodes[0].local=glm::translate(glm::mat4(1),glm::vec3(0.1f,0,0));
        session.render(scene,view(),s); require(session.info().frames==1,"transform edit not detected");
        ++s.samples; session.render(scene,view(),s); require(session.info().frames==1,"sample settings edit not detected");
        s.render_width=12; const auto resized=session.render(scene,view(),s);
        require(resized.color.width==12 && resized.variance.width==12 && session.info().frames==1,"resize reset failed");
        require(session.info().reset_count>=initial.reset_count+7,"reset accounting");
        return metric("reset count",double(session.info().reset_count));
    });
    test(results,"session/display_changes_do_not_invalidate_linear_history",[] {
        const auto scene=plane_scene(true); auto s=options(); s.taa=true; s.max_bounces=1; ReferenceSession session;
        session.render(scene,view(),s); const auto first=session.info();
        s.debug=DebugView::normal; s.exposure=4; s.bloom=true; s.outline=true; s.hatching=true;
        const auto normal=session.render(scene,view(),s);
        require(session.info().frames==2 && session.info().reset_count==first.reset_count,"display-only change reset history");
        require(constant_error(normal.color,{0.5f,0.5f,1})<1e-5f,"normal debug polluted by NPR/bloom");
        s.debug=DebugView::final_color; s.bloom=false; s.outline=false; s.hatching=false;
        const auto final=session.render(scene,view(),s);
        require(session.info().frames==3 && constant_error(final.color,glm::vec3(0.3f))<1e-5f,"debug image fed back into HDR history");
        return "debug/exposure/style switches preserved 3-frame HDR history";
    });
    test(results,"session/real_multiframe_denoising",[] {
        const auto scene=plane_scene(); const auto c=view(); auto s=options(); s.denoise=true;
        auto high=s; high.samples=512; high.seed=707;
        const auto target=raw_render(scene,c,high,high.seed);
        ReferenceSession session; RenderOutput output; double raw_error=0; std::uint32_t last_seed=0;
        for (int frame=0;frame<12;++frame) {
            output=session.render(scene,c,s); const auto info=session.info();
            require(frame==0 || info.frame_seed!=last_seed,"consecutive frames repeat noise seed"); last_seed=info.frame_seed;
            const auto raw=raw_render(scene,c,s,info.frame_seed); raw_error+=image_error(raw.color,target.color);
        }
        const double filtered=image_error(output.color,target.color),ratio=filtered/(raw_error/12);
        require(raw_error>1e-5 && ratio<0.2,"actual path frames did not denoise: "+metric("MSE ratio",ratio));
        require(session.info().max_history_length>=11.9f,"real history did not accumulate 12 frames");
        return metric("filtered/raw MSE vs 512spp",ratio);
    });
    test(results,"session/svgf_variance_debug_and_clean_history",[] {
        const auto scene=plane_scene(); auto s=options(); s.svgf=true; s.debug=DebugView::variance;
        ReferenceSession session; RenderOutput out;
        for (int i=0;i<5;++i) out=session.render(scene,view(),s);
        float maximum=0; double error=0;
        for (std::size_t i=0;i<out.variance.pixels.size();++i) {
            const float variance=out.variance.pixels[i]; require(std::isfinite(variance) && variance>=0,"invalid output variance");
            maximum=std::max(maximum,variance); const float sigma=std::sqrt(variance);
            error+=glm::length(out.color.pixels[i]-glm::vec3(sigma/(1+sigma)));
        }
        require(maximum>1e-8f && error<1e-5,"variance debug not derived from real output variance");
        const auto age=session.info().max_history_length;
        s.debug=DebugView::final_color; out=session.render(scene,view(),s);
        require(session.info().frames==6 && session.info().max_history_length>age,"debug switch cleared accumulated moments");
        require(out.color.at(8,8).x>0.02f,"variance display was mistakenly accumulated as HDR");
        return metric("maximum variance",maximum)+", "+metric("history age",session.info().max_history_length);
    });
    test(results,"session/cancellation_exception_and_retry_transaction",[] {
        const auto scene=plane_scene(); auto s=options(); s.taa=true; ReferenceSession session,control;
        session.render(scene,view(),s); control.render(scene,view(),s); const auto before=session.info();
        std::atomic<bool> cancel{true};
        const auto cancelled=session.render(scene,view(),s,&cancel);
        require(cancelled.color.empty() && session.info().frames==before.frames,"cancelled frame consumed history");
        auto bad=s; bad.render_width=0; bool threw=false;
        try { session.render(scene,view(),bad); } catch (const std::exception&) { threw=true; }
        require(threw && session.info().frames==before.frames,"failed render mutated committed history");
        cancel=false; const auto retried=session.render(scene,view(),s,&cancel),expected=control.render(scene,view(),s);
        require(image_error(retried.color,expected.color)<1e-12 && session.info().frame_seed==control.info().frame_seed,
                "retry after cancellation/exception differs from uninterrupted sequence");
        session.reset(); require(session.info().frames==0,"explicit reset did not clear sequence");
        session.render(scene,view(),s); require(session.info().frames==1 && session.info().frame_seed==s.seed,"explicit reset did not restart sampling");
        return "cancel/exception committed nothing; deterministic retry matches uninterrupted control";
    });
    test(results,"session/inflight_cancel_discards_partial_render",[] {
        const auto scene=plane_scene(); auto s=options(); s.denoise=true; ReferenceSession session;
        session.render(scene,view(),s); const auto before=session.info();
        auto slow=s; slow.render_width=64; slow.render_height=64; slow.samples=65536;
        std::atomic<bool> cancel{false},entered{false}; RenderOutput result; std::exception_ptr failure;
        std::thread worker([&] {
            entered.store(true); try { result=session.render(scene,view(),slow,&cancel); }
            catch (...) { failure=std::current_exception(); }
        });
        while (!entered.load()) std::this_thread::yield();
        // Cancellation correctness is independent of whether this lands before
        // ray work or during it; the deliberately long render cannot finish here.
        std::this_thread::sleep_for(std::chrono::milliseconds(3)); cancel.store(true); worker.join();
        if (failure) std::rethrow_exception(failure);
        require(result.color.empty() && session.info().frames==before.frames && session.info().frame_seed==before.frame_seed,
                "in-flight cancel committed a partial frame or changed sequence");
        return "cancelled heavy actual render returned empty; committed frame/seed unchanged";
    });
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_TEMPORAL_SESSION_TEST_MAIN
int main() {
    const auto results=emberframe::lab::test_temporal_session(); int failures=0;
    for (const auto& result:results) {
        std::cout<<(result.passed ? "PASS ":"FAIL ")<<result.name<<": "<<result.detail<<'\n';
        failures+=result.passed ? 0:1;
    }
    std::cout<<results.size()-std::size_t(failures)<<'/'<<results.size()<<" passed\n";
    return failures ? 1:0;
}
#endif
