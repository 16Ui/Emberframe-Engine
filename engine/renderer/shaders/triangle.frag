#version 450

layout(location = 0) in vec3 vertexColor;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform MaterialParams { vec4 tint; } material;

void main()
{
    // Push Constant 是当前 Draw 的材质参数；颜色 Attachment 的 sRGB 编码由目标格式处理。
    outColor = vec4(vertexColor, 1.0) * material.tint;
}
