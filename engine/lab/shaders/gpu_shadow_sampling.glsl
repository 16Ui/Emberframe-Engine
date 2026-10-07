#ifndef EMBERFRAME_GPU_SHADOW_SAMPLING
#define EMBERFRAME_GPU_SHADOW_SAMPLING
#include "gpu_shadow_precision.glsl"
layout(std140,set=2,binding=0) uniform GpuShadows {
    mat4 lightVP[4];
    mat4 cameraView;
    vec4 splitFar;
    ivec4 controls;
    vec4 tuning;
    vec4 cascade[4];
    vec4 areaProjection;
    mat4 planeTransform;
} gpuShadows;
#ifndef GPU_SHADOW_UBO_ONLY
layout(set=2,binding=1) uniform sampler2D gpuShadowDepth[4];
layout(set=2,binding=2) uniform sampler2D gpuShadowMoments[4];
layout(set=2,binding=3) uniform sampler2D gpuShadowSat[4];

// 固定下标避免 sampledImageArrayDynamicIndexing 和 nonuniform features。
float gpu_shadow_depth(int c,ivec2 p) {
    p=clamp(p,ivec2(0),ivec2(gpuShadows.controls.w-1));
    if(c==1) return texelFetch(gpuShadowDepth[1],p,0).r;
    if(c==2) return texelFetch(gpuShadowDepth[2],p,0).r;
    return texelFetch(gpuShadowDepth[0],p,0).r;
}
// Moment/SAT 模式使用非级联深度 0，CSM 使用三层 PCF；描述符剩余元素有效别名。
ShadowSum gpu_shadow_prefix(ivec2 p) {
    if(any(lessThan(p,ivec2(0)))) return ShadowSum(vec4(0),vec4(0));
    return ShadowSum(texelFetch(gpuShadowSat[0],ivec2(p.x*2,p.y),0),texelFetch(gpuShadowSat[0],ivec2(p.x*2+1,p.y),0));
}
vec4 gpu_shadow_moments(ivec2 p,int radius) {
    int size=gpuShadows.controls.w;
    p=clamp(p,ivec2(0),ivec2(size-1));
    radius=clamp(radius,0,size);
    ivec2 lo=max(p-ivec2(radius),ivec2(0));
    ivec2 hi=min(p+ivec2(radius+1),ivec2(size));
    // 小核直接累积原始矩，避免大前缀和相减吞掉窄核的方差。
    if(radius<=3) {
        vec4 sum=vec4(0);
        for(int y=lo.y;y<hi.y;++y) for(int x=lo.x;x<hi.x;++x)
            sum+=texelFetch(gpuShadowMoments[0],ivec2(x,y),0);
        return sum/float((hi.x-lo.x)*(hi.y-lo.y));
    }
    ShadowSum sum=shadow_sum_add(shadow_sum_sub(shadow_sum_sub(gpu_shadow_prefix(hi-1),gpu_shadow_prefix(ivec2(lo.x-1,hi.y-1))),
        gpu_shadow_prefix(ivec2(hi.x-1,lo.y-1))),gpu_shadow_prefix(lo-1));
    float area=float((hi.x-lo.x)*(hi.y-lo.y));
    return clamp(sum.hi/area+sum.lo/area,vec4(0),vec4(1));
}
vec4 gpu_shadow_moments_at_uv(vec2 uv,int radius) {
    vec2 pixel=uv*float(gpuShadows.controls.w)-.5;ivec2 p=ivec2(floor(pixel));vec2 f=fract(pixel);
    return mix(mix(gpu_shadow_moments(p,radius),gpu_shadow_moments(p+ivec2(1,0),radius),f.x),
               mix(gpu_shadow_moments(p+ivec2(0,1),radius),gpu_shadow_moments(p+ivec2(1),radius),f.x),f.y);
}
vec4 gpu_shadow_filtered_moments(vec2 uv,float radius) {
    radius=clamp(radius,0,float(gpuShadows.controls.w));int lo=int(floor(radius));
    vec4 a=gpu_shadow_moments_at_uv(uv,lo);
    // VSSM 半径也必须连续；对原始矩插值仍对应有效的混合深度分布。
    if(fract(radius)<1e-5)return a;
    return mix(a,gpu_shadow_moments_at_uv(uv,min(lo+1,gpuShadows.controls.w)),fract(radius));
}
float gpu_shadow_cantelli(vec2 m,float z,float minVariance,float bleed) {
    if(z<=m.x) return 1;
    float variance=max(max(m.y-m.x*m.x,0),minVariance);
    float delta=z-m.x,p=variance/(variance+delta*delta);
    return clamp((p-bleed)/max(1-bleed,1e-5),0,1);
}
float gpu_shadow_vsm(vec4 m,float z) {
    return gpu_shadow_cantelli(m.xy,z,gpuShadows.tuning.y,gpuShadows.tuning.z);
}
float gpu_shadow_msm(vec4 m,float z) {
    if(z<=0) return 1;
    if(z>1) return 0;
    // 不把“小方差”突然切成硬比较：相邻像素跨过阈值会形成墙面条纹。
    // 下方连续的 moment bias 对近常量分布作正则化；退化分解仍走有界 VSM。
    vec4 b=mix(m,vec4(.5,1.0/3.0,.25,.2),gpuShadows.tuning.w);
    // 四阶原始矩 Hamburger MSM，与 CPU shadow_gi 的 Gauss-Radau 解一致。
    float d1=b.y-b.x*b.x,l21=b.z-b.x*b.y;
    if(d1<=1e-7) return gpu_shadow_vsm(m,z);
    float d2=b.w-b.y*b.y-l21*l21/d1;
    if(d2<=1e-8) return gpu_shadow_vsm(m,z);
    float l=l21/d1,y1=z-b.x,y2=z*z-b.y-l*y1;
    float c2=y2/d2,c1=y1/d1-l*c2,c0=1-b.x*c1-b.y*c2;
    float disc=c1*c1-4*c2*c0;
    if(abs(c2)<1e-7||disc<=0||isnan(disc)||isinf(disc)) return gpu_shadow_vsm(m,z);
    float q=-.5*(c1+(c1>=0 ? sqrt(disc):-sqrt(disc)));
    if(abs(q)<1e-10) return gpu_shadow_vsm(m,z);
    float a=q/c2,c=c0/q;
    if(a>c) { float t=a;a=c;c=t; }
    float da=(a-z)*(a-c),dc=(c-z)*(c-a);
    if(abs(da)<1e-7||abs(dc)<1e-7) return gpu_shadow_vsm(m,z);
    float wa=(b.y-(z+c)*b.x+z*c)/da,wc=(b.y-(z+a)*b.x+z*a)/dc;
    float blocked=(a<z ? wa:0)+(c<z ? wc:0);
    if(isnan(blocked)||isinf(blocked)) return gpu_shadow_vsm(m,z);
    return clamp(1-blocked,0,1);
}
ivec2 gpu_shadow_disk(int i,int count,int radius) {
    float r=float(radius)*sqrt((float(i)+.5)/float(count));
    float phi=6.28318530718*float(bitfieldReverse(uint(i)))*2.3283064365386963e-10;
    return ivec2(round(r*vec2(cos(phi),sin(phi))));
}
// 正交 Light VP 下，三角形平面在阴影 UV 空间中仍是线性深度平面。
// 将几何法线作为协向量转换（逆转置，而不是普通方向变换），求 dz/du、dz/dv。
// 使用实际三角形法线；平滑顶点法线或法线贴图不能描述深度图光栅化的平面。
vec2 gpu_shadow_receiver_gradient(int c,vec3 geometricNormal,vec3 p) {
    if(gpuShadows.areaProjection.x>0) {
        // 平面的常数项不能省略：透视投影下 NDC 深度平面依赖接收点的位置。
        vec4 plane=gpuShadows.planeTransform*vec4(geometricNormal,-dot(geometricNormal,p));
        if(abs(plane.z)<=1e-6*length(plane.xyz))return vec2(0);
        return -2*plane.xy/plane.z;
    }
    mat4 m=gpuShadows.lightVP[c];
    vec3 x=vec3(m[0].x,m[1].x,m[2].x),y=vec3(m[0].y,m[1].y,m[2].y),z=vec3(m[0].z,m[1].z,m[2].z);
    // 当前 VP 的三个轴相互正交，inverse-transpose 可按轴长度平方直接计算。
    vec3 plane=vec3(dot(geometricNormal,x)/max(dot(x,x),1e-20),
                    dot(geometricNormal,y)/max(dot(y,y),1e-20),
                    dot(geometricNormal,z)/max(dot(z,z),1e-20));
    if(abs(plane.z)<=1e-6*length(plane)) return vec2(0); // 侧对光源的退化投影。
    vec2 gradient=-2*plane.xy/plane.z; // clip.xy=[-1,1]，UV=[0,1]。
    return any(isnan(gradient))||any(isinf(gradient)) ? vec2(0):gradient;
}
float gpu_shadow_linear_depth(float z) {
    if(gpuShadows.areaProjection.x<=0)return z;
    float n=gpuShadows.areaProjection.x,f=gpuShadows.areaProjection.y;
    float distance=n*f/max(f-z*(f-n),1e-8);
    return clamp((distance-n)/(f-n),0,1);
}
float gpu_shadow_penumbra(float z,float blocker,float scale) {
    if(gpuShadows.areaProjection.x<=0)return (z-blocker)*scale;
    float n=gpuShadows.areaProjection.x,span=gpuShadows.areaProjection.y-n;
    float receiver=n+z*span,occluder=n+blocker*span;
    // 相似三角形得到世界半影，再除以接收面一纹素的世界尺寸。
    return gpuShadows.areaProjection.w*float(gpuShadows.controls.w)/(2*gpuShadows.areaProjection.z)
        *max(receiver-occluder,0)/max(receiver*occluder,1e-6);
}
float gpu_shadow_search_radius(float linearZ,int ordinaryRadius) {
    if(gpuShadows.areaProjection.x<=0)return ordinaryRadius;
    float distance=mix(gpuShadows.areaProjection.x,gpuShadows.areaProjection.y,linearZ);
    return clamp(gpuShadows.areaProjection.w*float(gpuShadows.controls.w)
        /(gpuShadows.areaProjection.z*max(distance,1e-5)),1,float(gpuShadows.controls.w));
}
float gpu_shadow_recentered_depth(int c,ivec2 texel,vec2 receiverUV,vec2 gradient) {
    texel=clamp(texel,ivec2(0),ivec2(gpuShadows.controls.w-1));
    vec2 sampleUV=(vec2(texel)+.5)/float(gpuShadows.controls.w);
    // 把该纹素深度换算回接收点位置，等价于逐 tap 计算接收平面深度。
    // 连“中心 tap”也要校正：接收点 UV 通常并不位于纹素中心。
    return gpu_shadow_depth(c,texel)-dot(gradient,sampleUV-receiverUV);
}
// 先比较、再双线性插值可见性；不能插值两块不同表面的深度后再比较。
// 所有 tap 的接收平面仍以原始 receiverUV 为基准，避免斜面 acne。
float gpu_shadow_compare_linear(int c,vec2 sampleUV,float z,vec2 receiverUV,vec2 gradient) {
    vec2 pixel=sampleUV*float(gpuShadows.controls.w)-.5;
    ivec2 p=ivec2(floor(pixel));vec2 f=fract(pixel);
    float a=z<=gpu_shadow_recentered_depth(c,p,receiverUV,gradient)?1:0;
    float b=z<=gpu_shadow_recentered_depth(c,p+ivec2(1,0),receiverUV,gradient)?1:0;
    float d=z<=gpu_shadow_recentered_depth(c,p+ivec2(0,1),receiverUV,gradient)?1:0;
    float e=z<=gpu_shadow_recentered_depth(c,p+ivec2(1),receiverUV,gradient)?1:0;
    return mix(mix(a,b,f.x),mix(d,e,f.x),f.y);
}
float gpu_shadow_pcf(int cascade,ivec2 p,float z,int radius,vec2 uv,vec2 gradient) {
    radius=clamp(radius,0,gpuShadows.controls.w);
    if(radius==0)return z<=gpu_shadow_recentered_depth(cascade,p,uv,gradient)?1:0;
    float sum=0;
    if(radius<=2) {
        for(int y=-radius;y<=radius;++y) for(int x=-radius;x<=radius;++x)
            sum+=gpu_shadow_compare_linear(cascade,uv+vec2(x,y)/float(gpuShadows.controls.w),z,uv,gradient);
        return sum/float((2*radius+1)*(2*radius+1));
    }
    for(int i=0;i<32;++i) sum+=z<=gpu_shadow_recentered_depth(cascade,p+gpu_shadow_disk(i,32,radius),uv,gradient) ? 1:0;
    return sum/32;
}
float gpu_shadow_soft_pcf(int cascade,float z,float radius,vec2 uv,vec2 gradient) {
    float sum=0;
    // 连续半径 + 连续纹素位置，避免 ceil(radius) 和整数圆盘把半影切成同心条带。
    // 固定 Vogel 圆盘在相邻像素/帧中不随机旋转，不把条带替换为时间闪烁。
    for(int i=0;i<64;++i) {
        float r=radius*sqrt((float(i)+.5)/64.0),phi=float(i)*2.39996322973;
        vec2 offset=r*vec2(cos(phi),sin(phi))/float(gpuShadows.controls.w);
        sum+=gpu_shadow_compare_linear(cascade,uv+offset,z,uv,gradient);
    }
    return sum/64;
}
float gpu_shadow_pcss(int cascade,ivec2 p,float z,int radius,float scale,vec2 uv,vec2 gradient) {
    float sum=0;int count=0;
    if(radius<=3) {
        for(int y=-radius;y<=radius;++y) for(int x=-radius;x<=radius;++x) {
            float d=gpu_shadow_recentered_depth(cascade,p+ivec2(x,y),uv,gradient);
            if(d<z) {sum+=gpu_shadow_linear_depth(gpu_shadow_depth(cascade,p+ivec2(x,y)));++count;}
        }
    } else for(int i=0;i<64;++i) {
        float d=gpu_shadow_recentered_depth(cascade,p+gpu_shadow_disk(i,64,radius),uv,gradient);
        // 校正值只用于判断是否遮挡；遮挡者距离必须读取原始深度。
        // 把 recentered depth 当距离会随接收面斜率改变光源/遮挡物间距。
        if(d<z) {sum+=gpu_shadow_linear_depth(gpu_shadow_depth(cascade,p+gpu_shadow_disk(i,64,radius)));++count;}
    }
    if(count==0) return 1;
    float filterRadius=clamp(gpu_shadow_penumbra(gpu_shadow_linear_depth(z),sum/float(count),scale),0,float(gpuShadows.controls.w));
    return gpu_shadow_soft_pcf(cascade,z,filterRadius,uv,gradient);
}
vec4 gpu_shadow_receiver_moment_tap(int c,ivec2 tap,vec2 uv,vec2 gradient) {
    // 普通 SAT 只保存深度幂，不能在查询时消除任意接收平面的倾斜。
    // 对倾斜面先逐纹素校正投影深度，再生成矩；不能把平面本身的变化当遮挡者。
    float d=gpu_shadow_linear_depth(clamp(gpu_shadow_recentered_depth(c,tap,uv,gradient),0,1));
    float d2=d*d;return vec4(d,d2,d2*d,d2*d2);
}
vec4 gpu_shadow_receiver_moments_linear(int c,vec2 sampleUV,vec2 uv,vec2 gradient) {
    vec2 pixel=sampleUV*float(gpuShadows.controls.w)-.5;ivec2 p=ivec2(floor(pixel));vec2 f=fract(pixel);
    return mix(mix(gpu_shadow_receiver_moment_tap(c,p,uv,gradient),gpu_shadow_receiver_moment_tap(c,p+ivec2(1,0),uv,gradient),f.x),
               mix(gpu_shadow_receiver_moment_tap(c,p+ivec2(0,1),uv,gradient),gpu_shadow_receiver_moment_tap(c,p+ivec2(1),uv,gradient),f.x),f.y);
}
vec4 gpu_shadow_receiver_moments(int c,vec2 uv,float radius,vec2 gradient) {
    // 平行接收面仍用 SAT 的精确矩形查询；倾斜面使用有界 GPU 采样，不回退到 CPU。
    if(dot(gradient,gradient)<1e-12)return gpu_shadow_filtered_moments(uv,radius);
    radius=clamp(radius,0,float(gpuShadows.controls.w));
    precise vec4 sum=vec4(0),correction=vec4(0);
    if(radius<1e-5)return gpu_shadow_receiver_moments_linear(c,uv,uv,gradient);
    // 全部半径使用同一种圆盘核，不能在某个阈值突然从方形切成圆形。
    for(int i=0;i<64;++i) {
        float r=radius*sqrt((float(i)+.5)/64),phi=float(i)*2.399963229728653;
        // 四阶矩接近常量时，普通 64 项求和的舍入误差会变成假的方差。
        // Kahan 补偿累计；precise 禁止把补偿表达式重新结合/收缩掉。
        precise vec4 value=gpu_shadow_receiver_moments_linear(c,uv+r*vec2(cos(phi),sin(phi))/float(gpuShadows.controls.w),uv,gradient)-correction;
        precise vec4 next=sum+value;
        correction=(next-sum)-value;sum=next;
    }
    return sum/64;
}
float gpu_shadow_vssm(int c,vec2 uv,float z,float radius,float scale,vec2 gradient) {
    vec2 m=gpu_shadow_receiver_moments(c,uv,radius,gradient).xy;
    // 非遮挡深度的条件均值用 receiver z 近似；多峰分布仍可能漏光。
    float lit=gpu_shadow_cantelli(m,z,0,0),blocked=1-lit;
    if(blocked<1e-5) return 1;
    float blocker=clamp((m.x-lit*z)/blocked,0,max(z,0));
    // 条件均值假设在多峰分布中可能给出接近灯面的虚假遮挡者。
    // 最终核不能超出取得该估计的搜索支持域，否则局部估计会突然查询整张图，形成灰色等值线。
    // 极宽半影受此预算限制；这是有界 VSSM，不是无限核的精确面积光积分。
    float filterRadius=clamp(gpu_shadow_penumbra(z,blocker,scale),0,min(radius,float(gpuShadows.controls.w)));
    return gpu_shadow_vsm(gpu_shadow_receiver_moments(c,uv,filterRadius,gradient),z);
}
float gpu_shadow_cascade_visibility(int c,vec3 p,vec3 n,vec3 l,vec3 geometricNormal) {
    int mode=gpuShadows.controls.x;
    bool compareMode=mode<=2||mode==6;
    // 深度比较先做精确平面校正，只保留小的 world-unit Bias 和浮点舍入容差。
    // 不再通过大法线偏移消除条纹，从而保留近距离遮挡/接触阴影。
    vec4 q=gpuShadows.lightVP[c]*vec4(p,1);
    if(q.w<=0)return 1;
    vec3 projected=q.xyz/q.w;vec2 uv=projected.xy*.5+.5;
    if(any(lessThan(uv,vec2(0)))||any(greaterThanEqual(uv,vec2(1)))||projected.z<0||projected.z>1) return 1;
    float slope=1+2*(1-clamp(dot(n,l),0,1));
    float bias=compareMode ? max(gpuShadows.tuning.x/gpuShadows.cascade[c].x,1e-6)
                           : gpuShadows.tuning.x*slope/gpuShadows.cascade[c].x;
    if(compareMode&&gpuShadows.areaProjection.x>0) {
        float nearZ=gpuShadows.areaProjection.x,farZ=gpuShadows.areaProjection.y;
        float distance=nearZ*farZ/max(farZ-projected.z*(farZ-nearZ),1e-8);
        bias=max(1e-7,nearZ*farZ/(farZ-nearZ)*gpuShadows.tuning.x/max(distance*distance,1e-8));
    }
    float z=(compareMode?projected.z:gpu_shadow_linear_depth(projected.z))-bias;
    ivec2 pixel=ivec2(uv*float(gpuShadows.controls.w));
    vec2 gradient=gpu_shadow_receiver_gradient(c,geometricNormal,p);
    if(mode==0) return gpu_shadow_pcf(c,pixel,z,0,uv,gradient);
    if(mode==1||mode==6) return gpu_shadow_pcf(c,pixel,z,1,uv,gradient);
    if(mode==2) return gpu_shadow_pcss(c,pixel,z,int(ceil(gpu_shadow_search_radius(gpu_shadow_linear_depth(z),int(gpuShadows.cascade[c].w)))),gpuShadows.cascade[c].z,uv,gradient);
    if(mode==3) return gpu_shadow_vsm(gpu_shadow_receiver_moments(c,uv,2,gradient),z);
    if(mode==4) return gpu_shadow_vssm(c,uv,z,gpu_shadow_search_radius(z,int(gpuShadows.cascade[c].w)),gpuShadows.cascade[c].z,gradient);
    return gpu_shadow_msm(gpu_shadow_receiver_moments(c,uv,2,gradient),z);
}
float gpu_shadow_visibility(vec3 p,vec3 n,vec3 l,vec3 geometricNormal) {
    if(gpuShadows.controls.z<0) return 1;
    if(gpuShadows.areaProjection.x>0) {
        // 面积灯自身所在平面在中心透视图的近平面之前，不属于该阴影图的接收区。
        // 先判断投影范围，再判断朝向；否则灯面上微小的浮点误差会让 N.L 正负交替。
        float lightDepth=(gpuShadows.lightVP[0]*vec4(p,1)).w;
        if(lightDepth<=gpuShadows.areaProjection.x)return 1;
    }
    // 单中心阴影图代表点/方向光的可见性。背向该中心的几何面被自身遮挡，
    // 不应把接近退化的接收平面向大半影核外推，产生斜向条纹。
    if(dot(geometricNormal,l)<=0)return 0;
    if(gpuShadows.controls.x!=6) return gpu_shadow_cascade_visibility(0,p,n,l,geometricNormal);
    float depth=-(gpuShadows.cameraView*vec4(p,1)).z;
    if(depth<gpuShadows.splitFar.w||depth>gpuShadows.splitFar.z) return 1;
    int c=depth<=gpuShadows.splitFar.x ? 0:(depth<=gpuShadows.splitFar.y ? 1:2);
    float value=gpu_shadow_cascade_visibility(c,p,n,l,geometricNormal);
    // 级联末尾 10% 平滑交接；下一层投影的近边界有相同重叠。
    if(c<2) {
        float begin=c==0 ? gpuShadows.splitFar.w:gpuShadows.splitFar[c-1];
        float end=gpuShadows.splitFar[c],blend=smoothstep(mix(begin,end,.9),end,depth);
        if(blend>0) value=mix(value,gpu_shadow_cascade_visibility(c+1,p,n,l,geometricNormal),blend);
    }
    return value;
}
// 独立 Compute 诊断明确传入解析几何法线；不依赖 fragment derivatives。
float gpu_shadow_visibility(vec3 p,vec3 n,vec3 l) { return gpu_shadow_visibility(p,n,l,n); }
#endif
#endif
