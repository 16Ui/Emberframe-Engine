#ifndef LAB_COMMON
#define LAB_COMMON
struct LightData { vec4 positionRange; vec4 directionKind; vec4 colorIntensity; };
layout(std140,set=0,binding=0) uniform Globals {
    mat4 vp,view,projection,lightVP;
    vec4 cameraNear,skyTopFar,skyBottomExposure;
    ivec4 dimensions; // width,height,tileX,tileY
    ivec4 modes;      // light count,culling,debug,shading
    ivec4 options;    // shadows (0 hard/1 PCF),GI,reverse Z,energy compensation
    vec4 post;       // bloom enabled,strength,threshold,shadow bias
    ivec4 misc;      // shadow-light index,cluster slices,swapchain sRGB,CPU reference
    LightData lights[64];
} g;
layout(set=0,binding=1) uniform sampler2D shadowMap;
struct MaterialData { vec4 baseColor,emissiveNormal,factors; ivec4 flags; vec4 lobes; };
layout(std430,set=0,binding=2) readonly buffer Materials { MaterialData materials[]; };
layout(std430,set=0,binding=3) readonly buffer LightLists { uint lightLists[]; };
layout(set=0,binding=4) uniform sampler2D gPosition;
layout(set=0,binding=5) uniform sampler2D gNormal;
layout(set=0,binding=6) uniform sampler2D gAlbedo;
layout(set=0,binding=7) uniform sampler2D gEmission;
layout(set=0,binding=8) uniform sampler2D hdrImage;
layout(set=0,binding=9) uniform sampler2D referenceImage;
layout(set=0,binding=10) uniform sampler2D energyLut;
layout(set=0,binding=13) uniform sampler2D screenAO;
layout(set=0,binding=14) uniform sampler2D gTangent;
layout(set=0,binding=15) uniform sampler2D gMeta;
layout(set=0,binding=16) uniform sampler2D gPrtIrradiance;
const float PI=3.14159265359;
vec3 unit(vec3 v,vec3 fallback) { float l=dot(v,v); return l>1e-16 ? v*inversesqrt(l) : fallback; }
vec3 geometricSurfaceNormal(vec3 p,vec3 fallback) {
    // 位置导数描述真正参与光栅化的三角形；不使用平滑法线/法线贴图。
    vec3 n=unit(cross(dFdx(p),dFdy(p)),unit(fallback,vec3(0,1,0)));
    return dot(n,fallback)<0 ? -n:n;
}
vec2 octEncodeNormal(vec3 n) {
    n/=max(abs(n.x)+abs(n.y)+abs(n.z),1e-20);
    vec2 signXY=vec2(n.x>=0 ? 1:-1,n.y>=0 ? 1:-1);
    return n.z>=0 ? n.xy:(1-abs(n.yx))*signXY;
}
vec3 octDecodeNormal(vec2 p) {
    vec3 n=vec3(p,1-abs(p.x)-abs(p.y));
    if(n.z<0) n.xy=(1-abs(n.yx))*vec2(n.x>=0 ? 1:-1,n.y>=0 ? 1:-1);
    return unit(n,vec3(0,1,0));
}
vec2 energy(float cosine,float roughness) {
    ivec2 size=textureSize(energyLut,0);vec2 p=clamp(vec2(cosine,roughness),0,1)*vec2(size-1);
    ivec2 a=ivec2(floor(p)),b=min(a+1,size-1);vec2 t=fract(p);
    return mix(mix(texelFetch(energyLut,a,0).rg,texelFetch(energyLut,ivec2(b.x,a.y),0).rg,t.x),
               mix(texelFetch(energyLut,ivec2(a.x,b.y),0).rg,texelFetch(energyLut,b,0).rg,t.x),t.y);
}
vec3 kulla(vec3 f0,float nv,float nl,float roughness) {
    if(g.options.w==0) return vec3(0);
    vec2 ev=energy(nv,roughness);float el=energy(nl,roughness).r,ea=ev.g;
    if(1-ea<1e-6) return vec3(0);
    // 真实方向反照率 LUT 的缺失能量项；Favg 的几何级数补偿微表面多次反射。
    vec3 favg=f0+(1-f0)/21;
    return favg*favg*ea/(1-favg*(1-ea))*((1-ev.r)*(1-el)/(PI*(1-ea)));
}
#define GPU_LIGHTING_KULLA(f0,nv,nl,r) kulla(f0,nv,nl,r)
#define GPU_LIGHTING_AREA_SHARED_KULLA
#include "gpu_lighting.glsl"
#include "gpu_shadow_sampling.glsl"
#include "scene_resources.glsl"
vec3 evaluateBrdf(vec3 n,vec3 v,vec3 l,vec3 albedo,float rough,float metal) {
    float nv=max(dot(n,v),1e-4),nl=max(dot(n,l),0);if(nl<=0) return vec3(0);
    vec3 f0=mix(vec3(.04),albedo,metal),h=unit(l+v,n);float nh=max(dot(n,h),0),vh=max(dot(v,h),0);
    if(g.modes.w==1){float exponent=min(8192,2/pow(rough,4)-2);return albedo*(1-metal)/PI+f0*((exponent+8)/(8*PI))*pow(nh,exponent);}
    if(g.modes.w==3)return albedo/PI;
    float a=rough*rough,a2=a*a,d=nh*nh*(a2-1)+1,D=a2/(PI*d*d);
    float gv=nl*sqrt(nv*nv*(1-a2)+a2),gl=nv*sqrt(nl*nl*(1-a2)+a2);
    vec3 F=f0+(1-f0)*pow(1-vh,5);
    return (1-F)*(1-metal)*albedo/PI+D*.5/max(gv+gl,1e-5)*F+kulla(f0,nv,nl,rough);
}
vec3 environmentIntegral(vec3 n,vec3 v,vec3 albedo,float rough,float metal) {
    vec3 t=unit(cross(abs(n.y)<.99 ? vec3(0,1,0):vec3(1,0,0),n),vec3(1,0,0));mat3 frame=mat3(t,cross(n,t),n);
    vec3 result=vec3(0);float a2=pow(rough,4);
    // 32 cosine + 32 GGX NDF samples, balanced mixture PDF. Including BRDF*cos/pdf
    // preserves specular directionality and roughness; this is not ambient-color scaling.
    for(uint i=0u;i<64u;i++) {
        uint k=i/2u;float u=(float(k)+.5)/32,azimuth=2*PI*float(bitfieldReverse(k))*2.3283064365386963e-10;
        float cosTheta=i%2u==0u ? sqrt(1-u):sqrt((1-u)/(1+(a2-1)*u));float sinTheta=sqrt(max(1-cosTheta*cosTheta,0));
        vec3 direction=frame*vec3(cos(azimuth)*sinTheta,sin(azimuth)*sinTheta,cosTheta);
        vec3 l=i%2u==0u ? direction:reflect(-v,direction);float nl=dot(n,l);if(nl<=0)continue;
        vec3 h=unit(v+l,n);float nh=max(dot(n,h),0),vh=max(dot(v,h),1e-6),d=nh*nh*(a2-1)+1;
        float pdf=.5*nl/PI+.5*a2/(PI*d*d)*nh/(4*vh);
        vec3 sky=max(mix(g.skyBottomExposure.xyz,g.skyTopFar.xyz,l.y*.5+.5),vec3(0));
        result+=evaluateBrdf(n,v,l,albedo,rough,metal)*sky*(nl/max(pdf,1e-8));
    }
    return max(result/64,vec3(0));
}
int sliceAt(float z) {
    float t=log(max(z,g.cameraNear.w)/g.cameraNear.w)/log(g.skyTopFar.w/g.cameraNear.w);
    return clamp(int(t*float(g.misc.y)),0,g.misc.y-1);
}
uint bucketAt(vec3 world) {
    ivec2 tile=clamp(ivec2(gl_FragCoord.xy)/16,ivec2(0),g.dimensions.zw-1);
    int z=g.modes.y==2 ? sliceAt(-(g.view*vec4(world,1)).z) : 0;
    return uint((z*g.dimensions.w+tile.y)*g.dimensions.z+tile.x)*65u;
}
float visibility(vec3 p,vec3 n,vec3 l,vec3 geometricNormal) {
    if(sceneResources.modes.z!=0&&sceneResources.grid.w!=0)return scene_sdf_visibility(p,geometricNormal,l);
    return gpu_shadow_visibility(p,n,l,geometricNormal);
}
/* 原入门 PCF 留作教学对照；生产路径使用共享高级阴影模块。
float introductoryVisibility(vec3 p,vec3 n,vec3 l) {
    if(g.misc.x<0) return 1;
    vec4 q=g.lightVP*vec4(p,1); vec3 v=q.xyz/q.w;
    vec2 uv=v.xy*.5+.5;
    if(any(lessThan(uv,vec2(0)))||any(greaterThan(uv,vec2(1)))||v.z<0||v.z>1) return 1;
    // 光源深度统一普通 Z；接收偏置随斜率增加，避免把相机 reverse-Z 混进阴影比较。
    float z=v.z-g.post.w*(1+2*(1-max(dot(n,l),0)));
    ivec2 extent=textureSize(shadowMap,0);
    int radius=g.options.x==0 ? 0 : 1; float sum=0;
    for(int y=-radius;y<=radius;y++) for(int x=-radius;x<=radius;x++) {
        ivec2 tc=ivec2(uv*vec2(extent))+ivec2(x,y);
        float d=any(lessThan(tc,ivec2(0)))||any(greaterThanEqual(tc,extent)) ? 1 : texelFetch(shadowMap,tc,0).r;
        sum+=z<=d ? 1 : 0;
    }
    return sum/float((radius*2+1)*(radius*2+1));
}
*/
vec3 shade(vec3 p,vec3 n,vec3 albedo,float rough,float metal,vec3 emission,float ao,vec4 tangent,vec4 lobes,vec3 geometricNormal,vec3 bakedIrradiance,out vec3 indirect) {
    vec3 v=unit(g.cameraNear.xyz-p,n); float nv=max(dot(n,v),1e-4);
    rough=clamp(rough,.045,1); metal=clamp(metal,0,1);
    vec3 f0=mix(vec3(.04),albedo,metal);
    ao*=texelFetch(screenAO,clamp(ivec2(gl_FragCoord.xy),ivec2(0),textureSize(screenAO,0)-1),0).r;
    vec3 irradiance=sceneResources.modes.x==0 ? gpu_latlong(gpuDiffuseIbl,n,0)
        :sceneResources.modes.x==2&&sceneResources.modes.y!=0 ? bakedIrradiance:scene_sh_irradiance(n);
    indirect=g.options.y==0 ? (gpu_environment_diffuse(n,v,albedo,metal,irradiance)+gpu_environment_specular(n,v,albedo,rough,metal))*ao : vec3(0);
    vec3 outColor=emission+indirect;
    // debug shadow 查询主灯实际 visibility，即使 N.L<=0 或灯表把灯剔除也可诊断。
    vec3 shadowDirection=g.misc.x>=0&&int(g.lights[g.misc.x].directionKind.w)==2
        ? unit(g.lights[g.misc.x].positionRange.xyz-p,n)
        : g.misc.x>=0 ? unit(-g.lights[g.misc.x].directionKind.xyz,vec3(0,1,0)):n;
    float shadowDebug=1;
    if(g.modes.z==7&&g.misc.x>=0)shadowDebug=int(g.lights[g.misc.x].directionKind.w)==2
        ? gpu_shadow_visibility(p,n,shadowDirection,geometricNormal):visibility(p,n,shadowDirection,geometricNormal);
    uint bucket=0u,count=uint(g.modes.x);
    if(g.modes.y!=0) { bucket=bucketAt(p); count=min(lightLists[bucket],64u); }
    for(uint k=0u;k<count;k++) {
        uint i=g.modes.y==0 ? k : lightLists[bucket+1u+k];
        LightData light=g.lights[i]; vec3 l; float attenuation=1;
        if(int(light.directionKind.w)==2){
            l=unit(light.positionRange.xyz-p,n);
            // LTC / 材质积分只给未遮挡辐亮度；面积光中心阴影图提供近似可见性。
            float vis=int(i)==g.misc.x ? gpu_shadow_visibility(p,n,l,geometricNormal):1;
            if(int(i)==g.misc.x)shadowDebug=vis;
            outColor+=gpu_area_light(int(i),p,n,v,albedo,rough,metal,tangent,lobes)*vis;
            continue;
        }
        if(int(light.directionKind.w)==0) l=unit(-light.directionKind.xyz,vec3(0,1,0));
        else {
            vec3 delta=light.positionRange.xyz-p; float d2=dot(delta,delta); float d=sqrt(d2);
            l=unit(delta,n); float window=max(1-pow(d/max(light.positionRange.w,.001),4),0);
            attenuation=window*window/max(d2,.01);
        }
        float nl=max(dot(n,l),0); if(nl<=0) continue;
        float vis=int(i)==g.misc.x ? visibility(p,n,l,geometricNormal) : 1;
        if(int(i)==g.misc.x) shadowDebug=vis;
        vec3 radiance=light.colorIntensity.rgb*light.colorIntensity.w*attenuation*vis;
        if(g.modes.w==3) {
            float band=floor(nl*4)/3; outColor+=albedo*radiance*band; continue;
        }
        vec3 brdf=g.modes.w==2 ? gpu_disney_brdf(n,v,l,albedo,rough,metal,tangent,lobes):evaluateBrdf(n,v,l,albedo,rough,metal);
        outColor+=brdf*radiance*nl;
    }
    int dbg=g.modes.z;
    if(dbg==1) return albedo;
    if(dbg==2) return n*.5+.5;
    if(dbg==3) { vec4 q=g.vp*vec4(p,1); return vec3(clamp(q.z/q.w,0,1)); }
    if(dbg==4) return vec3(rough);
    if(dbg==5) return vec3(metal);
    if(dbg==6) return vec3(ao);
    if(dbg==7) return vec3(shadowDebug);
    if(dbg==8) return indirect;
    if(dbg==11) return vec3(float(count)/64, float(count)/16, float(count)/4);
    return max(outColor,vec3(0));
}
#endif
