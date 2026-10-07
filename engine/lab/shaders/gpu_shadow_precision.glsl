#ifndef GPU_SHADOW_PRECISION
#define GPU_SHADOW_PRECISION
// 两个 float 保存高低部分；不需要 shaderFloat64。
// 大 SAT 中先做补偿相减，再转回 float；不能先丢弃低位再相减。
struct ShadowSum { vec4 hi; vec4 lo; };
ShadowSum shadow_sum_add(ShadowSum a,ShadowSum b) {
    precise vec4 s=a.hi+b.hi;
    precise vec4 v=s-a.hi;
    precise vec4 e=(a.hi-(s-v))+(b.hi-v);
    e=e+a.lo+b.lo;
    precise vec4 hi=s+e;
    precise vec4 lo=e-(hi-s);
    return ShadowSum(hi,lo);
}
ShadowSum shadow_sum_sub(ShadowSum a,ShadowSum b) {
    return shadow_sum_add(a,ShadowSum(-b.hi,-b.lo));
}
#endif
