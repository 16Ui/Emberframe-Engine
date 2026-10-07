#version 450

layout(location = 0) in vec3 vertexColor;
layout(location = 0) out vec4 outColor;

void main()
{
    // 光栅化阶段会为每个 Fragment 插值三个顶点的颜色。
    outColor = vec4(vertexColor, 1.0);
}

