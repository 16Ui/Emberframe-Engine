#include "postprocess.h"
#include <cmath>
#include <sstream>
#ifdef EMBERFRAME_POSTPROCESS_TEST_MAIN
#include <iostream>
#endif

namespace emberframe::lab {
namespace {
Camera test_camera() {
    Camera c; c.position={0,0,0}; c.target={0,0,-1}; c.fov=60;
    c.near_plane=0.05f; c.far_plane=100; return c;
}
GBuffer plane(int w,int h,const Camera& camera,float depth=3,glm::vec2 jitter={0,0}) {
    GBuffer g(w,h); const auto inv_view=glm::inverse(camera.view());
    const float sy=std::tan(glm::radians(camera.fov)*0.5f),sx=sy*float(w)/h;
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        auto& p=g.at(x,y);
        const glm::vec3 v(((x+0.5f)*2/w-1-jitter.x)*sx*depth,
                          (1-(y+0.5f)*2/h-jitter.y)*sy*depth,-depth);
        p.position=glm::vec3(inv_view*glm::vec4(v,1));
        p.normal=glm::vec3(inv_view*glm::vec4(0,0,1,0));
        p.linear_depth=depth; p.object=1; p.material=1; p.valid=true; p.albedo=glm::vec3(0.5f);
    }
    return g;
}
void demand(bool condition,const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string value(const char* label,double v) {
    std::ostringstream s; s<<label<<'='<<v; return s.str();
}
bool finite_image(const Image<glm::vec3>& im) {
    for (const auto& c:im.pixels) for (int k=0;k<3;++k)
        if (!std::isfinite(c[k]) || c[k]<0) return false;
    return true;
}
bool finite_image(const Image<float>& im) {
    for (float v:im.pixels) if (!std::isfinite(v) || v<0) return false;
    return true;
}
float max_error(const Image<glm::vec3>& im,glm::vec3 target) {
    float e=0;
    for (const auto& c:im.pixels) for (int k=0;k<3;++k) e=std::max(e,std::abs(c[k]-target[k]));
    return e;
}
double mse(const Image<glm::vec3>& im,float target,int border=0) {
    double error=0; int count=0;
    for (int y=border;y<im.height-border;++y) for (int x=border;x<im.width-border;++x) {
        const double d=im.at(x,y).x-target; error+=d*d; ++count;
    }
    return error/std::max(1,count);
}
Image<glm::vec3> noise(int w,int h,int frame) {
    Image<glm::vec3> im(w,h,glm::vec3(0));
    // 固定整数散列生成可复现噪声，不依赖实现相关的随机分布。
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        std::uint32_t v=std::uint32_t(x)*73856093u ^ std::uint32_t(y)*19349663u ^ std::uint32_t(frame+1)*83492791u;
        v^=v>>16; v*=0x7feb352du; v^=v>>15; v*=0x846ca68bu; v^=v>>16;
        im.at(x,y)=glm::vec3(1+(float(v&0xffffu)/65535.0f-0.5f)*1.6f);
    }
    return im;
}
double energy(const Image<glm::vec3>& im) {
    double total=0; for (const auto& p:im.pixels) total+=p.x; return total;
}
template<class F> void run(TestResults& results,const char* name,F&& test) {
    try { results.push_back({name,true,test()}); }
    catch (const std::exception& e) { results.push_back({name,false,e.what()}); }
    catch (...) { results.push_back({name,false,"unexpected non-standard exception"}); }
}
} // namespace

TestResults test_postprocess() {
    TestResults results;
    run(results,"postprocess/jitter_halton_ndc",[] {
        const auto a=taa_jitter(0,100,50),b=taa_jitter(1,100,50);
        demand(std::abs(a.x)<1e-7f && std::abs(a.y+1.0f/150)<1e-7f,"frame zero must equal Halton sample (0.5,1/3)");
        demand(std::abs(b.x+0.005f)<1e-7f && std::abs(b.y-1.0f/150)<1e-7f,"frame one sequence/sign incorrect");
        for (std::uint64_t i=0;i<256;++i) {
            const auto j=taa_jitter(i,100,50);
            demand(std::abs(j.x)<=0.01f && std::abs(j.y)<=0.02f,"jitter exceeds half a pixel");
        }
        demand(taa_jitter(0,0,2)==glm::vec2(0),"zero extent jitter must be zero");
        const auto last=taa_jitter(std::numeric_limits<std::uint64_t>::max(),100,50);
        demand(std::isfinite(last.x) && std::isfinite(last.y),"frame overflow");
        return "256 samples bounded; NDC X/Y scale and first two values checked";
    });
    run(results,"postprocess/ao_flat_plane_and_disabled",[] {
        auto c=test_camera(); auto g=plane(25,25,c); Settings s; s.samples=16;
        float error=0;
        for (auto mode:{AoMode::ssao,AoMode::gtao}) {
            s.ao=mode; const auto a=ambient_occlusion(g,c,s);
            for (float v:a.pixels) { demand(std::isfinite(v) && v>=0 && v<=1,"AO range"); error=std::max(error,std::abs(v-1)); }
        }
        demand(error<0.002f,"unoccluded plane should have visibility one: "+value("error",error));
        s.ao=AoMode::none; auto a=ambient_occlusion(g,c,s);
        demand(a.at(12,12)==1,"disabled AO");
        s.ao=AoMode::gtao; s.ao_radius=0; a=ambient_occlusion(g,c,s);
        demand(a.at(12,12)==1,"zero-radius AO");
        return value("maximum visibility error",error);
    });
    run(results,"postprocess/ao_near_occluder_and_radius",[] {
        auto c=test_camera(); auto g=plane(49,49,c); auto foreground=plane(49,49,c,2.72f);
        for (int y=0;y<49;++y) for (int x=27;x<38;++x) { g.at(x,y)=foreground.at(x,y); g.at(x,y).object=2; }
        Settings s; s.samples=64; s.ao_radius=1.1f;
        std::string report;
        for (auto mode:{AoMode::ssao,AoMode::gtao}) {
            s.ao=mode; const auto occluded=ambient_occlusion(g,c,s);
            s.ao_radius=0.08f; const auto small=ambient_occlusion(g,c,s); s.ao_radius=1.1f;
            const float near=occluded.at(25,24),far=occluded.at(3,24);
            demand(near<0.98f && near<far-0.015f,"occluder must lower visibility: "+value("near",near)+", "+value("far",far));
            demand(small.at(25,24)>near+0.01f,"world-space radius must exclude distant occluder");
            demand(finite_image(occluded),"AO nonfinite");
            report+=(mode==AoMode::ssao ? "SSAO " : "; GTAO ")+value("near",near);
        }
        return report;
    });
    run(results,"postprocess/gtao_cosine_integral_analytic_ring",[] {
        // 中心法线朝向相机，各方位地平线 θ=π/4。解析可见性 sin²θ=1/2。
        auto c=test_camera(); auto g=plane(65,65,c);
        const glm::vec3 center=g.at(32,32).position;
        for (int y=0;y<65;++y) for (int x=0;x<65;++x) {
            const float dx=float(x-32),dy=float(32-y),r=std::sqrt(dx*dx+dy*dy);
            if (r==0) continue;
            // 合成高度场保留每个像素的射线方向，求 z=3-r_xy 的交点。
            const float slope=2*std::tan(glm::radians(c.fov)*0.5f)*r/65;
            const float z=3/(1+slope),world_r=z*slope;
            g.at(x,y).position=center+glm::vec3(world_r*dx/r,world_r*dy/r,world_r);
            g.at(x,y).linear_depth=z;
        }
        Settings s; s.ao=AoMode::gtao; s.samples=16; s.ao_radius=1.5f;
        const float actual=ambient_occlusion(g,c,s).at(32,32);
        demand(std::abs(actual-0.5f)<0.012f,"GTAO must integrate cosine/Jacobian: "+value("visibility",actual));
        return value("analytic 0.5, measured",actual);
    });
    run(results,"postprocess/bilateral_constant_noise_and_edges",[] {
        auto c=test_camera(); auto g=plane(25,25,c);
        Image<glm::vec3> constant(25,25,glm::vec3(0.3f,1.5f,2));
        demand(max_error(joint_bilateral_filter(constant,g),{0.3f,1.5f,2})<1e-5f,"bilateral must preserve DC");
        const auto input=noise(25,25,0),smooth=joint_bilateral_filter(input,g);
        const double before=mse(input,1,3),after=mse(smooth,1,3);
        demand(after<before*0.25,"bilateral noise attenuation: "+value("ratio",after/before));
        Image<glm::vec3> edge(25,25,glm::vec3(0));
        for (int y=0;y<25;++y) for (int x=12;x<25;++x) { edge.at(x,y)=glm::vec3(4); g.at(x,y).object=2; }
        const auto filtered=joint_bilateral_filter(edge,g,4);
        demand(filtered.at(11,12).x<1e-6f && std::abs(filtered.at(12,12).x-4)<1e-5f,"must not blur across object IDs");
        return value("noise MSE ratio",after/before)+"; object edge preserved";
    });
    run(results,"postprocess/bilateral_normal_and_depth_guidance",[] {
        auto c=test_camera(); auto g=plane(17,17,c); Image<glm::vec3> im(17,17,glm::vec3(0));
        for (int y=0;y<17;++y) for (int x=9;x<17;++x) { im.at(x,y)=glm::vec3(10); g.at(x,y).normal={1,0,0}; }
        auto f=joint_bilateral_filter(im,g);
        demand(f.at(8,8).x<1e-6f,"normal crease leaks despite shared ID");
        for (int y=0;y<17;++y) for (int x=9;x<17;++x) {
            g.at(x,y).normal={0,0,1}; g.at(x,y).position.z=-7; g.at(x,y).linear_depth=7;
        }
        f=joint_bilateral_filter(im,g);
        demand(f.at(8,8).x<1e-4f,"depth/position discontinuity leaks despite shared ID");
        return value("depth edge contamination",f.at(8,8).x);
    });
    run(results,"postprocess/temporal_constant_and_reset",[] {
        auto c=test_camera(); auto g=plane(13,13,c); Settings s; s.taa=true;
        Image<glm::vec3> im(13,13,glm::vec3(0.2f,0.7f,1.4f)); TemporalFilter f;
        for (int i=0;i<8;++i) demand(max_error(f.process(im,g,c,s),{0.2f,0.7f,1.4f})<1e-5f,"constant temporal signal changes");
        demand(f.history_validity().at(6,6)>0.99f && f.history_length().at(6,6)==8,"history does not accumulate");
        demand(f.variance().at(6,6)<1e-6f,"constant signal variance");
        f.reset(); demand(f.variance().empty() && f.history_length().empty(),"reset retains resources");
        f.process(im,g,c,s); demand(f.history_validity().at(6,6)==0 && f.history_length().at(6,6)==1,"reset accepted stale history");
        return "8-frame DC preserved; history 8 -> reset -> 1; variance near zero";
    });
    run(results,"postprocess/taa_exact_ema_and_clamp",[] {
        auto c=test_camera(); auto g=plane(11,11,c); Settings s; s.taa=true; s.temporal_weight=0.1f;
        Image<glm::vec3> old(11,11,glm::vec3(0.8f)),cur(11,11,glm::vec3(0.2f));
        for (int y=0;y<11;++y) for (int x=0;x<11;++x) if ((x+y)%2) cur.at(x,y)=glm::vec3(1);
        TemporalFilter f; f.process(old,g,c,s); const auto blended=f.process(cur,g,c,s);
        demand(std::abs(blended.at(5,5).x-0.74f)<1e-4f,"a=.1 current=.2 previous=.8 must produce .74: "+value("actual",blended.at(5,5).x));
        cur=Image<glm::vec3>(11,11,glm::vec3(0.1f)); const auto changed=f.process(cur,g,c,s);
        demand(max_error(changed,glm::vec3(0.1f))<1e-5f,"clamp must respond to lighting step");
        return value("EMA",blended.at(5,5).x)+"; uniform lighting change clamped";
    });
    run(results,"postprocess/temporal_moving_object_pixel_sign",[] {
        auto c=test_camera(); auto previous=plane(17,13,c),current=previous;
        Image<glm::vec3> old(17,13,glm::vec3(0)),now=old;
        for (auto& p:previous.pixels) p.object=0;
        for (auto& p:current.pixels) p.object=0;
        for (int y=3;y<=9;++y) for (int x=3;x<=7;++x) { previous.at(x,y).object=8; old.at(x,y)=glm::vec3(0.8f); }
        for (int y=3;y<=9;++y) for (int x=6;x<=10;++x) {
            current.at(x,y).object=8; current.at(x,y).motion={3,0}; now.at(x,y)=glm::vec3((x+y)%2 ? 1.0f : 0.2f);
        }
        Settings s; s.taa=true; s.temporal_weight=0.1f; TemporalFilter f;
        f.process(old,previous,c,s); const auto out=f.process(now,current,c,s);
        demand(f.history_validity().at(10,6)>0.99f,"right-moving object must sample x-3, not x+3");
        demand(out.at(10,6).x>0.65f,"moving object failed to reuse history");
        demand(f.history_validity().at(4,6)==0 && out.at(4,6).x==0,"newly exposed background retained old object");
        return value("moving history pixel",out.at(10,6).x)+"; exposed background rejected";
    });
    run(results,"postprocess/temporal_depth_normal_id_and_bounds_rejection",[] {
        auto c=test_camera(); const auto base=plane(9,9,c); Settings s; s.taa=true;
        Image<glm::vec3> im(9,9,glm::vec3(0.8f));
        for (int reason=0;reason<5;++reason) {
            TemporalFilter f; auto g=base; f.process(im,g,c,s);
            auto& p=g.at(4,4);
            if (reason==0) { p.position.z=-5; p.linear_depth=5; }
            if (reason==1) p.normal={1,0,0};
            if (reason==2) p.object=2;
            if (reason==3) p.motion={100,0};
            if (reason==4) p.valid=false;
            f.process(im,g,c,s);
            demand(f.history_validity().at(4,4)==0,"rejection failed for reason "+std::to_string(reason));
        }
        return "five independent rejection causes checked";
    });
    run(results,"postprocess/temporal_bilinear_per_tap_validation",[] {
        auto c=test_camera(); auto g=plane(11,11,c); Settings s; s.taa=true;
        Image<glm::vec3> old(11,11,glm::vec3(0.8f)),cur(11,11,glm::vec3(0.2f));
        g.at(4,5).object=2; old.at(4,5)=glm::vec3(100);
        TemporalFilter f; f.process(old,g,c,s);
        g.at(4,5).object=1; g.at(5,5).motion={0.5f,0};
        for (int y=4;y<=6;++y) for (int x=4;x<=6;++x) if ((x+y)%2) cur.at(x,y)=glm::vec3(1);
        const auto out=f.process(cur,g,c,s);
        demand(std::abs(f.history_validity().at(5,5)-0.5f)<1e-5f,"invalid neighboring tap was not excluded");
        demand(std::abs(out.at(5,5).x-0.74f)<1e-4f,"remaining tap weights not renormalized");
        return value("accepted bilinear mass",f.history_validity().at(5,5));
    });
    run(results,"postprocess/temporal_camera_motion_world_fallback",[] {
        auto before=test_camera(),after=before; after.position.x=0.15f; after.target.x=0.15f;
        auto a=plane(21,21,before),b=plane(21,21,after); Settings s; s.taa=true;
        Image<glm::vec3> old(21,21,glm::vec3(0.8f)),cur(21,21,glm::vec3(0.2f));
        for (int y=0;y<21;++y) for (int x=0;x<21;++x) if ((x+y)%2) cur.at(x,y)=glm::vec3(1);
        TemporalFilter f; f.process(old,a,before,s); const auto out=f.process(cur,b,after,s);
        demand(f.history_validity().at(10,10)>0.99f,"camera reprojection rejected a static plane");
        demand(std::abs(out.at(10,10).x-0.74f)<1e-4f,"static camera motion did not retain history");
        demand(f.history_validity().at(20,10)==0,"offscreen world reprojection must be rejected");
        return "zero motion vectors reproject static plane with previous camera; offscreen rejected";
    });
    run(results,"postprocess/temporal_camera_depth_change",[] {
        auto before=test_camera(),after=before; after.position.z=0.3f; after.target.z=-0.7f;
        auto a=plane(15,15,before,3),b=plane(15,15,after,3.3f);
        Settings s; s.taa=true; Image<glm::vec3> im(15,15,glm::vec3(1)); TemporalFilter f;
        f.process(im,a,before,s); f.process(im,b,after,s);
        demand(f.history_validity().at(7,7)>0.99f,"compare depth in PREVIOUS camera space, not current linear depth");
        return "current depth 3.3, previous depth 3.0 matched through previous view matrix";
    });
    run(results,"postprocess/temporal_moving_depth_previous_positions",[] {
        auto c=test_camera(); auto a=plane(15,15,c,3),b=plane(15,15,c,2.6f);
        Image<glm::vec3> positions(15,15,glm::vec3(0));
        for (std::size_t i=0;i<positions.pixels.size();++i) positions.pixels[i]=a.pixels[i].position;
        Settings s; s.taa=true; Image<glm::vec3> im(15,15,glm::vec3(1)); TemporalFilter f;
        TemporalInput options; options.motion=MotionConvention::current_minus_previous_pixels_explicit;
        f.process(im,a,c,s,options); f.process(im,b,c,s,options);
        demand(f.history_validity().at(7,7)==0,"unknown motion in depth must reject");
        f.reset(); f.process(im,a,c,s,options); options.previous_world_positions=&positions;
        f.process(im,b,c,s,options);
        demand(f.history_validity().at(7,7)>0.99f,"previous world positions should allow depth-changing object");
        return "depth change rejected without correspondence; accepted with previous world positions";
    });
    run(results,"postprocess/temporal_ndc_motion_and_jitter_compensation",[] {
        auto c=test_camera(); auto a=plane(17,17,c),b=a;
        Image<glm::vec3> im(17,17,glm::vec3(1)); Settings s; s.taa=true; TemporalFilter f;
        TemporalInput options; options.motion=MotionConvention::backward_ndc;
        for (auto& p:a.pixels) p.object=0;
        a.at(6,9).object=7; b.at(8,8).object=7;
        b.at(8,8).motion={-4.0f/17,-2.0f/17}; // NDC +Y up; sample previous (6,9).
        f.process(im,a,c,s,options); f.process(im,b,c,s,options);
        demand(f.history_validity().at(8,8)>0.99f,"NDC motion sign/scale incorrect");
        f.reset(); options.motion=MotionConvention::current_minus_previous_pixels_explicit;
        options.jitter_ndc={0.5f/17,0}; a=plane(17,17,c,3,options.jitter_ndc);
        f.process(im,a,c,s,options);
        options.jitter_ndc={-0.5f/17,0}; b=plane(17,17,c,3,options.jitter_ndc);
        b.at(8,8).object=1; a.at(9,8).object=2;
        // Different jitter means half-pixel history lookup even with zero unjittered motion.
        f.reset(); TemporalInput prev_options=options; prev_options.jitter_ndc={0.5f/17,0};
        f.process(im,a,c,s,prev_options); f.process(im,b,c,s,options);
        demand(std::abs(f.history_validity().at(8,8)-0.5f)<1e-5f,"jitter delta missing or wrong sign");
        return "NDC (-4/W,-2/H) -> pixels (-2,+1); jitter moves sample +0.5 pixels";
    });
    run(results,"postprocess/temporal_resize_mode_and_cut_lifecycle",[] {
        auto c=test_camera(); auto g=plane(9,9,c); Image<glm::vec3> im(9,9,glm::vec3(1)); Settings s; s.taa=true;
        TemporalFilter f; f.process(im,g,c,s); f.process(im,g,c,s);
        TemporalInput opt; opt.camera_cut=true; f.process(im,g,c,s,opt);
        demand(f.history_validity().at(4,4)==0,"explicit camera cut");
        s.svgf=true; f.process(im,g,c,s); demand(f.history_validity().at(4,4)==0,"mode switch");
        c.fov=90; g=plane(9,9,c); f.process(im,g,c,s); demand(f.history_validity().at(4,4)==0,"automatic lens cut");
        im.reset(5,7,glm::vec3(1)); g=plane(5,7,c); f.process(im,g,c,s);
        demand(f.history_length().width==5 && f.history_length().height==7 && f.history_length().at(2,3)==1,"resize history");
        s.taa=false; s.svgf=false; f.process(im,g,c,s);
        demand(f.history_length().at(2,3)==0,"disabled filter retains history");
        return "explicit cut, lens cut, mode switch, resize, disable reset checked";
    });
    run(results,"postprocess/hw5_spatial_temporal_noise_reduction",[] {
        auto c=test_camera(); auto g=plane(25,25,c); Settings s; s.denoise=true; s.temporal_weight=0.1f;
        TemporalFilter f; Image<glm::vec3> out; double raw_error=0;
        for (int frame=0;frame<12;++frame) { const auto input=noise(25,25,frame); raw_error+=mse(input,1,4); out=f.process(input,g,c,s); }
        const double final_error=mse(out,1,4),ratio=final_error/(raw_error/12);
        demand(ratio<0.06,"HW5 should suppress noise through both stages: "+value("MSE ratio",ratio));
        demand(f.history_length().at(12,12)==12,"HW5 history tracking");
        demand(finite_image(out),"HW5 finite output");
        return value("12-frame MSE/raw ratio",ratio);
    });
    run(results,"postprocess/svgf_constant_and_variance",[] {
        auto c=test_camera(); auto g=plane(17,17,c); Settings s; s.svgf=true; TemporalFilter f;
        Image<glm::vec3> im(17,17,glm::vec3(0.5f));
        for (int frame=0;frame<8;++frame) {
            const auto out=f.process(im,g,c,s); demand(max_error(out,glm::vec3(0.5f))<1e-5f,"SVGF constant signal");
        }
        const auto moments=f.moments().at(8,8);
        demand(std::abs(moments.x-0.5f)<1e-6f && std::abs(moments.y-0.25f)<1e-6f,"SVGF moments do not measure luminance");
        demand(f.variance().at(8,8)<1e-7f,"SVGF DC variance not zero");
        return value("m1",moments.x)+", "+value("m2",moments.y);
    });
    run(results,"postprocess/svgf_noise_moments_atrous_and_edge",[] {
        auto c=test_camera(); auto g=plane(33,33,c); Settings s; s.svgf=true; s.temporal_weight=0.1f;
        TemporalFilter f; Image<glm::vec3> output; double raw=0;
        for (int frame=0;frame<10;++frame) { const auto im=noise(33,33,frame); raw+=mse(im,1,4); output=f.process(im,g,c,s); }
        const double ratio=mse(output,1,4)/(raw/10);
        const auto m=f.moments().at(16,16); const float temporal_var=m.y-m.x*m.x;
        demand(ratio<0.04,"SVGF noise reduction inadequate: "+value("MSE ratio",ratio));
        demand(temporal_var>0.005f,"no meaningful temporal second moment");
        demand(f.variance().at(16,16)<temporal_var,"a-trous variance was not propagated");
        demand(finite_image(f.variance()) && finite_image(output),"SVGF nonfinite output");
        f.reset(); Image<glm::vec3> edge(33,33,glm::vec3(0.1f));
        for (int y=0;y<33;++y) for (int x=17;x<33;++x) { edge.at(x,y)=glm::vec3(5); g.at(x,y).object=9; }
        output=f.process(edge,g,c,s);
        demand(std::abs(output.at(16,16).x-0.1f)<1e-5f && std::abs(output.at(17,16).x-5)<1e-4f,"a-trous leaks across edge");
        return value("10-frame MSE ratio",ratio)+", "+value("temporal variance",temporal_var);
    });
    run(results,"postprocess/svgf_short_history_bootstrap",[] {
        auto c=test_camera(); auto g=plane(17,17,c); Settings s; s.svgf=true; TemporalFilter f;
        const auto im=noise(17,17,3); const auto out=f.process(im,g,c,s);
        const auto m=f.moments().at(8,8);
        demand(std::abs(m.y-m.x*m.x)<1e-6f,"first sample moments inconsistent");
        demand(f.variance().at(8,8)>0,"short history requires spatial variance bootstrap");
        demand(mse(out,1,3)<mse(im,1,3)*0.1,"first-frame variance must drive spatial denoising");
        return value("first-frame propagated variance",f.variance().at(8,8));
    });
    run(results,"postprocess/bloom_additive_disabled_threshold_dc",[] {
        Settings s; s.bloom=false; Image<glm::vec3> im(19,13,glm::vec3(4,2,1));
        demand(max_error(bloom(im,s),glm::vec3(0))==0,"disabled bloom must return black additive image");
        s.bloom=true; s.bloom_strength=0.25f; s.bloom_threshold=4;
        demand(max_error(bloom(im,s),glm::vec3(0))==0,"threshold extraction");
        s.bloom_threshold=2; auto out=bloom(im,s);
        const float e=max_error(out,glm::vec3(0.5f,0.25f,0.125f));
        demand(e<1e-5f,"bloom DC/hue must not depend on pyramid levels: "+value("error",e));
        im.reset(1,1,glm::vec3(4,2,1)); demand(max_error(bloom(im,s),{0.5f,0.25f,0.125f})<1e-6f,"singleton bloom");
        return value("DC error",e)+"; black when disabled/under threshold; hue preserved";
    });
    run(results,"postprocess/bloom_multilevel_spread_energy_linearity",[] {
        Image<glm::vec3> impulse(64,64,glm::vec3(0)); impulse.at(32,32)=glm::vec3(16,8,4);
        Settings s; s.bloom=true; s.bloom_strength=0.5f; s.bloom_threshold=0;
        const auto a=bloom(impulse,s); const double ratio=energy(a)/8.0;
        demand(a.at(32,32).x>0 && a.at(32,32).x<8,"blur should distribute impulse peak");
        demand(a.at(42,32).x>1e-5f,"coarse levels must spread beyond 5-tap support");
        demand(ratio>0.75 && ratio<1.2,"normalized pyramid energy bound: "+value("ratio",ratio));
        s.bloom_strength=1; const auto b=bloom(impulse,s);
        float error=0; for (std::size_t i=0;i<a.pixels.size();++i) error=std::max(error,glm::length(b.pixels[i]-2.0f*a.pixels[i]));
        demand(error<1e-5f,"bloom strength must be linear");
        demand(std::abs(a.at(42,32).y/a.at(42,32).x-0.5f)<1e-5f,"spread hue changes");
        return value("total flux/input extracted flux",ratio)+", "+value("10px spread",a.at(42,32).x);
    });
    run(results,"postprocess/npr_outline_hatching_toon",[] {
        auto c=test_camera(); auto g=plane(24,24,c); Image<glm::vec3> im(24,24,glm::vec3(0.3f)); Settings s;
        demand(max_error(apply_npr(im,g,s),glm::vec3(0.3f))<1e-7f,"NPR disabled should preserve HDR");
        s.outline=true; demand(max_error(apply_npr(im,g,s),glm::vec3(0.3f))<1e-7f,"flat surface falsely outlined");
        for (int y=0;y<24;++y) for (int x=12;x<24;++x) g.at(x,y).object=2;
        auto out=apply_npr(im,g,s);
        demand(out.at(11,12).x<0.03f && std::abs(out.at(8,12).x-0.3f)<1e-6f,"outline edge/interior incorrect");
        s.outline=false; s.hatching=true; out=apply_npr(im,g,s); int ink=0,clear=0;
        for (const auto& p:out.pixels) { if (p.x<0.1f) ++ink; if (p.x>0.29f) ++clear; }
        demand(ink>24 && clear>240,"hatching must have lines and untouched gaps");
        im.reset(24,24,glm::vec3(1)); demand(max_error(apply_npr(im,g,s),glm::vec3(1))==0,"bright region should not hatch");
        s.hatching=false; s.shading=ShadingMode::toon; im.reset(24,24,glm::vec3(0.3f));
        demand(max_error(apply_npr(im,g,s),glm::vec3(0.32f))<1e-5f,"toon luminance band");
        return "object boundary darkened; interior unchanged; "+std::to_string(ink)+" hatch pixels; toon band .32";
    });
    run(results,"postprocess/nonfinite_and_empty_robustness",[] {
        auto c=test_camera(); auto g=plane(7,7,c); Image<glm::vec3> im(7,7,glm::vec3(1));
        const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
        im.at(1,1)={nan,inf,-1}; im.at(2,2)=glm::vec3(1e30f);
        g.at(3,3).linear_depth=nan; g.at(4,4).normal={nan,0,0};
        Settings s; s.svgf=true; s.ao=AoMode::gtao; s.outline=true; s.hatching=true; TemporalFilter f;
        auto out=f.process(im,g,c,s); out=f.process(im,g,c,s);
        demand(finite_image(out) && finite_image(f.variance()),"temporal nonfinite output");
        demand(finite_image(bloom(im,s)) && finite_image(apply_npr(im,g,s)),"bloom/NPR nonfinite output");
        demand(finite_image(ambient_occlusion(g,c,s)),"AO invalid geometry");
        Image<glm::vec3> empty; GBuffer eg;
        demand(f.process(empty,eg,c,s).empty() && bloom(empty,s).empty() && apply_npr(empty,eg,s).empty(),"empty image behavior");
        demand(ambient_occlusion(eg,c,s).empty(),"empty AO behavior");
        return "NaN/Inf colors, huge HDR, invalid geometry and empty images remain finite/safe";
    });
    run(results,"postprocess/extent_validation",[] {
        auto c=test_camera(); auto g=plane(3,3,c); Image<glm::vec3> im(4,3,glm::vec3(1)); Settings s; s.taa=true;
        int rejected=0; TemporalFilter f;
        try { f.process(im,g,c,s); } catch (const std::invalid_argument&) { ++rejected; }
        try { joint_bilateral_filter(im,g); } catch (const std::invalid_argument&) { ++rejected; }
        try { apply_npr(im,g,s); } catch (const std::invalid_argument&) { ++rejected; }
        im.pixels.pop_back(); try { bloom(im,s); } catch (const std::invalid_argument&) { ++rejected; }
        demand(rejected==4,"dimensions must be checked before indexing");
        return "four malformed/mismatched extent cases threw invalid_argument";
    });
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_POSTPROCESS_TEST_MAIN
int main() {
    const auto tests=emberframe::lab::test_postprocess(); int failures=0;
    for (const auto& test:tests) {
        std::cout<<(test.passed ? "PASS " : "FAIL ")<<test.name<<": "<<test.detail<<'\n';
        if (!test.passed) ++failures;
    }
    std::cout<<tests.size()-std::size_t(failures)<<'/'<<tests.size()<<" passed\n";
    return failures ? 1:0;
}
#endif
