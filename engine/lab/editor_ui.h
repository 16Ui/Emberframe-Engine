#pragma once
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>

namespace emberframe::lab::editor {
struct Layout {
    float width=0,height=0,left=0,right=0;
    float menu=36,toolbar=34,status=26;
    float body_height() const { return std::max(0.f,height-menu-status); }
    float viewport_width() const { return std::max(0.f,width-left-right); }
    float viewport_height() const { return std::max(0.f,height-menu-toolbar-status); }
};
inline Layout layout(float width,float height,bool show_scene,bool show_inspector,float scene_width,float inspector_width) {
    Layout result;result.width=std::max(0.f,width);result.height=std::max(0.f,height);
    // 小窗口优先保留视口；场景树自动收起，仍可通过菜单重新进入。
    if(show_scene&&width>=1100)result.left=std::clamp(scene_width,190.f,280.f);
    if(show_inspector)result.right=std::min(std::clamp(inspector_width,300.f,420.f),std::max(0.f,width-320.f-result.left));
    return result;
}
struct RenderSize { int width,height; };
inline RenderSize fit_resolution(float width,float height,float scale,
    std::uint64_t max_tiles=std::numeric_limits<std::uint64_t>::max(),
    std::uint64_t max_pixels=8294400) {
    if(width<=0||height<=0)return {8,8};
    // 默认 1x 指一个物理视口像素对应一个渲染像素，不再被 2048 上限暗中缩小。
    // 当前工作台同时持有多张全分辨率实验目标，最多 4K 像素量，并考虑设备灯表/显存预算。
    max_pixels=std::max<std::uint64_t>(64,max_pixels);max_tiles=std::max<std::uint64_t>(1,max_tiles);
    scale=std::min({std::clamp(scale,.5f,1.5f),4096.f/width,4096.f/height,
        float(std::sqrt(double(max_pixels)/(double(width)*height)))});
    auto size=[&](){return RenderSize{std::max(8,int(std::floor(width*scale))),std::max(8,int(std::floor(height*scale)))};};
    auto result=size();
    // 16x16 clustered tile 有向上取整；仅限制总像素还不足以保证 Buffer 满足驱动上限。
    while(std::uint64_t((result.width+15)/16)*((result.height+15)/16)>max_tiles){
        scale*=.99f;result=size();
    }
    return result;
}
inline bool properties(const char* id) {
    if(!ImGui::BeginTable(id,2,ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_NoSavedSettings))return false;
    ImGui::TableSetupColumn("Property",ImGuiTableColumnFlags_WidthFixed,112);
    ImGui::TableSetupColumn("Value",ImGuiTableColumnFlags_WidthStretch);
    return true;
}
inline void row(const char* label) {
    ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();ImGui::SetNextItemWidth(-FLT_MIN);ImGui::PushID(label);
}
template<class T> bool choice(const char* label,T& value,const char* entries) {
    row(label);int n=int(value);const bool changed=ImGui::Combo("##value",&n,entries);ImGui::PopID();if(changed)value=T(n);return changed;
}
inline bool toggle(const char* label,bool& value) {
    row(label);const bool changed=ImGui::Checkbox("##value",&value);ImGui::PopID();return changed;
}
inline bool scalar(const char* label,float& value,float lo,float hi,const char* format="%.2f",ImGuiSliderFlags flags=0) {
    row(label);const bool changed=ImGui::SliderFloat("##value",&value,lo,hi,format,flags);ImGui::PopID();return changed;
}
inline bool integer(const char* label,int& value,int lo,int hi) {
    row(label);const bool changed=ImGui::SliderInt("##value",&value,lo,hi);ImGui::PopID();return changed;
}
inline bool color(const char* label,float* value,ImGuiColorEditFlags flags=0) {
    row(label);const bool changed=ImGui::ColorEdit3("##value",value,flags|ImGuiColorEditFlags_NoLabel);ImGui::PopID();return changed;
}
inline bool vector(const char* label,float* value,float speed,float lo=0,float hi=0) {
    row(label);const bool changed=ImGui::DragFloat3("##value",value,speed,lo,hi,"%.2f");ImGui::PopID();return changed;
}
inline void title(const char* text) {
    ImGui::TextUnformatted(text);ImGui::Separator();
}
inline bool section(const char* label,bool open=false) {
    return ImGui::CollapsingHeader(label,open?ImGuiTreeNodeFlags_DefaultOpen:0);
}
} // namespace emberframe::lab::editor
