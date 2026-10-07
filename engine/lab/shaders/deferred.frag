#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(location=1) out vec4 indirectBaseline;
void main() {
    ivec2 tc=ivec2(gl_FragCoord.xy); vec4 p=texelFetch(gPosition,tc,0);
    if(p.w==0) { color=vec4(mix(g.skyTopFar.xyz,g.skyBottomExposure.xyz,uv.y),1);indirectBaseline=vec4(0); return; }
    vec4 n=texelFetch(gNormal,tc,0),a=texelFetch(gAlbedo,tc,0),e=texelFetch(gEmission,tc,0);
    vec4 meta=texelFetch(gMeta,tc,0);int materialId=clamp(int(meta.x),0,materials.length()-1);vec3 indirect;
    // 从 G-buffer 读取真正三角形法线，不在深度断层处用邻居位置重建平面。
    color=vec4(shade(p.xyz,unit(n.xyz,vec3(0,1,0)),a.rgb,n.w,a.w,e.rgb,e.w,texelFetch(gTangent,tc,0),materials[materialId].lobes,octDecodeNormal(meta.zw),texelFetch(gPrtIrradiance,tc,0).rgb,indirect),1);indirectBaseline=vec4(indirect,1);
}
