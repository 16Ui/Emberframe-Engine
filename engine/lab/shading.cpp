#include "shading.h"
#include <cmath>
#include <numeric>

namespace emberframe::lab {
namespace {
float sat(float x) { return std::clamp(x,0.0f,1.0f); }
float pow5(float x) { float x2=x*x;return x2*x2*x; }
float clean(float x) { return std::isfinite(x)?x:0.0f; }
glm::vec3 positive(glm::vec3 c) { return {std::max(0.0f,clean(c.x)),std::max(0.0f,clean(c.y)),std::max(0.0f,clean(c.z))}; }
void bound(int value,int minimum,int maximum,const char* name) {
    if(value<minimum||value>maximum) throw std::invalid_argument(name);
}
void budget(std::uint64_t work,std::uint64_t maximum=16777216) {
    if(work>maximum) throw std::invalid_argument("Precomputation work budget exceeded");
}
int wrap(int x,int n) { return (x%n+n)%n; }
glm::vec4 decode(glm::vec4 c,bool srgb) {
    if(srgb) { c.r=srgb_to_linear(c.r);c.g=srgb_to_linear(c.g);c.b=srgb_to_linear(c.b); } return c;
}
glm::vec4 encode(glm::vec4 c,bool srgb) {
    if(srgb) { c.r=linear_to_srgb(c.r);c.g=linear_to_srgb(c.g);c.b=linear_to_srgb(c.b); } return c;
}
glm::vec4 texel(const Texture& t,int level,int x,int y) {
    const auto& im=t.levels[std::size_t(level)];
    return decode(im.at(wrap(x,im.width),wrap(y,im.height)),t.srgb);
}
glm::vec4 texture_level(const Texture& t,glm::vec2 uv,int level,bool linear) {
    const auto& im=t.levels[std::size_t(level)];
    if(im.empty()) return glm::vec4(1);
    uv=glm::fract(uv);
    const glm::vec2 p=uv*glm::vec2(im.width,im.height)-(linear?0.5f:0.0f);
    const int x=int(std::floor(p.x)),y=int(std::floor(p.y));
    if(!linear) return texel(t,level,x,y);
    const glm::vec2 f=glm::fract(p);
    return glm::mix(glm::mix(texel(t,level,x,y),texel(t,level,x+1,y),f.x),
                    glm::mix(texel(t,level,x,y+1),texel(t,level,x+1,y+1),f.x),f.y);
}
glm::vec4 texture_trilinear(const Texture& t,glm::vec2 uv,float lod) {
    lod=std::clamp(clean(lod),0.0f,float(t.levels.size()-1));
    const int lo=int(lod),hi=std::min(lo+1,int(t.levels.size()-1));
    return glm::mix(texture_level(t,uv,lo,true),texture_level(t,uv,hi,true),lod-float(lo));
}
const Texture* texture(const Scene& scene,int index) {
    return index>=0&&std::size_t(index)<scene.textures.size()?&scene.textures[std::size_t(index)]:nullptr;
}
Material surface_material(const Scene& scene,const SurfaceSample& s) {
    if(s.material>=0&&std::size_t(s.material)<scene.materials.size()) return scene.materials[std::size_t(s.material)];
    return Material{};
}
void mips(Texture& t,bool normals) {
    if(t.levels.empty()||t.levels.front().empty()) { t.levels.clear();return; }
    if(normals&&t.srgb) throw std::invalid_argument("Normal textures must be linear");
    t.levels.resize(1);
    while(t.levels.back().width>1||t.levels.back().height>1) {
        const auto& src=t.levels.back();
        const int w=std::max(1,src.width/2),h=std::max(1,src.height/2);
        Image<glm::vec4> dst(w,h);
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
            // NPOT 的目标像素覆盖连续面积；边缘纹素不能因整除丢失。
            const double x0=double(x)*src.width/w,x1=double(x+1)*src.width/w;
            const double y0=double(y)*src.height/h,y1=double(y+1)*src.height/h;
            glm::dvec4 sum(0);double weight=0;
            for(int sy=int(y0);sy<int(std::ceil(y1));++sy) for(int sx=int(x0);sx<int(std::ceil(x1));++sx) {
                const double a=(std::min(x1,double(sx+1))-std::max(x0,double(sx)))
                              *(std::min(y1,double(sy+1))-std::max(y0,double(sy)));
                glm::vec4 c=decode(src.at(sx,sy),t.srgb);
                if(normals) c=glm::vec4(glm::vec3(c)*2.0f-1.0f,c.a);
                sum+=glm::dvec4(c)*a;weight+=a;
            }
            glm::vec4 c(sum/weight);
            if(normals) c=glm::vec4(safe_normalize(glm::vec3(c),{0,0,1})*0.5f+0.5f,c.a);
            dst.at(x,y)=encode(c,t.srgb);
        }
        t.levels.push_back(std::move(dst));
    }
}
template<class T> T grid(const Image<T>& im,float x,float y) {
    if(im.empty()) return T{};
    x=sat(x)*float(im.width-1);y=sat(y)*float(im.height-1);
    const int ix=int(x),iy=int(y);const float fx=x-ix,fy=y-iy;
    return (im.clamped(ix,iy)*(1-fx)+im.clamped(ix+1,iy)*fx)*(1-fy)
          +(im.clamped(ix,iy+1)*(1-fx)+im.clamped(ix+1,iy+1)*fx)*fy;
}
glm::vec2 material_alpha(const Material& m,const Settings& settings) {
    const float r=std::clamp(m.roughness,0.02f,1.0f),a=r*r;
    const float an=settings.shading==ShadingMode::disney?std::clamp(m.anisotropy,-0.95f,0.95f):0.0f;
    const float aspect=std::sqrt(1-0.9f*std::abs(an));
    glm::vec2 result(a/aspect,a*aspect);
    if(an<0) std::swap(result.x,result.y);
    return glm::max(result,glm::vec2(0.0004f));
}
float smith_lambda(glm::vec3 w,float ax,float ay) {
    const double z=std::abs(double(w.z));
    if(z<1e-10) return 1e20f;
    const double a=(double(ax)*w.x)*(double(ax)*w.x)+(double(ay)*w.y)*(double(ay)*w.y);
    return float(0.5*(std::sqrt(1+a/(z*z))-1));
}
glm::vec2 integrate_brdf(float nv,float roughness,int samples) {
    const float a=std::max(0.0004f,roughness*roughness);
    nv=std::max(0.0001f,nv);const glm::vec3 v(std::sqrt(std::max(0.0f,1-nv*nv)),0,nv);
    glm::dvec2 sum(0);
    for(int i=0;i<samples;++i) {
        const auto s=sample_ggx(v,a,a,hammersley(std::uint32_t(i),std::uint32_t(samples)));
        if(!s.valid) continue;
        const glm::vec3 h=safe_normalize(v+s.direction,{0,0,1});
        // 可见法线 PDF 抵消 D 和雅可比；剩下 G2/G1，避免掠射处巨大权重。
        const float weight=smith_g2(v,s.direction,a,a)/std::max(1e-20f,smith_g1(v,a,a));
        const float f=pow5(1-sat(glm::dot(v,h)));
        sum+=glm::dvec2((1-f)*weight,f*weight);
    }
    return glm::vec2(sum/double(samples));
}
}

float srgb_to_linear(float x) {
    x=std::max(0.0f,clean(x));return x<=0.04045f?x/12.92f:std::pow((x+0.055f)/1.055f,2.4f);
}
float linear_to_srgb(float x) {
    x=std::max(0.0f,clean(x));return x<=0.0031308f?12.92f*x:1.055f*std::pow(x,1.0f/2.4f)-0.055f;
}
glm::vec3 tone_map(glm::vec3 color,float exposure) {
    color=positive(color);const double e=std::max(0.0f,clean(exposure));
    for(int i=0;i<3;++i) {
        const double x=std::min(double(color[i])*e,1e12);
        color[i]=linear_to_srgb(sat(float((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14))));
    }
    return color;
}
glm::vec3 environment(const Scene& scene,glm::vec3 direction) {
    direction=safe_normalize(direction);return positive(glm::mix(scene.sky_bottom,scene.sky_top,sat(direction.y*0.5f+0.5f)));
}
void build_mips(Texture& t) { mips(t,false); }
void build_normal_mips(Texture& t) { mips(t,true); }
glm::vec4 sample_texture(const Texture& t,glm::vec2 uv,FilterMode mode,float lod,glm::vec2 dx,glm::vec2 dy) {
    if(t.levels.empty()||t.levels.front().empty()) return glm::vec4(1);
    uv={clean(uv.x),clean(uv.y)};dx={clean(dx.x),clean(dx.y)};dy={clean(dy.x),clean(dy.y)};
    const glm::vec2 size(t.levels.front().width,t.levels.front().height);
    const glm::vec2 x=dx*size,y=dy*size;
    // 纹理空间协方差 J*J^T 的特征值给出足迹长短轴，能处理旋转与剪切。
    const float a=x.x*x.x+y.x*y.x,b=x.x*x.y+y.x*y.y,c=x.y*x.y+y.y*y.y;
    const float disc=std::sqrt(std::max(0.0f,(a-c)*(a-c)+4*b*b));
    const float major=std::sqrt(std::max(0.0f,(a+c+disc)*0.5f));
    const float minor=std::sqrt(std::max(0.0f,(a+c-disc)*0.5f));
    const float auto_lod=std::log2(std::max(1.0f,major));
    if(mode==FilterMode::anisotropic&&major>1) {
        const float footprint=std::max({1.0f,minor,major/16.0f});
        const int taps=std::clamp(int(std::ceil(major/footprint)),1,16);
        glm::vec2 axis=std::abs(b)>1e-10f?glm::normalize(glm::vec2(b,(a+c+disc)*0.5f-a))
                                              :(a>=c?glm::vec2(1,0):glm::vec2(0,1));
        const glm::vec2 uv_axis=axis*major/size;
        glm::vec4 sum(0);
        for(int i=0;i<taps;++i) sum+=texture_trilinear(t,uv+uv_axis*((float(i)+0.5f)/taps-0.5f),std::max(lod,std::log2(footprint)));
        return sum/float(taps);
    }
    lod=std::clamp(std::max(clean(lod),auto_lod),0.0f,float(t.levels.size()-1));
    if(mode==FilterMode::nearest||mode==FilterMode::bilinear)
        return texture_level(t,uv,int(std::floor(lod+0.5f)),mode==FilterMode::bilinear);
    return texture_trilinear(t,uv,lod);
}
Material sample_material(const Scene& scene,const SurfaceSample& s,const Settings& settings) {
    return sample_material(scene,s,settings,glm::vec2(0),glm::vec2(0));
}
Material sample_material(const Scene& scene,const SurfaceSample& s,const Settings& settings,glm::vec2 dx,glm::vec2 dy) {
    Material m=surface_material(scene,s);m.base_color*=s.vertex_color;
    if(auto t=texture(scene,m.base_texture)) m.base_color*=sample_texture(*t,s.uv,settings.filter,0,dx,dy);
    if(auto t=texture(scene,m.mr_texture)) {
        const auto mr=sample_texture(*t,s.uv,settings.filter,0,dx,dy);m.roughness*=mr.g;m.metallic*=mr.b;
    }
    if(auto t=texture(scene,m.emissive_texture)) m.emissive*=glm::vec3(sample_texture(*t,s.uv,settings.filter,0,dx,dy));
    m.base_color=glm::max(m.base_color,glm::vec4(0));m.metallic=sat(m.metallic);m.roughness=std::clamp(m.roughness,0.02f,1.0f);
    return m;
}
glm::vec3 sample_normal(const Scene& scene,const SurfaceSample& s,const Settings& settings) {
    return sample_normal(scene,s,settings,glm::vec2(0),glm::vec2(0));
}
glm::vec3 sample_normal(const Scene& scene,const SurfaceSample& s,const Settings& settings,glm::vec2 dx,glm::vec2 dy) {
    const Material m=surface_material(scene,s);const auto f=shading_frame(s.normal,s.tangent);
    if(auto t=texture(scene,m.normal_texture)) {
        glm::vec3 v=glm::vec3(sample_texture(*t,s.uv,settings.filter,0,dx,dy))*2.0f-1.0f;
        v.x*=m.normal_scale;v.y*=m.normal_scale;return safe_normalize(f.world(safe_normalize(v,{0,0,1})),f.n);
    }
    return f.n;
}
float sample_occlusion(const Scene& scene,const SurfaceSample& s,const Settings& settings) {
    return sample_occlusion(scene,s,settings,glm::vec2(0),glm::vec2(0));
}
float sample_occlusion(const Scene& scene,const SurfaceSample& s,const Settings& settings,glm::vec2 dx,glm::vec2 dy) {
    const auto m=surface_material(scene,s);
    if(auto t=texture(scene,m.ao_texture)) return glm::mix(1.0f,sat(sample_texture(*t,s.uv,settings.filter,0,dx,dy).r),sat(m.ao_strength));
    return 1;
}
glm::vec3 ShadingFrame::local(glm::vec3 w) const { return {glm::dot(w,t),glm::dot(w,b),glm::dot(w,n)}; }
glm::vec3 ShadingFrame::world(glm::vec3 w) const { return t*w.x+b*w.y+n*w.z; }
ShadingFrame shading_frame(glm::vec3 n,glm::vec4 tangent) {
    ShadingFrame f;f.n=safe_normalize(n,{0,0,1});
    glm::vec3 t=glm::vec3(tangent)-f.n*glm::dot(f.n,glm::vec3(tangent));
    if(glm::dot(t,t)<1e-12f) t=glm::cross(std::abs(f.n.z)<0.999f?glm::vec3(0,0,1):glm::vec3(0,1,0),f.n);
    f.t=safe_normalize(t,{1,0,0});f.b=glm::cross(f.n,f.t)*(tangent.w<0?-1.0f:1.0f);return f;
}
glm::vec2 hammersley(std::uint32_t i,std::uint32_t count) {
    if(count==0) throw std::invalid_argument("Hammersley count is zero");
    std::uint32_t bits=i;bits=(bits<<16)|(bits>>16);
    bits=((bits&0x55555555u)<<1)|((bits&0xaaaaaaaau)>>1);
    bits=((bits&0x33333333u)<<2)|((bits&0xccccccccu)>>2);
    bits=((bits&0x0f0f0f0fu)<<4)|((bits&0xf0f0f0f0u)>>4);
    bits=((bits&0x00ff00ffu)<<8)|((bits&0xff00ff00u)>>8);
    return {(float(i)+0.5f)/float(count),float(double(bits)*2.3283064365386963e-10)};
}
DirectionSample sample_uniform_sphere(glm::vec2 u) {
    const float z=1-2*sat(u.x),r=std::sqrt(std::max(0.0f,1-z*z)),phi=2*pi*u.y;
    return {{r*std::cos(phi),r*std::sin(phi),z},1/(4*pi),true};
}
DirectionSample sample_uniform_hemisphere(glm::vec2 u) {
    const float z=sat(u.x),r=std::sqrt(std::max(0.0f,1-z*z)),phi=2*pi*u.y;
    return {{r*std::cos(phi),r*std::sin(phi),z},1/(2*pi),true};
}
DirectionSample sample_cosine_hemisphere(glm::vec2 u) {
    const float r=std::sqrt(sat(u.x)),phi=2*pi*u.y,z=std::sqrt(std::max(0.0f,1-sat(u.x)));
    return {{r*std::cos(phi),r*std::sin(phi),z},z/pi,z>0};
}
float cosine_hemisphere_pdf(float c) { return std::max(0.0f,c)/pi; }
float ggx_distribution(glm::vec3 h,float ax,float ay) {
    if(h.z<=0) return 0;
    ax=std::max(ax,0.0004f);ay=std::max(ay,0.0004f);
    const double x=double(h.x)/ax,y=double(h.y)/ay,z=h.z,q=x*x+y*y+z*z;
    return float(1/(double(pi)*ax*ay*q*q));
}
float smith_g1(glm::vec3 w,float ax,float ay) { return w.z>0?1/(1+smith_lambda(w,ax,ay)):0; }
float smith_g2(glm::vec3 v,glm::vec3 l,float ax,float ay) {
    return v.z>0&&l.z>0?1/(1+smith_lambda(v,ax,ay)+smith_lambda(l,ax,ay)):0;
}
glm::vec3 schlick_fresnel(glm::vec3 f0,float c) { return f0+(glm::vec3(1)-f0)*pow5(1-sat(c)); }
DirectionSample sample_ggx(glm::vec3 v,float ax,float ay,glm::vec2 u) {
    const glm::vec3 input_view=v;
    v=safe_normalize(v,{0,0,1});if(v.z<=0) return {};
    ax=std::max(ax,0.0004f);ay=std::max(ay,0.0004f);
    // Heitz 2018：先拉伸观察方向，在可见投影圆盘采样，再逆拉伸法线。
    const glm::vec3 vh=safe_normalize(glm::vec3(ax*v.x,ay*v.y,v.z),{0,0,1});
    const glm::vec3 t1=vh.z<0.99999f?safe_normalize(glm::cross(glm::vec3(0,0,1),vh),{1,0,0}):glm::vec3(1,0,0);
    const glm::vec3 t2=glm::cross(vh,t1);
    const float r=std::sqrt(std::min(sat(u.x),0.99999994f)),phi=2*pi*u.y;
    const float p1=r*std::cos(phi),s=0.5f*(1+vh.z);
    const float p2=(1-s)*std::sqrt(std::max(0.0f,1-p1*p1))+s*r*std::sin(phi);
    const glm::vec3 nh=p1*t1+p2*t2+std::sqrt(std::max(0.0f,1-p1*p1-p2*p2))*vh;
    const glm::vec3 h=safe_normalize(glm::vec3(ax*nh.x,ay*nh.y,std::max(0.0f,nh.z)),{0,0,1});
    const glm::vec3 l=glm::reflect(-v,h);
    return {l,ggx_pdf(input_view,l,ax,ay),l.z>0};
}
float ggx_pdf(glm::vec3 v,glm::vec3 l,float ax,float ay) {
    v=safe_normalize(v,{0,0,1});l=safe_normalize(l,{0,0,1});
    if(v.z<=0||glm::dot(v+l,v+l)<1e-16f) return 0;
    const glm::vec3 h=safe_normalize(v+l,{0,0,1});
    if(h.z<=0||glm::dot(v,h)<=0) return 0;
    return ggx_distribution(h,ax,ay)*smith_g1(v,ax,ay)/(4*v.z);
}
float mis_balance(float a,float b) {
    a=std::max(0.0f,clean(a));b=std::max(0.0f,clean(b));const double sum=double(a)+b;return sum>0?float(a/sum):0;
}
float mis_power(float a,float b) {
    a=std::max(0.0f,clean(a));b=std::max(0.0f,clean(b));const double aa=double(a)*a,bb=double(b)*b;return aa+bb>0?float(aa/(aa+bb)):0;
}
float area_to_solid_angle_pdf(float p,float d2,float cosine) {
    return p>0&&d2>0&&cosine>1e-8f?p*d2/cosine:0;
}

EnergyLut precompute_energy_lut(EnergyLutOptions o) {
    bound(o.cosine_resolution,2,128,"Energy cosine resolution");bound(o.roughness_resolution,2,128,"Energy roughness resolution");
    bound(o.samples,16,16384,"Energy samples");budget(std::uint64_t(o.cosine_resolution)*o.roughness_resolution*o.samples);
    EnergyLut table;table.directional.reset(o.cosine_resolution,o.roughness_resolution);table.average.resize(o.roughness_resolution);
    for(int y=0;y<o.roughness_resolution;++y) {
        const float r=float(y)/(o.roughness_resolution-1);
        for(int x=0;x<o.cosine_resolution;++x) {
            const auto ab=integrate_brdf(float(x)/(o.cosine_resolution-1),r,o.samples);
            table.directional.at(x,y)=sat(ab.x+ab.y);
        }
        // Eavg=2∫E(mu)*mu dmu；精确积分每段线性插值，和运行时查表约定一致。
        double integral=0;
        for(int x=0;x<o.cosine_resolution-1;++x) {
            const double lo=double(x)/(o.cosine_resolution-1),hi=double(x+1)/(o.cosine_resolution-1);
            const double ea=table.directional.at(x,y),eb=table.directional.at(x+1,y),slope=(eb-ea)/(hi-lo);
            integral+=2*((ea-slope*lo)*(hi*hi-lo*lo)/2+slope*(hi*hi*hi-lo*lo*lo)/3);
        }
        table.average[std::size_t(y)]=sat(float(integral));
    }
    return table;
}
float directional_albedo(const EnergyLut& t,float cosine,float r) { return grid(t.directional,sat(cosine),sat(r)); }
float average_albedo(const EnergyLut& t,float r) {
    if(t.average.empty()) return 1;
    const float y=sat(r)*float(t.average.size()-1);const auto i=std::size_t(y);
    return glm::mix(t.average[i],t.average[std::min(i+1,t.average.size()-1)],y-float(i));
}
namespace { const EnergyLut shared_energy_lut=precompute_energy_lut(); }
const EnergyLut& default_energy_lut() { return shared_energy_lut; }
glm::vec3 kulla_conty(const EnergyLut& t,glm::vec3 f0,float nv,float nl,float r) {
    if(nv<=0||nl<=0||t.directional.empty()) return glm::vec3(0);
    const float ev=directional_albedo(t,nv,r),el=directional_albedo(t,nl,r),ea=average_albedo(t,r);
    if(1-ea<1e-6f) return glm::vec3(0);
    const float missing=(1-ev)*(1-el)/(pi*(1-ea));
    // Schlick 半球平均 Favg=F0+(1-F0)/21；几何级数补回微表面内部多次反射。
    const glm::vec3 favg=f0+(glm::vec3(1)-f0)/21.0f;
    const glm::vec3 fms=favg*favg*ea/(glm::vec3(1)-favg*(1-ea));
    return fms*missing;
}
glm::vec3 evaluate_brdf(const Material& m,glm::vec3 n,glm::vec3 v,glm::vec3 l,glm::vec4 tangent,const Settings& s) {
    return evaluate_brdf(m,n,v,l,tangent,s,default_energy_lut());
}
glm::vec3 evaluate_brdf(const Material& m,glm::vec3 n,glm::vec3 v,glm::vec3 l,glm::vec4 tangent,const Settings& s,const EnergyLut& lut) {
    const auto frame=shading_frame(n,tangent);v=frame.local(safe_normalize(v));l=frame.local(safe_normalize(l));
    if(v.z<=1e-6f||l.z<=1e-6f) return glm::vec3(0);
    const glm::vec3 h=safe_normalize(v+l,{0,0,1});const float vh=sat(glm::dot(v,h));
    const glm::vec3 base=glm::clamp(glm::vec3(m.base_color),glm::vec3(0),glm::vec3(1));
    const float metal=sat(m.metallic),r=std::clamp(m.roughness,0.02f,1.0f);
    const glm::vec3 f0=glm::mix(glm::vec3(0.04f),base,metal),f=schlick_fresnel(f0,vh);
    if(s.shading==ShadingMode::blinn_phong) {
        const float exponent=std::min(8192.0f,2/(r*r*r*r)-2);
        return base*(1-metal)/pi+f0*((exponent+8)/(8*pi))*std::pow(std::max(0.0f,h.z),exponent);
    }
    if(s.shading==ShadingMode::toon) return base/pi; // banding is a lighting-stage operation
    const glm::vec2 a=material_alpha(m,s);
    glm::vec3 spec=f*(ggx_distribution(h,a.x,a.y)*smith_g2(v,l,a.x,a.y)/(4*v.z*l.z));
    if(s.energy_compensation&&(s.shading!=ShadingMode::disney||std::abs(m.anisotropy)<1e-5f)) spec+=kulla_conty(lut,f0,v.z,l.z,r);
    glm::vec3 diff=(glm::vec3(1)-f)*base*((1-metal)/pi);
    if(s.shading!=ShadingMode::disney) return diff+spec;
    const float fd90=0.5f+2*r*vh*vh;
    const float burley=(1+(fd90-1)*pow5(1-l.z))*(1+(fd90-1)*pow5(1-v.z));
    diff*=burley;
    const float lum=glm::dot(base,glm::vec3(0.3f,0.6f,0.1f));
    const glm::vec3 tint=lum>1e-6f?base/lum:glm::vec3(1);
    const glm::vec3 sheen=sat(m.sheen)*(1-metal)*pow5(1-vh)*glm::mix(glm::vec3(1),tint,0.5f);
    const float coat=sat(m.clearcoat)*0.25f,ca=std::clamp(m.clearcoat_roughness,0.001f,0.999f),ca2=ca*ca;
    const float d=(ca2-1)/(pi*std::log(ca2)*(1+(ca2-1)*h.z*h.z));
    const float cf=0.04f+0.96f*pow5(1-vh);
    const float cg=smith_g1(v,0.25f,0.25f)*smith_g1(l,0.25f,0.25f)/(4*v.z*l.z);
    // 清漆层双向透过率对称，避免层叠时将原有基底能量完整再加一遍。
    const float attenuation=(1-coat*(0.04f+0.96f*pow5(1-v.z)))*(1-coat*(0.04f+0.96f*pow5(1-l.z)));
    return (diff+spec+sheen)*attenuation+glm::vec3(coat*d*cf*cg);
}
float brdf_pdf(const Material& m,glm::vec3 n,glm::vec3 v,glm::vec3 l,glm::vec4 tangent,const Settings& s) {
    const auto frame=shading_frame(n,tangent);v=frame.local(safe_normalize(v));l=frame.local(safe_normalize(l));
    if(v.z<=0||l.z<=0) return 0;
    const auto a=material_alpha(m,s);
    return 0.5f*(cosine_hemisphere_pdf(l.z)+ggx_pdf(v,l,a.x,a.y));
}
BrdfSample sample_brdf(const Material& m,glm::vec3 n,glm::vec3 v,glm::vec4 tangent,const Settings& s,glm::vec3 u) {
    const auto frame=shading_frame(n,tangent);const auto vl=frame.local(safe_normalize(v));if(vl.z<=0) return {};
    const auto a=material_alpha(m,s);const auto sample=u.z<0.5f?sample_cosine_hemisphere(glm::vec2(u)):sample_ggx(vl,a.x,a.y,glm::vec2(u));
    if(!sample.valid) return {};
    const auto l=frame.world(sample.direction);const float p=brdf_pdf(m,n,v,l,tangent,s);
    return {l,evaluate_brdf(m,n,v,l,tangent,s),p,p>0};
}

std::array<float,9> sh9_basis(glm::vec3 d) {
    d=safe_normalize(d,{0,0,1});const float x=d.x,y=d.y,z=d.z;
    return {0.2820947918f,0.4886025119f*y,0.4886025119f*z,0.4886025119f*x,
            1.0925484306f*x*y,1.0925484306f*y*z,0.3153915653f*(3*z*z-1),
            1.0925484306f*x*z,0.5462742153f*(x*x-y*y)};
}
SH9 project_sh9(const RadianceFunction& radiance,int samples) {
    if(!radiance) throw std::invalid_argument("SH requires a radiance callback");
    bound(samples,16,1048576,"SH samples");std::array<glm::dvec3,9> sum{};
    for(int i=0;i<samples;++i) {
        const auto s=sample_uniform_sphere(hammersley(i,samples));const auto y=sh9_basis(s.direction);
        const auto c=glm::dvec3(radiance(s.direction));
        for(int k=0;k<9;++k) sum[k]+=c*double(y[k]);
    }
    SH9 result{};for(int k=0;k<9;++k) result[k]=glm::vec3(sum[k]*(4*double(pi)/samples));return result;
}
glm::vec3 evaluate_sh9(const SH9& c,glm::vec3 d) {
    const auto y=sh9_basis(d);glm::vec3 result(0);for(int k=0;k<9;++k) result+=c[k]*y[k];return result;
}
glm::vec3 evaluate_sh9_irradiance(const SH9& c,glm::vec3 n) {
    const auto y=sh9_basis(n);glm::vec3 result(0);
    for(int k=0;k<9;++k) result+=c[k]*y[k]*(k==0?pi:k<4?2*pi/3:pi/4);
    return result;
}
DiffusePrt bake_diffuse_prt(glm::vec3 position,glm::vec3 normal,const VisibilityFunction& visibility,PrtOptions o) {
    bound(o.samples,16,1048576,"PRT samples");if(o.ray_offset<0) throw std::invalid_argument("PRT ray offset");
    const auto frame=shading_frame(normal);const auto origin=position+frame.n*o.ray_offset;
    std::array<double,9> sum{};
    for(int i=0;i<o.samples;++i) {
        const auto d=frame.world(sample_cosine_hemisphere(hammersley(i,o.samples)).direction);
        const float vis=visibility?sat(visibility(origin,d)):1;const auto y=sh9_basis(d);
        // 余弦重要性采样 PDF=cos/pi，因此传输系数的权重为 pi*visibility。
        for(int k=0;k<9;++k) sum[k]+=double(vis)*y[k];
    }
    DiffusePrt result;for(int k=0;k<9;++k) result.transfer[k]=glm::vec3(float(sum[k]*pi/o.samples));return result;
}
glm::vec3 evaluate_diffuse_prt(const DiffusePrt& transfer,const SH9& env,glm::vec3 albedo) {
    glm::vec3 sum(0);for(int k=0;k<9;++k) sum+=transfer.transfer[k]*env[k];return sum*albedo/pi;
}
namespace {
std::uint32_t mix_bits(std::uint32_t x) {
    x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x;
}
float random_unit(std::uint32_t& state) { state=mix_bits(state+0x9e3779b9u);return float(state>>8)*(1.0f/16777216.0f); }
}
DiffusePrt bake_interreflection_prt(glm::vec3 position,glm::vec3 normal,const PrtTraceFunction& trace,int max_bounces,PrtOptions o) {
    if(!trace) throw std::invalid_argument("Interreflection PRT requires trace callback");
    bound(max_bounces,0,8,"PRT bounce bound");bound(o.samples,16,1048576,"PRT samples");
    if(o.ray_offset<0) throw std::invalid_argument("PRT ray offset");
    const auto frame=shading_frame(normal);std::array<glm::dvec3,9> sum{};
    for(int i=0;i<o.samples;++i) {
        std::uint32_t state=mix_bits(std::uint32_t(i)+42);
        Ray ray;ray.origin=position+frame.n*o.ray_offset;ray.t_min=o.ray_offset;
        ray.direction=frame.world(sample_cosine_hemisphere(hammersley(i,o.samples)).direction);
        glm::dvec3 throughput(1);
        for(int bounce=0;bounce<=max_bounces;++bounce) {
            auto hit=trace(ray);
            if(!hit) {
                const auto y=sh9_basis(ray.direction);for(int k=0;k<9;++k) sum[k]+=throughput*double(y[k]);break;
            }
            if(bounce==max_bounces) break;
            throughput*=glm::dvec3(glm::clamp(hit->albedo,glm::vec3(0),glm::vec3(1)));
            const auto hf=shading_frame(hit->normal);ray.origin=hit->position+hf.n*o.ray_offset;
            const float u=random_unit(state),v=random_unit(state);
            ray.direction=hf.world(sample_cosine_hemisphere({u,v}).direction);
        }
    }
    DiffusePrt result;for(int k=0;k<9;++k) result.transfer[k]=glm::vec3(sum[k]*(double(pi)/o.samples));return result;
}
GlossyPrt bake_glossy_prt(glm::vec3 position,glm::vec3 normal,glm::vec4 tangent,const Material& m,
    const Settings& settings,const VisibilityFunction& visibility,int outgoing_samples,int incoming_samples) {
    bound(outgoing_samples,8,1024,"Glossy PRT outgoing samples");bound(incoming_samples,16,4096,"Glossy PRT incoming samples");
    budget(std::uint64_t(outgoing_samples)*incoming_samples,1048576);
    const auto frame=shading_frame(normal,tangent);GlossyPrt result;
    std::array<std::array<glm::dvec3,9>,9> sum{};
    for(int o=0;o<outgoing_samples;++o) {
        const auto v=frame.world(sample_uniform_hemisphere(hammersley(o,outgoing_samples)).direction);const auto yo=sh9_basis(v);
        for(int i=0;i<incoming_samples;++i) {
            const auto local=sample_cosine_hemisphere(hammersley(i,incoming_samples));const auto l=frame.world(local.direction);
            const auto yi=sh9_basis(l);const float vis=visibility?sat(visibility(position+frame.n*1e-4f,l)):1;
            const glm::dvec3 f(evaluate_brdf(m,normal,v,l,tangent,settings)*vis);
            for(int a=0;a<9;++a) for(int b=0;b<9;++b) sum[a][b]+=f*double(yo[a]*yi[b]);
        }
    }
    const double weight=2*double(pi)*pi/(double(outgoing_samples)*incoming_samples);
    for(int a=0;a<9;++a) for(int b=0;b<9;++b) result.transfer[a][b]=glm::vec3(sum[a][b]*weight);
    return result;
}
SH9 apply_glossy_prt(const GlossyPrt& prt,const SH9& env) {
    SH9 outgoing{};for(int a=0;a<9;++a) for(int b=0;b<9;++b) outgoing[a]+=prt.transfer[a][b]*env[b];return outgoing;
}

namespace {
glm::vec3 latlong_direction(float u,float v) {
    const float theta=pi*v,phi=2*pi*u;return {std::sin(theta)*std::cos(phi),std::cos(theta),std::sin(theta)*std::sin(phi)};
}
glm::vec3 sample_latlong(const Image<glm::vec3>& im,glm::vec3 direction) {
    if(im.empty()) return glm::vec3(0);
    direction=safe_normalize(direction,{0,1,0});
    float u=std::atan2(direction.z,direction.x)/(2*pi);u-=std::floor(u);
    const float v=std::acos(std::clamp(direction.y,-1.0f,1.0f))/pi;
    const float x=u*im.width-0.5f,y=v*im.height-0.5f;const int ix=int(std::floor(x)),iy=int(std::floor(y));
    auto get=[&](int xx,int yy) { return im.at(wrap(xx,im.width),std::clamp(yy,0,im.height-1)); };
    return glm::mix(glm::mix(get(ix,iy),get(ix+1,iy),x-ix),glm::mix(get(ix,iy+1),get(ix+1,iy+1),x-ix),y-iy);
}
}
IblData precompute_ibl(const RadianceFunction& env,IblOptions o) {
    if(!env) throw std::invalid_argument("IBL requires radiance callback");
    bound(o.width,2,256,"IBL width");bound(o.height,2,128,"IBL height");bound(o.roughness_levels,2,10,"IBL roughness levels");
    bound(o.lut_resolution,2,128,"IBL LUT resolution");bound(o.samples,16,4096,"IBL samples");
    budget((std::uint64_t(o.width)*o.height*3+std::uint64_t(o.lut_resolution)*o.lut_resolution)*o.samples);
    IblData result;result.diffuse.reset(o.width,o.height);result.brdf.reset(o.lut_resolution,o.lut_resolution);
    for(int y=0;y<o.height;++y) for(int x=0;x<o.width;++x) {
        const auto n=latlong_direction((x+0.5f)/o.width,(y+0.5f)/o.height);const auto frame=shading_frame(n);
        glm::dvec3 sum(0);
        for(int i=0;i<o.samples;++i) sum+=glm::dvec3(env(frame.world(sample_cosine_hemisphere(hammersley(i,o.samples)).direction)));
        result.diffuse.at(x,y)=glm::vec3(sum*(double(pi)/o.samples));
    }
    for(int level=0;level<o.roughness_levels;++level) {
        const int w=std::max(1,o.width>>level),h=std::max(1,o.height>>level);Image<glm::vec3> map(w,h);
        const float r=float(level)/(o.roughness_levels-1),a=std::max(0.0004f,r*r);
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
            const auto n=latlong_direction((x+0.5f)/w,(y+0.5f)/h);const auto frame=shading_frame(n);
            if(level==0) { map.at(x,y)=env(n);continue; }
            glm::dvec3 sum(0);double weight=0;
            for(int i=0;i<o.samples;++i) {
                const auto s=sample_ggx({0,0,1},a,a,hammersley(i,o.samples));if(!s.valid) continue;
                // Split-sum 预过滤采用 N=V=R，N.L 归一化；不是普通颜色 Mip。
                sum+=glm::dvec3(env(frame.world(s.direction)))*double(s.direction.z);weight+=s.direction.z;
            }
            map.at(x,y)=weight>0?glm::vec3(sum/weight):env(n);
        }
        result.specular.push_back(std::move(map));
    }
    for(int y=0;y<o.lut_resolution;++y) for(int x=0;x<o.lut_resolution;++x)
        result.brdf.at(x,y)=integrate_brdf(float(x)/(o.lut_resolution-1),float(y)/(o.lut_resolution-1),o.samples);
    return result;
}
glm::vec3 sample_ibl_diffuse(const IblData& ibl,glm::vec3 n) { return sample_latlong(ibl.diffuse,n); }
glm::vec3 sample_ibl_specular(const IblData& ibl,glm::vec3 r,float roughness) {
    if(ibl.specular.empty()) return glm::vec3(0);
    const float level=sat(roughness)*float(ibl.specular.size()-1);const auto lo=std::size_t(level);
    return glm::mix(sample_latlong(ibl.specular[lo],r),sample_latlong(ibl.specular[std::min(lo+1,ibl.specular.size()-1)],r),level-float(lo));
}
glm::vec2 sample_brdf_lut(const IblData& ibl,float nv,float r) { return grid(ibl.brdf,sat(nv),sat(r)); }
glm::vec3 evaluate_ibl_specular(const IblData& ibl,glm::vec3 n,glm::vec3 v,glm::vec3 f0,float r) {
    n=safe_normalize(n);v=safe_normalize(v);const float nv=glm::dot(n,v);if(nv<=0) return glm::vec3(0);
    const auto ab=sample_brdf_lut(ibl,nv,r);return sample_ibl_specular(ibl,glm::reflect(-v,n),r)*(f0*ab.x+glm::vec3(ab.y));
}

namespace {
glm::mat3 ltc_matrix(const std::array<float,4>& p) {
    const float c=std::cos(p[3]),s=std::sin(p[3]);
    const glm::mat3 rotation(glm::vec3(c,0,-s),glm::vec3(0,1,0),glm::vec3(s,0,c));
    const glm::mat3 shape(glm::vec3(std::exp(p[0]),0,0),glm::vec3(0,std::exp(p[1]),0),glm::vec3(p[2],0,1));
    return rotation*shape;
}
float ltc_density(glm::vec3 direction,const glm::mat3& inverse,float determinant) {
    const glm::vec3 q=inverse*direction;if(q.z<=0) return 0;
    const double length2=glm::dot(q,q);
    // 方向归一化的雅可比为 1/(det(M)*|M^-1*w|^3)，余弦再除一次长度。
    return float(q.z/(double(pi)*determinant*length2*length2));
}
struct FitSample { glm::vec3 direction;float target,weight; };
LtcEntry fit_ltc(float nv,float roughness,int samples,int iterations) {
    nv=std::max(nv,0.02f);roughness=std::max(roughness,0.08f);
    const float alpha=roughness*roughness;const glm::vec3 v(std::sqrt(1-nv*nv),0,nv);
    const glm::vec2 ab=integrate_brdf(nv,roughness,samples*2);const float energy=std::max(1e-5f,ab.x+ab.y);
    std::vector<FitSample> points;points.reserve(samples*2);
    for(int technique=0;technique<2;++technique) for(int i=0;i<samples;++i) {
        const auto u=hammersley(i,samples);
        const glm::vec3 l=technique?sample_ggx(v,alpha,alpha,u).direction:sample_uniform_sphere(u).direction;
        const float proposal=0.5f*(1/(4*pi)+ggx_pdf(v,l,alpha,alpha));
        float target=0;
        if(l.z>0) {
            const auto h=safe_normalize(v+l,{0,0,1});
            target=ggx_distribution(h,alpha,alpha)*smith_g2(v,l,alpha,alpha)/(4*nv*energy);
        }
        points.push_back({l,target,1/proposal});
    }
    auto objective=[&](const std::array<float,4>& p) {
        const auto matrix=ltc_matrix(p),inverse=glm::inverse(matrix);const float determinant=glm::determinant(matrix);
        double error=0;
        for(const auto& point:points) {
            const double d=std::sqrt(ltc_density(point.direction,inverse,determinant))-std::sqrt(point.target);
            error+=d*d*point.weight;
        }
        return float(error/points.size());
    };
    std::array<float,4> p{std::log(std::max(0.02f,2*alpha)),std::log(std::max(0.02f,2*alpha)),0,-std::acos(nv)};
    std::array<float,4> step{0.65f,0.65f,0.5f,0.3f};float error=objective(p);
    for(int iteration=0;iteration<iterations;++iteration) {
        bool improved=false;
        for(int axis=0;axis<4;++axis) {
            auto best=p;float best_error=error;
            for(float sign:{-1.0f,1.0f}) {
                auto trial=p;trial[axis]+=sign*step[axis];
                trial[0]=std::clamp(trial[0],-7.0f,3.0f);trial[1]=std::clamp(trial[1],-7.0f,3.0f);
                trial[2]=std::clamp(trial[2],-8.0f,8.0f);trial[3]=std::clamp(trial[3],-1.56f,0.4f);
                const float candidate=objective(trial);if(candidate<best_error) { best=trial;best_error=candidate; }
            }
            if(best_error<error) { p=best;error=best_error;improved=true; }
        }
        if(!improved) for(auto& value:step) value*=0.55f;
    }
    return {glm::inverse(ltc_matrix(p)),ab,error};
}
ShadingFrame ltc_frame(glm::vec3 n,glm::vec3 v) { return shading_frame(n,glm::vec4(v,1)); }
bool emitting(const RectangleLight& light,glm::vec3 position) {
    const auto cross=glm::cross(light.half_u,light.half_v);
    if(glm::dot(cross,cross)<1e-16f) return false;
    return light.two_sided||glm::dot(cross,position-light.center)>1e-8f;
}
std::vector<glm::dvec3> clip_horizon(const std::vector<glm::dvec3>& polygon) {
    std::vector<glm::dvec3> result;if(polygon.empty()) return result;result.reserve(polygon.size()+2);
    auto a=polygon.back();bool inside_a=a.z>=0;
    for(const auto& b:polygon) {
        const bool inside_b=b.z>=0;
        if(inside_a!=inside_b) { const double t=a.z/(a.z-b.z);result.push_back(a+(b-a)*t); }
        if(inside_b) result.push_back(b);
        a=b;inside_a=inside_b;
    }
    return result;
}
}
LtcLut precompute_ltc(LtcOptions o) {
    bound(o.cosine_resolution,2,32,"LTC cosine resolution");bound(o.roughness_resolution,2,32,"LTC roughness resolution");
    bound(o.samples,32,2048,"LTC fitting samples");bound(o.iterations,1,96,"LTC iterations");
    budget(std::uint64_t(o.cosine_resolution)*o.roughness_resolution*o.samples*o.iterations*16,134217728);
    LtcLut result;result.entries.reset(o.cosine_resolution,o.roughness_resolution);
    for(int y=0;y<o.roughness_resolution;++y) for(int x=0;x<o.cosine_resolution;++x)
        result.entries.at(x,y)=fit_ltc(std::max(0.02f,float(x)/(o.cosine_resolution-1)),
                                      std::max(0.08f,float(y)/(o.roughness_resolution-1)),o.samples,o.iterations);
    return result;
}
LtcEntry sample_ltc(const LtcLut& lut,float nv,float r) {
    if(lut.entries.empty()) return {};
    const float x=sat(nv)*float(lut.entries.width-1),y=sat(r)*float(lut.entries.height-1);
    const int ix=int(x),iy=int(y);const float fx=x-ix,fy=y-iy;
    LtcEntry result;result.inverse=glm::mat3(0);result.fresnel_integral=glm::vec2(0);result.fit_error=0;
    for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx) {
        const float w=(dx?fx:1-fx)*(dy?fy:1-fy);const auto& entry=lut.entries.clamped(ix+dx,iy+dy);
        result.inverse+=entry.inverse*w;result.fresnel_integral+=entry.fresnel_integral*w;result.fit_error+=entry.fit_error*w;
    }
    // 粗表插值可能产生病态矩阵；退回最近有效格，不制造无穷大的积分。
    if(!std::isfinite(glm::determinant(result.inverse))||glm::determinant(result.inverse)<1e-8f)
        return lut.entries.clamped(int(std::round(x)),int(std::round(y)));
    return result;
}
float integrate_ltc_rectangle(const RectangleLight& light,glm::vec3 position,glm::vec3 normal,glm::vec3 view,const glm::mat3& inverse) {
    if(!emitting(light,position)) return 0;
    const auto frame=ltc_frame(normal,view);
    const std::array<glm::vec3,4> points{light.center-light.half_u-light.half_v,light.center+light.half_u-light.half_v,
                                       light.center+light.half_u+light.half_v,light.center-light.half_u+light.half_v};
    std::vector<glm::dvec3> polygon;for(const auto& p:points) polygon.push_back(glm::dvec3(frame.local(p-position)));
    polygon=clip_horizon(polygon);for(auto& p:polygon) p=glm::dmat3(inverse)*p;polygon=clip_horizon(polygon);
    if(polygon.size()<3) return 0;
    std::vector<glm::dvec3> unit;for(const auto& p:polygon) { const double q=glm::dot(p,p);if(q>1e-24) unit.push_back(p/std::sqrt(q)); }
    if(unit.size()<3) return 0;
    double sum=0;
    for(std::size_t i=0;i<unit.size();++i) {
        const auto a=unit[i],b=unit[(i+1)%unit.size()],cross=glm::cross(a,b);
        const double sine=glm::length(cross),cosine=std::clamp(glm::dot(a,b),-1.0,1.0);
        // 球面边积分：cross(a,b)*theta/sin(theta)，最终 z/(2*pi) 即余弦分布积分。
        if(sine>1e-12) sum+=cross.z*std::atan2(sine,cosine)/sine;
    }
    return sat(float(std::abs(sum)/(2*double(pi))));
}
glm::vec3 evaluate_ltc_rectangle(const LtcLut& lut,const RectangleLight& light,glm::vec3 position,
    glm::vec3 normal,glm::vec3 view,glm::vec3 diffuse,glm::vec3 f0,float roughness) {
    normal=safe_normalize(normal);view=safe_normalize(view);const float nv=glm::dot(normal,view);if(nv<=0) return glm::vec3(0);
    const float lambert=integrate_ltc_rectangle(light,position,normal,view,glm::mat3(1));
    const auto entry=sample_ltc(lut,nv,roughness);
    const float glossy=integrate_ltc_rectangle(light,position,normal,view,entry.inverse);
    return light.radiance*(diffuse*lambert+(f0*entry.fresnel_integral.x+glm::vec3(entry.fresnel_integral.y))*glossy);
}
glm::vec3 evaluate_rectangle_light(const LtcLut& lut,const RectangleLight& light,glm::vec3 position,
    glm::vec3 normal,glm::vec3 view,glm::vec4 tangent,const Material& material,const Settings& settings) {
    return evaluate_rectangle_light(lut,light,position,normal,view,tangent,material,settings,default_energy_lut());
}
glm::vec3 evaluate_rectangle_light(const LtcLut& lut,const RectangleLight& light,glm::vec3 position,
    glm::vec3 normal,glm::vec3 view,glm::vec4 tangent,const Material& material,const Settings& settings,const EnergyLut& energy) {
    normal=safe_normalize(normal);view=safe_normalize(view);const float nv=glm::dot(normal,view);
    if(nv<=0||!emitting(light,position)) return glm::vec3(0);
    const auto base=glm::clamp(glm::vec3(material.base_color),glm::vec3(0),glm::vec3(1));
    if(settings.shading==ShadingMode::toon)
        return light.radiance*base*integrate_ltc_rectangle(light,position,normal,view,glm::mat3(1));
    const bool pbr=settings.shading!=ShadingMode::blinn_phong&&settings.shading!=ShadingMode::disney;
    const float metal=sat(material.metallic),r=std::clamp(material.roughness,.02f,1.f);
    const auto f0=glm::mix(glm::vec3(.04f),base,metal);glm::vec3 result(0);
    if(pbr) {
        const auto diffuse=base*(1-metal)*(glm::vec3(1)-schlick_fresnel(f0,nv));
        result=evaluate_ltc_rectangle(lut,light,position,normal,view,diffuse,f0,material.roughness);
        if(!settings.energy_compensation) return result;
    }
    // 与 GPU 相同的有界 8x8 Gauss 节点；无随机闪烁，尖锐高光/近灯仍可能欠采样。
    constexpr std::array<float,8> nodes{-.9602898565f,-.7966664774f,-.5255324099f,-.1834346425f,
                                       .1834346425f,.5255324099f,.7966664774f,.9602898565f};
    constexpr std::array<float,8> weights{.1012285363f,.2223810345f,.3137066459f,.3626837834f,
                                         .3626837834f,.3137066459f,.2223810345f,.1012285363f};
    const auto area_vector=glm::cross(light.half_u,light.half_v);const float half_area=glm::length(area_vector);
    const auto emitter_normal=area_vector/half_area;glm::dvec3 sum(0);
    for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
        const auto delta=light.center+nodes[x]*light.half_u+nodes[y]*light.half_v-position;
        const float d2=glm::dot(delta,delta);if(d2<1e-12f) continue;
        const auto l=delta/std::sqrt(d2);const float nl=std::max(0.f,glm::dot(normal,l));
        const float lc=light.two_sided?std::abs(glm::dot(emitter_normal,-l)):std::max(0.f,glm::dot(emitter_normal,-l));
        if(nl<=1e-6f||lc<=0) continue;
        // PBR 只积分缺失能量；Disney 的 KC/清漆透过率由共享 BRDF 处理，Blinn 不加 GGX KC。
        const auto brdf=pbr?kulla_conty(energy,f0,nv,nl,r)
                           :evaluate_brdf(material,normal,view,l,tangent,settings,energy);
        // Gauss 权重在 [-1,1]^2；4*half_area 为矩形面积，接收/发光余弦各乘一次。
        sum+=glm::dvec3(brdf)*double(half_area*weights[x]*weights[y]*nl*lc/d2);
    }
    return result+light.radiance*glm::vec3(sum);
}

glm::vec3 integrate_rectangle_reference(const RectangleLight& light,glm::vec3 position,glm::vec3 normal,glm::vec3 view,
    glm::vec4 tangent,const Material& material,const Settings& settings,int samples) {
    bound(samples,16,1048576,"Rectangle reference samples");if(!emitting(light,position)) return glm::vec3(0);
    const auto area_vector=glm::cross(light.half_u,light.half_v);const float area=4*glm::length(area_vector);
    const auto ln=safe_normalize(area_vector);normal=safe_normalize(normal);glm::dvec3 sum(0);
    for(int i=0;i<samples;++i) {
        const auto u=hammersley(i,samples);const auto p=light.center+(2*u.x-1)*light.half_u+(2*u.y-1)*light.half_v;
        const auto delta=p-position;const float d2=glm::dot(delta,delta);if(d2<1e-12f) continue;
        const auto l=delta/std::sqrt(d2);const float nl=std::max(0.0f,glm::dot(normal,l));
        const float lc=light.two_sided?std::abs(glm::dot(ln,-l)):std::max(0.0f,glm::dot(ln,-l));
        // 面积积分到立体角的雅可比 cos(light)/distance²，面积只乘一次。
        sum+=glm::dvec3(evaluate_brdf(material,normal,view,l,tangent,settings)*light.radiance)*double(nl*lc*area/d2);
    }
    return glm::vec3(sum/double(samples));
}
}
