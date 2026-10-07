#version 450
layout(set=1,binding=0) uniform sampler2D baseTexture;
struct MaterialData { vec4 baseColor,emissiveNormal,factors; ivec4 flags; vec4 lobes; };
layout(std430,set=0,binding=2) readonly buffer Materials { MaterialData materials[]; };
layout(push_constant) uniform DrawPush { mat4 model; ivec4 ids; } draw;
layout(location=2) in vec2 texcoord;
layout(location=4) in vec4 vertexColor;
void main() {
    vec2 dx=dFdx(texcoord),dy=dFdy(texcoord);
    MaterialData m=materials[draw.ids.x];
    if(!(gl_FrontFacing!=(draw.ids.z!=0)) && m.flags.y==0) discard;
    if(m.flags.x==1 && textureGrad(baseTexture,texcoord,dx,dy).a*m.baseColor.a*vertexColor.a<m.factors.w) discard;
}
