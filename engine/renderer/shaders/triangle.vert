#version 450

layout(location = 0) out vec3 vertexColor;

void main()
{
    // 当前不需要 Vertex Buffer；一个 Draw 的三个顶点由序号直接生成。
    const vec2 positions[3] = vec2[3](
        vec2( 0.0, -0.65), vec2( 0.65, 0.55), vec2(-0.65, 0.55));
    const vec3 colors[3] = vec3[3](
        vec3(1.0, 0.18, 0.12), vec3(0.12, 0.85, 0.28), vec3(0.15, 0.35, 1.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    vertexColor = colors[gl_VertexIndex];
}
