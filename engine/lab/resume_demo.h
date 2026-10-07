#pragma once
#include "types.h"
#include <string_view>

namespace emberframe::lab {
struct ResumeDemo {
    Scene scene;
    Camera camera;
    Settings settings;
};
// 固定输入与渲染器共用真实 Scene/Settings，不使用替身图片或特殊着色器。
ResumeDemo make_resume_demo(std::string_view name);
// 相机回放只改变视点，不移动模型、不改 scene revision，允许真正复用 TAA 历史。
Camera replay_demo_camera(const Camera& origin,std::string_view motion,std::size_t frame,std::size_t frames);
struct DemoRayProbe {
    std::size_t rays=0,hits=0;
    std::uint64_t brute_triangle_tests=0,bvh_triangle_tests=0;
    double brute_ms=0,bvh_ms=0,build_ms=0;
    bool matches=true;
};
// 独立 CPU 求交区间；不把它当成 GPU 渲染帧率或整个路径追踪的加速比。
DemoRayProbe probe_demo_bvh(const Scene&,const Camera&);
TestResults test_resume_demo();
}
