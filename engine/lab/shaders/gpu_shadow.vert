#version 450
#extension GL_GOOGLE_include_directive : require
#define GPU_SHADOW_UBO_ONLY
#include "gpu_shadow_sampling.glsl"
layout(push_constant) uniform DrawPush { mat4 model; ivec4 ids; } draw;
layout(location=0) in vec3 position;
layout(location=2) in vec2 uv;
layout(location=4) in vec4 color;
layout(location=2) out vec2 texcoord;
layout(location=4) out vec4 vertexColor;
void main() {
    texcoord=uv;vertexColor=color;
    gl_Position=gpuShadows.lightVP[clamp(draw.ids.w,0,2)]*draw.model*vec4(position,1);
}
