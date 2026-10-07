#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
vec3 source(vec2 p) { return g.misc.w!=0 ? texture(referenceImage,p).rgb : texture(hdrImage,p).rgb; }
vec3 srgb(vec3 x) { return mix(12.92*x,1.055*pow(max(x,vec3(0)),vec3(1/2.4))-.055,step(vec3(.0031308),x)); }
void main() {
    vec3 c=max(source(uv),vec3(0));
    if(g.modes.z==0) {
        if(g.post.x>0 && g.misc.w==0) {
            vec2 texel=g.misc.w!=0 ? 1./vec2(textureSize(referenceImage,0)) : 1./vec2(textureSize(hdrImage,0));
            vec3 bloom=vec3(0); float weights=0;
            // 对亮部做有限高斯卷积，权重归一化，避免核大小直接增加能量。
            for(int y=-4;y<=4;y++) for(int x=-4;x<=4;x++) {
                float w=exp(-float(x*x+y*y)/8); vec3 b=source(uv+vec2(x,y)*texel*2);
                bloom+=max(b-g.post.z,vec3(0))*w; weights+=w;
            }
            c+=bloom*(g.post.y/weights);
        }
        c*=g.skyBottomExposure.w;
        c=clamp((c*(2.51*c+.03))/(c*(2.43*c+.59)+.14),0,1);
    } else c=clamp(c,0,1);
    if(g.misc.z==0) c=srgb(c); // sRGB swapchain 由附件转换；UNORM 才手动编码。
    color=vec4(c,1);
}
