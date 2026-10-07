#ifndef EMBERFRAME_EFFECTS_COMMON
#define EMBERFRAME_EFFECTS_COMMON
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
layout(set=0,binding=0) uniform sampler2D efPosition;
layout(set=0,binding=1) uniform sampler2D efNormal;
layout(set=0,binding=2) uniform sampler2D efAlbedo;
layout(set=0,binding=3) uniform sampler2D efEmission;
layout(set=0,binding=4) uniform sampler2D efSource;
layout(set=0,binding=5) uniform sampler2D efAux;
layout(set=0,binding=6) uniform sampler2D efHistory;
layout(set=0,binding=7) uniform sampler2D efHistoryPosition;
layout(set=0,binding=8) uniform sampler2D efHistoryNormal;
layout(set=0,binding=9) uniform sampler2D efHistoryMoments;
layout(set=0,binding=10) uniform sampler2D efBaseline;
layout(set=0,binding=11) uniform sampler2D efTangent;
layout(set=0,binding=12) uniform sampler2D efMeta;
layout(set=0,binding=13) uniform sampler2D efHistoryMeta;
layout(set=0,binding=14) uniform sampler2D efVolumeIndirect;
layout(set=0,binding=15) uniform sampler2D efOcclusion;
layout(rgba32f,set=0,binding=16) uniform writeonly image2D efOut;
layout(rgba32f,set=0,binding=17) uniform writeonly image2D efOutAux;
layout(std140,set=0,binding=18) uniform EffectsFrame {
    mat4 currentVP,previousVP,previousInverseVP,view,inverseView;
    vec4 cameraNear,skyTopFar,skyBottomRadius;
    ivec4 dimensions; // width,height,frame index,seed
    ivec4 modes;      // AO,GI,samples,debug
    ivec4 flags;      // history valid,baseline present,outline,hatching
    vec4 temporal;   // 当前帧 alpha,AO strength,bloom threshold,bloom strength
    vec4 jitter;     // current.xy,previous.xy (NDC)
    ivec4 extensions; // tangent present,metadata present,volume present,unused
} ef;
layout(push_constant) uniform EffectsPass {
    ivec4 control; // mode,step/radius,unused,unused
    vec4 parameters;
} ep;
const float EF_PI=3.14159265358979323846;
bool efInside(ivec2 p,ivec2 size) { return all(greaterThanEqual(p,ivec2(0)))&&all(lessThan(p,size)); }
ivec2 efClamp(ivec2 p,ivec2 size) { return clamp(p,ivec2(0),size-1); }
vec3 efUnit(vec3 p,vec3 fallback) { float l=dot(p,p); return l>1e-16 ? p*inversesqrt(l):fallback; }
vec3 efClean(vec3 p) { return clamp(mix(mix(p,vec3(0),isnan(p)),vec3(0),isinf(p)),vec3(0),vec3(60000)); }
float efLuminance(vec3 c) { return dot(c,vec3(.2126,.7152,.0722)); }
vec4 efFetch(sampler2D s,ivec2 p) { return texelFetch(s,efClamp(p,textureSize(s,0)),0); }
vec4 efBilinear(sampler2D s,vec2 uv) {
    vec2 q=uv*vec2(textureSize(s,0))-.5; ivec2 p=ivec2(floor(q));vec2 f=fract(q);
    return mix(mix(efFetch(s,p),efFetch(s,p+ivec2(1,0)),f.x),
               mix(efFetch(s,p+ivec2(0,1)),efFetch(s,p+ivec2(1,1)),f.x),f.y);
}
float efDepth(vec3 world) { return -(ef.view*vec4(world,1)).z; }
bool efProject(vec3 p,mat4 vp,out vec2 uv) {
    vec4 q=vp*vec4(p,1); if(q.w<=1e-6||any(isnan(q))||any(isinf(q)))return false;
    vec3 ndc=q.xyz/q.w;uv=ndc.xy*.5+.5; // VP 已含 Y 翻转；workbench 使用正 viewport height。
    return ndc.z>=0&&ndc.z<=1&&all(greaterThanEqual(uv,vec2(0)))&&all(lessThan(uv,vec2(1)));
}
uint efHash(uint v) { v=(v^(v>>16u))*0x7feb352du;v=(v^(v>>15u))*0x846ca68bu;return v^(v>>16u); }
float efRandom(ivec2 p,uint offset) {
    uint v=uint(p.x)*1973u+uint(p.y)*9277u+uint(ef.dimensions.w)*26699u+offset;
    return float(efHash(v)&0xffffffu)/16777216.;
}
float efRadical(uint n) { return float(bitfieldReverse(n))*2.3283064365386963e-10; }
mat3 efFrame(vec3 n) {
    vec3 t=efUnit(cross(abs(n.z)<.9 ? vec3(0,0,1):vec3(0,1,0),n),vec3(1,0,0));
    return mat3(t,cross(n,t),n);
}
float efGeometryWeight(ivec2 p,ivec2 q,float distanceScale) {
    vec4 a=efFetch(efPosition,p),b=efFetch(efPosition,q);
    if(a.w<.5||b.w<.5)return p==q ? 1:0;
    vec3 na=efUnit(efFetch(efNormal,p).xyz,vec3(0,0,1)),nb=efUnit(efFetch(efNormal,q).xyz,na);
    vec3 delta=b.xyz-a.xyz;float depth=max(efDepth(a.xyz),.001);
    // tangent-plane distance 能保留斜面；额外 depth 项抑制遮挡边界跨面泄漏。
    float plane=abs(dot(delta,na)),dz=abs(efDepth(b.xyz)-depth);
    float nw=pow(max(dot(na,nb),0),32);
    if(ef.extensions.y!=0&&any(greaterThan(abs(efFetch(efMeta,p).xy-efFetch(efMeta,q).xy),vec2(.1))))return 0;
    return nw*exp(-plane/max(.003,depth*.01*distanceScale)-dz/max(.01,depth*.12*distanceScale));
}
vec3 efYCoCg(vec3 c) { return vec3(.25*c.r+.5*c.g+.25*c.b,.5*c.r-.5*c.b,-.25*c.r+.5*c.g-.25*c.b); }
vec3 efRGB(vec3 c) { return vec3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z); }
// 对四个双线性 tap 分别验证 world position/normal，然后重新归一化权重。
// 静态几何能捕获相机运动；node 运动须由 caller revision/invalidate_history 清历史。
bool efReproject(ivec2 p,out vec3 history,out vec4 moments,out vec2 previousUV) {
    history=vec3(0);moments=vec4(0);previousUV=vec2(0);
    vec4 world=efFetch(efPosition,p);if(ef.flags.x==0||world.w<.5||!efProject(world.xyz,ef.previousVP,previousUV))return false;
    vec3 n=efUnit(efFetch(efNormal,p).xyz,vec3(0,0,1));
    float depth=max(efDepth(world.xyz),.001),tolerance=max(.01,depth*2./float(ef.dimensions.y));
    // 历史 texel 中心与重投影坐标通常不重合。用当前切平面预测该 texel 的
    // world position，再比较残差；直接比较两帧 world position 会拒绝斜面上的合法 tap。
    vec4 previousPlane=transpose(ef.previousInverseVP)*vec4(n,-dot(n,world.xyz));
    if(abs(previousPlane.z)<1e-8||any(isnan(previousPlane))||any(isinf(previousPlane)))return false;
    vec2 samplePixel=previousUV*vec2(ef.dimensions.xy)-.5,f=fract(samplePixel);
    ivec2 origin=ivec2(floor(samplePixel));float weights=0;
    for(int y=0;y<2;y++)for(int x=0;x<2;x++) {
        ivec2 q=origin+ivec2(x,y);if(!efInside(q,ef.dimensions.xy))continue;
        vec4 oldWorld=efFetch(efHistoryPosition,q),oldNormal=efFetch(efHistoryNormal,q);
        if(oldWorld.w<.5||dot(n,efUnit(oldNormal.xyz,n))<.9)continue;
        vec2 oldNDC=(vec2(q)+.5)/vec2(ef.dimensions.xy)*2-1;
        float oldDepth=-(dot(previousPlane.xy,oldNDC)+previousPlane.w)/previousPlane.z;
        if(oldDepth<0||oldDepth>1)continue;
        vec4 expected=ef.previousInverseVP*vec4(oldNDC,oldDepth,1);
        if(abs(expected.w)<1e-8||any(isnan(expected))||any(isinf(expected)))continue;
        if(length(oldWorld.xyz-expected.xyz/expected.w)>tolerance)continue;
        if(ef.extensions.y!=0&&any(greaterThan(abs(efFetch(efMeta,p).xy-efFetch(efHistoryMeta,q).xy),vec2(.1))))continue;
        // roughness 变化也拒绝；对象/material 变化由 scene revision 处理。
        if(abs(oldNormal.w-efFetch(efNormal,p).w)>.1)continue;
        float w=(x==0 ? 1-f.x:f.x)*(y==0 ? 1-f.y:f.y);
        history+=efClean(efFetch(efHistory,q).rgb)*w;moments+=efFetch(efHistoryMoments,q)*w;weights+=w;
    }
    if(weights<.1)return false;history/=weights;moments/=weights;return true;
}
vec3 efClampHistory(ivec2 p,vec3 old,float gamma) {
    vec3 mean=vec3(0),second=vec3(0),low=vec3(1e30),high=vec3(-1e30);float count=0;
    for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++) {
        ivec2 q=p+ivec2(x,y);if(!efInside(q,ef.dimensions.xy))continue;
        if(efFetch(efPosition,p).w>.5&&efGeometryWeight(p,q,1)<.05)continue;
        vec3 c=efYCoCg(efClean(efFetch(efSource,q).rgb));mean+=c;second+=c*c;low=min(low,c);high=max(high,c);count++;
    }
    mean/=max(count,1);vec3 deviation=sqrt(max(second/max(count,1)-mean*mean,vec3(0)));
    low=max(low,mean-gamma*deviation);high=min(high,mean+gamma*deviation);
    return efClean(efRGB(clamp(efYCoCg(old),low,high)));
}
#endif
