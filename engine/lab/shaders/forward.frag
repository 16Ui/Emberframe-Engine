#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "surface.glsl"
layout(location=0) out vec4 color;
layout(location=1) out vec4 indirectBaseline;
layout(location=5) in vec3 vertexIrradiance;
void main() {
    // 在 discard / 灯光分支前求位置导数，保证 quad 内的几何信息可用。
    vec3 geometricNormal=geometricSurfaceNormal(worldPosition,worldNormal);
    Surface s=surface();vec3 indirect;
    // 双面材质背面着色时，几何法线与着色法线必须使用同一面朝向。
    if(!(gl_FrontFacing!=(draw.ids.z!=0)))geometricNormal=-geometricNormal;
    color=vec4(shade(worldPosition,s.normal,s.albedo,s.rough,s.metal,s.emission,s.ao,worldTangent,materials[draw.ids.x].lobes,geometricNormal,vertexIrradiance,indirect),s.alpha);
    indirectBaseline=vec4(indirect,s.alpha);
}
