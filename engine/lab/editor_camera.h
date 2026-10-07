#pragma once
#include "geometry.h"

namespace emberframe::lab::editor {
// 按实际世界包围盒聚焦，而不是假定资源至少有半米大小。
// glTF 资源可能以毫米导出；相机距离与裁剪面都必须随模型尺寸调整。
inline void frame_bounds(const Aabb& bounds,Camera& camera) {
    if(bounds.empty())return;
    const double measured=glm::length(glm::dvec3(bounds.extent()))*.5;
    if(!std::isfinite(measured)||measured<=0)return;
    const float radius=float(std::max(measured,1e-7));
    const float half_fov=glm::radians(std::clamp(camera.fov,1.f,179.f))*.5f;
    const float distance=radius/std::sin(half_fov)*1.1f;
    camera.target=bounds.center();
    camera.position=camera.target+safe_normalize(glm::vec3(1,.6f,1.3f))*distance;
    camera.near_plane=std::max(1e-9f,radius*.002f);
    camera.far_plane=std::max(camera.near_plane*100.f,distance+radius*12.f);
}
}
