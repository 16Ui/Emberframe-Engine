#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "surface.glsl"
layout(location=0) out vec4 positionCoverage;
layout(location=1) out vec4 normalRough;
layout(location=2) out vec4 albedoMetal;
layout(location=3) out vec4 emissionAO;
layout(location=4) out vec4 tangentDirection;
layout(location=5) out vec4 materialObject;
layout(location=6) out vec4 prtIrradiance;
layout(location=5) in vec3 vertexIrradiance;
void main() {
    vec3 geometricNormal=geometricSurfaceNormal(worldPosition,worldNormal);
    Surface s=surface(); positionCoverage=vec4(worldPosition,1);
    if(!(gl_FrontFacing!=(draw.ids.z!=0)))geometricNormal=-geometricNormal;
    normalRough=vec4(s.normal,s.rough); albedoMetal=vec4(s.albedo,s.metal);
    emissionAO=vec4(s.emission,s.ao);
    // RG 保留准确材质/对象 ID；BA 用八面体编码保存几何法线，不新增 Attachment。
    tangentDirection=worldTangent;materialObject=vec4(draw.ids.x,draw.ids.w,octEncodeNormal(geometricNormal));
    // 独立目标保留透视正确插值的照度；不占用原六目标的任何通道。
    prtIrradiance=vec4(vertexIrradiance,1);
}
