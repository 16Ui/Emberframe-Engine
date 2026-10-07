#pragma once
#include "editor_transform.h"
#include "editor_history.h"
#include "editor_objects.h"
#include "visual_experiments.h"

namespace emberframe::lab::editor {
enum class TopicEntry { current_scene,preset_scene,cpu_comparison };
inline const char* topic_entry_name(TopicEntry entry) {
    switch(entry){case TopicEntry::preset_scene:return "预设场景演示";
        case TopicEntry::cpu_comparison:return "独立 CPU 对照";default:return "应用到当前场景";}
}
inline bool topic_entry_available(int topic,TopicEntry entry) {
    if(topic<0||topic>=31||topic==5)return false;
    if(entry==TopicEntry::cpu_comparison)return supports_visual_experiment(topic)||topic==9||topic==23||topic==24;
    // 包围体相交、采样策略等不伪装成可见特效；BVH/八叉树作用于真实选物/射线查询。
    if(entry==TopicEntry::current_scene)return topic!=8;
    return topic!=2&&topic!=3&&topic!=4&&topic!=6&&topic!=7&&topic!=8;
}
// 一次预览保存一次原状态；切换专题不覆盖它。原 UndoStack 的回调继续引用主程序变量。
struct TopicReturnState {
    Snapshot original;
    UndoStack history{64,256*1024*1024};
    int preset=0,visual_topic=-1,selected_node=-1,selected_light=-1,selected_material=0,selected_mesh=0;
    int requested_panel=0;
    PropertyTarget property_target=PropertyTarget::none;
    int selected_texture=-1;
    bool fit_viewport=true,experiment_current_scene=false,show_move_tool=true,uniform_scale=true;
    float render_scale=1;
    TransformTool transform_tool=TransformTool::translate;
    Camera replay_origin;
    bool replay_running=false;
    std::size_t replay_index=0;
    std::string replay;
    std::filesystem::path project_path;
    std::array<char,1024> project_path_text{};
};
} // namespace emberframe::lab::editor
