#version 450
#extension GL_GOOGLE_include_directive : require
#define LAB_VERTEX
// Vertex shader declares only the bindings it actually uses.
struct LightData { vec4 positionRange,directionKind,colorIntensity; };
layout(std140,set=0,binding=0) uniform Globals {
    mat4 vp,view,projection,lightVP;
    vec4 cameraNear,skyTopFar,skyBottomExposure;
    ivec4 dimensions,modes,options; vec4 post; ivec4 misc; LightData lights[64];
} g;
layout(push_constant) uniform DrawPush { mat4 model; ivec4 ids; } draw;
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in vec4 tangent;
layout(location=4) in vec4 color;
layout(location=5) in vec3 bakedIrradiance;
layout(location=0) out vec3 worldPosition;
layout(location=1) out vec3 worldNormal;
layout(location=2) out vec2 texcoord;
layout(location=3) out vec4 worldTangent;
layout(location=4) out vec4 vertexColor;
layout(location=5) out vec3 vertexIrradiance;
void main() {
    vec4 p=draw.model*vec4(position,1); worldPosition=p.xyz;
    mat3 m=mat3(draw.model); worldNormal=transpose(inverse(m))*normal;
    worldTangent=vec4(m*tangent.xyz,tangent.w*(determinant(m)<0 ? -1:1));
    texcoord=uv; vertexColor=color;
    // CPU 已对每实例 world-space transfer 应用环境；不能再乘 model 或 albedo。
    vertexIrradiance=bakedIrradiance;
    gl_Position=(draw.ids.y!=0 ? g.lightVP:g.vp)*p;
}
