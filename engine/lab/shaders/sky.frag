#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(location=1) out vec4 indirectBaseline;
void main() { color=vec4(mix(g.skyTopFar.xyz,g.skyBottomExposure.xyz,uv.y),1);indirectBaseline=vec4(0); }
