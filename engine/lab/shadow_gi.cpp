#include "shadow_gi.h"
#include "geometry.h"
#include "shading.h"
#include <cmath>
#include <numeric>
#include <utility>

namespace emberframe::lab {
namespace {
constexpr float sh0=0.2820947918f,sh1=0.4886025119f;
float saturate(float x) { return std::clamp(x,0.0f,1.0f); }
float max_component(glm::vec3 v) { return std::max({v.x,v.y,v.z}); }
bool finite(glm::vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
std::size_t grid_index(int x,int y,int z,int n) { return std::size_t(x)+std::size_t(n)*(y+std::size_t(n)*z); }
bool inside(glm::vec3 p,glm::vec3 lo,glm::vec3 hi) {
    return p.x>=lo.x&&p.y>=lo.y&&p.z>=lo.z&&p.x<=hi.x&&p.y<=hi.y&&p.z<=hi.z;
}
struct Bounds { glm::vec3 lo{-1},hi{1}; };
Bounds scene_bounds(const std::vector<Triangle>& triangles) {
    if(triangles.empty()) return {};
    Bounds b{glm::vec3(1e30f),glm::vec3(-1e30f)};
    for(const auto& t:triangles) for(const auto& v:t.vertices) {
        if(!finite(v.position)) throw std::invalid_argument("Non-finite geometry");
        b.lo=glm::min(b.lo,v.position);b.hi=glm::max(b.hi,v.position);
    }
    const float pad=std::max(0.02f,max_component(b.hi-b.lo)*0.025f);
    b.lo-=glm::vec3(pad);b.hi+=glm::vec3(pad);return b;
}
void validate_bounds(glm::vec3 lo,glm::vec3 hi) {
    if(!finite(lo)||!finite(hi)||glm::any(glm::lessThanEqual(hi,lo)))
        throw std::invalid_argument("Volume bounds must be finite and nonempty");
}
bool box_interval(const Ray& r,glm::vec3 lo,glm::vec3 hi,float& begin,float& end) {
    begin=r.t_min;end=r.t_max;
    for(int k=0;k<3;++k) {
        if(std::abs(r.direction[k])<1e-12f) { if(r.origin[k]<lo[k]||r.origin[k]>hi[k]) return false; }
        else { float a=(lo[k]-r.origin[k])/r.direction[k],b=(hi[k]-r.origin[k])/r.direction[k];
            if(a>b) std::swap(a,b);
            begin=std::max(begin,a);end=std::min(end,b);if(begin>end) return false; }
    }
    return true;
}
float radical_inverse(std::uint32_t v) {
    v=(v<<16)|(v>>16);v=((v&0x55555555u)<<1)|((v&0xaaaaaaaau)>>1);
    v=((v&0x33333333u)<<2)|((v&0xccccccccu)>>2);
    v=((v&0x0f0f0f0fu)<<4)|((v&0xf0f0f0f0u)>>4);
    v=((v&0x00ff00ffu)<<8)|((v&0xff00ff00u)>>8);
    return float(double(v)*2.3283064365386963e-10);
}
struct Basis {
    glm::vec3 x,y,z;
    explicit Basis(glm::vec3 direction):z(safe_normalize(direction)) {
        x=safe_normalize(glm::cross(std::abs(z.y)<0.95f?glm::vec3(0,1,0):glm::vec3(1,0,0),z));
        y=glm::cross(z,x);
    }
    glm::vec3 world(glm::vec3 local) const { return x*local.x+y*local.y+z*local.z; }
};
glm::vec3 cosine_direction(glm::vec3 normal,float u,float v) {
    const float r=std::sqrt(saturate(u)),phi=2*pi*v;
    return Basis(normal).world({r*std::cos(phi),r*std::sin(phi),std::sqrt(std::max(0.0f,1-u))});
}
glm::vec3 triangle_normal(const Triangle& t) {
    return safe_normalize(glm::cross(t.vertices[1].position-t.vertices[0].position,
                                     t.vertices[2].position-t.vertices[0].position));
}
Material base_material(const Scene& s,int index) {
    return index>=0&&index<int(s.materials.size())?s.materials[std::size_t(index)]:Material{};
}
glm::vec3 diffuse_color(const SurfaceSample& s) {
    return glm::clamp(s.albedo,glm::vec3(0),glm::vec3(1))*(1-saturate(s.metallic));
}
glm::vec3 diffuse_color(const Material& m) {
    return glm::clamp(glm::vec3(m.base_color),glm::vec3(0),glm::vec3(1))*(1-saturate(m.metallic));
}
std::array<float,4> sh_basis(glm::vec3 d) { return {sh0,sh1*d.x,sh1*d.y,sh1*d.z}; }
glm::vec3 evaluate_sh(const ShRgb& s,glm::vec3 d) {
    auto b=sh_basis(d);glm::vec3 c(0);for(int k=0;k<4;++k)c+=s[k]*b[k];return c;
}
// The SAME basis projects receiver points and launches depth rays. Light.direction
// points from emitter into the scene, so increasing dot(p,forward) means farther.
struct LightProjection {
    Basis frame;
    glm::vec3 lo{1e30f},hi{-1e30f};
    LightProjection(const std::vector<Triangle>& triangles,glm::vec3 direction):frame(direction) {
        const Bounds b=scene_bounds(triangles);
        for(int mask=0;mask<8;++mask) {
            const glm::vec3 p((mask&1)?b.hi.x:b.lo.x,(mask&2)?b.hi.y:b.lo.y,(mask&4)?b.hi.z:b.lo.z);
            const glm::vec3 q(glm::dot(p,frame.x),glm::dot(p,frame.y),glm::dot(p,frame.z));
            lo=glm::min(lo,q);hi=glm::max(hi,q);
        }
    }
    glm::vec3 project(glm::vec3 p) const {
        return (glm::vec3(glm::dot(p,frame.x),glm::dot(p,frame.y),glm::dot(p,frame.z))-lo)/(hi-lo);
    }
    Ray ray(float u,float v) const {
        return {frame.world({glm::mix(lo.x,hi.x,u),glm::mix(lo.y,hi.y,v),lo.z}),frame.z,1e-5f,hi.z-lo.z};
    }
    float pixel_area(int res) const { return (hi.x-lo.x)*(hi.y-lo.y)/float(res*res); }
    float texel_size(int res) const { return std::min(hi.x-lo.x,hi.y-lo.y)/float(res); }
};
Image<float> light_depth(const LightProjection& projection,const Bvh& bvh,int res) {
    Image<float> result(res,res,1);
    for(int y=0;y<res;++y)for(int x=0;x<res;++x) {
        const auto r=projection.ray((x+0.5f)/res,(y+0.5f)/res);
        const Hit h=bvh.intersect(r);if(h.triangle>=0)result.at(x,y)=saturate(h.t/(projection.hi.z-projection.lo.z));
    }
    return result;
}
bool valid_uv(glm::vec2 uv) { return uv.x>=0&&uv.y>=0&&uv.x<1&&uv.y<1; }
}

MomentSummedArea::MomentSummedArea(const Image<float>& depth):width_(depth.width),height_(depth.height) {
    prefix_.assign(std::size_t(width_+1)*(height_+1),glm::dvec4(0));
    for(int y=0;y<height_;++y) {
        glm::dvec4 row(0);
        for(int x=0;x<width_;++x) {
            const double z=depth.at(x,y);
            if(!std::isfinite(z)||z<0||z>1)throw std::invalid_argument("Moment depth must be in [0,1]");
            row+=glm::dvec4(z,z*z,z*z*z,z*z*z*z);
            prefix_[std::size_t(y+1)*(width_+1)+x+1]=row+prefix_[std::size_t(y)*(width_+1)+x+1];
        }
    }
}
glm::dvec4 MomentSummedArea::sum(int x0,int y0,int x1,int y1) const {
    x0=std::clamp(x0,0,width_);x1=std::clamp(x1,0,width_);
    y0=std::clamp(y0,0,height_);y1=std::clamp(y1,0,height_);
    if(x1<=x0||y1<=y0||prefix_.empty())return glm::dvec4(0);
    const auto p=[&](int x,int y){return prefix_[std::size_t(y)*(width_+1)+x];};
    // 四个前缀和完成任意矩形查询；用 double 降低宽核相减的消减误差。
    return p(x1,y1)-p(x0,y1)-p(x1,y0)+p(x0,y0);
}
glm::dvec4 MomentSummedArea::mean(int x0,int y0,int x1,int y1) const {
    const int w=std::clamp(x1,0,width_)-std::clamp(x0,0,width_);
    const int h=std::clamp(y1,0,height_)-std::clamp(y0,0,height_);
    return w>0&&h>0?sum(x0,y0,x1,y1)/double(w*h):glm::dvec4(0);
}
float cantelli_visibility(glm::dvec2 m,double z,double min_variance,float bleed) {
    if(z<=m.x)return 1;
    const double variance=std::max({0.0,min_variance,m.y-m.x*m.x});
    const double delta=z-m.x;
    const float p=float(variance/(variance+delta*delta));
    return saturate((p-std::clamp(bleed,0.0f,0.999f))/(1-std::clamp(bleed,0.0f,0.999f)));
}
float msm_visibility(glm::dvec4 moments,double z,double bias,double depth_bias) {
    z-=std::max(0.0,depth_bias);
    if(z<=0)return 1;
    if(z>1)return 0;
    const double variance=moments.y-moments.x*moments.x;
    if(variance<1e-12)return z<=moments.x?1.0f:0.0f;
    const glm::dvec4 b=glm::mix(moments,glm::dvec4(0.5,1.0/3,0.25,0.2),std::clamp(bias,0.0,1.0));
    // Hankel 矩矩阵 LDL^T 分解。解 M*c=(1,t,t²)，多项式的另两根
    // 构成含接收深度 t 的三点 Gauss-Radau 分布，匹配前四阶矩。
    const double d1=b.y-b.x*b.x,l21=b.z-b.x*b.y;
    const double d2=b.w-b.y*b.y-l21*l21/d1;
    if(d1<=1e-15||d2<=1e-15)return cantelli_visibility({moments.x,moments.y},z);
    const double l=l21/d1;
    const double y1=z-b.x,y2=z*z-b.y-l*y1;
    const double c2=y2/d2,c1=y1/d1-l*c2,c0=1-b.x*c1-b.y*c2;
    const double disc=c1*c1-4*c2*c0;
    if(std::abs(c2)<1e-14||disc<=0||!std::isfinite(disc))
        return cantelli_visibility({moments.x,moments.y},z);
    const double root=std::sqrt(disc);
    // Stable quadratic roots avoid cancellation for highly separated depths.
    const double q=-0.5*(c1+std::copysign(root,c1));
    if(std::abs(q)<1e-20)return cantelli_visibility({moments.x,moments.y},z);
    double a=q/c2,c=c0/q;if(a>c)std::swap(a,c);
    const double da=(a-z)*(a-c),dc=(c-z)*(c-a);
    if(std::abs(da)<1e-14||std::abs(dc)<1e-14)
        return cantelli_visibility({moments.x,moments.y},z);
    const double wa=(b.y-(z+c)*b.x+z*c)/da;
    const double wc=(b.y-(z+a)*b.x+z*a)/dc;
    // t 上的质量按可见处理：返回遮挡下界的补集（可见性上界）。
    const double blocked=(a<z?wa:0)+(c<z?wc:0);
    return std::isfinite(blocked)?float(std::clamp(1-blocked,0.0,1.0)):cantelli_visibility({b.x,b.y},z);
}
BlockerEstimate conditional_blocker_mean(glm::dvec2 m,float z) {
    const float lit=cantelli_visibility(m,z,0),blocked=1-lit;
    if(blocked<1e-5f)return {};
    const float mean=float((m.x-lit*z)/blocked);
    return {std::clamp(mean,0.0f,std::max(z,0.0f)),blocked,true};
}
MomentShadowMap::MomentShadowMap(Image<float> depth):depth_(std::move(depth)),sat_(depth_) {}
glm::dvec4 MomentShadowMap::moments(glm::vec2 uv,int radius) const {
    if(depth_.empty()||!valid_uv(uv))return glm::dvec4(1);
    const int x=std::min(int(uv.x*depth_.width),depth_.width-1),y=std::min(int(uv.y*depth_.height),depth_.height-1);
    radius=std::clamp(radius,0,std::max(depth_.width,depth_.height));
    return sat_.mean(x-radius,y-radius,x+radius+1,y+radius+1);
}
float MomentShadowMap::hard(glm::vec2 uv,float z) const {
    if(depth_.empty()||!valid_uv(uv))return 1;
    return z<=depth_.at(int(uv.x*depth_.width),int(uv.y*depth_.height))?1.0f:0.0f;
}
glm::dvec4 MomentShadowMap::filtered_moments(glm::vec2 uv,float radius) const {
    if(depth_.empty()||!valid_uv(uv))return glm::dvec4(1);
    radius=std::clamp(radius,0.0f,float(std::max(depth_.width,depth_.height)));
    const auto pixel=uv*glm::vec2(depth_.width,depth_.height)-.5f;
    const auto p=glm::ivec2(glm::floor(pixel));const auto f=glm::dvec2(glm::fract(pixel));
    const auto sample=[&](int r) {
        const auto box=[&](int x,int y) {
            x=std::clamp(x,0,depth_.width-1);y=std::clamp(y,0,depth_.height-1);
            return sat_.mean(x-r,y-r,x+r+1,y+r+1);
        };
        return glm::mix(glm::mix(box(p.x,p.y),box(p.x+1,p.y),f.x),
                        glm::mix(box(p.x,p.y+1),box(p.x+1,p.y+1),f.x),f.y);
    };
    const int r=int(std::floor(radius));const auto a=sample(r);
    return glm::fract(radius)<1e-5f?a:glm::mix(a,sample(r+1),double(glm::fract(radius)));
}
float MomentShadowMap::pcf(glm::vec2 uv,float z,int radius,int count) const {
    if(depth_.empty()||!valid_uv(uv))return 1;
    if(radius<=0)return hard(uv,z);
    radius=std::min(radius,std::max(depth_.width,depth_.height));
    const int x=int(uv.x*depth_.width),y=int(uv.y*depth_.height);
    float result=0;
    if(radius<=2) {
        const auto pixel=uv*glm::vec2(depth_.width,depth_.height)-.5f;
        const auto base=glm::ivec2(glm::floor(pixel));const auto f=glm::fract(pixel);
        const auto compare=[&](int i,int j){return z<=depth_.clamped(i,j)?1.0f:0.0f;};
        for(int j=-radius;j<=radius;++j)for(int i=-radius;i<=radius;++i) {
            const int a=base.x+i,b=base.y+j;
            result+=glm::mix(glm::mix(compare(a,b),compare(a+1,b),f.x),glm::mix(compare(a,b+1),compare(a+1,b+1),f.x),f.y);
        }
        return result/float((2*radius+1)*(2*radius+1));
    }
    count=std::clamp(count,1,128);
    for(int i=0;i<count;++i) {
        const float r=radius*std::sqrt((i+0.5f)/count),phi=2*pi*radical_inverse(std::uint32_t(i));
        result+=z<=depth_.clamped(x+int(std::round(r*std::cos(phi))),y+int(std::round(r*std::sin(phi))))?1.0f:0.0f;
    }
    return result/count;
}
float MomentShadowMap::soft_pcf(glm::vec2 uv,float z,float radius) const {
    if(depth_.empty()||!valid_uv(uv))return 1;
    radius=std::clamp(radius,0.0f,float(std::max(depth_.width,depth_.height)));
    const auto compare=[&](int x,int y){return z<=depth_.clamped(x,y)?1.0f:0.0f;};
    float sum=0;
    for(int i=0;i<64;++i) {
        const float r=radius*std::sqrt((float(i)+.5f)/64),phi=float(i)*2.39996322973f;
        auto pixel=uv*glm::vec2(depth_.width,depth_.height)-.5f+r*glm::vec2(std::cos(phi),std::sin(phi));
        const auto p=glm::ivec2(glm::floor(pixel));const auto f=glm::fract(pixel);
        sum+=glm::mix(glm::mix(compare(p.x,p.y),compare(p.x+1,p.y),f.x),
                      glm::mix(compare(p.x,p.y+1),compare(p.x+1,p.y+1),f.x),f.y);
    }
    return sum/64;
}
BlockerEstimate MomentShadowMap::blockers(glm::vec2 uv,float z,int radius,int count) const {
    if(depth_.empty()||!valid_uv(uv))return {};
    const int x=int(uv.x*depth_.width),y=int(uv.y*depth_.height);
    radius=std::clamp(radius,0,std::max(depth_.width,depth_.height));
    double sum=0;int blocked=0,samples=0;
    const auto add=[&](int i,int j){float d=depth_.clamped(i,j);++samples;if(d<z){sum+=d;++blocked;}};
    if(radius<=3)for(int j=-radius;j<=radius;++j)for(int i=-radius;i<=radius;++i)add(x+i,y+j);
    else {
        count=std::clamp(count,1,128);
        for(int i=0;i<count;++i) {
            const float r=radius*std::sqrt((i+0.5f)/count),phi=2*pi*radical_inverse(std::uint32_t(i));
            add(x+int(std::round(r*std::cos(phi))),y+int(std::round(r*std::sin(phi))));
        }
    }
    return blocked?BlockerEstimate{float(sum/blocked),float(blocked)/samples,true}:BlockerEstimate{};
}
float MomentShadowMap::pcss(glm::vec2 uv,float z,int radius,float scale) const {
    const auto b=blockers(uv,z,radius);if(!b.valid)return 1;
    const float filter=std::max(0.0f,z-b.depth)*std::max(0.0f,scale);
    return soft_pcf(uv,z,filter);
}
float MomentShadowMap::vssm(glm::vec2 uv,float z,int radius,float scale) const {
    const auto m=filtered_moments(uv,float(radius));const auto b=conditional_blocker_mean({m.x,m.y},z);
    if(!b.valid)return 1;
    // 与实时路径一样约束到产生条件均值估计的支持域，防止失真的近灯估计扩大到全图。
    const float filter=std::clamp(std::max(0.0f,z-b.depth)*std::max(0.0f,scale),0.0f,float(std::max(radius,0)));
    const auto filtered=filtered_moments(uv,filter);return cantelli_visibility({filtered.x,filtered.y},z);
}

void ReflectiveShadowMap::build(const Scene& scene,const std::vector<Triangle>& triangles,
                                const Bvh& bvh,const Light& light,int resolution) {
    samples_.clear();resolution=std::clamp(resolution,4,128);
    if(triangles.empty()||light.intensity<=0)return;
    const glm::vec3 intensity=glm::max(light.color,glm::vec3(0))*light.intensity;
    const LightProjection projection(triangles,light.direction);const Basis frame(light.direction);
    const int count=resolution*resolution;
    for(int i=0;i<count;++i) {
        Ray ray;glm::vec3 packet(0);float area=0;
        if(light.kind==LightKind::directional) {
            ray=projection.ray((i%resolution+0.5f)/resolution,(i/resolution+0.5f)/resolution);
            area=projection.pixel_area(resolution);packet=intensity*area;
        } else if(light.kind==LightKind::point) {
            const float z=1-2*(i+0.5f)/count,phi=2*pi*radical_inverse(std::uint32_t(i));
            ray={light.position,{std::sqrt(1-z*z)*std::cos(phi),z,std::sqrt(1-z*z)*std::sin(phi)},1e-4f,std::max(0.0f,light.range)};
            packet=intensity*(4*pi/count);
        } else {
            const float u=(i+0.5f)/count,v=radical_inverse(std::uint32_t(i));
            // 位置与方向采用不同低差异维度；不能把同一 uv 同时用于两者。
            const float du=std::fmod((i+0.5f)*0.754877666f,1.0f),dv=std::fmod((i+0.5f)*0.569840296f,1.0f);
            ray={light.position+frame.x*((u-0.5f)*light.size.x)+frame.y*((v-0.5f)*light.size.y),
                 cosine_direction(frame.z,du,dv),1e-4f,std::max(0.0f,light.range)};
            packet=intensity*(pi*std::abs(light.size.x*light.size.y)/count);
        }
        const Hit hit=bvh.intersect(ray);if(hit.triangle<0)continue;
        const Triangle& t=triangles.at(std::size_t(hit.triangle));const Material m=base_material(scene,t.material);
        glm::vec3 n=triangle_normal(t);float cosine=glm::dot(n,-ray.direction);
        if(cosine<=0) { if(!m.double_sided)continue;n=-n;cosine=-cosine; }
        // 正交像素面积 / cos 是表面面积；入射 cos 与它抵消，故 flux 不再乘 cos。
        if(light.kind==LightKind::directional)area/=std::max(cosine,1e-4f);
        else if(light.kind==LightKind::point)area=hit.t*hit.t*(4*pi/count)/std::max(cosine,1e-4f);
        samples_.push_back({ray.origin+hit.t*ray.direction,n,packet*diffuse_color(m),area});
    }
}
void ReflectiveShadowMap::set_samples(std::vector<RsmSample> samples) { samples_=std::move(samples); }
glm::vec3 ReflectiveShadowMap::gather(glm::vec3 p,glm::vec3 n,glm::vec3 albedo,int max_samples,
                                     float min_distance,const std::function<bool(const Ray&)>& occluded) const {
    if(samples_.empty()||max_samples<=0)return glm::vec3(0);
    n=safe_normalize(n);albedo=glm::max(albedo,glm::vec3(0));
    const int count=std::min(int(samples_.size()),max_samples);glm::vec3 sum(0);
    min_distance=std::max(min_distance,1e-5f);
    for(int i=0;i<count;++i) {
        const std::size_t index=std::min(samples_.size()-1,(std::size_t(2*i+1)*samples_.size())/std::size_t(2*count));
        const auto& s=samples_[index];const glm::vec3 delta=s.position-p;
        const float distance=glm::length(delta);if(distance<min_distance)continue;
        const glm::vec3 l=delta/distance;
        const float c=std::max(0.0f,glm::dot(n,l))*std::max(0.0f,glm::dot(s.normal,-l));if(c==0)continue;
        if(occluded&&occluded({p+n*min_distance*0.1f,l,min_distance*0.1f,distance-min_distance*0.2f}))continue;
        // 两个 Lambert 因子：VPL 把反射通量转成辐射强度，再由接收面转为 Lo。
        sum+=glm::max(s.flux,glm::vec3(0))*(c/(distance*distance*pi*pi));
    }
    return sum*albedo*(float(samples_.size())/count);
}

LightPropagationVolume::LightPropagationVolume(glm::vec3 lo,glm::vec3 hi,int resolution)
    :minimum_(lo),maximum_(hi),resolution_(std::clamp(resolution,2,32)) {
    validate_bounds(lo,hi);cell_size_=(hi-lo)/float(resolution_);clear();
}
void LightPropagationVolume::clear() {
    const ShRgb zero{glm::vec3(0),glm::vec3(0),glm::vec3(0),glm::vec3(0)};
    frontier_.assign(std::size_t(resolution_)*resolution_*resolution_,zero);accumulated_=frontier_;
}
void LightPropagationVolume::inject(std::span<const RsmSample> samples) {
    for(const auto& sample:samples) {
        const glm::vec3 n=safe_normalize(sample.normal);
        const glm::vec3 p=sample.position+n*(0.55f*std::min({cell_size_.x,cell_size_.y,cell_size_.z}));
        if(!inside(p,minimum_,maximum_))continue;
        const glm::ivec3 c=glm::clamp(glm::ivec3((p-minimum_)/cell_size_),glm::ivec3(0),glm::ivec3(resolution_-1));
        const auto index=grid_index(c.x,c.y,c.z,resolution_);auto basis=sh_basis(n);
        const glm::vec3 power=glm::max(sample.flux,glm::vec3(0));
        for(int k=0;k<4;++k) {
            // Lambert 出射角分布的 SH 系数：l=0 系数为 flux*Y00，
            // l=1 系数为 flux*(2/3)*Y1；积分后仍为原反射通量。
            const glm::vec3 v=power*(basis[k]*(k?2.0f/3:1.0f));
            frontier_[index][k]+=v;accumulated_[index][k]+=v;
        }
    }
}
void LightPropagationVolume::set_occupancy(std::vector<float> occupancy) {
    if(!occupancy.empty()&&occupancy.size()!=frontier_.size())throw std::invalid_argument("LPV occupancy size mismatch");
    for(float& v:occupancy)v=saturate(v);
    occupancy_=std::move(occupancy);
}
void LightPropagationVolume::propagate(int iterations,float attenuation) {
    iterations=std::clamp(iterations,0,32);attenuation=saturate(attenuation);
    const std::array<glm::ivec3,6> offsets={glm::ivec3(1,0,0),{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const ShRgb zero{glm::vec3(0),glm::vec3(0),glm::vec3(0),glm::vec3(0)};
    for(int iteration=0;iteration<iterations;++iteration) {
        std::vector<ShRgb> next(frontier_.size(),zero);
        for(int z=0;z<resolution_;++z)for(int y=0;y<resolution_;++y)for(int x=0;x<resolution_;++x) {
            const auto& src=frontier_[grid_index(x,y,z,resolution_)];
            const glm::vec3 energy=glm::max(src[0]/sh0,glm::vec3(0));if(max_component(energy)<1e-12f)continue;
            std::array<glm::vec3,6> weights;glm::vec3 total(0);
            for(int d=0;d<6;++d) { weights[d]=glm::max(evaluate_sh(src,glm::vec3(offsets[d])),glm::vec3(0));total+=weights[d]; }
            // 截断 SH 可能产生负瓣；先钳制六向权重，再按原 l=0 通量归一化，
            // 防止传播轮数增加时凭空增能。越界/遮挡贡献被吸收，不重分配。
            const glm::vec3 scale=energy*attenuation/glm::max(total,glm::vec3(1e-12f));
            for(int d=0;d<6;++d) {
                const glm::ivec3 c=glm::ivec3(x,y,z)+offsets[d];
                if(glm::any(glm::lessThan(c,glm::ivec3(0)))||glm::any(glm::greaterThanEqual(c,glm::ivec3(resolution_))))continue;
                const auto index=grid_index(c.x,c.y,c.z,resolution_);
                const float transmission=occupancy_.empty()?1:1-occupancy_[index];
                const glm::vec3 transferred=weights[d]*scale*transmission;auto basis=sh_basis(glm::vec3(offsets[d]));
                for(int k=1;k<4;++k)basis[k]*=2.0f/3.0f; // 与 GPU 一致的有限方向余弦瓣，不用 delta。
                for(int k=0;k<4;++k)next[index][k]+=transferred*basis[k];
            }
        }
        for(std::size_t i=0;i<next.size();++i)for(int k=0;k<4;++k)accumulated_[i][k]+=next[i][k];
        frontier_.swap(next);
    }
}
glm::vec3 LightPropagationVolume::frontier_energy() const {
    glm::vec3 energy(0);for(const auto& cell:frontier_)energy+=cell[0]/sh0;return energy;
}
glm::vec3 LightPropagationVolume::irradiance(glm::vec3 p,glm::vec3 normal) const {
    if(!inside(p,minimum_,maximum_))return glm::vec3(0);
    const glm::vec3 grid=(p-minimum_)/cell_size_-glm::vec3(0.5f);
    const glm::ivec3 base=glm::ivec3(glm::floor(grid));const glm::vec3 f=glm::fract(grid);
    const auto basis=sh_basis(-safe_normalize(normal));glm::vec3 result(0);
    for(int mask=0;mask<8;++mask) {
        const glm::ivec3 o(mask&1,(mask>>1)&1,(mask>>2)&1);
        const auto c=glm::clamp(base+o,glm::ivec3(0),glm::ivec3(resolution_-1));
        const float weight=(o.x?f.x:1-f.x)*(o.y?f.y:1-f.y)*(o.z?f.z:1-f.z);
        const auto& coeff=accumulated_[grid_index(c.x,c.y,c.z,resolution_)];glm::vec3 irradiance(0);
        for(int k=0;k<4;++k)irradiance+=coeff[k]*(basis[k]*(k?2*pi/3:pi));
        result+=glm::max(irradiance,glm::vec3(0))*weight;
    }
    const float area=(cell_size_.x*cell_size_.y+cell_size_.x*cell_size_.z+cell_size_.y*cell_size_.z)/3;
    return glm::max(result/area,glm::vec3(0));
}

namespace {
glm::vec3 closest_segment(glm::vec3 p,glm::vec3 a,glm::vec3 b) {
    const glm::vec3 d=b-a;const float q=glm::dot(d,d);return q>1e-20f?a+d*saturate(glm::dot(p-a,d)/q):a;
}
glm::vec3 closest_triangle(glm::vec3 p,const Triangle& t) {
    const glm::vec3 a=t.vertices[0].position,b=t.vertices[1].position,c=t.vertices[2].position;
    const glm::vec3 ab=b-a,ac=c-a,normal=glm::cross(ab,ac);
    if(glm::dot(normal,normal)<1e-18f) {
        glm::vec3 best=a;float distance=glm::dot(p-a,p-a);
        for(const auto q:{closest_segment(p,a,b),closest_segment(p,b,c),closest_segment(p,c,a)}) {
            const float d=glm::dot(p-q,p-q);if(d<distance){distance=d;best=q;}
        }
        return best;
    }
    const glm::vec3 ap=p-a;const float d1=glm::dot(ab,ap),d2=glm::dot(ac,ap);
    if(d1<=0&&d2<=0)return a;
    const glm::vec3 bp=p-b;const float d3=glm::dot(ab,bp),d4=glm::dot(ac,bp);
    if(d3>=0&&d4<=d3)return b;
    const float vc=d1*d4-d3*d2;if(vc<=0&&d1>=0&&d3<=0)return a+ab*(d1/(d1-d3));
    const glm::vec3 cp=p-c;const float d5=glm::dot(ab,cp),d6=glm::dot(ac,cp);
    if(d6>=0&&d5<=d6)return c;
    const float vb=d5*d2-d1*d6;if(vb<=0&&d2>=0&&d6<=0)return a+ac*(d2/(d2-d6));
    const float va=d3*d6-d5*d4;if(va<=0&&(d4-d3)>=0&&(d5-d6)>=0)return b+(c-b)*((d4-d3)/((d4-d3)+(d5-d6)));
    const float inv=1/(va+vb+vc);return a+ab*(vb*inv)+ac*(vc*inv);
}
bool triangle_box(const Triangle& triangle,glm::vec3 center,float half) {
    const std::array<glm::vec3,3> v={triangle.vertices[0].position-center,triangle.vertices[1].position-center,triangle.vertices[2].position-center};
    const std::array<glm::vec3,3> edges={v[1]-v[0],v[2]-v[1],v[0]-v[2]};
    const std::array<glm::vec3,3> unit={glm::vec3(1,0,0),{0,1,0},{0,0,1}};
    const auto separated=[&](glm::vec3 axis) {
        const float a=glm::dot(v[0],axis),b=glm::dot(v[1],axis),c=glm::dot(v[2],axis);
        const glm::vec3 aa=glm::abs(axis);const float extent=half*(aa.x+aa.y+aa.z)+1e-7f;
        return std::min({a,b,c})>extent||std::max({a,b,c})<-extent;
    };
    for(const auto axis:unit)if(separated(axis))return false;
    if(separated(glm::cross(edges[0],edges[1])))return false;
    for(const auto e:edges)for(const auto axis:unit)if(separated(glm::cross(e,axis)))return false;
    return true;
}
}
VoxelRadianceVolume::VoxelRadianceVolume(glm::vec3 lo,glm::vec3 hi,int resolution):resolution_(4) {
    validate_bounds(lo,hi);while(resolution_<std::clamp(resolution,4,64))resolution_*=2;
    const glm::vec3 center=(lo+hi)*0.5f;const float side=max_component(hi-lo);
    minimum_=center-glm::vec3(side*0.5f);maximum_=center+glm::vec3(side*0.5f);cell_size_=side/resolution_;
    levels_.push_back({resolution_,std::vector<glm::vec4>(std::size_t(resolution_)*resolution_*resolution_,glm::vec4(0))});
}
void VoxelRadianceVolume::set_voxel(int x,int y,int z,glm::vec3 radiance,float opacity) {
    if(x<0||y<0||z<0||x>=resolution_||y>=resolution_||z>=resolution_)throw std::out_of_range("Voxel coordinate");
    opacity=saturate(opacity);levels_[0].cells[grid_index(x,y,z,resolution_)]=glm::vec4(glm::max(radiance,glm::vec3(0))*opacity,opacity);
    if(levels_.size()>1)levels_.resize(1);
}
void VoxelRadianceVolume::voxelize(const std::vector<Triangle>& triangles,
    const std::function<glm::vec3(glm::vec3,glm::vec3,int)>& radiance) {
    levels_.resize(1);std::fill(levels_[0].cells.begin(),levels_[0].cells.end(),glm::vec4(0));
    std::vector<unsigned> counts(levels_[0].cells.size(),0);
    for(const auto& t:triangles) {
        glm::vec3 lo=glm::min(t.vertices[0].position,glm::min(t.vertices[1].position,t.vertices[2].position));
        glm::vec3 hi=glm::max(t.vertices[0].position,glm::max(t.vertices[1].position,t.vertices[2].position));
        if(glm::any(glm::lessThan(hi,minimum_))||glm::any(glm::greaterThan(lo,maximum_)))continue;
        const glm::ivec3 first=glm::clamp(glm::ivec3(glm::floor((lo-minimum_)/cell_size_-glm::vec3(1e-5f))),glm::ivec3(0),glm::ivec3(resolution_-1));
        const glm::ivec3 last=glm::clamp(glm::ivec3(glm::floor((hi-minimum_)/cell_size_+glm::vec3(1e-5f))),glm::ivec3(0),glm::ivec3(resolution_-1));
        const glm::vec3 normal=triangle_normal(t);
        for(int z=first.z;z<=last.z;++z)for(int y=first.y;y<=last.y;++y)for(int x=first.x;x<=last.x;++x) {
            const glm::vec3 center=minimum_+(glm::vec3(x,y,z)+glm::vec3(0.5f))*cell_size_;
            // 13 个分离轴保守检测三角形/体素相交，不能只把三角形顶点写进格子。
            if(!triangle_box(t,center,cell_size_*0.5f))continue;
            const auto i=grid_index(x,y,z,resolution_);
            const glm::vec3 color=radiance?glm::max(radiance(closest_triangle(center,t),normal,t.material),glm::vec3(0)):glm::vec3(0);
            levels_[0].cells[i]+=glm::vec4(color,0);++counts[i];
        }
    }
    for(std::size_t i=0;i<counts.size();++i)if(counts[i]) {
        levels_[0].cells[i]/=float(counts[i]);levels_[0].cells[i].a=1;
    }
    build_mips();
}
void VoxelRadianceVolume::build_mips() {
    levels_.resize(1);
    while(levels_.back().size>1) {
        const auto& child=levels_.back();const int n=child.size/2;
        Level parent{n,std::vector<glm::vec4>(std::size_t(n)*n*n,glm::vec4(0))};
        for(int z=0;z<n;++z)for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
            glm::vec4 sum(0);for(int mask=0;mask<8;++mask)
                sum+=child.cells[grid_index(2*x+(mask&1),2*y+((mask>>1)&1),2*z+((mask>>2)&1),child.size)];
            const float mean_alpha=sum.a/8;
            // 父层代表两倍长度：T_parent = T_child²。RGB 先按 alpha 求平均
            // 再预乘新 alpha，避免空体素把表面颜色错误地变黑。
            const float alpha=1-(1-mean_alpha)*(1-mean_alpha);
            parent.cells[grid_index(x,y,z,n)]=glm::vec4(sum.a>1e-10f?glm::vec3(sum)*(alpha/sum.a):glm::vec3(0),alpha);
        }
        levels_.push_back(std::move(parent));
    }
}
glm::vec4 VoxelRadianceVolume::sample(glm::vec3 p,float lod) const {
    if(!inside(p,minimum_,maximum_))return glm::vec4(0);
    lod=std::clamp(lod,0.0f,float(levels_.size()-1));const int level=int(lod);
    const auto sample_level=[&](int index) {
        const auto& l=levels_[std::size_t(index)];const glm::vec3 g=(p-minimum_)/(maximum_-minimum_)*float(l.size)-glm::vec3(0.5f);
        const auto base=glm::ivec3(glm::floor(g));const auto f=glm::fract(g);glm::vec4 result(0);
        for(int mask=0;mask<8;++mask) {
            const glm::ivec3 o(mask&1,(mask>>1)&1,(mask>>2)&1),c=glm::clamp(base+o,glm::ivec3(0),glm::ivec3(l.size-1));
            result+=l.cells[grid_index(c.x,c.y,c.z,l.size)]*((o.x?f.x:1-f.x)*(o.y?f.y:1-f.y)*(o.z?f.z:1-f.z));
        }
        return result;
    };
    return glm::mix(sample_level(level),sample_level(std::min(level+1,int(levels_.size()-1))),lod-level);
}
ConeTraceResult VoxelRadianceVolume::trace(glm::vec3 origin,glm::vec3 direction,float half_angle,float max_distance,int max_steps) const {
    ConeTraceResult result;direction=safe_normalize(direction);
    Ray ray{origin,direction,cell_size_*0.6f,std::max(0.0f,max_distance)};float begin,end;
    if(!box_interval(ray,minimum_,maximum_,begin,end))return result;
    const float tangent=std::tan(std::clamp(half_angle,0.0f,1.2f));float t=begin,transmission=1;
    for(int step=0;step<std::clamp(max_steps,0,512)&&t<=end&&transmission>0.01f;++step) {
        const float diameter=std::max(cell_size_,2*t*tangent);
        const float lod=std::clamp(std::log2(diameter/cell_size_),0.0f,float(levels_.size()-1));
        const float stride=std::max(cell_size_*0.5f,diameter*0.5f);const auto value=sample(origin+t*direction,lod);
        const float segment=std::min(stride,std::max(0.0f,end-t));
        // 当前采样的 alpha 对应 mip 格子长度；按实际步长重标定光学厚度。
        const float alpha=1-std::pow(std::max(0.0f,1-value.a),segment/(cell_size_*std::exp2(lod)));
        if(value.a>1e-8f)result.radiance+=transmission*glm::vec3(value)*(alpha/value.a);
        transmission*=1-alpha;t+=stride;++result.steps;
    }
    result.opacity=1-transmission;return result;
}

float sdf_sphere(glm::vec3 p,glm::vec3 center,float radius) { return glm::length(p-center)-std::max(0.0f,radius); }
float sdf_box(glm::vec3 p,glm::vec3 center,glm::vec3 half_extent) {
    const glm::vec3 q=glm::abs(p-center)-glm::max(half_extent,glm::vec3(0));
    return glm::length(glm::max(q,glm::vec3(0)))+std::min(0.0f,max_component(q));
}
SdfTraceResult sphere_trace(const DistanceFunction& field,const Ray& ray,float epsilon,int max_steps,float lipschitz) {
    SdfTraceResult result;const float speed=glm::length(ray.direction);
    if(!field||speed<1e-12f||ray.t_max<ray.t_min||!std::isfinite(speed))return result;
    epsilon=std::max(epsilon,1e-6f);lipschitz=std::max(lipschitz,1e-6f);
    float t=std::max(0.0f,ray.t_min);
    for(int i=0;i<std::clamp(max_steps,0,4096)&&t<=ray.t_max;++i) {
        const glm::vec3 p=ray.origin+t*ray.direction;const float distance=field(p);++result.steps;
        if(!std::isfinite(distance))return result;
        result.distance=t;result.position=p;
        if(std::abs(distance)<=epsilon) {
            result.hit=true;glm::vec3 gradient(0);
            for(int k=0;k<3;++k) { glm::vec3 offset(0);offset[k]=epsilon*2;gradient[k]=field(p+offset)-field(p-offset); }
            result.normal=safe_normalize(gradient,-ray.direction/speed);return result;
        }
        // |f|/L 是到零等值面的安全距离；内部起点也用绝对值寻找出口。
        t+=0.9f*std::abs(distance)/(lipschitz*speed);
    }
    return result;
}
float sdf_soft_shadow(const DistanceFunction& field,const Ray& ray,float softness,float epsilon,int max_steps,float lipschitz) {
    const float speed=glm::length(ray.direction);if(!field||speed<1e-12f)return 1;
    float result=1,t=std::max(ray.t_min,epsilon/speed);lipschitz=std::max(1e-6f,lipschitz);
    for(int i=0;i<std::clamp(max_steps,0,4096)&&t<ray.t_max;++i) {
        const float d=field(ray.origin+t*ray.direction);if(!std::isfinite(d))return result;
        if(d<=epsilon)return 0;
        result=std::min(result,std::max(0.0f,softness)*d/(t*speed));
        t+=0.9f*d/(lipschitz*speed);
    }
    return saturate(result);
}
BakedSdfGrid::BakedSdfGrid(glm::vec3 lo,glm::vec3 hi,int resolution)
    :minimum_(lo),maximum_(hi),resolution_(std::clamp(resolution,4,64)) {
    validate_bounds(lo,hi);cell_size_=(hi-lo)/float(resolution_);
    const int n=resolution_+1;values_.assign(std::size_t(n)*n*n,glm::length(hi-lo));
}
void BakedSdfGrid::bake(const DistanceFunction& field) {
    if(!field)throw std::invalid_argument("Missing SDF function");
    const int n=resolution_+1;
    for(int z=0;z<n;++z)for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
        const float value=field(minimum_+glm::vec3(x,y,z)*cell_size_);
        if(!std::isfinite(value))throw std::invalid_argument("Non-finite SDF sample");
        values_[grid_index(x,y,z,n)]=value;
    }
    glm::vec3 max_slope(0);
    for(int z=0;z<n;++z)for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
        const float here=values_[grid_index(x,y,z,n)];
        if(x<resolution_)max_slope.x=std::max(max_slope.x,std::abs(values_[grid_index(x+1,y,z,n)]-here)/cell_size_.x);
        if(y<resolution_)max_slope.y=std::max(max_slope.y,std::abs(values_[grid_index(x,y+1,z,n)]-here)/cell_size_.y);
        if(z<resolution_)max_slope.z=std::max(max_slope.z,std::abs(values_[grid_index(x,y,z+1,n)]-here)/cell_size_.z);
    }
    // 三线性插值的各偏导是对应网格边斜率的凸组合；这给出全域 Lipschitz 上界。
    // 追踪的是离散等值面，不能把此安全性误称为精确命中原始三角网格。
    lipschitz_=std::max(1e-6f,glm::length(max_slope));
}
void BakedSdfGrid::bake(const std::vector<Triangle>& triangles) {
    if(triangles.empty()) { bake([&](glm::vec3){return glm::length(maximum_-minimum_);});return; }
    const Bvh bvh(triangles);
    const glm::vec3 direction=safe_normalize(glm::vec3(1,0.371390676f,0.694746614f));
    const float tolerance=std::max(1e-6f,glm::length(maximum_-minimum_)*1e-6f);
    bake([&](glm::vec3 p) {
        float distance=std::numeric_limits<float>::max();
        for(const auto& t:triangles)distance=std::min(distance,glm::length(p-closest_triangle(p,t)));
        if(distance<=tolerance)return 0.0f;
        Ray ray{p,direction,tolerance,1e30f};std::size_t crossings=0;
        // 越过同一命中深度后重新查 BVH；共享边上的两个面不应重复计数。
        for(std::size_t i=0;i<=triangles.size();++i) {
            const Hit hit=bvh.intersect(ray);if(hit.triangle<0)break;
            ++crossings;ray.t_min=hit.t+tolerance;
        }
        return crossings%2?-distance:distance;
    });
}
float BakedSdfGrid::sample(glm::vec3 p) const {
    const glm::vec3 clamped=glm::clamp(p,minimum_,maximum_);
    const glm::vec3 grid=(clamped-minimum_)/cell_size_;
    const glm::ivec3 base=glm::min(glm::ivec3(glm::floor(grid)),glm::ivec3(resolution_-1));
    const glm::vec3 f=grid-glm::vec3(base);const int n=resolution_+1;float value=0;
    for(int mask=0;mask<8;++mask) {
        const glm::ivec3 o(mask&1,(mask>>1)&1,(mask>>2)&1),c=base+o;
        value+=values_[grid_index(c.x,c.y,c.z,n)]*(o.x?f.x:1-f.x)*(o.y?f.y:1-f.y)*(o.z?f.z:1-f.z);
    }
    return value+glm::length(p-clamped);
}
SdfTraceResult BakedSdfGrid::trace(const Ray& ray,float epsilon,int max_steps) const {
    float enter,exit;if(!box_interval(ray,minimum_,maximum_,enter,exit))return {};
    Ray bounded=ray;bounded.t_min=enter;bounded.t_max=exit;
    return sphere_trace([this](glm::vec3 p){return sample(p);},bounded,epsilon,max_steps,lipschitz_);
}
float BakedSdfGrid::geometric_error_bound() const { return glm::length(cell_size_)*0.5f; }

ScreenSpaceHit trace_screen_space(const GBuffer& gbuffer,const Camera& camera,const Ray& ray,const ScreenTraceSettings& options) {
    ScreenSpaceHit result;if(gbuffer.empty()||options.max_steps<=0)return result;
    const float speed=glm::length(ray.direction);if(speed<1e-12f)return result;
    const auto view=camera.view();const auto vp=camera.projection(float(gbuffer.width)/gbuffer.height,false)*view;
    const float pixel_slope=2*std::tan(glm::radians(camera.fov)*0.5f)/gbuffer.height;
    const float max_t=std::min(ray.t_max,std::max(0.0f,options.max_distance)/speed);
    const float thickness=std::max(options.thickness,1e-4f);
    struct Probe { bool valid=false;glm::ivec2 pixel{0};glm::vec2 uv{0};float delta=0,depth=0; };
    const auto probe=[&](float t) {
        Probe q;const glm::vec3 p=ray.origin+ray.direction*t;const glm::vec4 clip=vp*glm::vec4(p,1);
        if(clip.w<=0||clip.z<0||clip.z>clip.w)return q;
        q.uv={0.5f*(clip.x/clip.w+1),0.5f*(1-clip.y/clip.w)};
        if(!valid_uv(q.uv))return q;
        q.pixel={int(q.uv.x*gbuffer.width),int(q.uv.y*gbuffer.height)};
        const auto& surface=gbuffer.at(q.pixel.x,q.pixel.y);if(!surface.valid)return q;
        q.depth=-(view*glm::vec4(p,1)).z;
        const float scene_depth=-(view*glm::vec4(surface.position,1)).z;
        if(scene_depth<=camera.near_plane)return q;
        q.delta=q.depth-scene_depth;q.valid=true;return q;
    };
    float t=std::max(ray.t_min,1e-4f),previous_t=t;Probe previous=probe(t);
    for(int i=0;i<std::clamp(options.max_steps,1,512)&&t<=max_t;++i) {
        Probe current=probe(t);++result.steps;
        if(current.valid&&current.delta>=0&&previous.valid&&previous.delta<=0&&t>previous_t) {
            float low=previous_t,high=t;Probe candidate=current;
            for(int j=0;j<std::clamp(options.refinement_steps,0,16);++j) {
                const float mid=(low+high)*0.5f;const Probe q=probe(mid);
                if(!q.valid)break;
                if(q.delta>=0){high=mid;candidate=q;}else low=mid;
            }
            const auto& hit=gbuffer.at(candidate.pixel.x,candidate.pixel.y);
            const glm::vec3 p=ray.origin+ray.direction*high;
            const float footprint=std::max(0.001f,candidate.depth*pixel_slope*1.5f);
            const float facing=glm::dot(safe_normalize(hit.normal),-ray.direction/speed);
            // 必须同时满足穿越、厚度、世界位置距离和朝向；越屏/空背景不能当命中。
            if(candidate.delta>=0&&candidate.delta<=thickness&&glm::length(hit.position-p)<=thickness+footprint
                &&glm::length(hit.position-ray.origin)>thickness*2&&facing>0.02f) {
                const float edge=std::min({candidate.uv.x,1-candidate.uv.x,candidate.uv.y,1-candidate.uv.y});
                result.valid=true;result.pixel=candidate.pixel;result.position=hit.position;result.distance=high*speed;
                result.confidence=saturate(edge/0.08f)*saturate(1-result.distance/std::max(options.max_distance,1e-4f))
                    *saturate(facing*4)*saturate(1-candidate.delta/thickness);return result;
            }
        }
        previous=current;previous_t=t;
        const float view_depth=std::max(camera.near_plane,-(view*glm::vec4(ray.origin+t*ray.direction,1)).z);
        const float stride=std::max({0.01f,view_depth*pixel_slope*1.5f,options.max_distance/(2*std::clamp(options.max_steps,1,512))});
        t+=stride/speed;
    }
    return result;
}
Image<glm::vec3> screen_space_indirect(const GBuffer& gbuffer,const Image<glm::vec3>& source,
    const Scene& scene,const Camera& camera,const Settings& settings) {
    if(source.width!=gbuffer.width||source.height!=gbuffer.height)throw std::invalid_argument("Screen source/GBuffer size mismatch");
    Image<glm::vec3> output(gbuffer.width,gbuffer.height,glm::vec3(0));
    if(settings.gi!=GiMode::ssr&&settings.gi!=GiMode::ssgi)return output;
    const int count=std::clamp(settings.samples,1,32);
    ScreenTraceSettings options;options.max_distance=std::min(camera.far_plane,30.0f);options.thickness=0.08f;
    for(int y=0;y<gbuffer.height;++y)for(int x=0;x<gbuffer.width;++x) {
        const auto& surface=gbuffer.at(x,y);if(!surface.valid)continue;
        const glm::vec3 n=safe_normalize(surface.normal),v=safe_normalize(camera.position-surface.position,n);
        const glm::vec3 origin=surface.position+n*std::max(0.005f,settings.shadow_bias);
        if(settings.gi==GiMode::ssr) {
            const glm::vec3 direction=safe_normalize(glm::reflect(-v,n));
            const auto hit=trace_screen_space(gbuffer,camera,{origin,direction,0.02f,options.max_distance},options);
            const glm::vec3 fallback=environment(scene,direction);
            const float confidence=hit.valid?hit.confidence*(1-saturate(surface.roughness)*saturate(surface.roughness)):0;
            const glm::vec3 incoming=hit.valid?glm::mix(fallback,source.at(hit.pixel.x,hit.pixel.y),confidence):fallback;
            const glm::vec3 f0=glm::mix(glm::vec3(0.04f),glm::clamp(surface.albedo,glm::vec3(0),glm::vec3(1)),saturate(surface.metallic));
            const glm::vec3 fresnel=f0+(glm::vec3(1)-f0)*std::pow(1-saturate(glm::dot(n,v)),5.0f);
            output.at(x,y)=glm::max(incoming,glm::vec3(0))*fresnel;
        } else {
            glm::vec3 incoming(0);
            const std::uint32_t scramble=(std::uint32_t(x)*73856093u)^(std::uint32_t(y)*19349663u)^settings.seed;
            const float rotation=radical_inverse(scramble);
            for(int i=0;i<count;++i) {
                const glm::vec3 direction=cosine_direction(n,(i+0.5f)/count,std::fmod(radical_inverse(std::uint32_t(i))+rotation,1.0f));
                const auto hit=trace_screen_space(gbuffer,camera,{origin,direction,0.02f,options.max_distance},options);
                const glm::vec3 fallback=environment(scene,direction);
                incoming+=hit.valid?glm::mix(fallback,source.at(hit.pixel.x,hit.pixel.y),hit.confidence):fallback;
            }
            // 余弦 PDF=cos/pi 与 Lambert BRDF=albedo/pi 抵消；不再额外乘 cos/pi。
            output.at(x,y)=glm::max(incoming,glm::vec3(0))*diffuse_color(surface)/float(count);
        }
    }
    return output;
}

namespace {
glm::vec3 light_irradiance(const Scene& scene,const Bvh& bvh,glm::vec3 p,glm::vec3 n,float bias) {
    glm::vec3 sum(0);n=safe_normalize(n);bias=std::max(bias,1e-4f);
    for(const auto& light:scene.lights) {
        const glm::vec3 intensity=glm::max(light.color,glm::vec3(0))*std::max(0.0f,light.intensity);
        if(light.kind==LightKind::directional) {
            const glm::vec3 l=-safe_normalize(light.direction);
            const float cosine=std::max(0.0f,glm::dot(n,l));
            if(cosine>0&&!bvh.occluded({p+n*bias,l,bias,1e30f}))sum+=intensity*cosine;
        } else if(light.kind==LightKind::point) {
            const glm::vec3 delta=light.position-p;const float distance=glm::length(delta);
            if(distance<=bias||distance>light.range)continue;
            const glm::vec3 l=delta/distance;const float cosine=std::max(0.0f,glm::dot(n,l));
            if(cosine>0&&!bvh.occluded({p+n*bias,l,bias,distance-bias}))sum+=intensity*(cosine/(distance*distance));
        } else {
            const Basis basis(light.direction);constexpr int count=8;
            for(int i=0;i<count;++i) {
                const glm::vec3 emitter=light.position+basis.x*(((i+0.5f)/count-0.5f)*light.size.x)
                    +basis.y*((radical_inverse(std::uint32_t(i))-0.5f)*light.size.y);
                const glm::vec3 delta=emitter-p;const float distance=glm::length(delta);
                if(distance<=bias||distance>light.range)continue;
                const glm::vec3 l=delta/distance;
                const float cosine=std::max(0.0f,glm::dot(n,l))*std::max(0.0f,glm::dot(basis.z,-l));
                // 面光 intensity 是 Le；面积 PDF=1/A，几何项包含两端余弦/r²。
                if(cosine>0&&!bvh.occluded({p+n*bias,l,bias,distance-bias}))
                    sum+=intensity*(cosine*std::abs(light.size.x*light.size.y)/(count*distance*distance));
            }
        }
    }
    return sum;
}
LightProjection cascade_projection(LightProjection projection,const Camera& camera,float near_distance,float far_distance,float aspect) {
    const glm::vec3 forward=safe_normalize(camera.target-camera.position,{0,0,-1});
    const glm::vec3 right=safe_normalize(glm::cross(forward,{0,1,0}),{1,0,0}),up=glm::cross(right,forward);
    glm::vec2 lo(1e30f),hi(-1e30f);const float tangent=std::tan(glm::radians(camera.fov)*0.5f);
    for(const float distance:{near_distance,far_distance})for(int mask=0;mask<4;++mask) {
        const glm::vec3 p=camera.position+forward*distance+right*((mask&1?1.0f:-1.0f)*distance*tangent*aspect)
            +up*((mask&2?1.0f:-1.0f)*distance*tangent);
        const glm::vec2 q(glm::dot(p,projection.frame.x),glm::dot(p,projection.frame.y));lo=glm::min(lo,q);hi=glm::max(hi,q);
    }
    // 仅裁 XY；保留整个场景的光源深度，级联视锥外的投影遮挡物仍能投下阴影。
    const glm::vec2 clipped_lo=glm::max(lo,glm::vec2(projection.lo)),clipped_hi=glm::min(hi,glm::vec2(projection.hi));
    if(clipped_hi.x>clipped_lo.x&&clipped_hi.y>clipped_lo.y) {
        projection.lo.x=clipped_lo.x;projection.lo.y=clipped_lo.y;
        projection.hi.x=clipped_hi.x;projection.hi.y=clipped_hi.y;
    }
    return projection;
}
}

struct ShadowGi::Impl {
    struct ShadowLayer {
        LightProjection projection;
        MomentShadowMap map;
        float split_near=0,split_far=1e30f;
        ShadowLayer(LightProjection p,const Bvh& bvh,int resolution,float near_distance,float far_distance)
            :projection(std::move(p)),map(light_depth(projection,bvh,resolution)),split_near(near_distance),split_far(far_distance) {}
    };
    struct LightMaps { Light light;std::vector<ShadowLayer> layers; };
    Scene scene;std::vector<Triangle> triangles;Camera camera;Settings settings;Bvh bvh;
    std::vector<LightMaps> light_maps;
    std::vector<ReflectiveShadowMap> rsms;
    std::unique_ptr<LightPropagationVolume> lpv;
    std::unique_ptr<VoxelRadianceVolume> voxels;
    float scene_diagonal=1,lpv_surface_offset=0;

    Impl(const Scene& s,const std::vector<Triangle>& ts,const Camera& c,const Settings& options)
        :scene(s),triangles(ts),camera(c),settings(options),bvh(triangles) {
        const Bounds bounds=scene_bounds(triangles);scene_diagonal=glm::length(bounds.hi-bounds.lo);
        const int resolution=std::clamp(settings.shadow_resolution,16,512);
        const glm::vec3 forward=safe_normalize(camera.target-camera.position,{0,0,-1});
        float scene_far=camera.near_plane*2;
        for(int mask=0;mask<8;++mask) {
            const glm::vec3 p((mask&1)?bounds.hi.x:bounds.lo.x,(mask&2)?bounds.hi.y:bounds.lo.y,(mask&4)?bounds.hi.z:bounds.lo.z);
            scene_far=std::max(scene_far,glm::dot(p-camera.position,forward));
        }
        scene_far=std::max(camera.near_plane*1.1f,std::min(camera.far_plane,scene_far));
        for(const auto& light:scene.lights)if(light.kind==LightKind::directional) {
            LightMaps maps;maps.light=light;const LightProjection projection(triangles,light.direction);
            if(settings.shadows==ShadowMode::csm) {
                float previous=camera.near_plane;
                for(int i=1;i<=3;++i) {
                    const float fraction=i/3.0f;
                    const float split=0.65f*camera.near_plane*std::pow(scene_far/camera.near_plane,fraction)
                        +0.35f*(camera.near_plane+(scene_far-camera.near_plane)*fraction);
                    maps.layers.emplace_back(cascade_projection(projection,camera,previous,split,
                        float(std::max(1,settings.render_width))/std::max(1,settings.render_height)),bvh,resolution,previous,split);
                    previous=split;
                }
            } else maps.layers.emplace_back(projection,bvh,resolution,0.0f,scene_far);
            light_maps.push_back(std::move(maps));
        }
        if(settings.gi==GiMode::rsm||settings.gi==GiMode::lpv) {
            for(const auto& light:scene.lights) {
                ReflectiveShadowMap rsm;rsm.build(scene,triangles,bvh,light,std::clamp(resolution/4,16,48));rsms.push_back(std::move(rsm));
            }
        }
        if(settings.gi==GiMode::lpv) {
            const glm::vec3 center=(bounds.lo+bounds.hi)*0.5f;
            const glm::vec3 half(max_component(bounds.hi-bounds.lo)*0.65f);
            lpv=std::make_unique<LightPropagationVolume>(center-half,center+half,16);
            // 保守体素化会填满接收面相交格；查询移出这一层，避免直接读到零遮挡格。
            lpv_surface_offset=1.5f*(2*half.x/16);
            VoxelRadianceVolume occupancy(center-half,center+half,16);occupancy.voxelize(triangles,{});
            std::vector<float> cells(16*16*16,0);
            for(int z=0;z<16;++z)for(int y=0;y<16;++y)for(int x=0;x<16;++x)
                cells[grid_index(x,y,z,16)]=occupancy.sample(center-half+(glm::vec3(x,y,z)+glm::vec3(0.5f))*(2.0f*half/16.0f)).a;
            lpv->set_occupancy(std::move(cells));
            for(const auto& rsm:rsms)lpv->inject(rsm.samples());
            lpv->propagate(std::clamp(settings.propagation_steps,0,16));
        }
        if(settings.gi==GiMode::voxel) {
            voxels=std::make_unique<VoxelRadianceVolume>(bounds.lo,bounds.hi,settings.voxel_resolution);
            voxels->voxelize(triangles,[&](glm::vec3 p,glm::vec3 n,int material) {
                const Material m=base_material(scene,material);
                return glm::max(m.emissive,glm::vec3(0))+diffuse_color(m)*light_irradiance(scene,bvh,p,n,settings.shadow_bias)/pi;
            });
        }
    }
    float layer_visibility(const ShadowLayer& layer,const SurfaceSample& surface) const {
        const float bias=std::max(0.0f,settings.shadow_bias);
        const glm::vec3 projected=layer.projection.project(surface.position+safe_normalize(surface.normal)*bias);
        if(!valid_uv(glm::vec2(projected))||projected.z<0||projected.z>1)return 1;
        const auto uv=glm::vec2(projected);const float span=layer.projection.hi.z-layer.projection.lo.z;
        const float z=projected.z-bias/span;
        const float scale=std::tan(std::clamp(settings.light_size,0.0f,0.75f))*span/layer.projection.texel_size(layer.map.depth().width);
        const int search=std::clamp(int(std::ceil(scale)),1,layer.map.depth().width);
        switch(settings.shadows) {
        case ShadowMode::hard:return layer.map.hard(uv,z);
        case ShadowMode::pcf:return layer.map.pcf(uv,z,1);
        case ShadowMode::pcss:return layer.map.pcss(uv,z,search,scale);
        case ShadowMode::vsm: {
            const auto m=layer.map.filtered_moments(uv,2);return cantelli_visibility({m.x,m.y},z);
        }
        case ShadowMode::vssm:return layer.map.vssm(uv,z,search,scale);
        case ShadowMode::msm:return msm_visibility(layer.map.filtered_moments(uv,2),z);
        case ShadowMode::csm:return layer.map.pcf(uv,z,1);
        }
        return 1;
    }
    float visibility(const SurfaceSample& surface,const Light& light) const {
        if(light.kind==LightKind::directional)for(const auto& maps:light_maps)
            if(glm::all(glm::equal(maps.light.direction,light.direction))) {
                const float depth=glm::dot(surface.position-camera.position,safe_normalize(camera.target-camera.position,{0,0,-1}));
                std::size_t index=0;while(index+1<maps.layers.size()&&depth>maps.layers[index].split_far)++index;
                const auto& layer=maps.layers[index];const float visibility=layer_visibility(layer,surface);
                if(index+1>=maps.layers.size())return visibility;
                const float blend_width=(layer.split_far-layer.split_near)*0.1f;
                const float blend=saturate((depth-(layer.split_far-blend_width))/std::max(blend_width,1e-6f));
                return glm::mix(visibility,layer_visibility(maps.layers[index+1],surface),blend);
            }
        const float bias=std::max(1e-4f,settings.shadow_bias);const glm::vec3 origin=surface.position+safe_normalize(surface.normal)*bias;
        if(light.kind==LightKind::directional)return bvh.occluded({origin,-safe_normalize(light.direction),bias,1e30f})?0.0f:1.0f;
        const int count=light.kind==LightKind::rectangle&&settings.shadows!=ShadowMode::hard?16:1;
        const Basis frame(light.direction);float visible=0;
        for(int i=0;i<count;++i) {
            glm::vec3 emitter=light.position;
            if(count>1)emitter+=frame.x*(((i+0.5f)/count-0.5f)*light.size.x)
                +frame.y*((radical_inverse(std::uint32_t(i))-0.5f)*light.size.y);
            const glm::vec3 delta=emitter-origin;const float distance=glm::length(delta);
            if(distance<=bias||!bvh.occluded({origin,delta/std::max(distance,1e-12f),bias,distance-bias}))visible+=1;
        }
        return visible/count;
    }
};

ShadowGi::ShadowGi(const Scene& scene,const std::vector<Triangle>& triangles,const Camera& camera,const Settings& settings)
    :impl_(std::make_shared<Impl>(scene,triangles,camera,settings)) {}
float ShadowGi::visibility(const SurfaceSample& surface,const Light& light) const { return impl_->visibility(surface,light); }
glm::vec3 ShadowGi::indirect(const SurfaceSample& surface,glm::vec3 view_direction) const {
    const glm::vec3 n=safe_normalize(surface.normal),albedo=diffuse_color(surface);
    if(impl_->settings.gi==GiMode::rsm) {
        glm::vec3 sum(0);const float minimum=std::max(0.01f,impl_->scene_diagonal*0.001f);
        const auto occluded=[&](const Ray& ray){return impl_->bvh.occluded(ray);};
        for(const auto& rsm:impl_->rsms)sum+=rsm.gather(surface.position,n,albedo,
            std::clamp(impl_->settings.samples*8,16,128),minimum,occluded);
        return sum;
    }
    if(impl_->settings.gi==GiMode::lpv&&impl_->lpv)
        return impl_->lpv->irradiance(surface.position+n*impl_->lpv_surface_offset,n)*albedo/pi;
    if(impl_->settings.gi==GiMode::voxel&&impl_->voxels) {
        const auto& volume=*impl_->voxels;const glm::vec3 origin=surface.position+n*volume.cell_size()*1.1f;
        constexpr int count=6;glm::vec3 diffuse(0);
        for(int i=0;i<count;++i) {
            const glm::vec3 direction=cosine_direction(n,(i+0.5f)/count,radical_inverse(std::uint32_t(i)));
            diffuse+=volume.trace(origin,direction,0.4f,impl_->scene_diagonal).radiance;
        }
        const glm::vec3 v=safe_normalize(view_direction,n),reflection=safe_normalize(glm::reflect(-v,n));
        const auto specular=volume.trace(origin,reflection,0.02f+0.6f*saturate(surface.roughness)*saturate(surface.roughness),impl_->scene_diagonal);
        const glm::vec3 f0=glm::mix(glm::vec3(0.04f),glm::clamp(surface.albedo,glm::vec3(0),glm::vec3(1)),saturate(surface.metallic));
        const glm::vec3 fresnel=f0+(glm::vec3(1)-f0)*std::pow(1-saturate(glm::dot(n,v)),5.0f);
        return diffuse*(albedo*(glm::vec3(1)-fresnel)/float(count))+specular.radiance*fresnel;
    }
    return glm::vec3(0);
}
} // namespace emberframe::lab
