#include "postprocess.h"
#include <cmath>
#include <utility>

namespace emberframe::lab {
namespace {
bool finite(glm::vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
bool finite(glm::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
float parameter(float x, float fallback) { return std::isfinite(x) ? x : fallback; }
glm::vec3 clean(glm::vec3 v) {
    for (int k=0;k<3;++k) v[k]=std::isfinite(v[k]) ? std::clamp(v[k],0.0f,1e10f) : 0.0f;
    return v;
}
float luminance(glm::vec3 v) { return glm::dot(v,glm::vec3(0.2126f,0.7152f,0.0722f)); }
template<class T> void check(const Image<T>& im) {
    if (im.width<0 || im.height<0 ||
        std::uint64_t(im.width)*std::uint64_t(im.height)!=im.pixels.size())
        throw std::invalid_argument("postprocess: malformed image extent");
}
template<class T, class U> void same_size(const Image<T>& a, const Image<U>& b) {
    check(a); check(b);
    if (a.width!=b.width || a.height!=b.height)
        throw std::invalid_argument("postprocess: image/G-buffer extent mismatch");
}
Image<glm::vec3> sanitized(const Image<glm::vec3>& im) {
    check(im); Image<glm::vec3> out=im;
    for (auto& c:out.pixels) c=clean(c);
    return out;
}
bool surface(const SurfaceSample& s) {
    return s.valid && s.linear_depth>0 && std::isfinite(s.linear_depth) &&
        finite(s.position) && finite(s.normal) && std::isfinite(glm::dot(s.normal,s.normal)) &&
        glm::dot(s.normal,s.normal)>1e-16f;
}
bool inside(int x,int y,int w,int h) { return x>=0 && y>=0 && x<w && y<h; }
glm::vec3 normal(const SurfaceSample& s) { return safe_normalize(s.normal,{0,0,1}); }
bool same_surface(const SurfaceSample& a,const SurfaceSample& b) {
    return surface(a) && surface(b) && a.object==b.object && a.material==b.material;
}
void check_camera(const Camera& c) {
    if (!finite(c.position) || !finite(c.target) || !std::isfinite(c.fov) ||
        c.fov<=0 || c.fov>=179 || !std::isfinite(c.near_plane) ||
        !std::isfinite(c.far_plane) || c.near_plane<=0 || c.far_plane<=c.near_plane ||
        glm::length(c.target-c.position)<1e-6f ||
        glm::length(glm::cross(safe_normalize(c.target-c.position),glm::vec3(0,1,0)))<1e-5f)
        throw std::invalid_argument("postprocess: degenerate camera");
}
glm::vec2 jitter_pixels(glm::vec2 j,int w,int h) { return j*glm::vec2(w*0.5f,-h*0.5f); }
bool project(glm::vec3 p,const glm::mat4& vp,int w,int h,glm::vec2& pixel) {
    const glm::vec4 clip=vp*glm::vec4(p,1);
    if (!std::isfinite(clip.w) || clip.w<=1e-6f) return false;
    const glm::vec3 ndc=glm::vec3(clip)/clip.w;
    if (!finite(ndc) || ndc.z<0 || ndc.z>1) return false;
    pixel={(ndc.x*0.5f+0.5f)*w-0.5f,(0.5f-ndc.y*0.5f)*h-0.5f};
    return finite(pixel);
}
float radical_inverse(std::uint64_t n,std::uint64_t base) {
    double factor=1.0, value=0.0;
    while (n) { factor/=double(base); value+=double(n%base)*factor; n/=base; }
    return float(value);
}
float rotation(int x,int y,std::uint32_t seed) {
    std::uint32_t n=std::uint32_t(x)*1973u+std::uint32_t(y)*9277u+seed*26699u;
    n=(n^(n>>16u))*0x7feb352du; n=(n^(n>>15u))*0x846ca68bu; n^=n>>16u;
    return float(n&0xffffffu)/16777216.0f;
}
float geometry_weight(const SurfaceSample& a,const SurfaceSample& b) {
    if (!same_surface(a,b)) return 0;
    const glm::vec3 na=normal(a), nb=normal(b), delta=b.position-a.position;
    const float nd=std::max(0.0f,glm::dot(na,nb));
    if (nd<0.75f) return 0;
    const float depth_scale=std::max(0.005f,0.025f*a.linear_depth);
    const float plane=std::max(std::abs(glm::dot(delta,na)),std::abs(glm::dot(delta,nb)));
    const float dd=std::abs(a.linear_depth-b.linear_depth);
    const glm::vec3 da=clean(a.albedo)-clean(b.albedo);
    return std::pow(nd,32.0f)*std::exp(-plane/depth_scale-dd/(4*depth_scale)
                                    -glm::dot(da,da)/0.08f);
}
glm::vec3 to_ycocg(glm::vec3 c) {
    return {0.25f*c.r+0.5f*c.g+0.25f*c.b,0.5f*c.r-0.5f*c.b,
            -0.25f*c.r+0.5f*c.g-0.25f*c.b};
}
glm::vec3 from_ycocg(glm::vec3 c) { return {c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z}; }
glm::vec3 clamp_history(glm::vec3 old,const Image<glm::vec3>& image,const GBuffer& g,
                       int x,int y,float gamma) {
    glm::vec3 low=to_ycocg(image.at(x,y)),high=low,mean(0),square(0);
    float count=0;
    for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
        const int xx=x+dx,yy=y+dy;
        if (!inside(xx,yy,image.width,image.height) || geometry_weight(g.at(x,y),g.at(xx,yy))<0.05f) continue;
        const glm::vec3 c=to_ycocg(image.at(xx,yy));
        low=glm::min(low,c); high=glm::max(high,c); mean+=c; square+=c*c; ++count;
    }
    if (count==0) return image.at(x,y);
    mean/=count;
    const glm::vec3 sigma=glm::sqrt(glm::max(square/count-mean*mean,glm::vec3(0)));
    // 在亮度/色度空间用邻域包围盒与统计区间的交集限制历史，抑制拖影与颜色串扰。
    low=glm::max(low,mean-gamma*sigma); high=glm::min(high,mean+gamma*sigma);
    return clean(from_ycocg(glm::clamp(to_ycocg(old),low,high)));
}
float spatial_variance(const Image<glm::vec3>& im,const GBuffer& g,int x,int y) {
    double sum=0,m1=0,m2=0;
    for (int dy=-2;dy<=2;++dy) for (int dx=-2;dx<=2;++dx) {
        const int xx=x+dx,yy=y+dy;
        if (!inside(xx,yy,im.width,im.height)) continue;
        const double w=geometry_weight(g.at(x,y),g.at(xx,yy))*std::exp(-double(dx*dx+dy*dy)/4.0);
        const double l=luminance(im.at(xx,yy)); sum+=w; m1+=w*l; m2+=w*l*l;
    }
    return sum>0 ? float(std::max(0.0,m2/sum-(m1/sum)*(m1/sum))) : 0;
}
Image<glm::vec3> atrous(Image<glm::vec3> input,const GBuffer& g,Image<float>& variance) {
    constexpr float kernel[5]={1.0f/16,4.0f/16,6.0f/16,4.0f/16,1.0f/16};
    for (int level=0;level<4;++level) {
        const int step=1<<level;
        Image<glm::vec3> output=input;
        Image<float> propagated=variance;
        for (int y=0;y<input.height;++y) for (int x=0;x<input.width;++x) {
            if (!surface(g.at(x,y))) continue;
            // 先平滑方差以避免单个偶然的零方差把当前像素永久冻结。
            float vblur=0,vweight=0;
            for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
                const int xx=x+dx,yy=y+dy;
                if (!inside(xx,yy,input.width,input.height)) continue;
                const float gw=geometry_weight(g.at(x,y),g.at(xx,yy));
                vblur+=gw*variance.at(xx,yy); vweight+=gw;
            }
            const float center_l=luminance(input.at(x,y));
            const float scale=4*std::sqrt(std::max(0.0f,vblur/std::max(vweight,1e-8f)))
                              +0.001f*std::max(1.0f,center_l);
            glm::vec3 sum(0); float sumw=0; double varsum=0;
            for (int ky=-2;ky<=2;++ky) for (int kx=-2;kx<=2;++kx) {
                const int xx=x+kx*step,yy=y+ky*step;
                if (!inside(xx,yy,input.width,input.height)) continue;
                const float w=kernel[kx+2]*kernel[ky+2]*geometry_weight(g.at(x,y),g.at(xx,yy))
                    *std::exp(-std::abs(luminance(input.at(xx,yy))-center_l)/scale);
                sum+=input.at(xx,yy)*w; sumw+=w; varsum+=double(w)*w*variance.at(xx,yy);
            }
            if (sumw>0) {
                output.at(x,y)=clean(sum/sumw);
                // 独立样本线性组合的方差为 Σ(w²σ²)/(Σw)²；相关性使其仍为近似。
                propagated.at(x,y)=float(varsum/(double(sumw)*sumw));
            }
        }
        input=std::move(output); variance=std::move(propagated);
    }
    return input;
}
Image<glm::vec3> gaussian(const Image<glm::vec3>& im) {
    constexpr float k[5]={1.0f/16,4.0f/16,6.0f/16,4.0f/16,1.0f/16};
    Image<glm::vec3> tmp(im.width,im.height,glm::vec3(0)),out(im.width,im.height,glm::vec3(0));
    for (int y=0;y<im.height;++y) for (int x=0;x<im.width;++x)
        for (int i=-2;i<=2;++i) tmp.at(x,y)+=im.clamped(x+i,y)*k[i+2];
    for (int y=0;y<im.height;++y) for (int x=0;x<im.width;++x)
        for (int i=-2;i<=2;++i) out.at(x,y)+=tmp.clamped(x,y+i)*k[i+2];
    return out;
}
glm::vec3 bilinear(const Image<glm::vec3>& im,float x,float y) {
    const int ix=int(std::floor(x)),iy=int(std::floor(y)); const float fx=x-ix,fy=y-iy;
    return glm::mix(glm::mix(im.clamped(ix,iy),im.clamped(ix+1,iy),fx),
                    glm::mix(im.clamped(ix,iy+1),im.clamped(ix+1,iy+1),fx),fy);
}
// GTAO: Jimenez et al., ATVI-TR-16-01, equations 5--8.
// https://www.activision.com/cdn/research/PracticalRealtimeStrategiesTRfinal.pdf
float horizon_integral(float a,float b,float negative_horizon,float positive_horizon) {
    const float gamma=std::atan2(b,a);
    const float lo=std::max(-negative_horizon,gamma-pi*0.5f);
    const float hi=std::min(positive_horizon,gamma+pi*0.5f);
    if (lo>=hi) return 0;
    // a=N·V, b=N·T 已包含投影法线的长度。被积函数是
    // (a cosθ+b sinθ)|sinθ|，必须在 θ=0 处分段，不能只平均地平线角。
    const auto F=[&](float t) { const float s=std::sin(t);
        return 0.5f*a*s*s+b*(0.5f*t-0.25f*std::sin(2*t)); };
    float value=0;
    if (lo<0) value+=F(lo)-F(std::min(hi,0.0f));
    if (hi>0) value+=F(hi)-F(std::max(lo,0.0f));
    return std::max(0.0f,value);
}
} // namespace

glm::vec2 taa_jitter(std::uint64_t frame,int width,int height) {
    if (width<=0 || height<=0) return glm::vec2(0);
    // Avoid overflow at UINT64_MAX while leaving the ordinary sequence unchanged.
    const std::uint64_t index=frame==std::numeric_limits<std::uint64_t>::max() ? 1 : frame+1;
    return {(radical_inverse(index,2)-0.5f)*2/width,
            (radical_inverse(index,3)-0.5f)*2/height};
}

Image<float> ambient_occlusion(const GBuffer& g,const Camera& camera,const Settings& s) {
    check(g); Image<float> out(g.width,g.height,1);
    const float radius=std::max(0.0f,parameter(s.ao_radius,0.5f));
    const float strength=std::clamp(parameter(s.ao_strength,1),0.0f,8.0f);
    if (g.empty() || s.ao==AoMode::none || radius<1e-6f || strength==0) return out;
    check_camera(camera);
    const glm::mat4 view=camera.view(),vp=camera.projection(float(g.width)/g.height)*view;
    const glm::vec3 right=glm::vec3(glm::inverse(view)[0]);
    const float focal=0.5f*g.height/std::tan(glm::radians(camera.fov)*0.5f);
    const float bias=std::max(1e-4f,radius*0.003f);
    for (int y=0;y<g.height;++y) for (int x=0;x<g.width;++x) {
        const auto& center=g.at(x,y); if (!surface(center)) continue;
        const glm::vec3 n=normal(center);
        float visibility=1;
        if (s.ao==AoMode::ssao) {
            const int count=std::clamp(s.samples,8,128);
            const glm::vec3 tangent=safe_normalize(glm::cross(std::abs(n.z)<0.9f ? glm::vec3(0,0,1) : glm::vec3(0,1,0),n));
            const glm::vec3 bitangent=glm::cross(n,tangent);
            float blocked=0;
            const float spin=rotation(x,y,s.seed)*2*pi;
            for (int i=0;i<count;++i) {
                const float u=(float(i)+0.5f)/count,phi=2*pi*radical_inverse(std::uint64_t(i+1),2)+spin;
                // 余弦半球 PDF=cosθ/π，因此每个可见性样本具有相同权重。
                const glm::vec3 direction=tangent*(std::sqrt(u)*std::cos(phi))
                    +bitangent*(std::sqrt(u)*std::sin(phi))+n*std::sqrt(1-u);
                const float distance=radius*(0.15f+0.85f*radical_inverse(std::uint64_t(i+1),3));
                const glm::vec3 probe=center.position+n*bias+direction*distance;
                glm::vec2 pixel;
                if (!project(probe,vp,g.width,g.height,pixel)) continue;
                if (pixel.x< -0.5f || pixel.y< -0.5f || pixel.x>=g.width-0.5f || pixel.y>=g.height-0.5f) continue;
                const int xx=int(std::floor(pixel.x+0.5f)),yy=int(std::floor(pixel.y+0.5f));
                if (!inside(xx,yy,g.width,g.height)) continue;
                const auto& q=g.at(xx,yy); if (!surface(q)) continue;
                const glm::vec3 delta=q.position-center.position;
                const float d=glm::length(delta),probe_depth=-(view*glm::vec4(probe,1)).z;
                if (d<radius && glm::dot(delta,n)>bias && q.linear_depth<probe_depth-bias) {
                    const float edge=std::clamp((radius-d)/(0.2f*radius),0.0f,1.0f);
                    blocked+=edge*edge*(3-2*edge);
                }
            }
            visibility=1-blocked/count;
        } else {
            const int slices=std::clamp(s.samples,2,16),steps=std::clamp(s.samples,4,32);
            const glm::vec3 v=safe_normalize(camera.position-center.position,{0,0,1});
            const glm::vec3 basis_t=safe_normalize(right-v*glm::dot(right,v),{1,0,0});
            const glm::vec3 basis_b=glm::cross(v,basis_t);
            const glm::vec4 clip_center=vp*glm::vec4(center.position,1);
            if (!std::isfinite(clip_center.w) || clip_center.w<=1e-6f) continue;
            const float radius_pixels=std::clamp(focal*radius/center.linear_depth,1.0f,96.0f);
            visibility=0;
            for (int slice=0;slice<slices;++slice) {
                const float phi=pi*(slice+rotation(x,y,s.seed))/slices;
                // φ 必须绕当前像素的视线均匀分布。直接均匀取屏幕角度会在
                // 广角图像边缘改变方位角 PDF，使无遮挡平面也系统性变暗。
                const glm::vec3 t=basis_t*std::cos(phi)+basis_b*std::sin(phi);
                const glm::vec4 clip_t=vp*glm::vec4(t,0);
                const glm::vec2 projected_t=glm::vec2(clip_t)*clip_center.w-glm::vec2(clip_center)*clip_t.w;
                const glm::vec2 screen_dir=glm::normalize(projected_t*glm::vec2(float(g.width),-float(g.height)));
                float horizons[2]={pi,pi};
                for (int side=0;side<2;++side) for (int k=0;k<steps;++k) {
                    const float u=float(k)/std::max(1,steps-1),r=1+(radius_pixels-1)*u*u;
                    const glm::vec2 p=glm::vec2(x,y)+screen_dir*r*(side==0 ? -1.0f : 1.0f);
                    const int xx=int(std::floor(p.x+0.5f)),yy=int(std::floor(p.y+0.5f));
                    if (!inside(xx,yy,g.width,g.height) || (xx==x && yy==y)) continue;
                    const auto& q=g.at(xx,yy); if (!surface(q)) continue;
                    const glm::vec3 delta=q.position-center.position;
                    const float d=glm::length(delta);
                    if (d<=bias || d>radius || glm::dot(delta,n)<=bias) continue;
                    const float angle=std::acos(std::clamp(glm::dot(delta/d,v),-1.0f,1.0f));
                    horizons[side]=std::min(horizons[side],angle);
                }
                visibility+=horizon_integral(glm::dot(n,v),glm::dot(n,t),horizons[0],horizons[1]);
            }
            visibility/=slices;
        }
        out.at(x,y)=std::clamp(1-strength*(1-visibility),0.0f,1.0f);
    }
    return out;
}

Image<glm::vec3> joint_bilateral_filter(const Image<glm::vec3>& input,const GBuffer& g,int radius) {
    same_size(input,g); auto out=sanitized(input); const auto source=out;
    radius=std::clamp(radius,0,8); if (radius==0) return out;
    const float sigma=std::max(1.0f,radius*0.75f);
    for (int y=0;y<input.height;++y) for (int x=0;x<input.width;++x) {
        if (!surface(g.at(x,y))) continue;
        glm::vec3 sum(0); float sumw=0;
        for (int dy=-radius;dy<=radius;++dy) for (int dx=-radius;dx<=radius;++dx) {
            const int xx=x+dx,yy=y+dy;
            if (!inside(xx,yy,input.width,input.height)) continue;
            const float w=std::exp(-float(dx*dx+dy*dy)/(2*sigma*sigma))*geometry_weight(g.at(x,y),g.at(xx,yy));
            sum+=source.at(xx,yy)*w; sumw+=w;
        }
        if (sumw>0) out.at(x,y)=clean(sum/sumw);
    }
    return out;
}

void TemporalFilter::reset() {
    history_={}; previous_={}; moments_={}; variance_={}; valid_={}; length_={};
    previous_jitter_=glm::vec2(0); previous_mode_=-1; ready_=false;
}

Image<glm::vec3> TemporalFilter::process(const Image<glm::vec3>& current,const GBuffer& g,
                                      const Camera& camera,const Settings& s) {
    return process(current,g,camera,s,TemporalInput{});
}

Image<glm::vec3> TemporalFilter::process(const Image<glm::vec3>& current,const GBuffer& g,
                                      const Camera& camera,const Settings& s,const TemporalInput& options) {
    same_size(current,g);
    if (options.previous_world_positions) same_size(current,*options.previous_world_positions);
    if (!finite(options.jitter_ndc)) throw std::invalid_argument("postprocess: nonfinite jitter");
    if (current.empty()) { reset(); return sanitized(current); }
    const int mode=(s.taa ? 1:0)|(s.denoise && !s.svgf ? 2:0)|(s.svgf ? 4:0);
    const auto raw=sanitized(current);
    if (!mode) {
        reset(); variance_.reset(current.width,current.height); valid_=variance_; length_=variance_;
        return raw;
    }
    check_camera(camera);
    bool cut=options.camera_cut;
    if (ready_) {
        const float scene_scale=std::max(1.0f,glm::length(previous_camera_.target-previous_camera_.position));
        cut=cut || glm::length(camera.position-previous_camera_.position)>scene_scale*0.5f
            || glm::dot(safe_normalize(camera.target-camera.position),
                        safe_normalize(previous_camera_.target-previous_camera_.position))<0.7071067f
            || std::abs(camera.fov-previous_camera_.fov)>5.0f
            || camera.near_plane!=previous_camera_.near_plane || camera.far_plane!=previous_camera_.far_plane;
    }
    if (cut || history_.width!=current.width || history_.height!=current.height ||
        mode!=previous_mode_ || options.motion!=previous_motion_) reset();
    const int w=current.width,h=current.height;
    const auto spatial=s.denoise && !s.svgf ? joint_bilateral_filter(raw,g) : raw;
    Image<glm::vec3> temporal=spatial;
    Image<glm::vec2> next_moments(w,h);
    Image<float> next_length(w,h),next_variance(w,h),next_valid(w,h);
    const glm::mat4 prev_view=ready_ ? previous_camera_.view() : camera.view();
    const glm::mat4 prev_vp=(ready_ ? previous_camera_ : camera).projection(float(w)/h)*prev_view;
    const glm::vec2 prev_jitter=jitter_pixels(previous_jitter_,w,h),cur_jitter=jitter_pixels(options.jitter_ndc,w,h);
    const float user_alpha=std::clamp(parameter(s.temporal_weight,0.1f),0.0f,1.0f);
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        const auto& center=g.at(x,y);
        const float l=luminance(raw.at(x,y));
        glm::vec2 old_moments(0); glm::vec3 old_color(0); float weight=0,old_length=0;
        glm::vec3 expected=center.position;
        if (options.previous_world_positions) expected=options.previous_world_positions->at(x,y);
        if (ready_ && surface(center) && finite(center.motion) && finite(expected)) {
            glm::vec2 coord(0);
            bool projected=true;
            const bool zero_fallback=options.motion==MotionConvention::current_minus_previous_pixels
                && center.motion.x==0 && center.motion.y==0;
            if (options.motion==MotionConvention::world_plus_object_pixels || zero_fallback) {
                projected=project(center.position,prev_vp,w,h,coord);
                coord+=center.motion+prev_jitter;
            } else {
                glm::vec2 displacement=center.motion;
                if (options.motion==MotionConvention::backward_ndc) displacement=jitter_pixels(center.motion,w,h);
                if (options.motion==MotionConvention::current_minus_previous_pixels ||
                    options.motion==MotionConvention::current_minus_previous_pixels_explicit) displacement=-center.motion;
                coord=glm::vec2(x,y)-cur_jitter+displacement+prev_jitter;
            }
            const float expected_depth=-(prev_view*glm::vec4(expected,1)).z;
            if (projected && finite(coord) && coord.x>=-1e-4f && coord.y>=-1e-4f &&
                coord.x<=w-1+1e-4f && coord.y<=h-1+1e-4f && expected_depth>0) {
                // 矩阵往返的浮点误差不应让静止图像的第一行/列失去历史。
                coord=glm::clamp(coord,glm::vec2(0),glm::vec2(w-1,h-1));
                const int ix=int(std::floor(coord.x)),iy=int(std::floor(coord.y));
                const float fx=coord.x-ix,fy=coord.y-iy;
                for (int dy=0;dy<2;++dy) for (int dx=0;dx<2;++dx) {
                    const int xx=ix+dx,yy=iy+dy;
                    const float bw=(dx ? fx:1-fx)*(dy ? fy:1-fy);
                    if (bw<=0 || !inside(xx,yy,w,h)) continue;
                    const auto& old=previous_.at(xx,yy);
                    const float tolerance=std::max(0.01f,0.02f*expected_depth);
                    // 对四个双线性采样点逐一验证；先插值深度会掩盖边缘的反遮挡。
                    if (!same_surface(center,old) || glm::dot(normal(center),normal(old))<0.9f ||
                        std::abs(old.linear_depth-expected_depth)>tolerance ||
                        std::abs(glm::dot(old.position-expected,normal(old)))>tolerance ||
                        glm::length(clean(old.albedo)-clean(center.albedo))>0.3f) continue;
                    old_color+=history_.at(xx,yy)*bw; old_moments+=moments_.at(xx,yy)*bw;
                    old_length+=length_.at(xx,yy)*bw; weight+=bw;
                }
            }
        }
        float alpha=1;
        next_length.at(x,y)=surface(center) ? 1.0f : 0.0f;
        if (weight>=0.25f && old_length>0) {
            old_color/=weight; old_moments/=weight; old_length/=weight;
            const glm::vec3 clipped=clamp_history(old_color,spatial,g,x,y,s.svgf ? 3.0f : (s.denoise ? 2.0f : 1.5f));
            const float clipped_l=luminance(clipped);
            // 历史颜色被钳制后，将一阶矩平移到新亮度，同时保留非负中心方差。
            const float old_var=std::max(0.0f,old_moments.y-old_moments.x*old_moments.x);
            old_moments={clipped_l,clipped_l*clipped_l+old_var};
            next_length.at(x,y)=std::min(64.0f,old_length+1);
            alpha=s.svgf ? std::max(user_alpha,1.0f/next_length.at(x,y)) : user_alpha;
            temporal.at(x,y)=clean(glm::mix(clipped,spatial.at(x,y),alpha));
            next_valid.at(x,y)=weight;
        }
        // 一二阶矩用同一 alpha，方差来自 E[L²]-E[L]²，不是伪造噪声常量。
        next_moments.at(x,y)=glm::mix(old_moments,glm::vec2(l,l*l),alpha);
        const auto m=next_moments.at(x,y);
        float var=std::max(0.0f,m.y-m.x*m.x);
        if (s.svgf && surface(center) && next_length.at(x,y)<4) {
            const float bootstrap=spatial_variance(raw,g,x,y);
            var=glm::mix(bootstrap,var,std::max(0.0f,(next_length.at(x,y)-1)/3));
        }
        next_variance.at(x,y)=surface(center) ? var : 0;
    }
    history_=temporal; previous_=g; moments_=std::move(next_moments);
    length_=std::move(next_length); valid_=std::move(next_valid); variance_=std::move(next_variance);
    previous_camera_=camera; previous_jitter_=options.jitter_ndc; previous_motion_=options.motion;
    previous_mode_=mode; ready_=true;
    // SVGF reference: Schied et al. 2017; four finite CPU wavelet levels.
    // https://research.nvidia.com/labs/rtr/publication/schied2017spatiotemporal/
    return s.svgf ? atrous(std::move(temporal),g,variance_) : temporal;
}

Image<glm::vec3> bloom(const Image<glm::vec3>& hdr,const Settings& s) {
    const auto source=sanitized(hdr);
    Image<glm::vec3> output(hdr.width,hdr.height,glm::vec3(0));
    const float strength=std::clamp(parameter(s.bloom_strength,0.08f),0.0f,64.0f);
    if (!s.bloom || hdr.empty() || strength==0) return output;
    const float threshold=std::max(0.0f,parameter(s.bloom_threshold,1));
    Image<glm::vec3> bright(hdr.width,hdr.height);
    for (std::size_t i=0;i<bright.pixels.size();++i) {
        const auto c=source.pixels[i]; const float peak=std::max({c.r,c.g,c.b});
        bright.pixels[i]=c*(std::max(0.0f,peak-threshold)/std::max(peak,1e-8f));
    }
    std::vector<Image<glm::vec3>> pyramid; pyramid.push_back(gaussian(bright));
    while (pyramid.size()<6 && (pyramid.back().width>1 || pyramid.back().height>1)) {
        const auto& prev=pyramid.back(); Image<glm::vec3> down((prev.width+1)/2,(prev.height+1)/2);
        for (int y=0;y<down.height;++y) for (int x=0;x<down.width;++x) {
            glm::vec3 sum(0); float count=0;
            for (int dy=0;dy<2;++dy) for (int dx=0;dx<2;++dx)
                if (inside(x*2+dx,y*2+dy,prev.width,prev.height)) { sum+=prev.at(x*2+dx,y*2+dy); ++count; }
            down.at(x,y)=sum/count;
        }
        pyramid.push_back(gaussian(down));
    }
    // 每层核归一化，再以 1/层数混合，保证常量亮场不会随 mip 数量无故增亮。
    const float scale=strength/float(pyramid.size());
    for (const auto& level:pyramid) for (int y=0;y<hdr.height;++y) for (int x=0;x<hdr.width;++x) {
        const float xx=(x+0.5f)*level.width/hdr.width-0.5f,yy=(y+0.5f)*level.height/hdr.height-0.5f;
        output.at(x,y)+=bilinear(level,xx,yy)*scale;
    }
    for (auto& c:output.pixels) c=clean(c);
    return output;
}

Image<glm::vec3> apply_npr(const Image<glm::vec3>& input,const GBuffer& g,const Settings& s) {
    same_size(input,g); auto out=sanitized(input);
    for (int y=0;y<input.height;++y) for (int x=0;x<input.width;++x) {
        const auto& center=g.at(x,y); if (!surface(center)) continue;
        glm::vec3 c=out.at(x,y); const float l=luminance(c);
        if (s.shading==ShadingMode::toon && l>1e-8f) {
            const float band=l<0.2f ? 0.12f : l<0.45f ? 0.32f : l<0.75f ? 0.62f : 1.0f;
            c*=band/l;
        }
        if (s.hatching) {
            const float darkness=1-std::clamp(l,0.0f,1.0f);
            const int p1=(x+y)%8,p2=((x-y)%8+8)%8;
            // 暗部增加第二方向与附加线；像素间距固定，因此这里的宽度单位是像素。
            const bool ink=(darkness>0.2f && p1==0) || (darkness>0.5f && p2==0)
                || (darkness>0.75f && p1==4);
            if (ink) c*=0.25f;
        }
        if (s.outline) {
            bool edge=false;
            for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
                if (dx==0 && dy==0) continue;
                const int xx=x+dx,yy=y+dy;
                if (!inside(xx,yy,input.width,input.height)) continue; // 图像边界本身不是物体轮廓。
                const auto& q=g.at(xx,yy);
                edge=edge || !surface(q) || q.object!=center.object ||
                    (surface(q) && (std::abs(q.linear_depth-center.linear_depth)>std::max(0.01f,center.linear_depth*0.035f)
                    || glm::dot(normal(q),normal(center))<0.8f));
            }
            if (edge) c*=0.08f;
        }
        out.at(x,y)=clean(c);
    }
    return out;
}
} // namespace emberframe::lab
