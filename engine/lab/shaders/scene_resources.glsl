#ifndef EMBERFRAME_SCENE_RESOURCES_GLSL
#define EMBERFRAME_SCENE_RESOURCES_GLSL
layout(std140,set=4,binding=0) uniform SceneResources {
    vec4 radianceSH[9];
    vec4 sdfMin,sdfMax;
    ivec4 grid;
    vec4 trace; // Lipschitz、覆盖半径、softness、world bias。
    ivec4 modes; // diffuse mode、PRT ready、SDF requested、保留。
} sceneResources;
layout(std430,set=4,binding=1) readonly buffer SceneDistanceGrid { float sceneDistances[]; };
vec3 scene_sh_irradiance(vec3 direction) {
    vec3 n=unit(direction,vec3(0,1,0));float x=n.x,y=n.y,z=n.z;
    float basis[9]=float[9](.2820947918,.4886025119*y,.4886025119*z,.4886025119*x,
        1.0925484306*x*y,1.0925484306*y*z,.3153915653*(3*z*z-1),1.0925484306*x*z,.5462742153*(x*x-y*y));
    vec3 irradiance=vec3(0);
    // 原始 radiance SH 只在这里 cosine 卷积一次，不含 albedo/pi。
    for(int i=0;i<9;++i)irradiance+=sceneResources.radianceSH[i].rgb*basis[i]*(i==0?PI:i<4?2*PI/3:PI/4);
    return irradiance;
}
float scene_sdf_texel(ivec3 p) {
    ivec3 n=sceneResources.grid.xyz;p=clamp(p,ivec3(0),n-1);
    return sceneDistances[(p.z*n.y+p.y)*n.x+p.x];
}
float scene_sdf_distance(vec3 world) {
    vec3 inside=clamp(world,sceneResources.sdfMin.xyz,sceneResources.sdfMax.xyz);
    if(any(notEqual(inside,world)))return length(world-inside);
    vec3 q=clamp((world-sceneResources.sdfMin.xyz)/(sceneResources.sdfMax.xyz-sceneResources.sdfMin.xyz),0,1)*vec3(sceneResources.grid.xyz-1);
    ivec3 a=min(ivec3(floor(q)),sceneResources.grid.xyz-2);vec3 f=q-vec3(a);
    float low=mix(mix(scene_sdf_texel(a),scene_sdf_texel(a+ivec3(1,0,0)),f.x),mix(scene_sdf_texel(a+ivec3(0,1,0)),scene_sdf_texel(a+ivec3(1,1,0)),f.x),f.y);
    float high=mix(mix(scene_sdf_texel(a+ivec3(0,0,1)),scene_sdf_texel(a+ivec3(1,0,1)),f.x),mix(scene_sdf_texel(a+ivec3(0,1,1)),scene_sdf_texel(a+1),f.x),f.y);
    // 与 sample_scene_sdf 同式：插值距离减去八角点覆盖距离的加权和。
    vec3 cell=(sceneResources.sdfMax.xyz-sceneResources.sdfMin.xyz)/vec3(sceneResources.grid.xyz-1);
    float error=0;
    for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x){
        vec3 corner=vec3(x,y,z);vec3 weight=mix(1-f,f,corner);
        error+=weight.x*weight.y*weight.z*length(cell*(corner-f));
    }
    return max(mix(low,high,f.z)-error,0);
}
float scene_sdf_visibility(vec3 world,vec3 geometricNormal,vec3 direction) {
    if(sceneResources.modes.z==0||sceneResources.grid.w==0)return 1;
    vec3 l=unit(direction,vec3(0,1,0)),n=unit(geometricNormal,l);
    // 粗网格必须离开接收面的覆盖半径；该空间偏置由实际格距决定。
    float diagonal=2*sceneResources.trace.y,epsilon=max(diagonal*.001,1e-5);
    float offset=max(sceneResources.trace.w,diagonal*1.05);
    vec3 origin=world+n*offset;
    float enter=0,exit=1e30;
    // 闭区间 slab，显式处理平行轴，避免 0*infinity 产生 NaN。
    for(int axis=0;axis<3;++axis){
        float lo=sceneResources.sdfMin[axis],hi=sceneResources.sdfMax[axis];
        if(abs(l[axis])<1e-8){if(origin[axis]<lo||origin[axis]>hi)return 1;}
        else{float a=(lo-origin[axis])/l[axis],b=(hi-origin[axis])/l[axis];enter=max(enter,min(a,b));exit=min(exit,max(a,b));}
    }
    if(exit<=enter)return 1;
    float t=max(enter,epsilon),visibility=1;
    for(int step=0;step<512&&t<=exit;++step){
        float distance=scene_sdf_distance(origin+l*t);
        if(distance<=epsilon)return 0;
        visibility=min(visibility,sceneResources.trace.z*distance/max(t,offset));
        t+=max(.9*distance/max(sceneResources.trace.x,1),epsilon);
    }
    // 达到步数上限时保守返回遮挡；不能把未收敛查询标成无遮挡。
    return t>exit?clamp(visibility,0,1):0;
}
#endif
