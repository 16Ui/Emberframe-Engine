#ifndef EMBERFRAME_VOLUME_COMMON
#define EMBERFRAME_VOLUME_COMMON
// 独立 set0 ABI，不能包含 renderer 的 common.glsl。
const float VPI=3.14159265358979323846;
const float Y00=0.28209479177387814;
const float Y1=0.4886025119029199;
layout(std140,set=0,binding=0) uniform VolumeParams {
    mat4 lightVP,inverseLightVP;
    vec4 boundsMinCell,boundsMaxSide,cameraPosition;
    vec4 lightPositionRange,lightDirectionKind,lightColorIntensity;
    ivec4 dimensions; // output w/h, resolution, triangle count
    ivec4 rsmDimensions; // RSM w/h, stratification grid x/y
    ivec4 modes; // GiMode, propagation iterations, mip count, viewport y sign
    vec4 transport; // propagation attenuation, distance regularizer, world bias, reserved
    uvec4 random; // 随机种子、稀疏开关、节点容量、页项数量
    ivec4 geometry; // BVH node count, RSM face count, tile width/height
    vec4 lightSize;
    mat4 lightFaces[6],inverseLightFaces[6];
} v;
layout(set=0,binding=1) uniform sampler2D mainPosition;
layout(set=0,binding=2) uniform sampler2D mainNormal;
layout(set=0,binding=3) uniform sampler2D mainAlbedo;
layout(set=0,binding=4) uniform sampler2D rsmPosition;
layout(set=0,binding=5) uniform sampler2D rsmNormal;
layout(set=0,binding=6) uniform sampler2D rsmAlbedo;
struct WorldTriangle { vec4 a,b,c,diffuse,emissive; };
layout(std430,set=0,binding=7) readonly buffer TriangleBuffer { WorldTriangle triangles[]; };
struct RsmPacket { vec4 positionArea,normalValid,flux; };
layout(std430,set=0,binding=8) buffer RsmBuffer { RsmPacket packets[]; };
// 每个颜色通道依次为 (Y00,Y1x,Y1y,Y1z)。三个平面：frontier A、B、accumulated。
// 每次 dispatch 每个 cell 只有一个写者，所以完全不需要浮点 atomic。
struct SH { vec4 r,g,b; };
layout(std430,set=0,binding=9) buffer ShBuffer { SH fields[]; };
layout(rgba32f,set=0,binding=10) uniform image3D sourceVolume;
layout(set=0,binding=11) uniform sampler3D radianceVolume;
layout(rgba32f,set=0,binding=12) uniform writeonly image2D indirectOutput;
layout(rgba32f,set=0,binding=13) uniform writeonly image3D destinationMip;
// 稠密页地址只保存索引，辐亮度/光学 opacity 存在 GPU compact 的非空节点里。
// 页项 0 是空格，其余为 nodeIndex+1；每帧 slot 独占这三个 SSBO。
layout(std430,set=0,binding=14) buffer SparsePages { uint sparsePages[]; };
layout(std430,set=0,binding=15) buffer SparseNodes { vec4 sparseNodes[]; };
layout(std430,set=0,binding=16) buffer SparseCounters {
    uvec4 sparseCounts; // 分配尝试数、溢出数、实际存储数、保留为 0
    uvec4 sparseLevelCounts[2]; // 0..5 级实际存储数，剩余分量为 0
};
struct GeometryNode { vec4 minimum,maximum; uvec4 range; };
layout(std430,set=0,binding=17) readonly buffer GeometryBvh { GeometryNode geometryNodes[]; };
layout(push_constant) uniform VolumePush { ivec4 pass; } pc; // 源 SH 基址、目标 SH 基址、mip 边长、稀疏 mip 级数
uint sparseLevelOffset(int level) {
    uint offset=0u;
    for(int mip=0;mip<level;++mip) {
        uint size=uint(v.dimensions.z>>mip); offset+=size*size*size;
    }
    return offset;
}
uint sparsePageIndex(ivec3 cell,int level) {
    uint size=uint(v.dimensions.z>>level);
    return sparseLevelOffset(level)+uint(cell.x)+size*(uint(cell.y)+size*uint(cell.z));
}
vec3 unitVector(vec3 d) { float q=dot(d,d); return q>1e-20?d*inversesqrt(q):vec3(0,1,0); }
vec4 shBasis(vec3 d) { return vec4(Y00,Y1*d); }
SH zeroSH() { return SH(vec4(0),vec4(0),vec4(0)); }
SH addSH(SH a,SH b) { return SH(a.r+b.r,a.g+b.g,a.b+b.b); }
SH projectSH(vec3 power,vec4 basis) { return SH(power.r*basis,power.g*basis,power.b*basis); }
vec3 evaluateSH(SH s,vec4 basis) { return vec3(dot(s.r,basis),dot(s.g,basis),dot(s.b,basis)); }
int cellCount() { return v.dimensions.z*v.dimensions.z*v.dimensions.z; }
int cellIndex(ivec3 p) { return p.x+v.dimensions.z*(p.y+v.dimensions.z*p.z); }
bool insideCell(ivec3 p) { return all(greaterThanEqual(p,ivec3(0)))&&all(lessThan(p,ivec3(v.dimensions.z))); }
bool insideWorld(vec3 p) { return all(greaterThanEqual(p,v.boundsMinCell.xyz))&&all(lessThan(p,v.boundsMaxSide.xyz)); }
vec3 diffuseAlbedo(vec4 albedo) { return clamp(albedo.rgb,0,1)*(1.0-clamp(albedo.a,0,1)); }
vec3 incidentIrradiance(vec3 p,vec3 n) {
    vec3 incoming; float falloff=1;
    if(int(v.lightDirectionKind.w)==0) incoming=-unitVector(v.lightDirectionKind.xyz);
    else {
        vec3 delta=v.lightPositionRange.xyz-p; float d2=dot(delta,delta);
        if(d2<1e-12||(int(v.lightDirectionKind.w)==1&&sqrt(d2)>v.lightPositionRange.w)) return vec3(0);
        incoming=delta*inversesqrt(d2); falloff=1.0/d2;
        if(int(v.lightDirectionKind.w)==2) {
            // 矩形灯强度是 radiance；灯心近似还需乘真实面积和灯面发光余弦。
            // 这不是 LTC 面积积分，也不把面积灯冒充同强度的全向点光。
            falloff*=v.lightSize.x*v.lightSize.y*max(dot(unitVector(v.lightDirectionKind.xyz),-incoming),0);
        }
    }
    return v.lightColorIntensity.rgb*v.lightColorIntensity.w*falloff*max(dot(n,incoming),0.0);
}
int rsmFace(vec3 p) {
    if(v.geometry.y!=6)return 0;
    vec3 d=p-v.lightPositionRange.xyz,a=abs(d);
    if(a.x>=a.y&&a.x>=a.z)return d.x>=0?0:1;
    if(a.y>=a.z)return d.y>=0?2:3;
    return d.z>=0?4:5;
}
int rsmPacketCount() { return v.rsmDimensions.z*v.rsmDimensions.w*v.geometry.y; }
// lightVP 包括 camera 中使用的 z convention；比较由 world position 投影出来的深度。
// 不依赖 reversed-Z clear depth，position.a 是唯一有效性标记。
float rsmVisibility(vec3 p,vec3 n) {
    int face=rsmFace(p);
    vec4 clip=v.lightFaces[face]*vec4(p+n*v.transport.z,1);
    if(clip.w<=1e-8) return 0;
    vec3 ndc=clip.xyz/clip.w;
    vec2 uv=ndc.xy*.5+.5;
    if(v.modes.w<0) uv.y=1-uv.y;
    if(any(lessThan(uv,vec2(-1e-5)))||any(greaterThan(uv,vec2(1+1e-5)))||ndc.z<0||ndc.z>1) return 0;
    // 两个 cube 面共享的边界也必须落到本面的有效纹素，不能因 uv==1 留下黑缝。
    ivec2 px=clamp(ivec2(uv*vec2(v.geometry.zw)),ivec2(0),v.geometry.zw-1)+ivec2(face*v.geometry.z,0);
    vec4 blocker=texelFetch(rsmPosition,px,0);
    if(blocker.a<=0) return 1;
    vec3 toLight=int(v.lightDirectionKind.w)==0?-unitVector(v.lightDirectionKind.xyz):unitVector(v.lightPositionRange.xyz-p);
    // 相同 light ray 上离光源更近的 blocker；方向/点光一致，避开 reversed-Z 分支。
    return dot(blocker.xyz-p,toLight)>max(v.transport.z*2,v.boundsMinCell.w*.35)?0.0:1.0;
}
uint hashVolume(uint x) { x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; return x^(x>>16); }
const ivec3 neighbors[6]=ivec3[6](ivec3(1,0,0),ivec3(-1,0,0),ivec3(0,1,0),ivec3(0,-1,0),ivec3(0,0,1),ivec3(0,0,-1));
#endif
