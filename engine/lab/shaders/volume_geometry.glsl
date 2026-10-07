#ifndef EMBERFRAME_VOLUME_GEOMETRY
#define EMBERFRAME_VOLUME_GEOMETRY
// 三角形/AABB 13 个 SAT 轴：三坐标轴、面法线、3 edges x 3 axes。
// 浮点容差仅扩张体素，不收缩；退化面保留线段/点的保守占据。
bool separatedAxis(vec3 a,vec3 b,vec3 c,vec3 axis,float halfSize) {
    float x=dot(a,axis),y=dot(b,axis),z=dot(c,axis);
    float r=(halfSize+1e-5*v.boundsMinCell.w)*dot(abs(axis),vec3(1));
    return min(x,min(y,z))>r||max(x,max(y,z))<-r;
}
bool triangleVoxel(WorldTriangle t,vec3 center,float halfSize) {
    vec3 a=t.a.xyz-center,b=t.b.xyz-center,c=t.c.xyz-center;
    vec3 lo=min(a,min(b,c)),hi=max(a,max(b,c));
    float h=halfSize+1e-5*v.boundsMinCell.w;
    if(any(greaterThan(lo,vec3(h)))||any(lessThan(hi,vec3(-h)))) return false;
    vec3 edges[3]=vec3[3](b-a,c-b,a-c);
    if(separatedAxis(a,b,c,cross(edges[0],edges[1]),halfSize)) return false;
    for(int e=0;e<3;++e) for(int k=0;k<3;++k) {
        vec3 axis=vec3(0); axis[k]=1;
        if(separatedAxis(a,b,c,cross(edges[e],axis),halfSize)) return false;
    }
    return true;
}
vec3 closestSegment(vec3 p,vec3 a,vec3 b) {
    vec3 ab=b-a; return a+ab*clamp(dot(p-a,ab)/max(dot(ab,ab),1e-20),0,1);
}
vec3 closestTriangle(vec3 p,WorldTriangle t) {
    vec3 a=t.a.xyz,b=t.b.xyz,c=t.c.xyz,ab=b-a,ac=c-a,n=cross(ab,ac);
    if(dot(n,n)<1e-20) {
        vec3 x=closestSegment(p,a,b),y=closestSegment(p,a,c),z=closestSegment(p,b,c);
        if(dot(p-y,p-y)<dot(p-x,p-x)) x=y;
        if(dot(p-z,p-z)<dot(p-x,p-x)) x=z;
        return x;
    }
    float d1=dot(ab,p-a),d2=dot(ac,p-a);
    if(d1<=0&&d2<=0) return a;
    float d3=dot(ab,p-b),d4=dot(ac,p-b);
    if(d3>=0&&d4<=d3) return b;
    float vc=d1*d4-d3*d2;
    if(vc<=0&&d1>=0&&d3<=0) return a+ab*(d1/(d1-d3));
    float d5=dot(ab,p-c),d6=dot(ac,p-c);
    if(d6>=0&&d5<=d6) return c;
    float vb=d5*d2-d1*d6;
    if(vb<=0&&d2>=0&&d6<=0) return a+ac*(d2/(d2-d6));
    float va=d3*d6-d5*d4;
    if(va<=0&&(d4-d3)>=0&&(d5-d6)>=0) return b+(c-b)*((d4-d3)/((d4-d3)+(d5-d6)));
    return a+(ab*vb+ac*vc)/(va+vb+vc);
}
#endif
