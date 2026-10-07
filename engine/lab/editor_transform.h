#pragma once
#include "editor_gizmo.h"
#include "systems.h"

namespace emberframe::lab::editor {
enum class TransformTool { translate,rotate,scale };
inline const char* transform_tool_name(TransformTool tool) {
    switch(tool){case TransformTool::rotate:return "旋转";case TransformTool::scale:return "缩放";default:return "平移";}
}
inline glm::vec3 world_axis(int axis) { glm::vec3 result(0);result[axis]=1;return result; }
inline glm::vec3 ring_point(glm::vec3 origin,int axis,float radius,float angle) {
    return origin+radius*(std::cos(angle)*world_axis((axis+1)%3)+std::sin(angle)*world_axis((axis+2)%3));
}
// 射线与旋转圆环所在平面求交；接近平行时不启动拖动，避免角度突然跳变。
inline std::optional<glm::vec3> ring_direction(const Ray& ray,glm::vec3 origin,glm::vec3 normal) {
    const float denominator=glm::dot(ray.direction,normal);
    if(std::abs(denominator)<.02f)return {};
    const float t=glm::dot(origin-ray.origin,normal)/denominator;
    if(t<=0)return {};
    const auto radial=ray.origin+t*ray.direction-origin;
    if(glm::dot(radial,radial)<1e-12f)return {};
    return safe_normalize(radial);
}
inline void require_editable_matrix(const glm::dmat4& matrix) {
    for(int c=0;c<4;++c)for(int r=0;r<4;++r)
        if(!std::isfinite(matrix[c][r]))throw std::invalid_argument("变换包含无效或过大的数值。");
    if(std::abs(matrix[0].w)>1e-6||std::abs(matrix[1].w)>1e-6||std::abs(matrix[2].w)>1e-6||std::abs(matrix[3].w-1)>1e-6)
        throw std::invalid_argument("模型变换必须是仿射矩阵。");
    const glm::dmat3 axes(matrix);
    const double measure=glm::length(axes[0])*glm::length(axes[1])*glm::length(axes[2]);
    if(measure<=0||!std::isfinite(measure)||std::abs(glm::determinant(axes))<=measure*1e-12)
        throw std::invalid_argument("缩放不能为零，变换坐标轴不能退化。");
}
inline void set_node_local(Scene& scene,int node,const glm::mat4& local) {
    require_editable_matrix(glm::dmat4(local));
    if(scene.revision==UINT64_MAX)throw std::overflow_error("Scene revision exhausted");
    scene.nodes.at(node).local=local;scene.baked_resources.reset();++scene.revision;
}
inline void set_node_world(Scene& scene,int index,const glm::mat4& world) {
    glm::dmat4 local(world);const auto& node=scene.nodes.at(index);
    if(node.parent>=0){const auto parent=glm::dmat4(scene_world_transforms(scene).at(node.parent));
        require_editable_matrix(parent);local=glm::inverse(parent)*local;}
    set_node_local(scene,index,glm::mat4(local));
}
struct LocalTransform {
    glm::vec3 position{0},rotation_degrees{0},scale{1};
    glm::vec3 shear{0}; // XY / XZ / YZ；编辑角度时保留导入模型已有的剪切。
};
inline LocalTransform local_transform(const glm::mat4& matrix) {
    require_editable_matrix(glm::dmat4(matrix));LocalTransform result;result.position=glm::vec3(matrix[3]);
    glm::vec3 x(matrix[0]),y(matrix[1]),z(matrix[2]);
    result.scale.x=glm::length(x);x/=result.scale.x;
    float xy=glm::dot(x,y);y-=xy*x;result.scale.y=glm::length(y);y/=result.scale.y;
    float xz=glm::dot(x,z);z-=xz*x;float yz=glm::dot(y,z);z-=yz*y;
    result.scale.z=glm::length(z);z/=result.scale.z;
    // 将反射保留为有符号缩放，而不是抹掉导入模型的镜像变换。
    if(glm::dot(glm::cross(x,y),z)<0){x=-x;result.scale.x=-result.scale.x;xy=-xy;xz=-xz;}
    result.shear={xy/result.scale.y,xz/result.scale.z,yz/result.scale.z};
    const float pitch=std::asin(std::clamp(-x.z,-1.f,1.f));
    glm::vec3 angles{0,pitch,0};
    if(std::abs(std::cos(pitch))>1e-5f){angles.x=std::atan2(y.z,z.z);angles.z=std::atan2(x.y,x.x);}
    else angles.z=std::atan2(-y.x,y.y);
    result.rotation_degrees=glm::degrees(angles);return result;
}
inline glm::mat4 compose_local_transform(const LocalTransform& pose) {
    for(int i=0;i<3;++i)if(!std::isfinite(pose.scale[i])||std::abs(pose.scale[i])<1e-4f)
        throw std::invalid_argument("缩放绝对值不能小于 0.0001；负值表示镜像。");
    const auto angle=glm::radians(pose.rotation_degrees);
    // 检查器采用局部 XYZ 欧拉角，矩阵为 Rz * Ry * Rx。位置不会被旋转或缩放带走。
    const auto rotation=glm::rotate(glm::mat4(1),angle.z,world_axis(2))*
        glm::rotate(glm::mat4(1),angle.y,world_axis(1))*glm::rotate(glm::mat4(1),angle.x,world_axis(0));
    const glm::vec3 x(rotation[0]),y(rotation[1]),z(rotation[2]);glm::mat4 result(1);
    result[0]=glm::vec4(x*pose.scale.x,0);
    result[1]=glm::vec4((x*pose.shear.x+y)*pose.scale.y,0);
    result[2]=glm::vec4((x*pose.shear.y+y*pose.shear.z+z)*pose.scale.z,0);
    result[3]=glm::vec4(pose.position,1);require_editable_matrix(glm::dmat4(result));return result;
}
} // namespace emberframe::lab::editor
