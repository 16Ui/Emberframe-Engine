layout(set=1,binding=0) uniform sampler2D baseTexture;
layout(set=1,binding=1) uniform sampler2D mrTexture;
layout(set=1,binding=2) uniform sampler2D normalTexture;
layout(set=1,binding=3) uniform sampler2D aoTexture;
layout(set=1,binding=4) uniform sampler2D emissiveTexture;
layout(push_constant) uniform DrawPush { mat4 model; ivec4 ids; } draw;
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 worldNormal;
layout(location=2) in vec2 texcoord;
layout(location=3) in vec4 worldTangent;
layout(location=4) in vec4 vertexColor;
struct Surface { vec3 albedo,normal,emission; float rough,metal,ao,alpha; };
vec4 filteredTexture(sampler2D image,vec2 uv,int mode,vec2 dx,vec2 dy) {
    if(mode!=3) return textureGrad(image,uv,dx,dy);
    // 手工沿纹理足迹长轴采样，不依赖未启用的 samplerAnisotropy 设备 Feature。
    // 每个 tap 缩短长轴导数，保留短轴决定的 Mip；权重平均不额外增加能量。
    vec2 size=vec2(textureSize(image,0));
    float lx=length(dx*size),ly=length(dy*size);bool xMajor=lx>=ly;
    float major=max(lx,ly),minor=max(min(lx,ly),1e-4);
    int taps=int(clamp(ceil(major/minor),1.,16.));vec2 axis=xMajor?dx:dy;
    vec2 gx=xMajor?dx/float(taps):dx,gy=xMajor?dy:dy/float(taps);vec4 sum=vec4(0);
    for(int i=0;i<taps;++i)sum+=textureGrad(image,uv+axis*((float(i)+.5)/float(taps)-.5),gx,gy);
    return sum/float(taps);
}
Surface surface() {
    // quad 内导数必须在任何 discard / 材质分支之前求出。
    // 否则树叶、头发等 alpha 边缘的剩余片元可能选错 Mip，产生线条/闪烁。
    vec2 dx=dFdx(texcoord),dy=dFdy(texcoord);
    MaterialData m=materials[draw.ids.x]; Surface s;
    bool front=gl_FrontFacing!=(draw.ids.z!=0);
    if(!front && m.flags.y==0) discard;
    vec4 base=filteredTexture(baseTexture,texcoord,m.flags.w,dx,dy)*m.baseColor*vertexColor;
    if(m.flags.x==1 && base.a<m.factors.w) discard;
    vec4 mr=filteredTexture(mrTexture,texcoord,m.flags.w,dx,dy);
    s.albedo=max(base.rgb,vec3(0)); s.alpha=clamp(base.a,0,1);
    s.rough=clamp(m.factors.y*mr.g,.045,1); s.metal=clamp(m.factors.x*mr.b,0,1);
    vec3 n=unit(worldNormal,vec3(0,1,0));
    vec3 t=worldTangent.xyz-n*dot(n,worldTangent.xyz);
    vec3 fallback=unit(cross(abs(n.y)<.99 ? vec3(0,1,0):vec3(1,0,0),n),vec3(1,0,0));
    t=unit(t,fallback); vec3 b=cross(n,t)*worldTangent.w;
    vec3 map=m.flags.z!=0 ? filteredTexture(normalTexture,texcoord,m.flags.w,dx,dy).xyz*2-1 : vec3(0,0,1); map.xy*=m.emissiveNormal.w;
    // 切线去掉法线投影，再用 handedness 构造 B，支持镜像 UV 和负缩放。
    s.normal=unit(mat3(t,b,n)*map,n); if(!front) s.normal=-s.normal;
    s.emission=filteredTexture(emissiveTexture,texcoord,m.flags.w,dx,dy).rgb*m.emissiveNormal.rgb;
    s.ao=mix(1,filteredTexture(aoTexture,texcoord,m.flags.w,dx,dy).r,clamp(m.factors.z,0,1));
    return s;
}
