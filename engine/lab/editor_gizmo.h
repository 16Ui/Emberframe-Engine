#pragma once
#include "types.h"
#include <optional>

namespace emberframe::lab::editor {
struct ViewportRect { float x=0,y=0,width=0,height=0; };
// 渲染图有可能留黑边：选物与拖动必须使用实际图像矩形，而不是整个窗口。
inline ViewportRect image_rect(float x,float y,float width,float height,int render_width,int render_height) {
    if(width<=0||height<=0||render_width<=0||render_height<=0)return {};
    const float scale=std::min(width/render_width,height/render_height);
    const float w=render_width*scale,h=render_height*scale;
    return {x+(width-w)*.5f,y+(height-h)*.5f,w,h};
}
inline std::optional<glm::vec2> project_handle(glm::vec3 world,const Camera& camera,const ViewportRect& rect,bool reversed) {
    if(rect.width<=0||rect.height<=0)return {};
    const auto clip=camera.projection(rect.width/rect.height,reversed)*camera.view()*glm::vec4(world,1);
    if(clip.w<=.00001f)return {};
    const auto ndc=glm::vec3(clip)/clip.w;
    if(ndc.z<0||ndc.z>1)return {};
    return glm::vec2(rect.x+(ndc.x+1)*rect.width*.5f,rect.y+(1-ndc.y)*rect.height*.5f);
}
inline Ray handle_ray(glm::vec2 screen,const Camera& camera,const ViewportRect& rect,bool reversed) {
    if(rect.width<=0||rect.height<=0)throw std::invalid_argument("Empty viewport");
    const glm::vec2 ndc{2*(screen.x-rect.x)/rect.width-1,1-2*(screen.y-rect.y)/rect.height};
    const auto q=glm::inverse(camera.projection(rect.width/rect.height,reversed)*camera.view())*glm::vec4(ndc,reversed?1.f:0.f,1);
    return {camera.position,safe_normalize(glm::vec3(q)/q.w-camera.position),.0001f,camera.far_plane};
}
// 求鼠标射线与世界坐标轴之间的最近点。轴几乎正对镜头时禁用，避免除以极小数后跳变。
inline std::optional<float> axis_parameter(const Ray& ray,glm::vec3 origin,glm::vec3 axis) {
    const float b=glm::dot(axis,ray.direction),denominator=1-b*b;
    if(denominator<.015f)return {};
    const auto r=ray.origin-origin;
    return (glm::dot(axis,r)-b*glm::dot(ray.direction,r))/denominator;
}
inline float segment_distance(glm::vec2 p,glm::vec2 a,glm::vec2 b) {
    const auto delta=b-a;const float length2=glm::dot(delta,delta);
    const float t=length2>1e-6f?std::clamp(glm::dot(p-a,delta)/length2,0.f,1.f):0;
    return glm::length(p-(a+t*delta));
}
struct TranslationDrag {
    bool active=false;int axis=0,light=-1,node=-1;
    glm::vec3 origin{0};float initial_parameter=0;
    // 保留平移字段布局；旋转/缩放也按拖动起始矩阵计算，避免每帧累乘产生漂移。
    int operation=0; // 0 平移，1 世界轴旋转，2 局部轴/统一缩放。
    glm::mat4 initial_local{1},initial_world{1};
    glm::vec3 axis_direction{1,0,0},initial_direction{1,0,0};
    glm::vec2 initial_mouse{0};
    float handle_length=1,last_angle=0,accumulated_angle=0;
};
} // namespace emberframe::lab::editor
