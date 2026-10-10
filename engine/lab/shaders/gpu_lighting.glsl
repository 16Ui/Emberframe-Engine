#ifndef EMBERFRAME_GPU_LIGHTING_GLSL
#define EMBERFRAME_GPU_LIGHTING_GLSL

const float GPU_LIGHTING_PI=3.14159265358979323846;
struct GpuRectangle {
    vec4 centerKind; // w=2 rectangle，其他类型不发出面积光
    vec4 halfU;
    vec4 halfV;
    vec4 radianceTwoSided;
};
layout(std140,set=3,binding=0) uniform GpuLightingUniform {
    vec4 skyTop;
    vec4 skyBottom;
    ivec4 metadata; // 原场景 light count，roughness levels，ShadingMode，KC enabled
    vec4 environment; // intensity, cos(rotation), sin(rotation), has HDR
    GpuRectangle rectangles[64];
} gpuLighting;
layout(set=3,binding=1) uniform sampler2D gpuDiffuseIbl;
layout(set=3,binding=2) uniform sampler2D gpuSpecularIbl;
layout(set=3,binding=3) uniform sampler2D gpuBrdfLut;
layout(set=3,binding=4) uniform sampler2D gpuLtcInverse;
layout(set=3,binding=5) uniform sampler2D gpuLtcAmplitude;
layout(set=3,binding=6) uniform sampler2D gpuEnvironmentBackground;

vec3 gpu_unit(vec3 v,vec3 fallback) {
    float q=dot(v,v);return q>1e-20 ? v*inversesqrt(q):fallback;
}
float gpu_pow5(float x) { float x2=x*x;return x2*x2*x; }
vec3 gpu_fresnel(vec3 f0,float cosine) {
    return f0+(1-f0)*gpu_pow5(1-clamp(cosine,0,1));
}
// 与 shading_frame 相同：正交化 tangent，退化轴阈值 .999，保留镜像 UV 手性。
mat3 gpu_shading_frame(vec3 normal,vec4 tangent) {
    vec3 n=gpu_unit(normal,vec3(0,0,1));
    vec3 t=tangent.xyz-n*dot(n,tangent.xyz);
    if(dot(t,t)<1e-12) t=cross(abs(n.z)<.999 ? vec3(0,0,1):vec3(0,1,0),n);
    t=gpu_unit(t,vec3(1,0,0));
    return mat3(t,cross(n,t)*(tangent.w<0 ? -1:1),n);
}
float gpu_smith_lambda(vec3 w,vec2 alpha) {
    float z=abs(w.z);if(z<1e-10) return 1e20;
    vec2 a=alpha*w.xy;
    // 等价于 .5*(sqrt(1+a/z²)-1)，避免平滑材质的相消。
    float q=dot(a,a);return q/(2*z*(sqrt(z*z+q)+z));
}
float gpu_ggx_distribution(vec3 h,vec2 alpha) {
    if(h.z<=0) return 0;
    vec3 s=vec3(h.xy/alpha,h.z);float q=dot(s,s);
    return 1/(GPU_LIGHTING_PI*alpha.x*alpha.y*q*q);
}
#ifndef GPU_LIGHTING_KULLA
#define GPU_LIGHTING_KULLA(f0,nv,nl,r) vec3(0)
#elif !defined(GPU_LIGHTING_AREA_SHARED_KULLA)
#define GPU_LIGHTING_AREA_SHARED_KULLA
#endif
// Burley 2012 diffuse + aniso GGX + GTR1 clearcoat + sheen；方向均离开表面。
vec3 gpu_disney_brdf(vec3 normal,vec3 view,vec3 light,vec3 albedo,float roughness,
                     float metallic,vec4 tangent,vec4 lobes) {
    mat3 frame=gpu_shading_frame(normal,tangent);
    vec3 v=transpose(frame)*gpu_unit(view,vec3(0,1,0));
    vec3 l=transpose(frame)*gpu_unit(light,vec3(0,1,0));
    if(v.z<=1e-6||l.z<=1e-6) return vec3(0);
    vec3 h=gpu_unit(v+l,vec3(0,0,1));float vh=clamp(dot(v,h),0,1);
    vec3 base=clamp(albedo,0,1);float metal=clamp(metallic,0,1),r=clamp(roughness,.02,1);
    vec3 f0=mix(vec3(.04),base,metal),f=gpu_fresnel(f0,vh);
    float an=clamp(lobes.z,-.95,.95),aspect=sqrt(1-.9*abs(an));
    vec2 alpha=vec2(r*r/aspect,r*r*aspect);if(an<0) alpha=alpha.yx;
    alpha=max(alpha,vec2(.0004));
    float lv=gpu_smith_lambda(v,alpha),ll=gpu_smith_lambda(l,alpha);
    vec3 spec=f*(gpu_ggx_distribution(h,alpha)/(4*v.z*l.z*(1+lv+ll)));
    if(gpuLighting.metadata.w!=0&&abs(lobes.z)<1e-5)
        spec+=GPU_LIGHTING_KULLA(f0,v.z,l.z,r);
    float fd90=.5+2*r*vh*vh;
    float burley=(1+(fd90-1)*gpu_pow5(1-l.z))*(1+(fd90-1)*gpu_pow5(1-v.z));
    vec3 diff=(1-f)*base*((1-metal)*burley/GPU_LIGHTING_PI);
    float lum=dot(base,vec3(.3,.6,.1));vec3 tint=lum>1e-6 ? base/lum:vec3(1);
    vec3 sheen=clamp(lobes.w,0,1)*(1-metal)*gpu_pow5(1-vh)*mix(vec3(1),tint,.5);
    float coat=clamp(lobes.x,0,1)*.25,ca=clamp(lobes.y,.001,.999),ca2=ca*ca;
    float d=(ca2-1)/(GPU_LIGHTING_PI*log(ca2)*(1+(ca2-1)*h.z*h.z));
    float cf=.04+.96*gpu_pow5(1-vh);
    float cg=1/((1+gpu_smith_lambda(v,vec2(.25)))*(1+gpu_smith_lambda(l,vec2(.25)))*4*v.z*l.z);
    float attenuation=(1-coat*(.04+.96*gpu_pow5(1-v.z)))*(1-coat*(.04+.96*gpu_pow5(1-l.z)));
    return (diff+spec+sheen)*attenuation+vec3(coat*d*cf*cg);
}
vec3 gpu_disney_brdf(vec3 n,vec3 v,vec3 l,vec3 alb,float rough,float metal,vec4 tangent,
                     float coat,float coatRough,float aniso,float sheen) {
    return gpu_disney_brdf(n,v,l,alb,rough,metal,tangent,vec4(coat,coatRough,aniso,sheen));
}

// 端点 LUT 与 texel-center latlong 的坐标约定不同，分别显式插值。
vec4 gpu_grid(sampler2D table,vec2 uv) {
    ivec2 sz=textureSize(table,0);vec2 p=clamp(uv,0,1)*vec2(sz-1);
    ivec2 a=ivec2(floor(p)),b=min(a+1,sz-1);vec2 f=fract(p);
    return mix(mix(texelFetch(table,a,0),texelFetch(table,ivec2(b.x,a.y),0),f.x),
               mix(texelFetch(table,ivec2(a.x,b.y),0),texelFetch(table,b,0),f.x),f.y);
}
vec3 gpu_latlong_texel(sampler2D image,ivec2 pixel,int level) {
    ivec2 sz=textureSize(image,level);
    return texelFetch(image,ivec2((pixel.x%sz.x+sz.x)%sz.x,clamp(pixel.y,0,sz.y-1)),level).rgb;
}
vec3 gpu_latlong(sampler2D image,vec3 direction,int level) {
    vec3 d=gpu_unit(direction,vec3(0,1,0));
    // 逆旋转世界方向到环境坐标；所有环境查表只在这里乘一次线性强度。
    float c=gpuLighting.environment.y,s=gpuLighting.environment.z;
    d=vec3(c*d.x-s*d.z,d.y,s*d.x+c*d.z);
    // pole 的 atan(0,0) 未定义，显式指定经度。
    float u=dot(d.xz,d.xz)>1e-20 ? atan(d.z,d.x)/(2*GPU_LIGHTING_PI):0;
    vec2 uv=vec2(fract(u),acos(clamp(d.y,-1,1))/GPU_LIGHTING_PI);
    vec2 p=uv*vec2(textureSize(image,level))-.5;ivec2 a=ivec2(floor(p));vec2 f=fract(p);
    return mix(mix(gpu_latlong_texel(image,a,level),gpu_latlong_texel(image,a+ivec2(1,0),level),f.x),
               mix(gpu_latlong_texel(image,a+ivec2(0,1),level),gpu_latlong_texel(image,a+1,level),f.x),f.y)*gpuLighting.environment.x;
}
// 主程序背景阶段必须使用本入口，而不是 common.glsl 中的独立解析天空。
// 切换 HDR 的后台预过滤尚未完成时，背景与物体都继续引用同一旧环境。
vec3 gpu_environment_background(vec3 direction) {
    if(gpuLighting.environment.w>.5)return gpu_latlong(gpuEnvironmentBackground,direction,0);
    vec3 d=gpu_unit(direction,vec3(0,1,0));
    return max(mix(gpuLighting.skyBottom.rgb,gpuLighting.skyTop.rgb,clamp(d.y*.5+.5,0,1)),vec3(0))*gpuLighting.environment.x;
}
vec3 gpu_prefiltered_specular(vec3 reflection,float roughness) {
    int count=max(gpuLighting.metadata.y,1);float level=clamp(roughness,0,1)*float(count-1);
    int lo=int(level),hi=min(lo+1,count-1);
    return mix(gpu_latlong(gpuSpecularIbl,reflection,lo),gpu_latlong(gpuSpecularIbl,reflection,hi),fract(level));
}
// 共享 CPU 的 isotropic split-sum：diffuse 存 E，spec 存卷积 radiance，A/B 不含 F0。
// 不含 AO、遮挡或额外 Disney IBL lobes；调用方不能再叠加旧 environmentIntegral。
// 替换 diffuse 时显式传入辐照度；SH/PRT 与 IBL 采用相同接收面材质权重。
vec3 gpu_environment_diffuse(vec3 normal,vec3 view,vec3 albedo,float metallic,vec3 irradiance) {
    vec3 n=gpu_unit(normal,vec3(0,1,0)),v=gpu_unit(view,vec3(0,1,0));
    float nv=dot(n,v);if(nv<=0) return vec3(0);
    vec3 base=clamp(albedo,0,1);float metal=clamp(metallic,0,1);
    if(gpuLighting.metadata.z==3) return base*irradiance/GPU_LIGHTING_PI;
    vec3 f0=mix(vec3(.04),base,metal);
    return base*(1-metal)*(1-gpu_fresnel(f0,nv))*irradiance/GPU_LIGHTING_PI;
}
// 环境镜面 split-sum 与 KC 保持原近似；PRT 不会误遮挡镜面或重复加 ambient。
vec3 gpu_environment_specular(vec3 normal,vec3 view,vec3 albedo,float roughness,float metallic) {
    vec3 n=gpu_unit(normal,vec3(0,1,0)),v=gpu_unit(view,vec3(0,1,0));
    float nv=dot(n,v);if(nv<=0||gpuLighting.metadata.z==3)return vec3(0);
    vec3 base=clamp(albedo,0,1);float metal=clamp(metallic,0,1),r=clamp(roughness,0,1);
    vec3 f0=mix(vec3(.04),base,metal);vec3 lut=gpu_grid(gpuBrdfLut,vec2(nv,r)).rgb;vec2 ab=lut.rg;
    vec3 result=gpu_prefiltered_specular(reflect(-v,n),r)*(f0*ab.x+ab.y);
    if(gpuLighting.metadata.w!=0) {
        // 积分共享 KC：∫(1-E(l))*N.L dl = pi*(1-Eavg)。仅补 missing energy，
        // 不再加一次 GGX。E(v)=A+B，与本次 one-scatter 使用同一 LUT，白炉闭合。
        // 非恒定天空以 cosine irradiance 近似 KC 环境卷积，非完整 MIS 积分。
        float ev=clamp(ab.x+ab.y,0,1),ea=clamp(lut.b,0,1);
        vec3 favg=f0+(1-f0)/21;
        vec3 fms=favg*favg*ea/max(1-favg*(1-ea),vec3(1e-6));
        result+=fms*(1-ev)*gpu_latlong(gpuDiffuseIbl,n,0)/GPU_LIGHTING_PI;
    }
    return result;
}
vec3 gpu_prefiltered_environment(vec3 n,vec3 v,vec3 albedo,float roughness,float metallic) {
    return gpu_environment_diffuse(n,v,albedo,metallic,gpu_latlong(gpuDiffuseIbl,gpu_unit(n,vec3(0,1,0)),0))
        +gpu_environment_specular(n,v,albedo,roughness,metallic);
}

struct GpuLtcEntry { mat3 inverse;vec2 amplitude; };
GpuLtcEntry gpu_ltc_lookup(float nv,float roughness) {
    vec2 uv=clamp(vec2(nv,roughness),0,1);
    vec4 a=gpu_grid(gpuLtcInverse,uv),b=gpu_grid(gpuLtcAmplitude,uv);
    mat3 m=mat3(vec3(a.x,0,a.z),vec3(0,b.x,0),vec3(a.y,0,a.w));
    float det=determinant(m);
    // 与 CPU sample_ltc 同样对病态插值回退最近格点。
    if(isnan(det)||isinf(det)||det<1e-8) {
        ivec2 sz=textureSize(gpuLtcInverse,0);
        ivec2 p=ivec2(floor(uv*vec2(sz-1)+.5));
        a=texelFetch(gpuLtcInverse,p,0);b=texelFetch(gpuLtcAmplitude,p,0);
        m=mat3(vec3(a.x,0,a.z),vec3(0,b.x,0),vec3(a.y,0,a.w));
    }
    return GpuLtcEntry(m,b.yz);
}
// 凸四边形两次平面裁剪最多六个顶点；容量 12 也覆盖近边界重复点。
int gpu_clip_horizon(in vec3 inputPolygon[12],int count,out vec3 outputPolygon[12]) {
    if(count==0) return 0;int written=0;vec3 a=inputPolygon[count-1];bool ina=a.z>=0;
    for(int i=0;i<count;++i) {
        vec3 b=inputPolygon[i];bool inb=b.z>=0;
        if(ina!=inb) outputPolygon[written++]=mix(a,b,a.z/(a.z-b.z));
        if(inb) outputPolygon[written++]=b;
        a=b;ina=inb;
    }
    return written;
}
float gpu_ltc_integral(GpuRectangle light,vec3 p,vec3 n,vec3 v,mat3 inverse) {
    vec3 areaNormal=cross(light.halfU.xyz,light.halfV.xyz);
    if(dot(areaNormal,areaNormal)<1e-16) return 0;
    if(light.radianceTwoSided.w==0&&dot(areaNormal,p-light.centerKind.xyz)<=1e-8) return 0;
    mat3 frame=gpu_shading_frame(n,vec4(v,1));
    vec3 polygon[12],clipped[12];vec3 c=light.centerKind.xyz-p,u=light.halfU.xyz,w=light.halfV.xyz;
    polygon[0]=transpose(frame)*(c-u-w);polygon[1]=transpose(frame)*(c+u-w);
    polygon[2]=transpose(frame)*(c+u+w);polygon[3]=transpose(frame)*(c-u+w);
    // 先裁物理 N.L>=0，再线性变换，再裁变换后余弦半球；不能省略第一次裁剪。
    int count=gpu_clip_horizon(polygon,4,clipped);
    for(int i=0;i<count;++i) polygon[i]=inverse*clipped[i];
    count=gpu_clip_horizon(polygon,count,clipped);if(count<3) return 0;
    int unitCount=0;
    for(int i=0;i<count;++i) { float q=dot(clipped[i],clipped[i]);if(q>1e-24) polygon[unitCount++]=clipped[i]*inversesqrt(q); }
    if(unitCount<3) return 0;float sum=0;
    for(int i=0;i<unitCount;++i) {
        vec3 a=polygon[i],b=polygon[(i+1)%unitCount],edge=cross(a,b);float sine=length(edge);
        if(sine>1e-12) sum+=edge.z*atan(sine,clamp(dot(a,b),-1,1))/sine;
    }
    return clamp(abs(sum)/(2*GPU_LIGHTING_PI),0,1);
}
// 单独保留 GGX LTC 基线：KC 只补缺失能量，不重算/增益缩放单次散射。
vec3 gpu_area_light_ltc(GpuRectangle light,vec3 p,vec3 n,vec3 v,vec3 base,float roughness,float metallic) {
    if(light.centerKind.w!=2||dot(n,v)<=0)return vec3(0);
    if(light.centerKind.w!=2) return vec3(0);
    float nv=dot(n,v),metal=clamp(metallic,0,1);vec3 f0=mix(vec3(.04),base,metal);
    vec3 diffuse=base*(1-metal)*(1-gpu_fresnel(f0,nv));
    GpuLtcEntry ltc=gpu_ltc_lookup(nv,roughness);
    float diffuseIntegral=gpu_ltc_integral(light,p,n,v,mat3(1));
    float glossyIntegral=gpu_ltc_integral(light,p,n,v,ltc.inverse);
    return light.radianceTwoSided.rgb*(diffuse*diffuseIntegral+(f0*ltc.amplitude.x+ltc.amplitude.y)*glossyIntegral);
}
// 独立 include 的 KC 用现有 A/B/Eavg 表；生产 include 仍用共享 energyLut 的宏。
// 只适用于各向同性 GGX；不将它冒充 Blinn 或各向异性 Disney 的能量补偿。
vec3 gpu_area_kulla(vec3 f0,float nv,float nl,float r) {
    if(nv<=0||nl<=0) return vec3(0);
#ifdef GPU_LIGHTING_AREA_SHARED_KULLA
    return GPU_LIGHTING_KULLA(f0,nv,nl,r);
#else
    vec3 table=gpu_grid(gpuBrdfLut,vec2(nv,r)).rgb;
    vec2 incoming=gpu_grid(gpuBrdfLut,vec2(nl,r)).rg;
    float ev=clamp(table.r+table.g,0,1),el=clamp(incoming.x+incoming.y,0,1),ea=clamp(table.b,0,1);
    if(1-ea<1e-6) return vec3(0);
    vec3 favg=f0+(1-f0)/21;
    return favg*favg*ea/max(1-favg*(1-ea),vec3(1e-6))*((1-ev)*(1-el)/(GPU_LIGHTING_PI*(1-ea)));
#endif
}
// 已包含 BRDF*cos 的面积积分。光色为 radiance，没有点光 distance/range window。
vec3 gpu_area_light(int index,vec3 p,vec3 normal,vec3 view,vec3 albedo,float roughness,float metallic,
                    vec4 tangent,vec4 lobes) {
    if(index<0||index>=min(gpuLighting.metadata.x,64)) return vec3(0);
    GpuRectangle light=gpuLighting.rectangles[index];if(light.centerKind.w!=2) return vec3(0);
    vec3 n=gpu_unit(normal,vec3(0,1,0)),v=gpu_unit(view,vec3(0,1,0));float nv=dot(n,v);
    if(nv<=0) return vec3(0);
    vec3 base=clamp(albedo,0,1),radiance=light.radianceTwoSided.rgb;
    int mode=gpuLighting.metadata.z;
    if(mode==3) return radiance*base*gpu_ltc_integral(light,p,n,v,mat3(1));
    bool pbr=mode!=1&&mode!=2;
    vec3 result=pbr ? gpu_area_light_ltc(light,p,n,v,base,roughness,metallic):vec3(0);
    bool isotropicKC=gpuLighting.metadata.w!=0&&(pbr||(mode==2&&abs(lobes.z)<1e-5));
    if(pbr&&!isotropicKC) return result;
    vec3 areaNormal=cross(light.halfU.xyz,light.halfV.xyz);float halfArea=length(areaNormal);
    if(halfArea*halfArea<1e-16) return vec3(0);
    if(light.radianceTwoSided.w==0&&dot(areaNormal,p-light.centerKind.xyz)<=1e-8) return vec3(0);
    vec3 emitterNormal=areaNormal/halfArea;
    float metal=clamp(metallic,0,1),r=clamp(roughness,.02,1);vec3 f0=mix(vec3(.04),base,metal);
    float exponent=min(8192.0,2/(r*r*r*r)-2);
    // 固定 8x8 Gauss 矩形求积，无随机闪烁；尖锐高光/近灯仍有有限采样误差。
    const float nodes[8]=float[8](-.9602898565,-.7966664774,-.5255324099,-.1834346425,
                                  .1834346425,.5255324099,.7966664774,.9602898565);
    const float weights[8]=float[8](.1012285363,.2223810345,.3137066459,.3626837834,
                                   .3626837834,.3137066459,.2223810345,.1012285363);
    vec3 sum=vec3(0);
    for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
        vec3 delta=light.centerKind.xyz+nodes[x]*light.halfU.xyz+nodes[y]*light.halfV.xyz-p;
        float d2=dot(delta,delta);if(d2<1e-12) continue;
        vec3 l=delta*inversesqrt(d2);float nl=max(dot(n,l),0);
        float lc=dot(emitterNormal,-l);lc=light.radianceTwoSided.w!=0 ? abs(lc):max(lc,0);
        if(nl<=1e-6||lc<=0) continue;
        vec3 brdf;
        if(pbr) brdf=gpu_area_kulla(f0,nv,nl,r);
        else if(mode==1) {
            vec3 h=gpu_unit(v+l,n);
            brdf=base*(1-metal)/GPU_LIGHTING_PI+f0*((exponent+8)/(8*GPU_LIGHTING_PI))*pow(max(dot(n,h),0),exponent);
        } else {
            brdf=gpu_disney_brdf(n,v,l,base,r,metal,tangent,lobes);
#ifndef GPU_LIGHTING_AREA_SHARED_KULLA
            // 独立 include 的 Disney 宏默认零，在此补一次；清漆双向透过率一致。
            if(isotropicKC) {
                float coat=clamp(lobes.x,0,1)*.25;
                float attenuation=(1-coat*(.04+.96*gpu_pow5(1-nv)))*(1-coat*(.04+.96*gpu_pow5(1-nl)));
                brdf+=gpu_area_kulla(f0,nv,nl,r)*attenuation;
            }
#endif
        }
        // weights 积分于 [-1,1]^2，halfArea*4 即面积；N.L 与发光面余弦各乘一次。
        sum+=brdf*(halfArea*weights[x]*weights[y]*nl*lc/d2);
    }
    return result+radiance*sum;
}
vec3 gpu_area_light(int index,vec3 p,vec3 n,vec3 v,vec3 albedo,float roughness,float metallic) {
    return gpu_area_light(index,p,n,v,albedo,roughness,metallic,vec4(0,0,0,1),vec4(0,.15,0,0));
}
vec3 gpu_area_light(uint index,vec3 p,vec3 n,vec3 v,vec3 albedo,float roughness,float metallic,
                    vec4 tangent,vec4 lobes) {
    if(index>=64u) return vec3(0);
    return gpu_area_light(int(index),p,n,v,albedo,roughness,metallic,tangent,lobes);
}
vec3 gpu_area_light(uint index,vec3 p,vec3 n,vec3 v,vec3 albedo,float roughness,float metallic) {
    if(index>=64u) return vec3(0);
    return gpu_area_light(int(index),p,n,v,albedo,roughness,metallic);
}
#endif

// 编译验证入口：直接编译本文件 -S comp -DGPU_LIGHTING_VALIDATION，生产 include 不生成入口。
#ifdef GPU_LIGHTING_VALIDATION
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
layout(std430,set=0,binding=0) buffer LightingDiagnostic { vec4 values[]; } diagnostic;
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i<64u) {
        float f=float(i),r=.02+.98*float(i%9u)/8,metal=float(i%3u)/2;
        vec3 v=gpu_unit(vec3(.45,.25,.1+.9*float(i%7u)/6),vec3(0,0,1));
        vec3 l=gpu_unit(vec3(-.7+.2*float(i%8u),.35,.2+.8*float(i%5u)/4),vec3(0,0,1));
        if(i==62u) l.z=-l.z;if(i==63u) v.z=-v.z;
        vec4 lobes=vec4(float(i%5u)/4,.001+.998*float(i%4u)/3,-.95+1.9*float(i%6u)/5,float(i%4u)/3);
        diagnostic.values[i]=vec4(gpu_disney_brdf(vec3(0,0,1),v,l,vec3(.7,.2,.08),r,metal,
            vec4(1,.2,.1,i%2u==0u ? 1:-1),lobes),1);
    } else if(i<80u) {
        uint j=i-64u;float angle=float(j)*2*GPU_LIGHTING_PI/16;
        vec3 n=gpu_unit(vec3(cos(angle),-.95+1.9*float(j%5u)/4,sin(angle)),vec3(0,1,0));
        if(j==0u) n=vec3(0,1,0);if(j==1u) n=vec3(0,-1,0);
        if(j==2u) n=gpu_unit(vec3(1,0,-.00001),vec3(1,0,0));
        vec3 v=gpu_unit(n+vec3(.2,.1,.15),n);
        diagnostic.values[i]=vec4(gpu_prefiltered_environment(n,v,vec3(.7,.2,.08),float(j%7u)/6,float(j%3u)/2),1);
    } else if(i<96u) {
        int j=int(i-80u);vec3 n=vec3(0,0,1),v=gpu_unit(vec3(.2*float(j%3),.1,1),n);
        // 旧诊断槽验证 LTC 表/几何基线，独立于测试宿主选中的 Disney/KC 模式。
        diagnostic.values[i]=vec4(gpu_area_light_ltc(gpuLighting.rectangles[j],vec3(0),n,v,vec3(.7,.2,.08),.2+.8*float(j%5)/4,float(j%3)/2),1);
    } else if(i<112u) {
        int j=int(i-96u);
        diagnostic.values[i]=vec4(gpu_ltc_integral(gpuLighting.rectangles[j],vec3(0),vec3(0,0,1),vec3(0,0,1),mat3(1)),0,0,1);
    } else if(i<128u) {
        uint j=i-112u;GpuLtcEntry e=gpu_ltc_lookup(float(j%4u)/3,float(j/4u)/3);
        diagnostic.values[i]=vec4(e.inverse[0][0],e.inverse[1][1],e.amplitude);
    } else if(i<144u) {
        uint j=i-128u;float nv=.03+.97*float(j%4u)/3;
        vec3 v=vec3(sqrt(max(1-nv*nv,0)),0,nv);
        diagnostic.values[i]=vec4(gpu_prefiltered_environment(vec3(0,0,1),v,vec3(1),float(j/4u)/3,1),1);
    } else if(i<148u) {
        diagnostic.values[i]=vec4(gpu_area_light_ltc(gpuLighting.rectangles[0],vec3(0),vec3(0,0,1),vec3(0,0,1),vec3(1),float(i-140u)/7,1),1);
    } else if(i<164u) {
        int j=int(i-148u);vec3 n=vec3(0,0,1),v=gpu_unit(vec3(.2*float(j%3),.1,1),n);
        vec4 tangent=vec4(.8,.6,0,j%2==0 ? 1:-1),lobes=vec4(.7,.25,-.6+.6*float(j%3),.4);
        diagnostic.values[i]=vec4(gpu_area_light(j,vec3(0),n,v,vec3(.7,.2,.08),.45+.55*float(j%5)/4,float(j%3)/2,tangent,lobes),1);
    } else if(i<180u) {
        uint j=i-164u;vec3 n=vec3(0,0,1),v=gpu_unit(vec3(.2*float(j%3u),.1,1),n);
        vec3 legacy=gpu_area_light(j,vec3(0),n,v,vec3(.7,.2,.08),.7,.3);
        vec3 neutral=gpu_area_light(int(j),vec3(0),n,v,vec3(.7,.2,.08),.7,.3,vec4(0,0,0,1),vec4(0,.15,0,0));
        diagnostic.values[i]=vec4(legacy-neutral,1);
    } else if(i<192u) {
        uint j=i-180u;float angle=float(j)*2*GPU_LIGHTING_PI/12;
        vec3 d=gpu_unit(vec3(cos(angle),-.6+.6*float(j%3u),sin(angle)),vec3(0,1,0));
        if(j==0u)d=vec3(0,1,0);if(j==1u)d=vec3(0,-1,0);
        if(j==2u)d=gpu_unit(vec3(1,0,-.00001),vec3(1,0,0));
        diagnostic.values[i]=vec4(gpu_environment_background(d),1);
    }
}
#endif

#ifdef GPU_LIGHTING_FRAGMENT_VALIDATION
layout(location=0) out vec4 lightingDiagnosticColor;
void main() {
    vec3 n=vec3(0,0,1),v=gpu_unit(vec3(.2,.1,1),n),l=gpu_unit(vec3(.3,.4,1),n);
    vec3 color=gpu_disney_brdf(n,v,l,vec3(.7,.2,.08),.5,.3,vec4(1,0,0,-1),vec4(.8,.15,.7,.4));
    color+=gpu_prefiltered_environment(n,v,vec3(.7,.2,.08),.5,.3);
    color+=gpu_area_light(0u,vec3(0),n,v,vec3(.7,.2,.08),.5,.3,vec4(1,0,0,-1),vec4(.8,.15,.7,.4));
    color+=gpu_area_light(0,vec3(0),n,v,vec3(.7,.2,.08),.5,.3);
    lightingDiagnosticColor=vec4(color,1);
}
#endif
