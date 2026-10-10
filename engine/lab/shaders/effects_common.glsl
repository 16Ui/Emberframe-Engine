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
    ivec4 extensions; // tangent present,metadata present,volume present,motion object count
} ef;
// 与 CPU EffectsObjectMotion 完全一致：mat4(64) + uvec4(16)，std430 stride=80。
// 普通 runtime 数据数组不需要 descriptor indexing，也不是 runtime descriptor array。
struct EffectsRigidMotion {mat4 currentToPreviousWorld;uvec4 identity;};
layout(std430,set=0,binding=19) readonly buffer EffectsObjectMotions {
    EffectsRigidMotion objects[];
} efMotions;
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
// 当前像素属于哪一个对象，由 GB meta.y 精确索引；只有主线提供持久对应关系才借用历史。
// 返回上一帧世界位置/法线和身份。空表仅保留兼容的静态世界重投影。
bool efPreviousSurface(ivec2 p,out vec3 previousWorld,out vec3 previousNormal,out vec2 previousIdentity) {
    vec4 world=efFetch(efPosition,p);if(world.w<.5)return false;
    previousWorld=world.xyz;previousNormal=efUnit(efFetch(efNormal,p).xyz,vec3(0,0,1));
    previousIdentity=ef.extensions.y!=0 ? efFetch(efMeta,p).xy:vec2(0);
    if(ef.extensions.w==0)return true;
    float id=previousIdentity.y;
    // NaN/负数/越界/非整数一律拒绝，不能让新对象随机索引某个旧对象。
    if(isnan(id)||isinf(id)||id<0||id>=float(ef.extensions.w)||floor(id)!=id)return false;
    EffectsRigidMotion object=efMotions.objects[uint(id)];
    if(object.identity.y!=1u||object.identity.x>16777215u)return false;
    vec4 old=object.currentToPreviousWorld*vec4(world.xyz,1);
    mat3 a=mat3(object.currentToPreviousWorld);
    if(any(isnan(old))||any(isinf(old))||abs(old.w-1)>1e-5||abs(determinant(a))<1e-20)return false;
    vec3 oldNormal=transpose(inverse(a))*previousNormal;
    if(any(isnan(oldNormal))||any(isinf(oldNormal))||dot(oldNormal,oldNormal)<1e-20)return false;
    previousWorld=old.xyz;previousNormal=normalize(oldNormal);previousIdentity.y=float(object.identity.x);return true;
}
// 四个双线性 tap 各自检查：持久对象+材质、上一帧位置/深度、旋转后的法线。
// 新露出的同对象另一面仍须过 depth/normal 测试，不会因 object ID 相同就复用历史。
bool efReproject(ivec2 p,out vec3 history,out vec4 moments,out vec2 previousUV) {
    history=vec3(0);moments=vec4(0);previousUV=vec2(0);
    vec3 world,n;vec2 identity;
    if(ef.flags.x==0||!efPreviousSurface(p,world,n,identity)||!efProject(world,ef.previousVP,previousUV))return false;
    // 历史 texel 中心与重投影坐标通常不重合。用当前切平面预测该 texel 的
    // world position，再比较残差；直接比较两帧 world position 会拒绝斜面上的合法 tap。
    vec4 previousPlane=transpose(ef.previousInverseVP)*vec4(n,-dot(n,world));
    if(abs(previousPlane.z)<1e-8||any(isnan(previousPlane))||any(isinf(previousPlane)))return false;
    vec2 samplePixel=previousUV*vec2(ef.dimensions.xy)-.5,f=fract(samplePixel);
    ivec2 origin=ivec2(floor(samplePixel));float weights=0;
    for(int y=0;y<2;y++)for(int x=0;x<2;x++) {
        ivec2 q=origin+ivec2(x,y);if(!efInside(q,ef.dimensions.xy))continue;
        vec4 oldWorld=efFetch(efHistoryPosition,q),oldNormal=efFetch(efHistoryNormal,q);
        if(oldWorld.w<.5||dot(n,efUnit(oldNormal.xyz,n))<.9)continue;
        if(ef.extensions.y!=0&&any(greaterThan(abs(identity-efFetch(efHistoryMeta,q).xy),vec2(.1))))continue;
        vec2 oldNDC=(vec2(q)+.5)/vec2(ef.dimensions.xy)*2-1;
        float oldDepth=-(dot(previousPlane.xy,oldNDC)+previousPlane.w)/previousPlane.z;
        if(oldDepth<0||oldDepth>1)continue;
        vec4 expected=ef.previousInverseVP*vec4(oldNDC,oldDepth,1);
        if(abs(expected.w)<1e-8||any(isnan(expected))||any(isinf(expected)))continue;
        vec3 expectedWorld=expected.xyz/expected.w;
        // 使用上一帧平面的像素足迹建立尺度容差，不能拿移动后的 current depth 比旧深度。
        // 局部斜面在历史 texel 中心的位置不同，先用平面求出期望深度再比较。
        vec2 neighborNDC=oldNDC+vec2(0,2./float(ef.dimensions.y));
        float neighborDepth=-(dot(previousPlane.xy,neighborNDC)+previousPlane.w)/previousPlane.z;
        vec4 neighbor=ef.previousInverseVP*vec4(neighborNDC,neighborDepth,1);
        float tolerance=.01;
        if(abs(neighbor.w)>1e-8)tolerance=max(tolerance,length(neighbor.xyz/neighbor.w-expectedWorld)*1.5);
        if(length(oldWorld.xyz-expectedWorld)>tolerance)continue;
        // 显式比较 previous clip depth：排除同对象远近表面/遮挡解除历史。
        vec4 oldClip=ef.previousVP*vec4(oldWorld.xyz,1);
        vec4 deltaClip=ef.previousVP*vec4(expectedWorld+n*tolerance,1);
        if(oldClip.w<=1e-6||deltaClip.w<=1e-6)continue;
        float depthTolerance=max(1e-6,abs(deltaClip.z/deltaClip.w-oldDepth));
        if(abs(oldClip.z/oldClip.w-oldDepth)>depthTolerance)continue;
        // roughness 变化也是局部拒绝，不是全图失效。
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
