#version 450

layout(location = 0) out vec3 vertexColor;

void main()
{
    // B9 暂时不创建 Vertex Buffer，而是通过 gl_VertexIndex 生成三个顶点。
    // 这样可以只关注 Shader、Graphics Pipeline、Dynamic Rendering 和 Draw Call。
    const vec2 positions[3] = vec2[3](
        vec2( 0.0, -0.65),
        vec2( 0.65, 0.55),
        vec2(-0.65, 0.55)
    );

    const vec3 colors[3] = vec3[3](
        vec3(1.0, 0.18, 0.12),
        vec3(0.12, 0.85, 0.28),
        vec3(0.15, 0.35, 1.0)
    );

    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    vertexColor = colors[gl_VertexIndex];
}

