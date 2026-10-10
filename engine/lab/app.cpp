#define SDL_MAIN_HANDLED
#include "types.h"
#include "config_applicability.h"
#include "systems.h"
#include "geometry.h"
#include "shading.h"
#include "environment.h"
#include "showcase.h"
#include "shadow_gi.h"
#include "postprocess.h"
#include "reference_renderer.h"
#include "temporal_session.h"
#include "mesh_lod.h"
#include "scene_resources.h"
#include "scene_geometry.h"
#include "visual_experiments.h"
#include "gpu_upload.h"
#include "editor_demos.h"
#include "shader_assets.h"
#include "session_log.h"
#include "vulkan_workbench.h"
#include "diagnostics.h"
#include "editor_ui.h"
#include "editor_assets.h"
#include "editor_lights.h"
#include "editor_gizmo.h"
#include "editor_camera.h"
#include "editor_transform.h"
#include "editor_topics.h"
#include "editor_files.h"
#include "editor_scene.h"
#include "editor_objects.h"
#include "editor_history.h"
#include "resume_demo.h"
#include "platform/sdl_window.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <imgui.h>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <future>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>
#include <deque>
#include <cctype>

using namespace emberframe::lab;
namespace {
// SDL 拖入路径和 ImGui 文本都采用 UTF-8；Windows 文件系统使用宽字符。
// 明确转换，避免中文模型名经过系统代码页后变成乱码或找不到文件。
std::filesystem::path path_from_utf8(std::string_view text){
    return std::filesystem::path(std::u8string(text.begin(),text.end()));
}
std::string path_to_utf8(const std::filesystem::path& path){
    const auto text=path.u8string();return std::string(reinterpret_cast<const char*>(text.data()),text.size());
}
bool force_portable=false;
std::filesystem::path application_directory(){
    char* base=SDL_GetBasePath();const auto path=base?path_from_utf8(base):std::filesystem::current_path();SDL_free(base);return path;
}
bool portable_mode(){return force_portable||std::filesystem::exists(application_directory()/"portable.ini");}
std::filesystem::path asset_root(){return portable_mode()?application_directory():path_from_utf8(EMBERFRAME_SOURCE_DIR);}
std::filesystem::path runtime_output_directory(){
    if(!portable_mode())return asset_root()/"output";
    // 解压包可能位于只读目录；用户状态不写入源码路径或 Program Files。
    char* base=SDL_GetPrefPath("16Ui","EmberFrame");if(!base)throw std::runtime_error(SDL_GetError());
    auto root=path_from_utf8(base)/"output";SDL_free(base);return root;
}
int& texture_slot(Material& material,TextureRole role){
    switch(role){case TextureRole::base_color:return material.base_texture;case TextureRole::metallic_roughness:return material.mr_texture;
        case TextureRole::normal:return material.normal_texture;case TextureRole::occlusion:return material.ao_texture;default:return material.emissive_texture;}
}
int texture_slot(const Material& material,TextureRole role){
    switch(role){case TextureRole::base_color:return material.base_texture;case TextureRole::metallic_roughness:return material.mr_texture;
        case TextureRole::normal:return material.normal_texture;case TextureRole::occlusion:return material.ao_texture;default:return material.emissive_texture;}
}
const char* texture_role_name(TextureRole role){
    const char* names[]={"基础颜色","金属 / 粗糙度","法线","环境遮蔽 AO","自发光"};return names[int(role)];
}
// 后台 QEM 返回的 LOD0 必须仍与当前输入一致，不能覆盖任务期间的 UV/材质索引编辑。
// 不比较派生照度或名称；提交时仅替换 lods，保留当前原网格和所有无关编辑。
bool same_lod_input(const Mesh& a,const Mesh& b){
    if(a.indices!=b.indices||a.vertices.size()!=b.vertices.size()||a.primitives.size()!=b.primitives.size())return false;
    for(std::size_t i=0;i<a.vertices.size();++i){const auto& x=a.vertices[i];const auto& y=b.vertices[i];
        if(x.position!=y.position||x.normal!=y.normal||x.uv!=y.uv||x.tangent!=y.tangent||x.color!=y.color)return false;
    }
    for(std::size_t i=0;i<a.primitives.size();++i){const auto& x=a.primitives[i];const auto& y=b.primitives[i];
        if(x.first_index!=y.first_index||x.index_count!=y.index_count||x.material!=y.material)return false;
    }
    return true;
}
struct Options {
    Settings settings;
    // 工程加载提供默认设置；用户明确给出的启动参数必须随后覆盖，不能静默失效。
    std::vector<std::function<void(Settings&)>> setting_overrides;
    int preset=0,frames=0,visual_topic=-1,ui_panel=0,window_width=1440,window_height=900;
    bool headless=false,smoke=false,compare=false,reload_shaders=false,benchmark=false,gpu_hidden=false,no_ui=false;
    bool fit_viewport=true,maximized=false,verify_window=false;
    bool verify_editor=false;
    bool developer_tools=false;
    bool uncached_bounds=false;
    bool profiler_visible=false;
    bool new_scene=false;
    bool preset_explicit=false;
    int benchmark_frames=0;
    std::filesystem::path asset,project,output,screenshot;
    std::filesystem::path make_import_project;
    std::filesystem::path benchmark_output,graph_output;
    std::filesystem::path profile_output;
    std::filesystem::path environment_path;
    std::string showcase;
    bool object_replay=false;
    std::string resume_demo,replay;
    std::filesystem::path sequence_directory,demo_stats;
    int sequence_frames=48;
};
int number(const char* value,int minimum,int maximum){
    int n=0;const std::string_view s(value);auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),n);
    if(e!=std::errc()||p!=s.data()+s.size()||n<minimum||n>maximum)throw std::invalid_argument("Invalid numeric argument: "+std::string(value));return n;
}
Options parse(int argc,char** argv){
    Options o;o.settings.energy_compensation=false;
    auto set=[&]<class T>(T Settings::*member,T value){
        o.settings.*member=value;
        o.setting_overrides.emplace_back([member,value](Settings& target){target.*member=value;});
    };
    o.settings.shadow_resolution=1024; // 日常预览避免默认低分辨率阴影被误认为整个画面像素低。
    // 双击启动优先清晰预览；脚本回归仍可显式指定小分辨率。
    if(argc==1){o.settings.render_width=1280;o.settings.render_height=720;}
    for(int i=1;i<argc;++i){
        const std::string a=argv[i];
        auto value=[&](){if(i+1>=argc)throw std::invalid_argument("Missing value for "+a);return argv[++i];};
        if(a=="--headless")o.headless=true;
        else if(a=="--portable")force_portable=true;
        else if(a=="--environment")o.environment_path=path_from_utf8(value());
        else if(a=="--showcase"){o.showcase=value();(void)parse_showcase(o.showcase);}
        else if(a=="--object-replay")o.object_replay=true;
        else if(a=="--developer-tools")o.developer_tools=true;
        else if(a=="--new-scene")o.new_scene=true;
        else if(a=="--make-import-project")o.make_import_project=path_from_utf8(value());
        else if(a=="--gpu-hidden")o.gpu_hidden=true;
        else if(a=="--no-ui")o.no_ui=true;
        else if(a=="--resume-demo"){o.resume_demo=value();o.settings=make_resume_demo(o.resume_demo).settings;
            o.setting_overrides.emplace_back([preset=o.settings](Settings& target){target=preset;});}
        else if(a=="--camera-replay")o.replay=value();
        else if(a=="--capture-sequence")o.sequence_directory=path_from_utf8(value());
        else if(a=="--sequence-frames")o.sequence_frames=number(value(),8,240);
        else if(a=="--demo-stats")o.demo_stats=path_from_utf8(value());
        else if(a=="--benchmark")o.benchmark=true;
        else if(a=="--benchmark-frames")o.benchmark_frames=number(value(),3,1000);
        else if(a=="--benchmark-output")o.benchmark_output=path_from_utf8(value());
        else if(a=="--graph-output")o.graph_output=path_from_utf8(value());
        else if(a=="--profile-output")o.profile_output=path_from_utf8(value());
        else if(a=="--uncached-bounds")o.uncached_bounds=true;
        else if(a=="--profiler")o.profiler_visible=true;
        else if(a=="--compare"){o.compare=true;o.frames=8;o.fit_viewport=false;}
        else if(a=="--reload-shaders")o.reload_shaders=true;
        else if(a=="--smoke-test"){o.smoke=true;o.frames=12;}
        else if(a=="--preset"){o.preset=number(value(),0,3);o.preset_explicit=true;}
        else if(a=="--topic")o.visual_topic=number(value(),1,32)-1;
        else if(a=="--frames")o.frames=number(value(),1,100000);
        else if(a=="--window-width")o.window_width=number(value(),640,3840);
        else if(a=="--window-height")o.window_height=number(value(),480,2160);
        else if(a=="--ui-panel"){o.ui_panel=number(value(),0,4);if(o.ui_panel==4)o.developer_tools=true;}
        else if(a=="--width"){set(&Settings::render_width,number(value(),8,4096));o.fit_viewport=false;}
        else if(a=="--height"){set(&Settings::render_height,number(value(),8,4096));o.fit_viewport=false;}
        else if(a=="--fit-viewport")o.fit_viewport=true;
        else if(a=="--maximized")o.maximized=true;
        else if(a=="--verify-window")o.verify_window=true;
        else if(a=="--verify-editor")o.verify_editor=true;
        else if(a=="--samples")set(&Settings::samples,number(value(),1,4096));
        else if(a=="--bounces")set(&Settings::max_bounces,number(value(),1,32));
        else if(a=="--output")o.output=path_from_utf8(value());
        else if(a=="--screenshot")o.screenshot=path_from_utf8(value());
        else if(a=="--load")o.asset=path_from_utf8(value());
        else if(a=="--project")o.project=path_from_utf8(value());
        else if(a=="--path"){
            const std::string v=value();
            if(v=="forward")set(&Settings::path,RenderPath::forward);
            else if(v=="deferred")set(&Settings::path,RenderPath::deferred);
            else if(v=="cpu")set(&Settings::path,RenderPath::cpu_raster);
            else if(v=="trace")set(&Settings::path,RenderPath::path_trace);
            else throw std::invalid_argument("Unknown path");
        }else if(a=="--shadow")set(&Settings::shadows,ShadowMode(number(value(),0,6)));
        else if(a=="--gi")set(&Settings::gi,GiMode(number(value(),0,6)));
        else if(a=="--environment-diffuse")set(&Settings::environment_diffuse,EnvironmentDiffuse(number(value(),0,2)));
        else if(a=="--sdf-shadows")set(&Settings::sdf_shadows,true);
        else if(a=="--dense-voxels")set(&Settings::sparse_voxels,false);
        else if(a=="--sdf-resolution")set(&Settings::sdf_resolution,number(value(),8,64));
        else if(a=="--bake-samples")set(&Settings::bake_samples,number(value(),16,4096));
        else if(a=="--auto-lod")set(&Settings::auto_lod,true);
        else if(a=="--spatial")set(&Settings::spatial_structure,SpatialStructure(number(value(),0,1)));
        else if(a=="--ao")set(&Settings::ao,AoMode(number(value(),0,2)));
        else if(a=="--culling")set(&Settings::culling,LightCulling(number(value(),0,2)));
        else if(a=="--debug")set(&Settings::debug,DebugView(number(value(),0,11)));
        else if(a=="--shading")set(&Settings::shading,ShadingMode(number(value(),0,3)));
        else if(a=="--filter")set(&Settings::filter,FilterMode(number(value(),0,3)));
        else if(a=="--energy")set(&Settings::energy_compensation,true);
        else if(a=="--normal-z")set(&Settings::reversed_z,false);
        else if(a=="--no-bloom")set(&Settings::bloom,false);
        else if(a=="--taa")set(&Settings::taa,true);
        else if(a=="--denoise")set(&Settings::denoise,true);
        else if(a=="--svgf")set(&Settings::svgf,true);
        else if(a=="--outline")set(&Settings::outline,true);
        else if(a=="--hatching")set(&Settings::hatching,true);
        else if(a=="--help"){
            std::cout<<"EmberFrame Workbench\n"
              <<"  --preset 0..3   Material studio / GI room / shadows / many lights\n"
              <<"  --load model.glb|model.gltf|model.obj   --project scene.ember\n"
              <<"  --new-scene (empty editable scene)\n"
              <<"  --make-import-project NEW.ember (create free-asset project; no render or tests)\n"
              <<"  --path forward|deferred|cpu|trace\n"
              <<"  --headless --width 640 --height 360 --samples 8 --output result.bmp\n"
              <<"  --shadow 0..6: hard,PCF,PCSS,VSM,VSSM,MSM,CSM\n"
              <<"  --gi 0..6: environment,SSR,SSGI,RSM,LPV,voxel,none\n"
              <<"  --environment-diffuse 0..2: IBL,SH,PRT --sdf-shadows\n"
              <<"  --bake-samples 16..4096 --sdf-resolution 8..64 --auto-lod --spatial 0..1: BVH,Octree\n"
              <<"  --ao 0..2: none,SSAO,GTAO\n"
              <<"  --culling 0..2: all,tiled,clustered --energy --normal-z\n"
              <<"  --debug 0..11 --shading 0..3 --filter 0..3\n"
              <<"  --taa --denoise --svgf --frames 8 (multi-frame reference)\n"
              <<"  --topic 11|12|13|30 (explicit CPU comparison; normal scene uses rendering options)\n"
              <<"  --compare (GPU/CPU albedo check) --reload-shaders\n"
              <<"  --frames N --screenshot capture.bmp --smoke-test --no-bloom\n"
              <<"  --maximized --verify-window (window lifecycle diagnostic)\n";
            std::cout<<"  --gpu-hidden (real Vulkan, no visible window)\n  --benchmark --benchmark-output DIR (JobSystem)\n  --benchmark-frames N --benchmark-output DIR --graph-output DIR\n";
            std::cout<<"  --profile-output NEW_DIR (completed per-Pass CPU/GPU JSON) --uncached-bounds (controlled bounds A/B)\n";
            std::cout<<"  --portable (exe assets; user-state outside the source checkout)\n";
            std::cout<<"  --window-width 640..3840 --window-height 480..2160\n  --ui-panel 0..4 (render/material/demos/project/developer)\n  --developer-tools (show manual CPU references and diagnostics; starts nothing)\n  --fit-viewport (default; explicit --width/--height keeps fixed output size)\n";
            std::cout<<"  --resume-demo pbr|kc|lights|shadows|temporal|geometry (place before overrides)\n"
                     <<"  --camera-replay static|pan|orbit|dolly --capture-sequence NEW_DIR\n"
                     <<"  --sequence-frames 8..240 --demo-stats result.json\n";
            std::exit(0);
        }else throw std::invalid_argument("Unknown argument: "+a);
    }
    if(!o.replay.empty())replay_demo_camera(Camera{},o.replay,0,8);
    if(!o.sequence_directory.empty()&&(o.headless||o.compare||o.benchmark||o.benchmark_frames||!o.output.empty()||o.visual_topic>=0))
        throw std::invalid_argument("Sequence capture requires a separate Vulkan run; cannot combine headless/output/compare/benchmark/topic");
    return o;
}
void frame_camera(const Scene& scene,Camera& camera){
    if(scene.name=="Colored-box GI room"){
        // 房间从正面的开口观察；按整个地板包围盒聚焦会落到墙外，看不到 GI。
        camera.position={0,1.55f,6.8f};camera.target={0,1.4f,0};
        camera.near_plane=.05f;camera.far_plane=40;return;
    }
    Aabb bounds;for(const auto& t:flatten_scene(scene))for(const auto& v:t.vertices)bounds.expand(v.position);
    if(bounds.empty())return;
    editor::frame_bounds(bounds,camera);
}
template<class T> bool combo(const char* title,T& value,const char* entries){
    int n=int(value);if(ImGui::Combo(title,&n,entries)){value=T(n);return true;}return false;
}
void help(const char* text){
    if(ImGui::IsItemHovered()){ImGui::BeginTooltip();ImGui::PushTextWrapPos(ImGui::GetFontSize()*26);ImGui::TextUnformatted(text);ImGui::PopTextWrapPos();ImGui::EndTooltip();}
}
void theme(){
    auto& s=ImGui::GetStyle();ImGui::StyleColorsDark();
    // 编辑器工具风格：中性灰、细边框、紧凑行高；蓝色仅标记选择/激活，不把每个按钮涂成蓝卡片。
    s.WindowRounding=0;s.ChildRounding=0;s.FrameRounding=2;s.GrabRounding=1;s.TabRounding=0;s.PopupRounding=3;
    s.FramePadding={6,3};s.ItemSpacing={7,5};s.ItemInnerSpacing={5,4};s.WindowPadding={10,9};s.CellPadding={0,3};
    s.WindowBorderSize=1;s.ChildBorderSize=1;s.FrameBorderSize=1;s.TabBorderSize=0;s.ScrollbarSize=10;s.GrabMinSize=8;
    s.Colors[ImGuiCol_Text]={.83f,.84f,.86f,1};s.Colors[ImGuiCol_TextDisabled]={.48f,.50f,.53f,1};
    s.Colors[ImGuiCol_WindowBg]={.115f,.122f,.135f,1};s.Colors[ImGuiCol_ChildBg]={.115f,.122f,.135f,1};
    s.Colors[ImGuiCol_PopupBg]={.15f,.16f,.18f,1};s.Colors[ImGuiCol_Border]={.225f,.235f,.255f,1};
    s.Colors[ImGuiCol_FrameBg]={.085f,.092f,.104f,1};s.Colors[ImGuiCol_FrameBgHovered]={.17f,.185f,.21f,1};s.Colors[ImGuiCol_FrameBgActive]={.20f,.23f,.27f,1};
    s.Colors[ImGuiCol_Button]={.18f,.19f,.21f,1};s.Colors[ImGuiCol_ButtonHovered]={.25f,.27f,.30f,1};s.Colors[ImGuiCol_ButtonActive]={.28f,.34f,.42f,1};
    s.Colors[ImGuiCol_CheckMark]={.43f,.64f,.85f,1};s.Colors[ImGuiCol_SliderGrab]={.40f,.45f,.52f,1};s.Colors[ImGuiCol_SliderGrabActive]={.47f,.66f,.87f,1};
    s.Colors[ImGuiCol_Header]={.185f,.195f,.215f,1};s.Colors[ImGuiCol_HeaderHovered]={.24f,.26f,.29f,1};s.Colors[ImGuiCol_HeaderActive]={.23f,.34f,.46f,1};
    s.Colors[ImGuiCol_Tab]={.115f,.122f,.135f,1};s.Colors[ImGuiCol_TabHovered]={.24f,.26f,.29f,1};s.Colors[ImGuiCol_TabActive]={.225f,.26f,.31f,1};
    s.Colors[ImGuiCol_Separator]={.24f,.25f,.27f,1};s.Colors[ImGuiCol_ScrollbarBg]={.10f,.107f,.12f,1};s.Colors[ImGuiCol_ScrollbarGrab]={.26f,.28f,.31f,1};
    // UI 颜色以视觉上的 sRGB 值设计；sRGB Attachment 会编码，先转成线性以免界面发灰。
    for(auto& c:s.Colors){c.x=srgb_to_linear(c.x);c.y=srgb_to_linear(c.y);c.z=srgb_to_linear(c.z);}
}
TestResults run_checks(int group){
    switch(group){
    case 0:return test_geometry();case 1:return test_shading();case 2:return test_shadow_gi();
    case 3:return test_postprocess();case 4:return test_systems();case 5:return test_reference();case 6:return test_temporal_session();
    case 7:return test_mesh_lod();case 8:return test_visual_experiments();
    case 9:return test_shader_assets(EMBERFRAME_GLSLANG_PATH);
        case 11:return test_diagnostics();case 12:return test_scene_resources();case 13:return test_scene_geometry();case 14:return test_editor_assets();case 15:return test_editor_lights();
    default:{
        TestResults result;for(int i=0;i<16;++i){if(i==10)continue;auto v=run_checks(i);result.insert(result.end(),v.begin(),v.end());}return result;
    }}
}
struct Background {
    std::future<RenderOutput> render;
    std::atomic<bool> cancel{false};
    std::uint64_t generation=0;
    bool visual=false; // 独立对照不经过 ReferenceSession，不能使用它的累积帧计数。
    ~Background(){cancel=true;if(render.valid())render.wait();}
    bool busy()const{return render.valid();}
};
struct Topic{const char* name;const char* why;int group;};
const Topic topics[]={
 {"D01 齐次裁剪","在除以 w 前裁掉越界几何，保留交点属性。",0},
 {"D02 共享边覆盖","让相邻三角形在共享边上恰好覆盖一次。",0},
 {"D03 纹理足迹与过滤","比较最近邻、双线性、Mip、各向异性。",1},
 {"D04 深度精度","反向浮点深度、端点和重建约定。",5},
 {"D05 CPU/GPU 对照","相同场景、矩阵、采样和光照分项对照。",5},
 {"D06 包围体与相交","射线、三角形、AABB、球与 OBB。",0},
 {"D07 BVH","SAH 构建、遍历与动态 refit。",0},
 {"D08 八叉树","空间分区，跨格对象驻留父节点。",0},
 {"D09 采样与 MIS","概率密度、重要性采样和多策略权重。",1},
 {"D10 路径追踪","BVH、直接光采样、MIS 和多次反弹。",5},
 {"D11 球谐 SH","方向函数投影与漫反射余弦卷积。",1},
 {"D12 PRT","可见性传输系数和低阶光泽传输矩阵。",1},
 {"D13 IBL 预计算","环境预过滤、BRDF LUT 和数值积分。",1},
 {"D14 Kulla–Conty","预计算方向反照率，补偿微表面多次散射。",1},
 {"D15 Disney 材质","清漆、各向异性与织物反射分量。",1},
 {"D16 LTC 面积光","拟合变换余弦分布并解析积分矩形光。",1},
 {"D17 前向 / 延迟","改变光照计算时机，共享材质 BRDF。",4},
 {"D18 分块 / 聚簇灯表","GPU 建立每组候选灯索引列表。",4},
 {"D19 VSM / SAT","过滤矩统计，通过区域和估计可见性。",2},
 {"D20 VSSM","统计近似遮挡比例与条件遮挡深度。",2},
 {"D21 MSM","四矩可见性估计与数值稳定。",2},
 {"D22 SSR","相机深度查询反射命中和环境回退。",2},
 {"D23 SSGI","从屏幕可见表面估计一次间接光。",2},
 {"D24 HW5 时空降噪","联合双边、历史匹配、拒绝、钳制、累积。",3},
 {"D25 SVGF","历史矩与方差引导多尺度滤波。",3},
 {"D26 RSM","光源可见表面作为间接光样本。",2},
 {"D27 LPV","SH 网格注入、邻格传播和查询。",2},
 {"D28 体素化","保守占用、辐亮度与三维层级。",2},
 {"D29 体素锥体追踪","按锥体足迹选层级并累积光与遮挡。",2},
 {"D30 SDF","距离场生成、保守步进与距离场阴影。",2},
 {"D31 非真实感渲染","色带、轮廓与排线的稳定表达。",3},
 {"D32 统一验收","固定输入、错误场景、数值与性能报告。",10}
};
void validate_gpu_reference(const FrameReadback& frame,const Scene& scene,const Camera& camera,Settings settings,const std::filesystem::path& output){
    settings.path=RenderPath::cpu_raster;settings.debug=DebugView::albedo;
    settings.render_width=frame.image.width;settings.render_height=frame.image.height;
    settings.gi=GiMode::none;settings.bloom=false;settings.taa=settings.denoise=settings.svgf=false;
    settings.outline=settings.hatching=false;
    const auto reference=render_reference(scene,camera,settings);
    Image<glm::vec3> difference(frame.image.width,frame.image.height);
    double total=0;float worst=0;std::size_t count=0,bad=0;
    for(int y=1;y<frame.image.height-1;++y)for(int x=1;x<frame.image.width-1;++x){
        const auto& p=reference.surfaces.at(x,y);if(!p.valid)continue;
        // 排除几何轮廓和对象交界，只比较两侧约定明确的内部像素；不按 GPU 结果筛样本。
        bool interior=true;for(auto d:{glm::ivec2(-1,0),glm::ivec2(1,0),glm::ivec2(0,-1),glm::ivec2(0,1)}){
            const auto& q=reference.surfaces.at(x+d.x,y+d.y);interior&=q.valid&&q.object==p.object;
        }if(!interior)continue;
        const auto error=glm::abs(frame.image.at(x,y)-p.albedo);
        const float peak=std::max({error.x,error.y,error.z});
        total+=(error.x+error.y+error.z)/3;worst=std::max(worst,peak);++count;bad+=peak>.01f;
        difference.at(x,y)=glm::min(error*20.f,glm::vec3(1));
    }
    if(count==0)throw std::runtime_error("CPU/GPU comparison had no covered interior pixels");
    const double mae=total/count,bad_ratio=double(bad)/count;
    save_pfm(output/"gpu-albedo.pfm",frame.image);save_pfm(output/"cpu-albedo.pfm",reference.color);
    save_bmp(output/"comparison-error-x20.bmp",difference);
    std::ofstream report(output/"comparison.json");
    report<<"{\"pixels\":"<<count<<",\"meanAbsoluteError\":"<<mae<<",\"maximumError\":"<<worst<<",\"badPixelFraction\":"<<bad_ratio<<"}\n";
    std::cout<<"[CPU/GPU] pixels="<<count<<" MAE="<<mae<<" max="<<worst<<" badRatio="<<bad_ratio<<"\n";
    if(mae>.005||bad_ratio>.01)throw std::runtime_error("CPU/GPU albedo comparison exceeded tolerance; inspect output/comparison.json");
}
int run_window(Options options,Scene scene,Camera camera){
    SessionLog session_log(runtime_output_directory()/"workbench.log");
    emberframe::platform::SdlWindow window({.title="EmberFrame Editor",.width=std::uint32_t(options.window_width),.height=std::uint32_t(options.window_height),.resizable=true,.borderless=true});
    SDL_SetWindowMinimumSize(window.native_handle(),640,480);
    // 初始尺寸按当前屏幕工作区收敛，高缩放下不能把底部状态栏放到任务栏下面。
    SDL_Rect usable{};
    if(SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(window.native_handle()),&usable)==0){
        const int width=std::min(options.window_width,std::max(640,usable.w-24));
        const int height=std::min(options.window_height,std::max(480,usable.h-24));
        SDL_SetWindowSize(window.native_handle(),width,height);
        SDL_SetWindowPosition(window.native_handle(),usable.x+(usable.w-width)/2,usable.y+(usable.h-height)/2);
    }
    if(options.verify_window){
        // 验证只操作本次诊断进程自己的窗口，不读取/操作用户其它应用。
        SDL_PumpEvents();const auto restored=window.extent();
        window.toggle_maximized();SDL_PumpEvents();
        int x=0,y=0;SDL_GetWindowPosition(window.native_handle(),&x,&y);const auto maximized=window.extent();
        std::cout<<"[Window check] work="<<usable.x<<","<<usable.y<<","<<usable.w<<","<<usable.h<<" max="<<x<<","<<y<<","<<maximized.width<<","<<maximized.height<<" flag="<<window.maximized()<<"\n";
        if(!window.maximized()||x<usable.x-1||y<usable.y-1||x+int(maximized.width)>usable.x+usable.w+1||y+int(maximized.height)>usable.y+usable.h+1)throw std::runtime_error("Borderless maximize escaped monitor work area");
        window.toggle_maximized();SDL_PumpEvents();
        if(window.maximized()||window.extent().width!=restored.width||window.extent().height!=restored.height)throw std::runtime_error("Borderless restore dimensions changed");
        window.minimize();SDL_PumpEvents();
        if(!(SDL_GetWindowFlags(window.native_handle())&SDL_WINDOW_MINIMIZED))throw std::runtime_error("Borderless minimize failed");
        SDL_RestoreWindow(window.native_handle());SDL_PumpEvents();
        if(SDL_SetWindowFullscreen(window.native_handle(),SDL_WINDOW_FULLSCREEN_DESKTOP)!=0)throw std::runtime_error(SDL_GetError());
        SDL_PumpEvents();if(!window.fullscreen())throw std::runtime_error("Borderless fullscreen failed");
        if(SDL_SetWindowFullscreen(window.native_handle(),0)!=0)throw std::runtime_error(SDL_GetError());
        SDL_PumpEvents();if(window.fullscreen())throw std::runtime_error("Borderless fullscreen restore failed");
        std::cout<<"PASS window maximize/work-area/restore/minimize/fullscreen; DPI="<<window.display_scale()<<"\n";
    }
    if(options.maximized)window.toggle_maximized();
    if(options.gpu_hidden)SDL_HideWindow(window.native_handle());
    char* base_path=SDL_GetBasePath();
    const auto executable_dir=path_from_utf8(base_path?base_path:"");
    SDL_free(base_path);
    const auto shader_dir=executable_dir/"lab_shaders";
    VulkanWorkbench gpu(window.native_handle(),shader_dir);
    gpu.set_bounds_cache_enabled(!options.uncached_bounds);
    theme();
    auto settings=options.settings;UndoStack undo(64,256*1024*1024);ReferenceSession reference_session;ReferenceSessionInfo temporal_info;
    Camera replay_origin=camera;bool replay_running=!options.replay.empty();std::size_t replay_index=0;
    struct MotionReplay {int node=-1;glm::mat4 original{1};std::uint64_t tick=0;bool active=false;};
    MotionReplay motion_replay;
    const bool sequence_capture=!options.sequence_directory.empty();
    std::size_t sequence_index=0,sequence_warmup=0;bool sequence_pending=false;
    std::ofstream sequence_report;glm::vec3 capture_center(0);bool capture_center_valid=false;
    const auto sequence_started=std::chrono::steady_clock::now();
    auto next_progress=sequence_started+std::chrono::seconds(3);
    if(sequence_capture){
        if(std::filesystem::exists(options.sequence_directory))throw std::invalid_argument("Sequence directory exists; choose a new directory");
        std::filesystem::create_directories(options.sequence_directory);
        sequence_report.open(options.sequence_directory/"sequence.partial.json");
        if(!sequence_report)throw std::runtime_error("Cannot create sequence manifest");
        sequence_report<<"{\"schema\":\"emberframe.demo-sequence.v1\",\"profile\":"<<std::quoted(options.resume_demo)
            <<",\"motion\":"<<std::quoted(options.replay.empty()?"static":options.replay)
            <<",\"gpuTimingIncludesReadback\":true,\"frames\":[\n";
    }
    std::filesystem::path output_dir=runtime_output_directory();
    std::filesystem::create_directories(output_dir);
    GpuTimingReport gpu_timings;gpu_timings.warmup_frames=30;std::uint64_t last_timing_serial=0;std::size_t eligible_samples=0;
    std::vector<FrameProfile> measured_profiles;std::uint64_t last_profile_serial=0;
    std::future<SystemBenchmarkReport> system_benchmark;
    std::filesystem::path project_path=options.project.empty()?
        (options.new_scene?std::filesystem::path{}:output_dir/"session.ember"):options.project;
    std::string ini=path_to_utf8(output_dir/"workbench.ini");ImGui::GetIO().IniFilename=ini.c_str();
    std::array<char,1024> path_text{};std::snprintf(path_text.data(),path_text.size(),"%s",path_to_utf8(project_path).c_str());
    std::string notice="场景树右键新建模型 / 光源；Ctrl + 左键旋转物体，Alt + 左键环绕相机。下方资源编辑共享数据。",error;
    bool running=true,dirty=true,fullscreen=false;std::uint64_t generation=1,presented=0;
    // 内置场景的第一个网格是只有两个三角形的地板；默认选择后面的模型体验 QEM。
    int selected_material=0,selected_topic=0,preset=options.preset,visual_topic=options.visual_topic,selected_mesh=scene.meshes.size()>1?1:0;
    bool experiment_current_scene=!options.asset.empty();
    bool show_scene=true,show_inspector=true,show_profiler=options.profiler_visible;float scene_width=224,inspector_width=340;
    // 工具开关不读 ImGui ini：普通启动始终隐藏；CLI 显式 CPU/专题请求才打开。
    bool show_developer_tools=options.developer_tools||options.visual_topic>=0||
        settings.path==RenderPath::cpu_raster||settings.path==RenderPath::path_trace;
    int selected_demo=0,active_demo=-1,selected_cpu_reference=0;
    bool fit_viewport=options.fit_viewport;float render_scale=1;
    int viewport_pixel_width=0,viewport_pixel_height=0;bool resolution_limited=false;
    int requested_panel=options.ui_panel,selected_node=-1,selected_light=-1;
    editor::PropertyTarget property_target=editor::PropertyTarget::none;
    int selected_texture=-1;
    bool object_rotation_active=false;
    int object_rotation_node=-1;
    glm::vec2 object_rotation_start{0};glm::mat4 object_rotation_initial{1};
    if(options.visual_topic>=0)requested_panel=4;
    bool show_move_tool=true,replace_import=false;
    editor::TransformTool transform_tool=editor::TransformTool::translate;
    bool uniform_scale=true;
    std::unique_ptr<editor::TopicReturnState> topic_return;
    editor::TopicEntry active_topic_entry=editor::TopicEntry::current_scene;
    int active_topic=-1;
    editor::TranslationDrag translation_drag;
    std::shared_ptr<const editor::Snapshot> edit_before;
    std::string edit_label;
    std::uint64_t document_epoch=1,import_epoch=0,texture_epoch=0;
    bool imported_replace=false;
    std::deque<std::filesystem::path> model_queue;
    std::filesystem::path replace_request,texture_request;
    bool open_texture_popup=false,open_replace_popup=false;
    bool open_new_scene_popup=false,new_scene_floor=true,new_scene_light=true;
    std::array<char,128> new_scene_name{};std::snprintf(new_scene_name.data(),new_scene_name.size(),"Untitled scene");
    TextureRole texture_role=TextureRole::base_color;
    int texture_material=0,imported_texture_material=0;
    TextureRole imported_texture_role=TextureRole::base_color;
    std::future<Texture> imported_texture;
    std::array<char,1024> texture_path_text{};
    std::array<char,1024> model_path_text{};
    std::array<char,128> scene_filter{};
    std::future<QemResult> simplified;std::uint64_t simplification_generation=0,simplification_epoch=0;int simplification_mesh=0;
    SceneGeometryRuntime scene_geometry;
    std::future<Mesh> lod_chain_build;std::uint64_t lod_source_hash=0,lod_epoch=0;int lod_mesh_index=0;
    std::future<std::shared_ptr<const SceneBakeResources>> scene_bake;
    std::atomic<bool> scene_bake_cancel{false};std::string bake_request_key,bake_running_key;std::uint64_t bake_epoch=0;
    std::unique_ptr<ShaderLibrary> shader_library;std::future<ShaderBuildResult> shader_build;
    Background bg;RenderOutput reference;std::uint64_t reference_generation=0;bool screenshot_requested=false,readback_requested=false,output_requested=false;
    std::unique_ptr<SceneUploadSnapshot> reference_snapshot;
    std::shared_ptr<const Scene> reference_source;
    std::uint64_t reference_source_generation=0;
    std::uint64_t reference_snapshot_generation=0,reference_failed_generation=0;
    std::optional<FrameReadback> comparison_frame;
    std::optional<FrameReadback> output_frame;RenderPath executed_path=settings.path;
    std::future<LoadedSceneAsset> imported;std::future<TestResults> checks;TestResults test_results;
    std::future<std::shared_ptr<const EnvironmentMap>> imported_environment;
    std::uint64_t environment_epoch=0;
    auto last_edit=std::chrono::steady_clock::now()-std::chrono::seconds(1);
    auto mark_dirty=[&](){dirty=true;++generation;last_edit=std::chrono::steady_clock::now();bg.cancel=true;};
    // 资源版本与姿态版本分开：鼠标连续拖动只更新矩阵/灯光，不重复上传纹理。
    std::uint64_t asset_counter=std::max<std::uint64_t>(1,scene.asset_revision);scene.asset_revision=asset_counter;
    auto stamp_assets=[&](){asset_counter=std::max(asset_counter,scene.asset_revision);if(asset_counter==UINT64_MAX)throw std::overflow_error("Asset revision exhausted");scene.asset_revision=++asset_counter;};
    std::uint64_t bake_geometry_epoch=0,bake_geometry_revision=0,bake_geometry_assets=0,bake_geometry_hash=0;
    auto bake_key=[&](){
        // 场景不变时复用几何键，避免实验预览的每个 UI 帧都遍历全部顶点。
        if(bake_geometry_epoch!=document_epoch||bake_geometry_revision!=scene.revision||bake_geometry_assets!=scene.asset_revision){
            bake_geometry_hash=geometry_fingerprint(scene);bake_geometry_epoch=document_epoch;
            bake_geometry_revision=scene.revision;bake_geometry_assets=scene.asset_revision;
        }
        std::ostringstream key;key<<bake_geometry_hash<<':'<<int(settings.environment_diffuse)<<':'<<settings.sdf_shadows<<':'<<settings.bake_samples<<':'<<settings.sdf_resolution<<std::setprecision(9);
        for(int i=0;i<3;++i)key<<':'<<scene.sky_top[i]<<':'<<scene.sky_bottom[i];key<<':'<<environment_fingerprint(scene);return key.str();
    };
    auto start_bake=[&](){
        if(scene_bake.valid())return;
        auto snapshot=scene;auto quality=settings;scene_bake_cancel=false;
        bake_request_key=bake_running_key=bake_key();bake_epoch=document_epoch;
        scene_bake=std::async(std::launch::async,[snapshot=std::move(snapshot),quality,&scene_bake_cancel](){return prepare_scene_resources(snapshot,quality,&scene_bake_cancel);});
        notice="正在后台准备当前场景的 SH / PRT / 距离场；完成前保留已有渲染。";
    };
    auto build_selected_lods=[&](){
        if(lod_chain_build.valid()||scene.meshes.empty())return;
        lod_mesh_index=std::clamp(selected_mesh,0,int(scene.meshes.size())-1);lod_source_hash=geometry_fingerprint(scene);lod_epoch=document_epoch;
        auto mesh=scene.meshes[lod_mesh_index];
        lod_chain_build=std::async(std::launch::async,[mesh=std::move(mesh)]() mutable {return build_mesh_lods(std::move(mesh));});
        notice="正在后台生成选中网格的 LOD 链；原网格会保留为 LOD0。";
    };
    auto finish_edit=[&](){
        translation_drag.active=false;
        object_rotation_active=false;
        if(!edit_before)return;
        if(edit_before->scene.revision==scene.revision){edit_before.reset();return;}
        try{editor::record_snapshot(undo,edit_label,edit_before,scene,camera,settings);edit_before.reset();}
        catch(...){editor::restore_snapshot(*edit_before,scene,camera,settings);stamp_assets();edit_before.reset();translation_drag.active=false;mark_dirty();throw;}
    };
    auto begin_edit=[&](const char* label){if(edit_before)return;if(undo.transaction_active())undo.commit();edit_before=std::make_shared<editor::Snapshot>(editor::Snapshot{scene,camera,settings});edit_label=label;};
    auto reset_document=[&](){finish_edit();++document_epoch;translation_drag.active=false;model_queue.clear();
        motion_replay={};
        // 切换/返回预览时不等待后台工作；旧任务即使完成也不能写回新文档。
        scene_bake_cancel=true;bake_request_key.clear();bg.cancel=true;reference_generation=0;
        reference_snapshot.reset();reference_source.reset();reference_failed_generation=0;
    };
    auto editor_change=[&](const char* label,auto&& change){
        finish_edit();
        if(undo.transaction_active())undo.commit();
        auto before=std::make_shared<editor::Snapshot>(editor::Snapshot{scene,camera,settings});
        try{change();stamp_assets();editor::record_snapshot(undo,label,before,scene,camera,settings);mark_dirty();}
        catch(...){editor::restore_snapshot(*before,scene,camera,settings);stamp_assets();mark_dirty();throw;}
    };
    auto enter_topic_preview=[&](){
        if(imported.valid()||imported_texture.valid())throw std::runtime_error("请等待模型/纹理导入完成后再进入专题，避免丢弃尚未提交的导入。");
        finish_edit();if(undo.transaction_active())undo.commit();
        if(!topic_return){
            // 先完成快照与字符串的分配，成功后才移动原撤销历史。仅首次进入保存原状态。
            auto saved=std::make_unique<editor::TopicReturnState>();saved->original={scene,camera,settings};
            saved->preset=preset;saved->visual_topic=visual_topic;saved->selected_node=selected_node;saved->selected_light=selected_light;
            saved->selected_material=selected_material;saved->selected_mesh=selected_mesh;saved->requested_panel=requested_panel;
            saved->property_target=property_target;saved->selected_texture=selected_texture;
            saved->fit_viewport=fit_viewport;saved->render_scale=render_scale;saved->experiment_current_scene=experiment_current_scene;
            saved->show_move_tool=show_move_tool;saved->transform_tool=transform_tool;saved->uniform_scale=uniform_scale;saved->replay_origin=replay_origin;
            saved->replay_running=replay_running;saved->replay_index=replay_index;saved->replay=options.replay;
            saved->project_path=project_path;saved->project_path_text=path_text;
            using std::swap;swap(undo,saved->history);topic_return=std::move(saved);
        }else{
            // 下一个专题仍从进入预览前的原场景出发，不叠加上个专题的灯光/参数。
            editor::restore_snapshot(topic_return->original,scene,camera,settings);stamp_assets();undo.clear();
            preset=topic_return->preset;selected_node=topic_return->selected_node;selected_light=topic_return->selected_light;
            selected_material=topic_return->selected_material;selected_mesh=topic_return->selected_mesh;
            property_target=topic_return->property_target;selected_texture=topic_return->selected_texture;
            fit_viewport=topic_return->fit_viewport;render_scale=topic_return->render_scale;
        }
        reset_document();replay_running=false;visual_topic=-1;experiment_current_scene=false;requested_panel=2;mark_dirty();
    };
    auto return_from_topic=[&](){
        if(!topic_return)return;
        finish_edit();if(undo.transaction_active())undo.commit();
        if(scene.revision==UINT64_MAX||topic_return->original.scene.revision==UINT64_MAX)throw std::overflow_error("Scene revision exhausted");
        const auto revision=std::max(scene.revision,topic_return->original.scene.revision)+1;
        reset_document();using std::swap;swap(scene,topic_return->original.scene);scene.revision=revision;
        camera=topic_return->original.camera;settings=topic_return->original.settings;swap(undo,topic_return->history);stamp_assets();
        preset=topic_return->preset;visual_topic=topic_return->visual_topic;selected_node=topic_return->selected_node;selected_light=topic_return->selected_light;
        selected_material=topic_return->selected_material;selected_mesh=topic_return->selected_mesh;
        property_target=topic_return->property_target;selected_texture=topic_return->selected_texture;
        fit_viewport=topic_return->fit_viewport;render_scale=topic_return->render_scale;experiment_current_scene=topic_return->experiment_current_scene;
        show_move_tool=topic_return->show_move_tool;transform_tool=topic_return->transform_tool;uniform_scale=topic_return->uniform_scale;
        replay_origin=topic_return->replay_origin;replay_running=topic_return->replay_running;replay_index=topic_return->replay_index;
        options.replay.swap(topic_return->replay);project_path.swap(topic_return->project_path);path_text=topic_return->project_path_text;
        requested_panel=2;topic_return.reset();active_topic=-1;active_demo=-1;error.clear();mark_dirty();
        notice="已返回进入专题前的场景：模型、材质、灯光、相机、渲染参数和原撤销历史均已恢复。";
    };
    auto preset_scene=[&](int id){if(undo.transaction_active())undo.commit();reset_document();preset=id;scene=make_demo_scene(id);scene.revision=++generation;stamp_assets();visual_topic=-1;frame_camera(scene,camera);selected_material=0;selected_node=selected_light=-1;property_target=editor::PropertyTarget::none;selected_texture=-1;undo.clear();mark_dirty();};
    auto open_showcase=[&](std::string_view id){
        try{enter_topic_preview();auto project=make_showcase(id);scene=std::move(project.scene);camera=project.camera;settings=project.settings;
            scene.revision=++generation;stamp_assets();selected_node=selected_light=-1;selected_material=0;property_target=editor::PropertyTarget::none;
            active_topic=active_demo=-1;preset=-1;undo.clear();mark_dirty();notice="代表场景已打开；可返回进入前的工程。";
        }catch(const std::exception& e){error=e.what();}
    };
    auto start_object_replay=[&]{
        int node=selected_node;
        if(node<0)for(std::size_t i=0;i<scene.nodes.size();++i)if(scene.nodes[i].mesh>=0&&
            (scene.nodes[i].name.starts_with("材质球")||scene.nodes[i].name=="花器"||scene.nodes[i].name.starts_with("实例"))){node=int(i);break;}
        if(node<0)for(std::size_t i=0;i<scene.nodes.size();++i)if(scene.nodes[i].mesh>=0){node=int(i);break;}
        if(node<0)throw std::runtime_error("场景没有可回放的网格节点。");
        motion_replay={node,scene.nodes.at(node).local,0,true};replay_running=false;
    };
    auto stop_object_replay=[&]{
        if(motion_replay.active&&motion_replay.node>=0&&motion_replay.node<int(scene.nodes.size())){
            scene.nodes[motion_replay.node].local=motion_replay.original;++scene.revision;mark_dirty();
        }motion_replay={};
    };
    auto import_asset=[&](std::filesystem::path path,bool replace=false){
        if(imported.valid()){if(!replace){model_queue.push_back(std::move(path));notice="模型已加入导入队列。";}return;}
        import_epoch=document_epoch;imported_replace=replace;
        notice="正在后台解析模型："+path_to_utf8(path);error.clear();
        imported=std::async(std::launch::async,[path](){return load_editor_model(path);});
    };
    auto request_model=[&](std::filesystem::path path,bool replace){
        std::snprintf(model_path_text.data(),model_path_text.size(),"%s",path_to_utf8(path).c_str());
        if(replace){replace_request=std::move(path);open_replace_popup=true;}
        else import_asset(std::move(path));
    };
    auto request_texture=[&](std::filesystem::path path,TextureRole role){
        if(imported_texture.valid()){notice="上一张纹理仍在读取，请完成后再导入。";return;}
        texture_request=std::move(path);texture_role=role;texture_material=selected_material;
        open_texture_popup=true;requested_panel=1;show_inspector=true;
    };
    auto request_environment=[&](std::filesystem::path path){
        if(imported_environment.valid()){notice="HDR 环境仍在读取，请稍候。";return;}
        environment_epoch=document_epoch;notice="正在后台读取 HDR 环境…";
        imported_environment=std::async(std::launch::async,[path]{return load_environment_hdr(path);});
    };
    auto browse=[&](editor::FileKind kind,TextureRole role=TextureRole::base_color){
        try{if(auto path=editor::choose_file(window.native_handle(),kind)){
            if(kind==editor::FileKind::model)request_model(*path,replace_import);
            else if(kind==editor::FileKind::texture)request_texture(*path,role);
            else if(kind==editor::FileKind::environment)request_environment(*path);
            else{std::snprintf(path_text.data(),path_text.size(),"%s",path_to_utf8(*path).c_str());requested_panel=3;show_inspector=true;}
            return true;
        }}catch(const std::exception& e){error=e.what();}return false;
    };
    auto set_node_position=[&](int index,glm::vec3 world){
        const auto transforms=scene_world_transforms(scene);auto& node=scene.nodes.at(index);
        glm::vec3 local=world;
        if(node.parent>=0){
            const auto parent=glm::dmat4(transforms.at(node.parent));const auto axes=glm::dmat3(parent);
            const double scale=glm::length(axes[0])*glm::length(axes[1])*glm::length(axes[2]);
            if(!std::isfinite(scale)||scale<=0||std::abs(glm::determinant(axes))<=scale*1e-12)throw std::runtime_error("父节点缩放为零或坐标轴退化，无法移动节点。");
            local=glm::vec3(glm::inverse(parent)*glm::dvec4(world,1));
            if(!std::isfinite(local.x)||!std::isfinite(local.y)||!std::isfinite(local.z))throw std::runtime_error("节点位置超出有效范围。");
        }
        node.local[3]=glm::vec4(local,1);scene.baked_resources.reset();++scene.revision;
    };
    auto select_light=[&](int index){finish_edit();selected_light=index;selected_node=-1;selected_texture=-1;property_target=editor::PropertyTarget::light;requested_panel=1;show_inspector=true;};
    auto select_resource=[&](editor::PropertyTarget target,int index){
        finish_edit();selected_node=selected_light=-1;selected_texture=-1;property_target=target;
        if(target==editor::PropertyTarget::mesh)selected_mesh=index;
        if(target==editor::PropertyTarget::material)selected_material=index;
        if(target==editor::PropertyTarget::texture)selected_texture=index;
        requested_panel=1;show_inspector=true;
    };
    auto history_step=[&](bool redo){
        try{translation_drag.active=false;finish_edit();if(redo?undo.redo():undo.undo()){
            stamp_assets();++document_epoch;model_queue.clear();scene_bake_cancel=true;bake_request_key.clear();mark_dirty();
            if(imported.valid()||imported_texture.valid())notice="撤销/重做已完成；未提交的后台导入会取消，避免绑定到错误场景。";
        }}catch(const std::exception& e){error=e.what();}
    };
    auto light_for_node=[&](int node){for(int i=0;i<int(scene.lights.size());++i)if(scene.lights[i].linked_node==node)return i;return -1;};
    auto select_scene_node=[&](int index){
        if(index<0||index>=int(scene.nodes.size()))throw std::out_of_range("无效节点");
        if(const int light=light_for_node(index);light>=0){select_light(light);return;}
        finish_edit();selected_node=index;selected_light=selected_texture=-1;property_target=editor::PropertyTarget::node;
        const int mesh=scene.nodes[index].mesh;
        if(mesh>=0&&mesh<int(scene.meshes.size())){
            selected_mesh=mesh;const auto& primitives=scene.meshes[mesh].primitives;
            if(!primitives.empty()&&primitives.front().material<scene.materials.size())selected_material=int(primitives.front().material);
        }
        requested_panel=1;show_inspector=true;
    };
    auto add_light=[&](LightKind kind){
        try{int index=-1;editor_change("添加光源",[&](){index=add_editor_light(scene,kind);
            if(kind!=LightKind::directional){auto light=scene.lights[index];light.position=camera.target+glm::vec3(0,std::max(.1f,glm::length(camera.position-camera.target)*.15f),0);update_editor_light(scene,index,light);}
        });select_light(index);}
        catch(const std::exception& e){error=e.what();}
    };
    auto add_object=[&](editor::NewObject kind,int parent){
        try{int index=-1;editor_change("新建场景节点",[&](){index=editor::create_object(scene,kind,parent,camera.target);});
            selected_node=index;selected_light=selected_texture=-1;property_target=editor::PropertyTarget::node;
            if(scene.nodes[index].mesh>=0){selected_mesh=scene.nodes[index].mesh;selected_material=editor::node_material(scene,index,0);}
            requested_panel=1;show_inspector=true;notice="已创建 "+scene.nodes[index].name+"；Ctrl + 左键拖动可旋转，Ctrl+Z 可撤销。";
        }catch(const std::exception& e){error=e.what();}
    };
    auto choose_transform_tool=[&](editor::TransformTool tool){try{finish_edit();transform_tool=tool;show_move_tool=true;}catch(const std::exception& e){error=e.what();}};
    auto viewport_rect=[&](){
        const auto display=ImGui::GetIO().DisplaySize;
        const auto shell=editor::layout(display.x,display.y,show_scene,show_inspector,scene_width,inspector_width);
        return editor::image_rect(shell.left,shell.menu+shell.toolbar,shell.viewport_width(),shell.viewport_height(),settings.render_width,settings.render_height);
    };
    auto selected_position=[&]()->std::optional<glm::vec3>{
        if(selected_light>=0&&selected_light<int(scene.lights.size())&&scene.lights[selected_light].kind!=LightKind::directional)return scene.lights[selected_light].position;
        if(selected_node>=0&&selected_node<int(scene.nodes.size()))return glm::vec3(scene_world_transforms(scene).at(selected_node)[3]);
        return {};
    };
    auto handle_length=[&](glm::vec3 p,const editor::ViewportRect& rect){
        const auto view=camera.view()*glm::vec4(p,1);
        return std::max(.0001f,-view.z)*2*std::tan(glm::radians(camera.fov)*.5f)*75/std::max(1.f,rect.height);
    };
    auto start_object_rotation=[&](glm::vec2 mouse){
        if(property_target!=editor::PropertyTarget::node||selected_node<0||selected_node>=int(scene.nodes.size()))return false;
        const auto rect=viewport_rect();
        if(mouse.x<rect.x||mouse.y<rect.y||mouse.x>=rect.x+rect.width||mouse.y>=rect.y+rect.height)return false;
        finish_edit();begin_edit("Ctrl 拖动旋转模型");object_rotation_node=selected_node;
        object_rotation_initial=scene_world_transforms(scene).at(selected_node);object_rotation_start=mouse;
        object_rotation_active=true;replay_running=false;return true;
    };
    auto move_object_rotation=[&](glm::vec2 mouse,bool snap){
        if(!object_rotation_active)return;
        editor::set_node_world(scene,object_rotation_node,editor::drag_rotation(object_rotation_initial,mouse-object_rotation_start,camera,snap));mark_dirty();
    };
    auto start_translation=[&](glm::vec2 mouse){
        if(!show_move_tool)return false;
        const auto origin=selected_position();if(!origin)return false;
        const auto rect=viewport_rect();const auto center=editor::project_handle(*origin,camera,rect,settings.reversed_z);if(!center)return false;
        // 灯板用光源参数维持尺寸/朝向联动；通用旋转/缩放仅作用于模型节点。
        const auto tool=selected_light>=0?editor::TransformTool::translate:transform_tool;
        const auto world=selected_node>=0?scene_world_transforms(scene).at(selected_node):glm::mat4(1);
        const float length=handle_length(*origin,rect);int hit_axis=-1;float closest=7;
        const auto ray=editor::handle_ray(mouse,camera,rect,settings.reversed_z);
        if(tool==editor::TransformTool::scale&&std::abs(mouse.x-center->x)<=7&&std::abs(mouse.y-center->y)<=7)hit_axis=3;
        for(int a=0;a<3&&hit_axis!=3;++a){
            if(tool==editor::TransformTool::rotate){
                if(!editor::ring_direction(ray,*origin,editor::world_axis(a)))continue;
                for(int segment=0;segment<64;++segment){
                    const auto p=editor::project_handle(editor::ring_point(*origin,a,length,2*pi*segment/64),camera,rect,settings.reversed_z);
                    const auto q=editor::project_handle(editor::ring_point(*origin,a,length,2*pi*(segment+1)/64),camera,rect,settings.reversed_z);
                    if(p&&q){const float distance=editor::segment_distance(mouse,*p,*q);if(distance<closest){hit_axis=a;closest=distance;}}
                }continue;
            }
            const auto axis=tool==editor::TransformTool::scale?safe_normalize(glm::vec3(world[a]),editor::world_axis(a)):editor::world_axis(a);
            const auto end=editor::project_handle(*origin+axis*length,camera,rect,settings.reversed_z);
            if(!end||!editor::axis_parameter(ray,*origin,axis))continue;
            // 中心保留给选物，箭头任意可见部分都可以抓住。
            if(glm::length(*end-*center)<12)continue;
            const float distance=editor::segment_distance(mouse,*center+(*end-*center)*.16f,*end);
            if(distance<closest){hit_axis=a;closest=distance;}
        }
        if(hit_axis<0)return false;
        begin_edit(tool==editor::TransformTool::rotate?"旋转模型":tool==editor::TransformTool::scale?"缩放模型":"XYZ 平移");
        translation_drag={};translation_drag.active=true;translation_drag.axis=hit_axis;translation_drag.light=selected_light;
        translation_drag.node=selected_node;translation_drag.origin=*origin;translation_drag.operation=int(tool);
        translation_drag.initial_world=world;translation_drag.initial_mouse=mouse;translation_drag.handle_length=length;
        if(selected_node>=0)translation_drag.initial_local=scene.nodes.at(selected_node).local;
        if(hit_axis<3){translation_drag.axis_direction=tool==editor::TransformTool::scale?
            safe_normalize(glm::vec3(world[hit_axis]),editor::world_axis(hit_axis)):editor::world_axis(hit_axis);
            if(tool==editor::TransformTool::rotate)translation_drag.initial_direction=*editor::ring_direction(ray,*origin,translation_drag.axis_direction);
            else translation_drag.initial_parameter=*editor::axis_parameter(ray,*origin,translation_drag.axis_direction);
        }
        replay_running=false;return true;
    };
    auto move_translation=[&](glm::vec2 mouse,bool snap){
        auto& drag=translation_drag;const auto ray=editor::handle_ray(mouse,camera,viewport_rect(),settings.reversed_z);
        if(drag.operation==int(editor::TransformTool::rotate)){
            const auto direction=editor::ring_direction(ray,drag.origin,drag.axis_direction);if(!direction)return;
            const float angle=std::atan2(glm::dot(drag.axis_direction,glm::cross(drag.initial_direction,*direction)),glm::dot(drag.initial_direction,*direction));
            drag.accumulated_angle+=std::remainder(angle-drag.last_angle,2*pi);drag.last_angle=angle;
            float applied=drag.accumulated_angle;if(snap)applied=std::round(applied/(pi/12))*(pi/12);
            const auto delta=glm::translate(glm::mat4(1),drag.origin)*glm::rotate(glm::mat4(1),applied,drag.axis_direction)*glm::translate(glm::mat4(1),-drag.origin);
            editor::set_node_world(scene,drag.node,delta*drag.initial_world);
        }else if(drag.operation==int(editor::TransformTool::scale)){
            float factor=1;
            if(drag.axis==3){const auto delta=mouse-drag.initial_mouse;factor=std::exp(std::clamp((delta.x-delta.y)*.01f,-6.f,6.f));}
            else{const auto parameter=editor::axis_parameter(ray,drag.origin,drag.axis_direction);if(!parameter)return;
                factor=1+(*parameter-drag.initial_parameter)/drag.handle_length;}
            factor=std::clamp(factor,.001f,1000.f);if(snap)factor=std::max(.001f,std::round(factor*10)/10);
            glm::vec3 scaling(1);if(drag.axis==3)scaling=glm::vec3(factor);else scaling[drag.axis]=factor;
            editor::set_node_local(scene,drag.node,drag.initial_local*glm::scale(glm::mat4(1),scaling));
        }else{
            const auto parameter=editor::axis_parameter(ray,drag.origin,drag.axis_direction);if(!parameter)return;
            float offset=*parameter-drag.initial_parameter;if(snap)offset=std::round(offset*10)/10;
            const auto position=drag.origin+drag.axis_direction*offset;
            if(drag.light>=0)set_editor_light_position(scene,drag.light,position);else set_node_position(drag.node,position);
        }
        mark_dirty();
    };
    auto save=[&]()->bool{
        if(topic_return){notice="专题正在临时预览，不会覆盖原工程。请先点“返回原场景”，再保存正式工程。";return false;}
        if(scene_bake.valid()||lod_chain_build.valid()||imported.valid()||imported_texture.valid()){
            notice="模型、纹理或场景资源仍在准备，请完成后保存。";return false;
        }
        try{
            finish_edit();auto destination=path_from_utf8(path_text.data());
            if(destination.empty()){auto chosen=editor::choose_file(window.native_handle(),editor::FileKind::save_project);if(!chosen)return false;destination=*chosen;}
            if(destination.extension()!=".ember")throw std::runtime_error("工程保存路径必须以 .ember 结尾，不会覆盖模型或图片文件。");
            save_project(destination,scene,camera,settings);project_path=destination;
            std::snprintf(path_text.data(),path_text.size(),"%s",path_to_utf8(project_path).c_str());
            notice="已保存："+path_to_utf8(project_path);error.clear();return true;
        }catch(const std::exception& e){error=e.what();return false;}
    };
    auto save_as=[&](){
        try{if(auto chosen=editor::choose_file(window.native_handle(),editor::FileKind::save_project)){
            const auto previous_text=path_text;std::snprintf(path_text.data(),path_text.size(),"%s",path_to_utf8(*chosen).c_str());
            if(!save())path_text=previous_text;
        }}catch(const std::exception& e){error=e.what();}
    };
    auto new_document=[&](){
        // 完成新场景的分配再换文档；更换后旧后台任务只会被丢弃，不写回新文档。
        auto next=editor::make_new_scene(new_scene_name.data(),new_scene_floor,new_scene_light);
        finish_edit();if(undo.transaction_active())undo.commit();reset_document();topic_return.reset();
        scene=std::move(next);scene.revision=++generation;stamp_assets();
        const int width=settings.render_width,height=settings.render_height;settings=Settings{};
        settings.shadow_resolution=1024;settings.render_width=width;settings.render_height=height;
        camera=Camera{};replay_origin=camera;replay_running=false;options.replay.clear();
        visual_topic=active_topic=active_demo=-1;preset=-1;selected_node=selected_light=-1;selected_material=selected_mesh=0;property_target=editor::PropertyTarget::none;selected_texture=-1;
        project_path.clear();path_text.fill(0);model_path_text.fill(0);texture_path_text.fill(0);replace_import=false;
        undo.clear();mark_dirty();requested_panel=3;show_scene=show_inspector=true;error.clear();
        notice="已创建独立场景：追加模型 → 添加光源 → 绑定纹理 → 另存为 .ember；旧工程文件没有改写。";
    };
    auto load=[&](){
        try{const auto requested_path=path_from_utf8(path_text.data());return_from_topic();finish_edit();if(undo.transaction_active())undo.commit();
            load_project(requested_path,scene,camera,settings);reset_document();project_path=requested_path;
            preset=-1;active_demo=active_topic=-1;replay_running=false;
            const bool saved_cpu=settings.path==RenderPath::cpu_raster||settings.path==RenderPath::path_trace;
            // 工程保存的是配置，不是启动离线任务的授权；CPU 参考需在开发者工具手动选择。
            if(saved_cpu)settings.path=RenderPath::forward;
            std::snprintf(path_text.data(),path_text.size(),"%s",path_to_utf8(project_path).c_str());scene.revision=++generation;stamp_assets();visual_topic=-1;
            selected_material=0;selected_node=selected_light=-1;property_target=editor::PropertyTarget::none;selected_texture=-1;undo.clear();mark_dirty();notice=saved_cpu?"已恢复工程；保存的 CPU 参考模式未自动启动，当前使用实时前向。":"已恢复场景和参数。";}
        catch(const std::exception& e){error=e.what();}
    };
    auto reload_shaders=[&](){
        if(shader_build.valid())return;
        try{
            if(!shader_library){
                if(!std::filesystem::exists(asset_root()/"engine/lab/shaders")||!std::filesystem::exists(path_from_utf8(EMBERFRAME_GLSLANG_PATH)))
                    throw std::runtime_error("当前演示包仅包含预编译 Shader；热重载需要源码和 Vulkan SDK 编译器，正常渲染不需要。");
                shader_library=std::make_unique<ShaderLibrary>(asset_root()/"engine/lab/shaders",output_dir/"shader-cache",path_from_utf8(EMBERFRAME_GLSLANG_PATH));
                shader_library->adopt_baseline(shader_dir);
            }
            auto* library=shader_library.get();shader_build=std::async(std::launch::async,[library](){return library->build();});
            notice="后台编译 Shader；失败保留原画面，成功后安全替换管线。";
        }catch(const std::exception& e){error=e.what();}
    };
    if(options.reload_shaders)reload_shaders();
    auto open_topic=[&](int topic,editor::TopicEntry entry){
        if(!editor::topic_entry_available(topic,entry))throw std::runtime_error("该专题没有这个可视入口；不会偷偷改场景或运行检查。");
        enter_topic_preview();active_demo=-1;active_topic=topic;active_topic_entry=entry;
        settings.path=RenderPath::forward;settings.debug=DebugView::final_color;visual_topic=-1;
        if(entry!=editor::TopicEntry::current_scene){
            settings.taa=settings.denoise=settings.svgf=false;settings.outline=settings.hatching=false;
            settings.gi=GiMode::environment;settings.ao=AoMode::none;settings.shading=ShadingMode::pbr;
            settings.environment_diffuse=EnvironmentDiffuse::ibl;settings.sdf_shadows=false;settings.auto_lod=false;
            const int scene_id=topic==0||topic==1||topic==10||topic==11||(topic>=18&&topic<=20)||(topic>=25&&topic<=29)?2:
                topic==9||topic==15||(topic>=21&&topic<=24)?1:topic==17?3:0;
            preset_scene(scene_id);
        }
        switch(topic){
        case 0:camera.near_plane=std::clamp(glm::length(camera.position-camera.target)*.95f,.001f,camera.far_plane*.95f);notice="近平面穿过当前模型，观察裁剪边界。";break;
        case 1:settings.debug=DebugView::albedo;notice="无光照颜色视图，观察相邻三角形的连续覆盖；不会自动运行数值检查。";break;
        case 2:settings.filter=FilterMode::trilinear;notice="在渲染页切换纹理过滤；需要场景中已有纹理。";break;
        case 3:settings.reversed_z=true;settings.debug=DebugView::depth;notice="查看反向深度；近远平面可以在相机参数中调整。";break;
        case 4:settings.debug=DebugView::albedo;notice="保留当前场景与相机，切换前向/CPU 光栅可观察材质颜色；此入口不会执行比较检查。";break;
        case 6:case 7:settings.spatial_structure=topic==6?SpatialStructure::bvh:SpatialStructure::octree;
            notice="保留当前场景，切换实际选物/射线查询结构；结构本身不会改变颜色。";break;
        case 9:settings.path=RenderPath::path_trace;settings.samples=16;notice="CPU 路径追踪参考，包含直接光采样、MIS 和多次反弹。";break;
        case 10:case 11:settings.gi=GiMode::environment;settings.environment_diffuse=topic==10?EnvironmentDiffuse::sh:EnvironmentDiffuse::prt;
            notice=topic==10?"当前场景使用 SH 环境漫反射，镜面 IBL 保留。":"为当前静态场景准备 PRT 可见性，再用于正常 GPU 光照。";break;
        case 12:settings.gi=GiMode::environment;settings.environment_diffuse=EnvironmentDiffuse::ibl;
            notice="正常场景使用预计算 IBL；要看预过滤贴图与 LUT，请选择独立 CPU 对照。";break;
        case 13:settings.energy_compensation=true;notice="启用 KC 能量补偿；在粗糙金属上对比更明显。";break;
        case 14:settings.shading=ShadingMode::disney;
            if(entry==editor::TopicEntry::preset_scene){scene.materials[1].clearcoat=.8f;scene.materials[2].anisotropy=.7f;scene.materials[3].sheen=.8f;++scene.revision;stamp_assets();}
            notice="使用 Disney 材质模型；清漆、各向异性与 Sheen 在材质页调节。";break;
        case 15:notice="现有矩形面积光使用 LTC；当前场景没有面积光时不会凭空添加。";break;
        case 16:settings.path=RenderPath::deferred;notice="保留几何与材质，改用 G-buffer 延迟光照。";break;
        case 17:settings.culling=LightCulling::clustered;settings.debug=DebugView::light_count;notice="GPU 聚簇灯表；切回最终颜色观察实际光照。";break;
        case 18:case 19:case 20:settings.shadows=topic==18?ShadowMode::vsm:topic==19?ShadowMode::vssm:ShadowMode::msm;
            settings.debug=DebugView::shadow;notice="统计阴影可见性：白色可见、黑色遮挡。";break;
        case 21:case 22:settings.path=RenderPath::deferred;settings.gi=topic==21?GiMode::ssr:GiMode::ssgi;
            settings.debug=DebugView::indirect;notice="屏幕空间间接光；当前相机看不到的几何不能凭深度图恢复。";break;
        case 23:case 24:settings.path=RenderPath::path_trace;settings.samples=1;settings.denoise=topic==23;settings.svgf=topic==24;
            notice="低采样 CPU 路径追踪连续累积，观察历史复用与降噪。";break;
        case 25:case 26:case 27:case 28:settings.path=RenderPath::deferred;settings.gi=topic==25?GiMode::rsm:topic==26?GiMode::lpv:GiMode::voxel;
            settings.debug=DebugView::indirect;notice="GPU 间接光支持完整场景：方向光、点光六面及面积光灯心近似；当前使用一盏主光源，不会自动回退 CPU。";break;
        case 29:settings.sdf_shadows=true;notice="从实际场景网格准备距离场，用于方向光阴影。";break;
        case 30:settings.shading=ShadingMode::toon;settings.outline=true;settings.hatching=true;notice="当前场景启用卡通色带、轮廓与排线。";break;
        }
        if(entry==editor::TopicEntry::cpu_comparison&&supports_visual_experiment(topic)){
            visual_topic=topic;settings.path=RenderPath::cpu_raster;notice=visual_experiment_guide(topic);
        }
        if(settings.path==RenderPath::path_trace||settings.path==RenderPath::cpu_raster||entry==editor::TopicEntry::cpu_comparison){
            // 独立 CPU 对照不跟随高 DPI 视口放大到数百万像素；返回时恢复原分辨率策略。
            fit_viewport=false;const float scale=std::min({1.f,640.f/settings.render_width,480.f/settings.render_height});
            settings.render_width=std::max(8,int(settings.render_width*scale));settings.render_height=std::max(8,int(settings.render_height*scale));
            settings.bloom=false;
        }
        if(entry==editor::TopicEntry::cpu_comparison){show_developer_tools=true;requested_panel=4;}
        notice=std::string(editor::topic_entry_name(entry))+" · "+topics[topic].name+"\n"+notice+"\n临时预览；点击“返回原场景”恢复，原工程文件未改动。";
        mark_dirty();
    };
    auto load_resume_profile=[&](const char* name){
        auto d=make_resume_demo(name);enter_topic_preview();
        const auto width=settings.render_width,height=settings.render_height;
        scene=std::move(d.scene);camera=d.camera;settings=d.settings;settings.render_width=width;settings.render_height=height;
        scene.revision=++generation;stamp_assets();visual_topic=-1;selected_material=selected_mesh=0;selected_node=selected_light=-1;property_target=editor::PropertyTarget::none;selected_texture=-1;
        replay_running=false;replay_origin=camera;replay_index=0;undo.clear();requested_panel=2;active_topic=-1;active_topic_entry=editor::TopicEntry::preset_scene;
        active_demo=0;
        for(int i=0;i<int(editor::demo_scenes.size());++i)if(std::string_view(name)==editor::demo_scenes[i].profile)active_demo=i;
        selected_demo=active_demo;mark_dirty();
        notice="演示场景："+std::string(editor::demo_scenes[active_demo].name)+"。返回原场景可恢复进入前的状态。";
    };
    auto compare_demo=[&](bool second){
        if(active_demo<0)return;
        switch(active_demo){
        case 0:settings.energy_compensation=second;break;
        case 1:settings.shadows=second?ShadowMode::pcss:ShadowMode::hard;break;
        case 2:settings.culling=second?LightCulling::clustered:LightCulling::all;break;
        case 3:settings.gi=second?GiMode::ssgi:GiMode::environment;break;
        case 4:settings.auto_lod=second;break;
        }
        mark_dirty();
    };
    auto open_demo=[&](const char* profile){
        try{load_resume_profile(profile);}catch(const std::exception& e){error=e.what();}
    };
    auto set_developer_tools=[&](bool visible){
        if(!visible){
            if(topic_return&&active_topic_entry==editor::TopicEntry::cpu_comparison)return_from_topic();
            // 隐藏开发页时停止其 CPU 任务；不能留一个看不见入口的离线渲染在后台。
            if(settings.path==RenderPath::cpu_raster||settings.path==RenderPath::path_trace){
                settings.path=RenderPath::forward;visual_topic=-1;mark_dirty();
            }
            if(requested_panel==4)requested_panel=0;
        }else {requested_panel=4;show_inspector=true;}
        show_developer_tools=visible;
    };
    auto ui=[&](){
        using namespace emberframe::lab::editor;
        const auto display=ImGui::GetIO().DisplaySize;
        const auto shell=layout(display.x,display.y,show_scene,show_inspector,scene_width,inspector_width);
        const auto& stats=gpu.stats();
        const auto ui_gpu_limitations=VulkanWorkbench::unsupported_modes(scene,settings);
        const bool ui_gpu_blocked=visual_topic<0&&(settings.path==RenderPath::forward||settings.path==RenderPath::deferred)&&!ui_gpu_limitations.empty();
        // ImGui 使用窗口逻辑坐标，Vulkan 呈现使用 drawable 坐标；高 DPI 下两者可能不同。
        int drawable_w=0,drawable_h=0;SDL_Vulkan_GetDrawableSize(window.native_handle(),&drawable_w,&drawable_h);
        const float sx=display.x>0?drawable_w/display.x:1,sy=display.y>0?drawable_h/display.y:1;
        const int left_pixels=int(std::lround(shell.left*sx)),right_pixels=int(std::lround(shell.right*sx));
        const int top_pixels=int(std::lround((shell.menu+shell.toolbar)*sy)),bottom_pixels=int(std::lround(shell.status*sy));
        viewport_pixel_width=std::max(0,drawable_w-left_pixels-right_pixels);
        viewport_pixel_height=std::max(0,drawable_h-top_pixels-bottom_pixels);
        gpu.set_viewport_insets(left_pixels,right_pixels,top_pixels,bottom_pixels);
        if(fit_viewport&&shell.viewport_width()>0&&shell.viewport_height()>0){
            const auto size=fit_resolution(float(viewport_pixel_width),float(viewport_pixel_height),render_scale,gpu.max_render_tiles(),gpu.max_render_pixels());
            resolution_limited=size.width+1<viewport_pixel_width*render_scale||size.height+1<viewport_pixel_height*render_scale;
            if(settings.render_width!=size.width||settings.render_height!=size.height){settings.render_width=size.width;settings.render_height=size.height;mark_dirty();}
        }else resolution_limited=false;
        constexpr ImGuiWindowFlags fixed=ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings;
        auto panel=[&](const char* name,ImVec2 pos,ImVec2 size,ImGuiWindowFlags flags=0){
            ImGui::SetNextWindowPos(pos);ImGui::SetNextWindowSize(size);
            // ImGui 默认最小窗口为 32px；细分隔线若不覆盖此值，会遮住相邻面板的文字。
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize,{1,1});ImGui::Begin(name,nullptr,fixed|flags);ImGui::PopStyleVar();
        };
        auto capture=[&](){try{auto path=output_dir/"capture.bmp";gpu.request_screenshot(path);notice="截图将在呈现后保存："+path_to_utf8(path);}catch(const std::exception& e){error=e.what();}};
        auto& select_node=select_scene_node;
        auto creation_menu=[&](int parent){
            if(ImGui::BeginMenu("新建模型")){
                if(ImGui::MenuItem("立方体"))add_object(NewObject::cube,parent);
                if(ImGui::MenuItem("球体"))add_object(NewObject::sphere,parent);
                if(ImGui::MenuItem("平面"))add_object(NewObject::plane,parent);
                ImGui::Separator();if(ImGui::MenuItem("导入模型到场景…")){replace_import=false;browse(FileKind::model);}
                ImGui::EndMenu();
            }
            if(ImGui::MenuItem("新建空节点"))add_object(NewObject::empty,parent);
            if(ImGui::BeginMenu("新建光源")){
                if(ImGui::MenuItem("方向光"))add_light(LightKind::directional);
                if(ImGui::MenuItem("点光源"))add_light(LightKind::point);
                if(ImGui::MenuItem("矩形面积光"))add_light(LightKind::rectangle);
                ImGui::EndMenu();
            }
            if(parent>=0)ImGui::TextDisabled("基础模型建为子节点；导入 / 光源添加到场景。");
        };
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{8,3});
        // 同一顶栏容纳品牌、菜单、空白拖动区和窗口按钮，避免额外叠一行系统标题栏。
        constexpr float control_width=46;
        const float controls_start=std::max(0.f,shell.width-control_width*3);
        float menu_end=0;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{8,10});
        panel("Editor menu",{0,0},{controls_start,shell.menu},ImGuiWindowFlags_MenuBar|ImGuiWindowFlags_NoScrollbar);
        if(ImGui::BeginMenuBar()){
            if(auto texture=gpu.application_icon_texture())ImGui::Image(reinterpret_cast<ImTextureID>(texture),{24,24});
            if(shell.width>=800){ImGui::TextUnformatted("EmberFrame");ImGui::Separator();}
            if(ImGui::BeginMenu("文件")){
                if(ImGui::MenuItem("新建场景…","Ctrl+N"))open_new_scene_popup=true;
                if(ImGui::MenuItem("保存工程","Ctrl+S"))save();
                if(ImGui::MenuItem("另存为…","Ctrl+Shift+S"))save_as();
                if(ImGui::MenuItem("打开工程…")&&browse(FileKind::project))load();
                if(ImGui::MenuItem("追加模型…")){replace_import=false;browse(FileKind::model);}
                if(ImGui::MenuItem("导入纹理…",nullptr,false,!scene.materials.empty()))browse(FileKind::texture);
                if(ImGui::MenuItem("导入 HDR 环境…",nullptr,false,!imported_environment.valid()))browse(FileKind::environment);
                if(ImGui::MenuItem("保存当前画面"))capture();
                ImGui::Separator();if(ImGui::MenuItem("退出","Esc"))running=false;ImGui::EndMenu();
            }
            if(ImGui::BeginMenu("编辑")){
                if(ImGui::MenuItem("撤销","Ctrl+Z"))history_step(false);
                if(ImGui::MenuItem("重做","Ctrl+Y"))history_step(true);ImGui::EndMenu();
            }
            if(ImGui::BeginMenu("视图")){
                ImGui::MenuItem("场景与资源",nullptr,&show_scene);ImGui::MenuItem("属性检查器",nullptr,&show_inspector);
                ImGui::MenuItem("性能分析",nullptr,&show_profiler);
                if(ImGui::MenuItem("开发者工具",nullptr,show_developer_tools)){
                    try{set_developer_tools(!show_developer_tools);}catch(const std::exception& e){error=e.what();}
                }
                if(shell.width<1100)ImGui::TextDisabled("窄窗口自动隐藏场景栏");
                if(ImGui::MenuItem("聚焦场景","F")){frame_camera(scene,camera);mark_dirty();}
                if(ImGui::MenuItem("全屏","F11")){fullscreen=!fullscreen;SDL_SetWindowFullscreen(window.native_handle(),fullscreen?SDL_WINDOW_FULLSCREEN_DESKTOP:0);gpu.resize();}
                ImGui::Separator();if(ImGui::MenuItem("还原布局")){show_scene=show_inspector=true;scene_width=224;inspector_width=340;requested_panel=0;}ImGui::EndMenu();
            }
            if(ImGui::BeginMenu("工具")){
                if(ImGui::MenuItem("平移","W",transform_tool==TransformTool::translate))choose_transform_tool(TransformTool::translate);
                if(ImGui::MenuItem("旋转","E",transform_tool==TransformTool::rotate))choose_transform_tool(TransformTool::rotate);
                if(ImGui::MenuItem("缩放","R",transform_tool==TransformTool::scale))choose_transform_tool(TransformTool::scale);
                ImGui::MenuItem("显示变换工具",nullptr,&show_move_tool);
                creation_menu(-1);
                ImGui::Separator();
                if(ImGui::MenuItem("渲染设置")){requested_panel=0;show_inspector=true;}
                if(ImGui::MenuItem("演示场景")){requested_panel=2;show_inspector=true;}
                if(ImGui::MenuItem("工程与资源")){requested_panel=3;show_inspector=true;}
                ImGui::EndMenu();
            }
            if(ImGui::BeginMenu("演示场景")){
                if(ImGui::MenuItem("返回原场景","Esc",false,bool(topic_return))){try{return_from_topic();}catch(const std::exception& e){error=e.what();}}
                ImGui::Separator();
                for(const auto& showcase:showcase_catalog())if(ImGui::MenuItem(std::string(showcase.title).c_str()))open_showcase(showcase.id);
                ImGui::Separator();
                for(const auto& demo:demo_scenes)if(ImGui::MenuItem(demo.name))open_demo(demo.profile);
                ImGui::Separator();
                if(ImGui::MenuItem("平移回放 / TAA、CSM")){options.replay="pan";replay_origin=camera;replay_index=0;replay_running=true;}
                if(ImGui::MenuItem("拉远回放 / LOD")){options.replay="dolly";replay_origin=camera;replay_index=0;replay_running=true;}
                if(ImGui::MenuItem("停止回放",nullptr,false,replay_running)){replay_running=false;camera=replay_origin;mark_dirty();}
                if(ImGui::MenuItem("所选物体运动 / TAA、SVGF",nullptr,false,!motion_replay.active)){
                    try{start_object_replay();}catch(const std::exception& e){error=e.what();}
                }
                if(ImGui::MenuItem("停止物体运动并还原",nullptr,false,motion_replay.active))stop_object_replay();
                ImGui::TextDisabled("回放不改变模型；资源完成后开始。");ImGui::EndMenu();
            }
            if(ImGui::BeginMenu("帮助")){
                ImGui::TextUnformatted("右键拖动：旋转    Shift + 右键：平移");ImGui::TextUnformatted("滚轮：缩放    F：聚焦    F11：全屏");
                ImGui::TextUnformatted("拖入 GLB / glTF / OBJ：追加模型。");ImGui::TextUnformatted("拖入 PNG / JPG / TGA / BMP：选择材质贴图槽。");
                ImGui::TextUnformatted("W 平移 / E 旋转 / R 缩放；在视口拖动对应手柄。");
                ImGui::TextUnformatted("Shift 吸附：位置 0.1 / 角度 15° / 缩放比例 0.1。");
                ImGui::TextUnformatted("缩放中心方框：等比缩放；灯光方向/尺寸在光源参数中编辑。");
                ImGui::TextUnformatted("Esc：取消当前拖动；专题预览中返回原场景。");
                ImGui::TextDisabled("侧栏内的操作不会转动相机。");ImGui::EndMenu();
            }
            menu_end=std::min(controls_start,ImGui::GetCursorScreenPos().x+8);
            ImGui::EndMenuBar();
        }ImGui::End();ImGui::PopStyleVar();
        const std::array<emberframe::platform::WindowDragRegion,1> drag_regions{{{menu_end,0,std::max(0.f,controls_start-menu_end),shell.menu}}};
        const std::array<emberframe::platform::WindowDragRegion,2> interactive_regions{{{0,0,menu_end,shell.menu},{controls_start,0,control_width*3,shell.menu}}};
        window.set_titlebar_drag_regions(drag_regions);window.set_titlebar_interactive_regions(interactive_regions);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
        panel("Window controls",{controls_start,0},{control_width*3,shell.menu},ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        const char* control_names[]{"最小化","最大化 / 还原","关闭"};
        for(int i=0;i<3;++i){
            if(i)ImGui::SameLine(0,0);
            ImGui::PushID(i);const auto p=ImGui::GetCursorScreenPos();
            const bool clicked=ImGui::InvisibleButton("window-control",{control_width,shell.menu});
            auto* draw=ImGui::GetWindowDrawList();
            if(ImGui::IsItemHovered()){
                const auto rgb=i==2?glm::vec3(180,30,30)/255.f:glm::vec3(65,65,69)/255.f;
                draw->AddRectFilled(p,{p.x+control_width,p.y+shell.menu},ImGui::ColorConvertFloat4ToU32({srgb_to_linear(rgb.x),srgb_to_linear(rgb.y),srgb_to_linear(rgb.z),1}));
                ImGui::SetTooltip("%s",control_names[i]);
            }
            const ImU32 ink=ImGui::GetColorU32(ImGuiCol_Text);const float x=p.x+control_width*.5f,y=p.y+shell.menu*.5f;
            if(i==0)draw->AddLine({x-5,y+3},{x+5,y+3},ink,1);
            else if(i==1){
                if(window.maximized()||window.fullscreen()){
                    draw->AddRect({x-2,y-5},{x+5,y+2},ink);draw->AddRectFilled({x-5,y-2},{x+2,y+5},ImGui::GetColorU32(ImGuiCol_WindowBg));draw->AddRect({x-5,y-2},{x+2,y+5},ink);
                }else draw->AddRect({x-5,y-5},{x+5,y+5},ink);
            }else{draw->AddLine({x-5,y-5},{x+5,y+5},ink,1.2f);draw->AddLine({x-5,y+5},{x+5,y-5},ink,1.2f);}
            if(clicked){
                if(i==0)window.minimize();
                else if(i==1){if(window.fullscreen()){fullscreen=false;SDL_SetWindowFullscreen(window.native_handle(),0);}else window.toggle_maximized();gpu.resize();}
                else running=false;
            }
            ImGui::PopID();
        }
        ImGui::End();ImGui::PopStyleVar();
        // 视口只保留高频操作；渲染算法、质量参数与诊断不挤在这里。
        panel("Viewport toolbar",{shell.left,shell.menu},{shell.viewport_width(),shell.toolbar},ImGuiWindowFlags_NoScrollbar);
        ImGui::SetNextItemWidth(std::clamp(shell.viewport_width()*.27f,100.f,185.f));
        const char* scene_names[]{"材质工作室","彩色房间 / GI","阴影测试","多光源"};
        if(ImGui::BeginCombo("##Scene preset",preset>=0?scene_names[preset]:scene.name.c_str())){
            for(int id=0;id<4;++id)if(ImGui::Selectable(scene_names[id],preset==id)){preset_scene(id);active_demo=active_topic=-1;}
            ImGui::EndCombo();
        }
        help("切换内置场景；导入的资产显示在左侧场景树。");
        ImGui::SameLine();ImGui::SetNextItemWidth(std::clamp(shell.viewport_width()*.27f,100.f,168.f));
        const char* render_paths=show_developer_tools?"Vulkan 前向\0Vulkan 延迟\0CPU 光栅参考\0CPU 路径追踪\0":"Vulkan 前向\0Vulkan 延迟\0";
        if(combo("##Render path",settings.path,render_paths)){visual_topic=-1;mark_dirty();}
        help("不支持的实时配置会暂停渲染并保留旧画面，不自动转 CPU。显式 CPU 参考只在开启开发者工具后提供。");
        if(shell.viewport_width()>480){
            ImGui::SameLine();ImGui::SetNextItemWidth(std::clamp(shell.viewport_width()*.25f,110.f,158.f));
            if(combo("##View mode",settings.debug,"最终颜色\0BaseColor\0世界法线\0深度\0粗糙度\0金属度\0AO\0阴影可见性\0间接光\0运动向量\0方差\0每组灯数\0"))mark_dirty();help("视口显示模式 / 渲染中间结果。");
        }else{
            ImGui::SameLine();if(ImGui::Button("显示"))ImGui::OpenPopup("View modes");
            if(ImGui::BeginPopup("View modes")){if(combo("显示模式",settings.debug,"最终颜色\0BaseColor\0世界法线\0深度\0粗糙度\0金属度\0AO\0阴影可见性\0间接光\0运动向量\0方差\0每组灯数\0"))mark_dirty();ImGui::EndPopup();}
        }
        if(shell.viewport_width()>740){ImGui::SameLine();if(ImGui::Button("聚焦")){frame_camera(scene,camera);mark_dirty();}help("聚焦整个场景 · F");}
        ImGui::SameLine();const char* compact_tool=transform_tool==TransformTool::translate?"W":transform_tool==TransformTool::rotate?"E":"R";
        const std::string tool_label=std::string(shell.viewport_width()<360?compact_tool:transform_tool_name(transform_tool))+"##Transform tool";
        if(ImGui::Button(tool_label.c_str()))ImGui::OpenPopup("Transform tools");
        help("W 平移、E 旋转、R 缩放。旋转沿世界轴，缩放沿模型局部轴。光源保留平移箭头。");
        if(ImGui::BeginPopup("Transform tools")){
            if(ImGui::MenuItem("平移","W",transform_tool==TransformTool::translate))choose_transform_tool(TransformTool::translate);
            if(ImGui::MenuItem("旋转","E",transform_tool==TransformTool::rotate))choose_transform_tool(TransformTool::rotate);
            if(ImGui::MenuItem("缩放","R",transform_tool==TransformTool::scale))choose_transform_tool(TransformTool::scale);
            ImGui::EndPopup();
        }
        ImGui::End();ImGui::PopStyleVar();
        if(shell.left>0){
            panel("Scene dock",{0,shell.menu},{shell.left,shell.body_height()});
            title("场景");ImGui::SetNextItemWidth(-FLT_MIN);ImGui::InputTextWithHint("##Scene filter","筛选节点…",scene_filter.data(),scene_filter.size());
            ImGui::BeginChild("Hierarchy",{0,std::max(100.f,shell.body_height()*.52f)},false);
            // 按真实 parent 构建层级；搜索时平铺匹配项，避免父节点不匹配时把子节点藏掉。
            std::vector<std::vector<int>> children(scene.nodes.size());std::vector<int> roots;
            for(int i=0;i<int(scene.nodes.size());++i){int p=scene.nodes[i].parent;if(p>=0&&p<int(scene.nodes.size())&&p!=i)children[p].push_back(i);else roots.push_back(i);}
            std::vector<bool> visited(scene.nodes.size());
            std::function<void(int,int)> draw_node=[&](int i,int depth){
                if(visited[i]||depth>64)return;visited[i]=true;
                const auto& node=scene.nodes[i];const auto name=(node.name.empty()?"Node":node.name)+"  "+std::to_string(i+1);
                ImGui::PushID(i);auto flags=ImGuiTreeNodeFlags_OpenOnArrow|ImGuiTreeNodeFlags_SpanAvailWidth;
                if(selected_node==i)flags|=ImGuiTreeNodeFlags_Selected;if(children[i].empty())flags|=ImGuiTreeNodeFlags_Leaf|ImGuiTreeNodeFlags_NoTreePushOnOpen;
                bool open=ImGui::TreeNodeEx(name.c_str(),flags);if(ImGui::IsItemClicked()&&!ImGui::IsItemToggledOpen())select_node(i);
                if(ImGui::BeginPopupContextItem("Node actions")){
                    const int light=light_for_node(i);
                    if(light>=0?selected_light!=light:selected_node!=i)select_node(i);
                    ImGui::TextUnformatted(name.c_str());ImGui::Separator();creation_menu(i);ImGui::EndPopup();
                }
                if(open&&!children[i].empty()){for(int child:children[i])draw_node(child,depth+1);ImGui::TreePop();}ImGui::PopID();
            };
            if(scene_filter[0]){
                for(int i=0;i<int(scene.nodes.size());++i){const auto name=(scene.nodes[i].name.empty()?"Node":scene.nodes[i].name)+"  "+std::to_string(i+1);if(name.find(scene_filter.data())==std::string::npos)continue;ImGui::PushID(i);if(ImGui::Selectable(name.c_str(),selected_node==i))select_node(i);if(ImGui::BeginPopupContextItem("Filtered node actions")){creation_menu(i);ImGui::EndPopup();}ImGui::PopID();}
            }else{for(int root:roots)draw_node(root,0);}
            if(scene.nodes.empty())ImGui::TextDisabled("没有场景节点");
            if(ImGui::BeginPopupContextWindow("Create scene object",ImGuiPopupFlags_MouseButtonRight|ImGuiPopupFlags_NoOpenOverItems)){
                creation_menu(-1);ImGui::EndPopup();
            }
            if(ImGui::TreeNodeEx("光源",ImGuiTreeNodeFlags_DefaultOpen)){
                for(int i=0;i<int(scene.lights.size());++i){ImGui::PushID(100000+i);std::string label="光源 "+std::to_string(i+1)+(scene.lights[i].kind==LightKind::rectangle?" · 面积":scene.lights[i].kind==LightKind::point?" · 点":" · 方向");if(ImGui::Selectable(label.c_str(),selected_light==i))select_light(i);ImGui::PopID();}ImGui::TreePop();
            }
            ImGui::EndChild();ImGui::Separator();title("场景资源");
            ImGui::BeginChild("Assets",{0,0},false);
            if(ImGui::TreeNode("网格", "网格  (%d)",int(scene.meshes.size()))){for(int i=0;i<int(scene.meshes.size());++i){ImGui::PushID(i);const auto label=scene.meshes[i].name.empty()?"Mesh "+std::to_string(i):scene.meshes[i].name;if(ImGui::Selectable(label.c_str(),property_target==PropertyTarget::mesh&&selected_mesh==i))select_resource(PropertyTarget::mesh,i);ImGui::PopID();}ImGui::TreePop();}
            if(ImGui::TreeNodeEx("Materials",ImGuiTreeNodeFlags_DefaultOpen,"材质  (%d)",int(scene.materials.size()))){for(int i=0;i<int(scene.materials.size());++i){ImGui::PushID(i);const auto label=scene.materials[i].name+"  "+std::to_string(i+1);if(ImGui::Selectable(label.c_str(),property_target==PropertyTarget::material&&selected_material==i))select_resource(PropertyTarget::material,i);ImGui::PopID();}ImGui::TreePop();}
            if(ImGui::TreeNode("Textures","纹理  (%d)",int(scene.textures.size()))){for(int i=0;i<int(scene.textures.size());++i){ImGui::PushID(i);if(ImGui::Selectable(scene.textures[i].name.c_str(),property_target==PropertyTarget::texture&&selected_texture==i))select_resource(PropertyTarget::texture,i);ImGui::PopID();}ImGui::TreePop();}
            ImGui::EndChild();ImGui::End();
        }
        if(shell.right>0){
        panel("Inspector dock",{shell.width-shell.right,shell.menu},{shell.right,shell.body_height()});
        title("属性检查器");
        const auto applicability=inspect_config(scene,settings);
        const auto& limitations=ui_gpu_limitations;
        const bool gpu_blocked=applicability.gpu_path_requested&&!limitations.empty();
        const bool gpu_rendering=applicability.gpu_path_requested&&!gpu_blocked&&visual_topic<0;
        // 执行路径/不支持的已加载选项必须可见，其余解释按需展开。
        ImGui::PushTextWrapPos();
        if(gpu_blocked){
            ImGui::TextColored({1,.77f,.4f,1},"实时预览已暂停 · 未启动 CPU 计算");
            for(const auto& why:limitations)ImGui::TextWrapped("%s",gpu_issue_text(why).c_str());
            ImGui::TextWrapped("当前保留最近完成的 GPU 画面；修正配置后继续渲染。");
        }
        else if(visual_topic>=0&&applicability.gpu_path_requested)ImGui::TextColored({1,.77f,.4f,1},"实际执行：CPU 专题对照");
        if(applicability.csm_unsupported)ImGui::TextColored({1,.77f,.4f,1},"CSM 当前不支持：场景没有方向光。");
        if(applicability.gpu_sdf_unsupported)ImGui::TextColored({1,.77f,.4f,1},"GPU SDF 当前不适用：无方向光，可关闭该选项。");
        ImGui::PopTextWrapPos();
        if(section("当前配置适用性")){
            if(!applicability.has_environment_input)ImGui::TextWrapped("黑色天空：IBL / SH / PRT 无环境输入。");
            if(settings.path!=RenderPath::path_trace){
                ImGui::TextWrapped("AO 只作用于间接光。");
                if(applicability.ao_no_indirect_input){
                    ImGui::TextWrapped(settings.gi==GiMode::environment?"环境光 + 黑色天空：AO 不改变最终颜色。":"间接光已关闭：AO 不改变最终颜色。");
                    ImGui::BeginDisabled(!applicability.ao_enabled);
                    if(ImGui::Button("查看 AO 调试图")){settings.debug=DebugView::ao;mark_dirty();}
                    ImGui::EndDisabled();
                    if(!applicability.ao_enabled&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("请先启用 SSAO 或 GTAO。");
                }
            }
            if(!applicability.has_directional_light)ImGui::TextWrapped("CSM 仅适用于方向光；当前选项已禁用。");
            if(applicability.gpu_path_requested&&!applicability.gpu_sdf_supported)ImGui::TextWrapped("GPU SDF 仅作用于方向光；矩形光使用阴影图。");
            if(gpu_rendering&&applicability.rectangle_center_shadow){
                ImGui::TextWrapped("矩形光阴影：从灯心作透视投影。");
                if(applicability.rectangle_light_count>1)ImGui::TextWrapped("无方向光时，仅首盏矩形光生成 GPU 阴影。");
                if(applicability.rectangle_penumbra_approximate){
                    ImGui::TextWrapped("PCSS / VSSM 估计有限矩形半影；可见率是近似。");
                    ImGui::TextWrapped("半影倍率 %.2fx；尺度 0.15 对应实际灯尺寸。",applicability.rectangle_softness_multiplier);
                }
            }
            if(gpu_rendering&&applicability.rectangle_light_count>0){
                ImGui::TextWrapped("面积光：PBR / Blinn / Disney 使用面积积分。");
                ImGui::TextWrapped("KC 仅各向同性 GGX；Blinn / 卡通不适用。");
            }
            if(!applicability.has_texture_input)ImGui::TextWrapped("无纹理采样输入；过滤选项已禁用。");
            if(applicability.low_light_count)ImGui::TextWrapped(applicability.light_count==1?
                "仅 1 盏灯：Tiled / Clustered 性能差异通常很小。":"没有灯：Tiled / Clustered 无灯可筛选。");
            ImGui::TextWrapped("同一配置下，前向 / 延迟最终颜色应一致。");
            if(selected_light<0&&!scene.materials.empty()){
                const int material=std::clamp(selected_material,0,int(scene.materials.size())-1);
                if(material==0&&(scene.name=="Colored-box GI room"||scene.name=="Material studio"||scene.name=="Shadow test"||scene.name=="Many lights"))
                    ImGui::TextWrapped("当前目标是地板共享材质；在资源列表选择材质，或从节点的材质引用进入编辑。");
                else ImGui::TextWrapped("共享材质目标：%s；节点只编辑实例变换和资源引用。",scene.materials[material].name.c_str());
            }
            if(applicability.gpu_volume_triangle_limit_exceeded)ImGui::TextWrapped("场景几何超过 GPU 索引寻址范围；没有截断模型或回退 CPU。");
            if(gpu_rendering&&applicability.volume_gi_selected){
                ImGui::TextWrapped("完整几何通过 BVH 加速；容量按设备显存 / Buffer 预算判断。");
                if(applicability.gpu_volume_no_emitting_light)ImGui::TextWrapped("当前没有发光光源：GI 可启用，但没有光可供反弹。");
                else ImGui::Text("GI 主光源：%d",applicability.gpu_volume_light_index+1);
                if(applicability.gpu_volume_point_cubemap)ImGui::TextWrapped("点光使用六面投影，覆盖四周。");
                if(applicability.gpu_volume_area_center_approximate)ImGui::TextWrapped("面积光 GI 使用灯心投影近似，不是完整面积积分。");
                if(applicability.gpu_volume_primary_source_only)ImGui::TextWrapped("GI 当前来自一盏主光源；其他光源仍参与直接照明。");
            }
            if(applicability.gpu_volume_resolution_unsupported)ImGui::TextWrapped("GPU GI 网格仅支持 16³ / 32³。");
            if(applicability.gpu_light_limit_exceeded)ImGui::TextWrapped("GPU 光源上限：64 盏。");
            if((settings.gi==GiMode::environment&&!applicability.has_environment_input)||applicability.gpu_volume_missing_directional){
                ImGui::BeginDisabled(!applicability.can_use_gpu_ssgi);
                if(ImGui::Button("切换 SSGI（GPU）",{-FLT_MIN,0})){
                    // 保留已有前向/延迟路径；从 CPU 对照切换时明确启用 GPU。
                    settings.gi=GiMode::ssgi;
                    if(!applicability.gpu_path_requested)settings.path=RenderPath::deferred;
                    visual_topic=-1;settings.debug=DebugView::final_color;mark_dirty();
                }
                ImGui::EndDisabled();
                if(!applicability.can_use_gpu_ssgi&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(applicability.csm_unsupported?"请先显式选择适用的阴影模式。":
                        settings.sdf_shadows&&!applicability.gpu_sdf_supported?"请先关闭不适用的 GPU SDF 阴影。":"GPU 光源数不能超过 64 盏。");
                else help("从屏幕可见表面估计间接光；不改变天空或直接光。少量采样会有噪点，可配合 TAA / HW5 降噪。");
            }
            if(gpu_blocked&&applicability.volume_gi_selected&&ImGui::Button("打开兼容的 GI 演示场景",{-FLT_MIN,0}))open_demo("shadows");
        }
        if(ImGui::BeginTabBar("Inspector panels",ImGuiTabBarFlags_FittingPolicyResizeDown)){
            // 消费本帧请求，但保留页内按钮新发出的请求，供下一帧跳转。
            const int panel_request=requested_panel;requested_panel=-1;
            if(ImGui::BeginTabItem("渲染设置",nullptr,panel_request==0?ImGuiTabItemFlags_SetSelected:0)){
                bool changed=false;
                if(section("光照与可见性",true)&&properties("Lighting properties")){
                    // 仅禁用不适用的 CSM，保留加载/脚本中的原始枚举，不自动改成 PCF。
                    constexpr const char* shadow_modes[]={"Hard","PCF","PCSS","VSM + SAT","VSSM","MSM","CSM"};
                    const bool csm_supported=applicability.csm_supported;
                    row("阴影");
                    const char* shadow_preview=settings.shadows==ShadowMode::csm&&!csm_supported?"CSM（当前不支持）":shadow_modes[std::clamp(int(settings.shadows),0,6)];
                    if(ImGui::BeginCombo("##value",shadow_preview)){
                        for(int i=0;i<7;++i){
                            const bool disabled=i==int(ShadowMode::csm)&&!csm_supported;
                            if(ImGui::Selectable(shadow_modes[i],int(settings.shadows)==i,disabled?ImGuiSelectableFlags_Disabled:0)){settings.shadows=ShadowMode(i);changed=true;}
                            if(disabled&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("CSM 只适用于方向光；当前场景没有方向光。");
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopID();
                    changed|=choice("间接光",settings.gi,"环境光\0SSR\0SSGI\0RSM\0LPV\0Voxel cone tracing\0关闭\0");
                    help("RSM / LPV / VCT 支持完整场景几何，不再限制 4096 个三角形。使用一盏主光源：方向光、点光六面或面积光灯心近似；无灯时没有可反弹光。SSR / SSGI 只使用屏幕可见数据，低采样可配合 SVGF。");
                    changed|=choice("环境漫反射",settings.environment_diffuse,"IBL 预卷积\0SH 球谐\0PRT 烘焙传输\0");help("作用于当前场景；PRT 为静态实例烘焙可见性，仍保留 IBL 镜面反射。需选择环境光间接光模式。");
                    ImGui::SeparatorText("环境照明");
                    ImGui::TextWrapped("%s",scene.environment_map?scene.environment_map->name().c_str():"解析天空（未导入 HDR）");
                    ImGui::BeginDisabled(imported_environment.valid());
                    if(ImGui::Button("导入 HDR…"))browse(FileKind::environment);
                    ImGui::EndDisabled();
                    if(scene.environment_map){
                        ImGui::SameLine();if(ImGui::Button("恢复解析天空"))editor_change("清除 HDR 环境",[&]{set_scene_environment(scene,{},1,0);});
                        float intensity=scene.environment_intensity,rotation=scene.environment_rotation;
                        if(ImGui::SliderFloat("环境强度",&intensity,0,8,"%.2f"))editor_change("环境强度",[&]{set_scene_environment(scene,scene.environment_map,intensity,rotation);});
                        if(ImGui::SliderFloat("环境旋转",&rotation,-180,180,"%.1f°"))editor_change("环境旋转",[&]{set_scene_environment(scene,scene.environment_map,intensity,rotation);});
                    }
                    // GPU 无方向光时禁止新启用，仍允许关闭加载/脚本中保留的值。
                    const bool sdf_supported=!applicability.gpu_path_requested||applicability.gpu_sdf_supported;
                    ImGui::BeginDisabled(!sdf_supported&&!settings.sdf_shadows);
                    changed|=toggle(applicability.gpu_path_requested?"SDF（方向光）":"距离场阴影",settings.sdf_shadows);
                    ImGui::EndDisabled();
                    if(!sdf_supported&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("GPU SDF 只适用于方向光；矩形光使用透视阴影图。已加载的值可在此关闭。");
                    else help(applicability.gpu_path_requested?
                        "GPU 沿方向光射线查询场景距离场；粗网格会限制细小遮挡物精度。":
                        "CPU 参考对矩形光查询到灯心的有限距离射线；这是中心可见性近似。");
                    if(settings.gi==GiMode::voxel){changed|=toggle("稀疏体素查询",settings.sparse_voxels);help("GPU 将非空体素组织成稀疏查询数据；体素化与 Mip 构建仍使用有界稠密中间态。");}
                    changed|=choice("环境遮蔽",settings.ao,"关闭\0SSAO\0GTAO\0");
                    changed|=choice("光源筛选",settings.culling,"全部灯\0Tiled 分块\0Clustered 聚簇\0");ImGui::EndTable();
                }
                if(gpu_rendering&&(settings.gi==GiMode::rsm||settings.gi==GiMode::lpv||settings.gi==GiMode::voxel)){
                    const int source=volume_gi_primary_light(scene);
                    if(source<0)ImGui::TextWrapped("GI 已启用；没有外部主光源，可能没有可见反弹光。");
                    else if(scene.lights[source].kind==LightKind::rectangle)ImGui::TextWrapped("GI 主光源 %d · 面积光灯心近似",source+1);
                    else if(scene.lights[source].kind==LightKind::point)ImGui::TextWrapped("GI 主光源 %d · 点光六面投影",source+1);
                    else ImGui::TextDisabled("GI 主光源 %d · 方向光",source+1);
                    ImGui::TextDisabled("完整几何 BVH · 主光源单次反弹近似");
                }
                if(section("材质与采样",true)&&properties("Sampling properties")){
                    changed|=choice("材质模型",settings.shading,"PBR / GGX\0Blinn–Phong\0Disney\0卡通\0");
                    ImGui::BeginDisabled(!applicability.has_texture_input);
                    changed|=choice("纹理过滤",settings.filter,"最近邻\0双线性\0三线性 / Mip\0各向异性\0");
                    ImGui::EndDisabled();
                    if(!applicability.has_texture_input&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("当前材质没有纹理采样输入。");
                    changed|=toggle("能量补偿",settings.energy_compensation);help("KC 仅补偿各向同性 GGX；Blinn、卡通与各向异性 Disney 不适用。");
                    changed|=toggle("Reversed-Z",settings.reversed_z);ImGui::EndTable();
                }
                if(section("几何与场景资源",false)&&properties("Scene resources properties")) {
                    changed|=toggle("自动 LOD",settings.auto_lod);
                    if(settings.auto_lod)changed|=scalar("屏幕误差 / px",settings.lod_error_pixels,.25f,16,"%.2f");
                    changed|=choice("射线空间结构",settings.spatial_structure,"BVH\0八叉树\0");help("用于实际场景射线查询和 CPU 路径追踪，不是另一种阴影效果。");
                    changed|=integer("烘焙采样",settings.bake_samples,16,512);
                    changed|=integer("距离场网格",settings.sdf_resolution,8,32);ImGui::EndTable();
                }
                if(section("几何与场景资源操作",false)) {
                    ImGui::BeginDisabled(scene_bake.valid());
                    if(ImGui::Button("烘焙当前场景资源",{-FLT_MIN,0})){bake_request_key.clear();start_bake();}
                    help("按当前启用的 PRT/距离场选项生成并缓存资源；未启用时只准备环境 SH。完成后再保存工程。");
                    ImGui::EndDisabled();ImGui::BeginDisabled(lod_chain_build.valid()||scene.meshes.empty());
                    if(ImGui::Button("生成选中网格 LOD 链",{-FLT_MIN,0}))build_selected_lods();
                    ImGui::EndDisabled();
                    ImGui::BeginDisabled(simplified.valid()||scene.meshes.empty());
                    if(ImGui::Button("简化选中网格（可撤销）",{-FLT_MIN,0})){
                        selected_mesh=std::clamp(selected_mesh,0,int(scene.meshes.size())-1);
                        auto mesh=scene.meshes[selected_mesh];simplification_generation=generation;simplification_epoch=document_epoch;simplification_mesh=selected_mesh;
                        simplified=std::async(std::launch::async,[mesh=std::move(mesh)](){QemOptions o;o.target_triangles=mesh.indices.size()/6;return simplify_qem(mesh,o);});
                        notice="后台简化选中网格，保护边界、材质与 UV 接缝。";
                    }
                    ImGui::EndDisabled();help("在左侧选择网格；这是一项明确的资源编辑，不是进入实验或运行验证。");
                    if(scene_bake.valid())ImGui::TextDisabled("烘焙中；模型变化会取消旧任务。");
                    if(scene.baked_resources)ImGui::TextDisabled("PRT %s · 距离场 %s",scene.baked_resources->prt?"已缓存":"未启用",scene.baked_resources->sdf?"已缓存":"未启用");
                    const auto& geometry_stats=scene_geometry.stats();
                    ImGui::TextWrapped("%s",geometry_stats.status.c_str());
                    ImGui::TextDisabled("几何 %llu → %llu triangles",(unsigned long long)geometry_stats.source_triangles,(unsigned long long)geometry_stats.selected_triangles);
                    ImGui::TextDisabled("空间树 %zu nodes · %llu 次重建",geometry_stats.spatial_nodes,(unsigned long long)geometry_stats.spatial_rebuilds);
                }
                if(section("后处理与时间",true)&&properties("Post properties")){
                    changed|=scalar("曝光",settings.exposure,.05f,8,"%.2f",ImGuiSliderFlags_Logarithmic);
                    changed|=toggle("Bloom",settings.bloom);if(settings.bloom)changed|=scalar("光晕强度",settings.bloom_strength,0,.5f);
                    changed|=toggle("TAA",settings.taa);changed|=toggle("HW5 降噪",settings.denoise);changed|=toggle("SVGF",settings.svgf);ImGui::EndTable();
                }
                if(section("非真实感风格")&&properties("NPR properties")){
                    changed|=toggle("轮廓描边",settings.outline);changed|=toggle("排线",settings.hatching);ImGui::EndTable();
                }
                if(changed)mark_dirty();
                if(section("质量与算法参数")&&properties("Quality properties")){
                    bool q=false;q|=integer("采样数",settings.samples,1,128);q|=integer("路径反弹",settings.max_bounces,1,12);
                    q|=toggle("适应视口",fit_viewport);
                    if(fit_viewport)q|=scalar("渲染比例",render_scale,.5f,1.5f,"%.2fx");
                    else {
                        q|=integer("渲染宽度",settings.render_width,160,4096);q|=integer("渲染高度",settings.render_height,90,4096);
                        const auto safe=fit_resolution(float(settings.render_width),float(settings.render_height),1,gpu.max_render_tiles(),gpu.max_render_pixels());
                        if(safe.width!=settings.render_width||safe.height!=settings.render_height){settings.render_width=safe.width;settings.render_height=safe.height;notice="分辨率已按设备灯表 / 显存预算等比例限制。";q=true;}
                    }
                    if(q)mark_dirty();
                    if(scalar("阴影 Bias",settings.shadow_bias,.00001f,.02f,"%.5f",ImGuiSliderFlags_Logarithmic))mark_dirty();
                    if(scalar("光源尺度",settings.light_size,.01f,1))mark_dirty();
                    help(gpu_rendering&&applicability.rectangle_center_shadow?
                        "矩形光：0.15 使用实际最大半边长；其它值调节 PCSS / VSSM 半影倍率，上限 5x。":
                        gpu_rendering?"方向光阴影的光源角尺度，用于 PCSS / VSSM 半影估计。":
                        "当前为 CPU 参考；GPU 半影尺度未使用。");
                    if(scalar("AO 半径",settings.ao_radius,.05f,3))mark_dirty();
                    if(scalar("当前帧权重",settings.temporal_weight,.01f,1))mark_dirty();
                    int shadow_quality=settings.shadow_resolution==128?0:settings.shadow_resolution==256?1:settings.shadow_resolution==512?2:settings.shadow_resolution==1024?3:4;
                    if(choice("阴影分辨率",shadow_quality,"128\0 256\0 512\0 1024\0 2048\0")){settings.shadow_resolution=128<<shadow_quality;mark_dirty();}
                    int volume_quality=settings.voxel_resolution==16?0:1;if(choice("GI 网格",volume_quality,"16³\0 32³\0")){settings.voxel_resolution=volume_quality?32:16;mark_dirty();}
                    if(integer("LPV 传播步数",settings.propagation_steps,1,8))mark_dirty();ImGui::EndTable();
                }
                ImGui::TextDisabled("渲染 %d x %d  |  视口 %d x %d",settings.render_width,settings.render_height,viewport_pixel_width,viewport_pixel_height);
                ImGui::TextDisabled("DPI %.0f%%  ·  字体按 %.0f%% 像素生成",stats.display_scale*100,stats.font_raster_scale*100);
                if(resolution_limited)ImGui::TextColored({1,.77f,.4f,1},"已触发设备 / 显存分辨率限制");
                ImGui::Separator();if(ImGui::Button("重新渲染"))mark_dirty();ImGui::SameLine();if(ImGui::Button("保存当前画面"))capture();
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("对象 / 资源",nullptr,panel_request==1?ImGuiTabItemFlags_SetSelected:0)){
                if(selected_node>=int(scene.nodes.size()))selected_node=-1;
                if(selected_light>=int(scene.lights.size()))selected_light=-1;
                auto edit_name=[&](const char* label,std::string& name,bool resource){
                    std::array<char,1024> text{};std::snprintf(text.data(),text.size(),"%s",name.c_str());
                    if(ImGui::InputText(label,text.data(),text.size())){begin_edit("修改名称");name=text.data();++scene.revision;if(resource)stamp_assets();mark_dirty();}
                };
                if(property_target==PropertyTarget::none)ImGui::TextWrapped("选择上方场景节点编辑实例；选择下方资源编辑共享数据。");
                if(property_target==PropertyTarget::node&&selected_node>=0){
                    const int node_index=selected_node;
                    ImGui::TextColored({.45f,.75f,1,1},"节点属性 · 只修改这个实例");
                    edit_name("节点名称",scene.nodes[node_index].name,false);
                    ImGui::TextWrapped("%s",scene.nodes[selected_node].name.c_str());
                    try{auto position=glm::vec3(scene_world_transforms(scene).at(selected_node)[3]);
                        if(properties("Node transform")){
                            if(vector("世界位置",&position.x,.03f)){begin_edit("移动模型");set_node_position(selected_node,position);mark_dirty();}
                            auto pose=local_transform(scene.nodes[selected_node].local);const auto original_scale=pose.scale;
                            const bool rotated=vector("局部旋转 °",&pose.rotation_degrees.x,.5f);
                            const bool scaled=vector("局部缩放",&pose.scale.x,.01f);
                            if(scaled&&uniform_scale){for(int a=0;a<3;++a)if(pose.scale[a]!=original_scale[a]){
                                pose.scale=original_scale*(pose.scale[a]/original_scale[a]);break;}}
                            if(rotated||scaled){const auto local=compose_local_transform(pose);begin_edit(rotated?"旋转模型":"缩放模型");set_node_local(scene,selected_node,local);mark_dirty();}
                            toggle("等比缩放",uniform_scale);ImGui::EndTable();
                        }
                    }catch(const std::exception& e){error=e.what();}
                    ImGui::TextDisabled("旋转/缩放相对父节点；负缩放表示镜像。");
                    ImGui::TextDisabled("Ctrl + 左键拖动旋转；Shift 为 15° 吸附。");
                    ImGui::Separator();title("引用的资源");
                    const int mesh_index=scene.nodes[node_index].mesh;
                    const char* mesh_name=mesh_index>=0&&mesh_index<int(scene.meshes.size())?scene.meshes[mesh_index].name.c_str():"无网格（空节点）";
                    if(ImGui::BeginCombo("网格引用",mesh_name)){
                        if(ImGui::Selectable("无网格（空节点）",mesh_index<0)){try{editor_change("解除节点网格引用",[&](){scene.nodes[node_index].mesh=-1;changed_objects(scene);});}catch(const std::exception& e){error=e.what();}}
                        for(int m=0;m<int(scene.meshes.size());++m){ImGui::PushID(m);if(ImGui::Selectable(scene.meshes[m].name.c_str(),mesh_index==m)){
                            try{editor_change("更换节点网格引用",[&](){scene.nodes[node_index].mesh=m;changed_objects(scene);});selected_mesh=m;}catch(const std::exception& e){error=e.what();}
                        }ImGui::PopID();}ImGui::EndCombo();
                    }
                    int inspect_mesh=-1,inspect_material=-1;
                    if(scene.nodes[node_index].mesh>=0){
                        const int mesh=scene.nodes[node_index].mesh;
                        ImGui::TextDisabled("%zu 个节点引用这份网格",mesh_instance_count(scene,mesh));
                        if(ImGui::Button("查看网格资源"))inspect_mesh=mesh;
                        const auto count=std::max<std::size_t>(1,scene.meshes[mesh].primitives.size());
                        for(std::size_t p=0;p<count;++p){
                            ImGui::PushID(int(p));const int material=node_material(scene,node_index,p);
                            ImGui::Text("分部 %zu 的材质",p+1);
                            const char* name=material>=0&&material<int(scene.materials.size())?scene.materials[material].name.c_str():"无材质";
                            if(ImGui::BeginCombo("##Instance material",name)){
                                for(int m=0;m<int(scene.materials.size());++m){ImGui::PushID(m);if(ImGui::Selectable(scene.materials[m].name.c_str(),material==m)){
                                    try{editor_change("仅更换本实例材质",[&](){assign_node_material(scene,node_index,p,m);});selected_mesh=scene.nodes[node_index].mesh;}
                                    catch(const std::exception& e){error=e.what();}
                                }ImGui::PopID();}ImGui::EndCombo();
                            }
                            if(material>=0&&material<int(scene.materials.size())&&ImGui::Button("编辑共享材质"))inspect_material=material;
                            if(ImGui::Button("创建独立材质并编辑")){
                                try{editor_change("创建实例独立材质",[&](){inspect_material=make_node_material_unique(scene,node_index,p);});selected_mesh=scene.nodes[node_index].mesh;}
                                catch(const std::exception& e){error=e.what();}
                            }
                            ImGui::PopID();
                        }
                        ImGui::TextWrapped("更换材质仅作用于本实例；编辑共享材质会影响其他引用者。独立副本保留原资源，派生 LOD 可重新生成。");
                    }
                    if(inspect_mesh>=0)select_resource(PropertyTarget::mesh,inspect_mesh);
                    else if(inspect_material>=0)select_resource(PropertyTarget::material,inspect_material);
                }
                if(property_target==PropertyTarget::mesh&&selected_mesh>=0&&selected_mesh<int(scene.meshes.size())){
                    ImGui::TextColored({1,.75f,.4f,1},"网格资源 · 共享几何数据");
                    auto& mesh=scene.meshes[selected_mesh];edit_name("资源名称",mesh.name,true);
                    ImGui::Text("顶点 %zu · 三角形 %zu",mesh.vertices.size(),mesh.indices.size()/3);
                    ImGui::Text("分部 %zu · 派生 LOD %zu",mesh.primitives.size(),mesh.lods.size());
                    ImGui::TextWrapped("这里不编辑位置或旋转。资源修改会影响所有引用它的节点。");
                    title("使用此资源的节点");
                    int inspect=-1;for(int n=0;n<int(scene.nodes.size());++n)if(scene.nodes[n].mesh==selected_mesh){ImGui::PushID(n);if(ImGui::Selectable(scene.nodes[n].name.c_str()))inspect=n;ImGui::PopID();}
                    ImGui::BeginDisabled(lod_chain_build.valid());if(ImGui::Button("生成资源 LOD 链"))build_selected_lods();ImGui::EndDisabled();
                    ImGui::TextDisabled("简化操作在渲染设置 → 几何与场景资源操作。");
                    if(inspect>=0)select_node(inspect);
                }
                if(property_target==PropertyTarget::texture&&selected_texture>=0&&selected_texture<int(scene.textures.size())){
                    auto& texture=scene.textures[selected_texture];
                    ImGui::TextColored({1,.75f,.4f,1},"纹理资源 · 共享图像与采样");edit_name("纹理名称",texture.name,true);
                    ImGui::Text("%s · %zu 个 Mip",texture.srgb?"sRGB 颜色数据":"线性数据",texture.levels.size());
                    if(!texture.levels.empty()&&!texture.levels.front().empty()){
                        const auto& image=texture.levels.front();ImGui::Text("尺寸 %d × %d",image.width,image.height);
                        // 用小网格显示实际图像通道，不为资源检查器引入 GPU 读回或纹理重建。
                        const float scale=std::min(std::min(192.f,ImGui::GetContentRegionAvail().x)/image.width,192.f/image.height);
                        const float width=image.width*scale,height=image.height*scale;
                        const auto origin=ImGui::GetCursorScreenPos();auto* draw=ImGui::GetWindowDrawList();
                        const int cols=std::min(48,image.width),rows=std::min(48,image.height);
                        for(int y=0;y<rows;++y)for(int x=0;x<cols;++x){auto c=image.at(x*image.width/cols,y*image.height/rows);
                            c=glm::clamp(c,glm::vec4(0),glm::vec4(1));draw->AddRectFilled({origin.x+x*width/cols,origin.y+y*height/rows},{origin.x+(x+1)*width/cols+.25f,origin.y+(y+1)*height/rows+.25f},ImGui::ColorConvertFloat4ToU32({srgb_to_linear(c.r),srgb_to_linear(c.g),srgb_to_linear(c.b),1}));
                        }ImGui::Dummy({width,height});ImGui::TextDisabled("原始 RGB 通道预览");
                    }
                    auto edited_s=texture.wrap_s,edited_t=texture.wrap_t;auto edited_mag=texture.mag_filter;bool changed=false;
                    int wrap_s=texture.wrap_s==Texture::Wrap::clamp_to_edge?1:texture.wrap_s==Texture::Wrap::mirrored_repeat?2:0;
                    int wrap_t=texture.wrap_t==Texture::Wrap::clamp_to_edge?1:texture.wrap_t==Texture::Wrap::mirrored_repeat?2:0;
                    const Texture::Wrap wraps[]={Texture::Wrap::repeat,Texture::Wrap::clamp_to_edge,Texture::Wrap::mirrored_repeat};
                    if(properties("Texture asset sampler")){
                        if(choice("U 寻址",wrap_s,"重复\0边缘钳制\0镜像重复\0")){edited_s=wraps[wrap_s];changed=true;}
                        if(choice("V 寻址",wrap_t,"重复\0边缘钳制\0镜像重复\0")){edited_t=wraps[wrap_t];changed=true;}
                        int mag=texture.mag_filter==Texture::Filter::nearest?0:1;
                        if(choice("放大过滤",mag,"最近邻\0线性\0")){edited_mag=mag?Texture::Filter::linear:Texture::Filter::nearest;changed=true;}
                        ImGui::EndTable();
                    }
                    if(changed){begin_edit("修改共享纹理采样");texture.wrap_s=edited_s;texture.wrap_t=edited_t;texture.mag_filter=edited_mag;++scene.revision;stamp_assets();mark_dirty();}
                    ImGui::TextWrapped("采样设置影响所有引用这张纹理的材质；纹理槽的绑定在材质资源页修改。");
                    for(int m=0;m<int(scene.materials.size());++m){const auto& material=scene.materials[m];bool used=false;
                        for(int r=0;r<5;++r)used|=texture_slot(material,TextureRole(r))==selected_texture;
                        if(used){ImGui::PushID(m);if(ImGui::Selectable(material.name.c_str()))select_resource(PropertyTarget::material,m);ImGui::PopID();}
                    }
                }
                if(property_target==PropertyTarget::material&&!scene.materials.empty()){
                    ImGui::TextColored({1,.75f,.4f,1},"材质资源 · 修改影响所有引用者");
                    selected_material=std::clamp(selected_material,0,int(scene.materials.size())-1);
                    edit_name("材质名称",scene.materials[selected_material].name,true);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    const auto preview=scene.materials[selected_material].name+"  "+std::to_string(selected_material+1);
                    if(ImGui::BeginCombo("##Select material",preview.c_str())){
                        for(int i=0;i<int(scene.materials.size());++i){ImGui::PushID(i);const auto label=scene.materials[i].name+"  "+std::to_string(i+1);if(ImGui::Selectable(label.c_str(),selected_material==i))selected_material=i;ImGui::PopID();}
                        ImGui::EndCombo();
                    }
                    Material edited=scene.materials[selected_material];bool changed=false;
                    if(section("表面",true)&&properties("Surface properties")){
                        changed|=color("基础颜色",&edited.base_color.x);
                        changed|=scalar("粗糙度",edited.roughness,.02f,1);changed|=scalar("金属度",edited.metallic,0,1);ImGui::EndTable();
                    }
                    if(section("扩展反射层",true)&&properties("Material lobes")){
                        changed|=scalar("清漆",edited.clearcoat,0,1);changed|=scalar("各向异性",edited.anisotropy,-.9f,.9f);changed|=scalar("织物 Sheen",edited.sheen,0,1);ImGui::EndTable();
                    }
                    if(section("自发光",true)&&properties("Emission properties")){changed|=color("颜色",&edited.emissive.x,ImGuiColorEditFlags_HDR|ImGuiColorEditFlags_Float);ImGui::EndTable();}
                    if(changed){
                        // 一个连续拖动是一条撤销记录，而不是每一帧塞进一条记录。
                        begin_edit("材质参数调整");
                        scene.materials[selected_material]=std::move(edited);++scene.revision;stamp_assets();mark_dirty();
                    }
                    if(section("纹理贴图",true)){
                        for(int role_index=0;role_index<5;++role_index){
                            const auto role=TextureRole(role_index);ImGui::PushID(role_index);
                            const int slot=texture_slot(scene.materials[selected_material],role);
                            ImGui::TextUnformatted(texture_role_name(role));
                            const bool srgb=role==TextureRole::base_color||role==TextureRole::emissive;
                            const char* preview=slot>=0&&slot<int(scene.textures.size())?scene.textures[slot].name.c_str():"无贴图";
                            ImGui::SetNextItemWidth(std::max(70.f,ImGui::GetContentRegionAvail().x-58));
                            if(ImGui::BeginCombo("##Texture slot",preview)){
                                if(ImGui::Selectable("无贴图",slot<0)){try{editor_change("移除贴图",[&](){texture_slot(scene.materials[selected_material],role)=-1;++scene.revision;});}catch(const std::exception& e){error=e.what();}}
                                for(int t=0;t<int(scene.textures.size());++t){
                                    if(scene.textures[t].srgb!=srgb)continue;
                                    ImGui::PushID(t);if(ImGui::Selectable(scene.textures[t].name.c_str(),slot==t)){
                                        try{editor_change("绑定现有贴图",[&](){texture_slot(scene.materials[selected_material],role)=t;++scene.revision;});}catch(const std::exception& e){error=e.what();}
                                    }ImGui::PopID();
                                }ImGui::EndCombo();
                            }
                            ImGui::SameLine();ImGui::BeginDisabled(imported_texture.valid());if(ImGui::Button("导入…"))browse(FileKind::texture,role);ImGui::EndDisabled();
                            if(role==TextureRole::metallic_roughness)ImGui::TextDisabled("G = 粗糙度，B = 金属度；还会乘上方系数。");
                            ImGui::PopID();
                        }
                        if(properties("Texture controls")){
                            auto updated=scene.materials[selected_material];bool change=false;
                            change|=scalar("法线强度",updated.normal_scale,0,3);change|=scalar("AO 强度",updated.ao_strength,0,1);ImGui::EndTable();
                            if(change){begin_edit("贴图强度调整");scene.materials[selected_material]=updated;++scene.revision;stamp_assets();mark_dirty();}
                        }
                        ImGui::TextDisabled("颜色贴图 × 基础颜色；导入不会重置系数。");
                    }
                }
                if(ImGui::Button("撤销 Ctrl+Z"))history_step(false);ImGui::SameLine();
                if(ImGui::Button("重做 Ctrl+Y"))history_step(true);
                ImGui::Separator();
                if(property_target==PropertyTarget::light)for(std::size_t i=0;i<scene.lights.size();++i){
                    if(selected_light>=0&&int(i)!=selected_light)continue;
                    if(selected_light<0&&i>=8){ImGui::TextDisabled("更多光源请在场景树中选择");break;}
                    ImGui::PushID(int(i));
                    if(ImGui::TreeNodeEx("light",selected_light==int(i)?ImGuiTreeNodeFlags_DefaultOpen:0,"光源 %d",int(i+1))){
                        auto l=scene.lights[i];bool changed=false;
                        if(properties("Light properties")){
                            changed|=scalar("强度",l.intensity,0,30);changed|=color("颜色",&l.color.x);
                            if(l.kind!=LightKind::directional)changed|=vector("位置",&l.position.x,.05f);
                            if(l.kind!=LightKind::point)changed|=vector("方向",&l.direction.x,.02f,-1,1);
                            if(l.kind==LightKind::rectangle){changed|=scalar("宽度",l.size.x,.05f,8);changed|=scalar("高度",l.size.y,.05f,8);}
                            if(l.kind==LightKind::point)changed|=scalar("作用范围",l.range,.1f,100);
                            ImGui::EndTable();
                        }
                        if(l.kind==LightKind::rectangle){
                            const char* name=l.linked_node>=0&&l.linked_node<int(scene.nodes.size())?scene.nodes[l.linked_node].name.c_str():"不关联灯板";
                            if(ImGui::BeginCombo("灯板节点",name)){
                                if(ImGui::Selectable("不关联灯板",l.linked_node<0)){l.linked_node=-1;changed=true;}
                                for(int n=0;n<int(scene.nodes.size());++n){
                                    if(scene.nodes[n].mesh<0)continue;
                                    auto candidate=l;candidate.linked_node=n;bool valid=true;
                                    try{validate_light_binding(scene,i,candidate);}catch(...){valid=false;}
                                    if(!valid)continue;
                                    ImGui::PushID(n);if(ImGui::Selectable(scene.nodes[n].name.c_str(),l.linked_node==n)){l.linked_node=n;changed=true;}ImGui::PopID();
                                }ImGui::EndCombo();
                            }
                            help("旧工程可选择原灯板恢复联动；这里只列出兼容的平面发光网格，关联时按光源位置对齐。");
                        }
                        if(changed){try{begin_edit("光源参数调整");update_editor_light(scene,i,l);mark_dirty();}catch(const std::exception& e){error=e.what();}}
                        if(l.kind==LightKind::directional)ImGui::TextDisabled("方向光改变方向；位置不影响照明。");
                        else ImGui::TextDisabled("视口 XYZ 箭头可拖动位置。");
                        if(l.kind==LightKind::point)ImGui::TextDisabled("点光源可照明；当前 GPU 未实现其阴影。");
                        if(l.linked_node>=0)ImGui::TextDisabled("灯板位置、朝向、尺寸与发光同步。");
                        ImGui::TreePop();
                    }ImGui::PopID();
                }
                if(property_target==PropertyTarget::light)ImGui::TextDisabled("光源属性属于场景，不是共享材质资源。");
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("演示场景",nullptr,panel_request==2?ImGuiTabItemFlags_SetSelected:0)){
                ImGui::TextWrapped("五类可观察、可切换 A/B 的 GPU 场景。算法仍在渲染设置里，不需要逐个进入专题。");
                ImGui::SetNextItemWidth(-FLT_MIN);
                if(ImGui::BeginCombo("##Demo scenes",demo_scenes[selected_demo].name)){
                    for(int i=0;i<int(demo_scenes.size());++i)if(ImGui::Selectable(demo_scenes[i].name,selected_demo==i))selected_demo=i;
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("观察目标");ImGui::TextWrapped("%s",demo_scenes[selected_demo].observe);
                ImGui::BeginDisabled(imported.valid()||imported_texture.valid());
                if(ImGui::Button("打开这个演示场景",{-FLT_MIN,0}))open_demo(demo_scenes[selected_demo].profile);
                ImGui::EndDisabled();
                if(topic_return){
                    ImGui::Separator();
                    if(active_demo>=0)ImGui::TextColored({.55f,.75f,.95f,1},"当前演示 · %s",demo_scenes[active_demo].name);
                    else ImGui::TextDisabled("当前处于开发者参考预览。");
                    if(ImGui::Button("返回原场景 · 恢复进入前状态",{-FLT_MIN,0})){try{return_from_topic();}catch(const std::exception& e){error=e.what();}}
                    ImGui::TextWrapped("返回会丢弃预览期间的修改；原工程文件与进入前的撤销历史保持不变。");
                }
                if(active_demo>=0){
                    const auto& demo=demo_scenes[active_demo];ImGui::Separator();ImGui::TextDisabled("对比当前演示");
                    if(ImGui::Button(demo.first,{-FLT_MIN,0}))compare_demo(false);
                    if(ImGui::Button(demo.second,{-FLT_MIN,0}))compare_demo(true);
                    if(active_demo==0){
                        if(ImGui::Button("打开同类白炉能量对照",{-FLT_MIN,0}))open_demo("kc");
                        ImGui::TextWrapped("白炉没有直接光与 Bloom；观察同一白色粗糙金属在开关 KC 后的亮度。日常材质仍可在材质页编辑。");
                    }
                    if(active_demo==2){
                        if(ImGui::Button("显示每组候选灯数",{-FLT_MIN,0})){settings.debug=DebugView::light_count;mark_dirty();}
                        if(ImGui::Button("恢复最终颜色",{-FLT_MIN,0})){settings.debug=DebugView::final_color;mark_dirty();}
                    }
                    if(active_demo==4){
                        ImGui::BeginDisabled(lod_chain_build.valid());
                        if(ImGui::Button("生成示例网格 LOD 链",{-FLT_MIN,0})){selected_mesh=0;build_selected_lods();}
                        ImGui::EndDisabled();
                        ImGui::TextWrapped("%s",scene_geometry.status().c_str());
                    }
                    if(ImGui::Button("前往渲染设置",{-FLT_MIN,0}))requested_panel=0;
                }
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("工程",nullptr,panel_request==3?ImGuiTabItemFlags_SetSelected:0)){
                if(ImGui::Button("新建场景…"))open_new_scene_popup=true;ImGui::SameLine();if(ImGui::Button("另存为…"))save_as();
                if(project_path.empty())ImGui::TextDisabled("新文档尚未保存；Ctrl+S 会询问独立工程路径。");
                ImGui::TextDisabled("工程路径 (.ember)");ImGui::SetNextItemWidth(-FLT_MIN);ImGui::InputText("##Project path",path_text.data(),path_text.size());help("工程保存场景、材质、纹理、光源和参数；与模型导入路径分开。");
                if(ImGui::Button("保存 Ctrl+S"))save();ImGui::SameLine();if(ImGui::Button("读取项目"))load();
                ImGui::Separator();ImGui::TextDisabled("模型导入路径");ImGui::SetNextItemWidth(-FLT_MIN);ImGui::InputTextWithHint("##Model path","GLB / glTF / OBJ",model_path_text.data(),model_path_text.size());
                ImGui::BeginDisabled(imported.valid());
                if(ImGui::Button("浏览模型…"))browse(FileKind::model);ImGui::SameLine();
                if(ImGui::Button("导入路径"))request_model(path_from_utf8(model_path_text.data()),replace_import);
                ImGui::Checkbox("替换场景（默认追加）",&replace_import);ImGui::EndDisabled();
                ImGui::TextDisabled("GLB / glTF / OBJ；FBX 请先转换为 GLB。");
                if(imported.valid())ImGui::TextDisabled("模型后台解析中，原场景继续显示…");
                if(section("独立纹理导入")){
                    ImGui::SetNextItemWidth(-FLT_MIN);ImGui::InputTextWithHint("##Texture path","PNG / JPG / TGA / BMP 路径",texture_path_text.data(),texture_path_text.size());
                    ImGui::BeginDisabled(imported_texture.valid()||scene.materials.empty());
                    if(ImGui::Button("浏览纹理…"))browse(FileKind::texture);
                    ImGui::SameLine();if(ImGui::Button("导入图片路径"))request_texture(path_from_utf8(texture_path_text.data()),texture_role);ImGui::EndDisabled();
                }
                if(section("免费示例资源")){
                    const auto sample_root=asset_root()/"assets/import_samples";
                    ImGui::TextWrapped("外部免费资源（CC0 / CC BY），可追加到任何场景；不会自动运行验证或切换场景。");
                    ImGui::BeginDisabled(imported.valid());
                    for(const auto& sample:editor::import_samples){
                        const auto model=sample_root/"models"/sample.file/(std::string(sample.file)+".glb");
                        ImGui::PushID(sample.file);ImGui::BeginDisabled(!std::filesystem::exists(model));
                        if(ImGui::Button(sample.name,{-FLT_MIN,0}))request_model(model,false);
                        ImGui::EndDisabled();ImGui::PopID();
                    }ImGui::EndDisabled();
                    const auto tiles=sample_root/"textures/Tiles074";
                    ImGui::BeginDisabled(imported_texture.valid()||scene.materials.empty());
                    ImGui::BeginDisabled(!std::filesystem::exists(tiles/"Tiles074_1K-JPG_Color.jpg"));
                    if(ImGui::Button("绑定地砖颜色…"))request_texture(tiles/"Tiles074_1K-JPG_Color.jpg",TextureRole::base_color);
                    ImGui::EndDisabled();
                    ImGui::BeginDisabled(!std::filesystem::exists(tiles/"Tiles074_1K-JPG_NormalGL.jpg"));
                    if(ImGui::Button("绑定地砖法线…"))request_texture(tiles/"Tiles074_1K-JPG_NormalGL.jpg",TextureRole::normal);
                    ImGui::EndDisabled();ImGui::EndDisabled();
                    ImGui::TextWrapped("选择目标材质后绑定贴图。示例工程已将灰度粗糙度正确打包到 G；独立灰度图不能直接作为金属/粗糙度合成图。");
                    ImGui::TextWrapped("资源缺失时运行 scripts/download-import-samples.ps1；来源与许可见 assets/import_samples/README.md。");
                }
                int upload_mib=int(gpu.stats().uploadBudget/(1024*1024));
                if(section("资源上传",true)&&properties("Upload properties")){if(integer("预算 MiB/帧",upload_mib,1,12))gpu.set_upload_budget(std::size_t(upload_mib)*1024*1024);ImGui::EndTable();}
                ImGui::Separator();ImGui::TextDisabled("场景资源");ImGui::Text("网格 %d · 材质 %d · 纹理 %d",int(scene.meshes.size()),int(scene.materials.size()),int(scene.textures.size()));
                ImGui::TextWrapped("输出：%s",path_to_utf8(output_dir).c_str());
                ImGui::TextWrapped("CPU 对照、Shader 工具与验证位于默认隐藏的开发者工具；视图菜单可手动开启。");
                ImGui::EndTabItem();
            }
            if(show_developer_tools&&ImGui::BeginTabItem("开发者工具",nullptr,panel_request==4?ImGuiTabItemFlags_SetSelected:0)){
                ImGui::TextWrapped("仅用于离线参考、数值验证和开发诊断。开启本页不启动任何任务，所有计算都需手动点击。");
                if(ImGui::Button("关闭开发者工具",{-FLT_MIN,0})){try{set_developer_tools(false);}catch(const std::exception& e){error=e.what();}}
                if(topic_return){
                    if(ImGui::Button("结束预览并返回原场景",{-FLT_MIN,0})){try{return_from_topic();}catch(const std::exception& e){error=e.what();}}
                    ImGui::TextWrapped("返回会丢弃预览修改并恢复原场景、相机、设置和撤销历史。");
                }
                if(section("独立 CPU 对照",true)){
                    constexpr int cpu_topics[]{9,10,11,12,23,24,29};
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if(ImGui::BeginCombo("##CPU references",topics[cpu_topics[selected_cpu_reference]].name)){
                        for(int i=0;i<7;++i)if(ImGui::Selectable(topics[cpu_topics[i]].name,i==selected_cpu_reference))selected_cpu_reference=i;
                        ImGui::EndCombo();
                    }
                    const int reference_topic=cpu_topics[selected_cpu_reference];
                    ImGui::TextWrapped("%s",topics[reference_topic].why);
                    ImGui::TextWrapped("非实时 CPU 计算，默认最多 640 × 480。独立对照只在变化后重算；HW5/SVGF 会有限累积历史。");
                    ImGui::BeginDisabled(imported.valid()||imported_texture.valid());
                    if(ImGui::Button("手动运行这个 CPU 对照",{-FLT_MIN,0})){
                        try{open_topic(reference_topic,TopicEntry::cpu_comparison);}
                        catch(const std::exception& e){const std::string reason=e.what();if(topic_return){try{return_from_topic();}catch(...){}}error=reason;}
                    }
                    ImGui::EndDisabled();
                    if(visual_topic==10||visual_topic==11){
                        if(ImGui::Checkbox("用当前预览场景替换轻量示例",&experiment_current_scene))mark_dirty();
                        if(experiment_current_scene)ImGui::TextWrapped("PRT 静态可见性烘焙可能很慢；普通 GPU 编辑不需要进入本对照。");
                    }
                    if(bg.busy())ImGui::TextDisabled("参考计算中，保留上一张完成的画面…");
                    else if(reference_snapshot)ImGui::TextDisabled("分帧准备参考场景…");
                    else if(reference_generation==generation)ImGui::TextDisabled("当前参考已完成 · %.1f ms",reference.cpu_ms);
                    if(ImGui::Button("输出线性 HDR 参考",{-FLT_MIN,0})){
                        try{if(reference.color.empty())throw std::runtime_error("请先手动运行 CPU 参考。");save_pfm(output_dir/"reference.pfm",reference.color);save_bmp(output_dir/"reference.bmp",reference.color,settings.exposure);notice="参考图已写入 output。";}
                        catch(const std::exception& e){error=e.what();}
                    }
                }
                if(section("算法验证（仅手动运行）",false)){
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if(ImGui::BeginCombo("##Validation topic",topics[selected_topic].name)){
                        for(int i=0;i<32;++i)if(ImGui::Selectable(topics[i].name,selected_topic==i))selected_topic=i;
                        ImGui::EndCombo();
                    }
                    ImGui::TextWrapped("%s",topics[selected_topic].why);
                    ImGui::BeginDisabled(checks.valid());
                    if(ImGui::Button("手动检查当前专题",{-FLT_MIN,0})){const int group=topics[selected_topic].group;checks=std::async(std::launch::async,[group]{return run_checks(group);});}
                    if(ImGui::Button("手动检查全部算法",{-FLT_MIN,0}))checks=std::async(std::launch::async,[]{return run_checks(10);});
                    ImGui::EndDisabled();
                    if(checks.valid())ImGui::TextColored({1,.8f,.4f,1},"检查运行中…");
                    int passed=0;for(const auto& t:test_results)passed+=t.passed;
                    ImGui::Text("检查结果 %d / %d",passed,int(test_results.size()));
                    ImGui::BeginChild("Manual validation results",{0,160},true);
                    for(const auto& t:test_results){
                        ImGui::TextColored(t.passed?ImVec4(.45f,.85f,.65f,1):ImVec4(1,.45f,.4f,1),"%s %s",t.passed?"PASS":"FAIL",t.name.c_str());
                        if(!t.passed)ImGui::TextWrapped("%s",t.detail.c_str());
                    }ImGui::EndChild();
                }
                if(section("Shader 开发",false)){
                    ImGui::BeginDisabled(shader_build.valid());
                    if(ImGui::Button("重新编译并热重载 Shader",{-FLT_MIN,0}))reload_shaders();
                    if(ImGui::Button("回退上一版 Shader",{-FLT_MIN,0})){
                        try{
                            auto previous=shader_library?shader_library->previous():nullptr;
                            if(!previous)throw std::runtime_error("还没有可回退的 Shader 版本。");
                            std::string reason;if(!gpu.reload_pipelines(previous->directory,&reason))throw std::runtime_error(reason);
                            shader_library->activate(previous);notice="已回退上一版 Shader。";
                        }catch(const std::exception& e){error=e.what();}
                    }ImGui::EndDisabled();
                    if(shader_build.valid())ImGui::TextDisabled("编译中，当前管线继续运行…");
                }
                if(section("渲染诊断与基准（仅手动运行）",false)){
                ImGui::Text("资源 %.1f MiB · 图 %zu pass / %zu barrier",stats.allocated_bytes/1048576.,stats.graph_passes,stats.graph_barriers);
                if(ImGui::Button("导出真实渲染图 JSON / DOT")){
                    try{const auto& graph_stats=gpu.stats();save_render_graph_snapshot(gpu.render_graph(),output_dir/"graph",{graph_stats.graph_serial,graph_stats.graph_revision,"Vulkan",graph_stats.graph_submitted?"submitted":"recorded"});notice="依赖图、资源生命周期和屏障已保存到 output/graph。";}catch(const std::exception& e){error=e.what();}
                }
                ImGui::BeginDisabled(system_benchmark.valid());
                if(ImGui::Button("运行任务系统基准")){auto folder=output_dir/"benchmark";system_benchmark=std::async(std::launch::async,[folder]{return run_system_benchmark(folder);});notice="后台测试串行与 1/2/4/8 线程；结束后可查看报告。";}
                ImGui::EndDisabled();
                if(ImGui::CollapsingHeader("本帧执行的 Pass")){for(const auto& row:render_graph_pass_list(gpu.render_graph()))ImGui::TextWrapped("%s",row.c_str());for(const auto& name:gpu.stats().effect_passes)ImGui::TextDisabled("  %s",name.c_str());}
                if(ImGui::CollapsingHeader("资源交接与屏障")){for(const auto& row:render_graph_barrier_list(gpu.render_graph()))ImGui::TextWrapped("%s",row.c_str());}
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
        }
        // 拖动细分隔线调整面板宽度；不改变渲染参数，不打断材质的撤销事务。
        auto splitter=[&](const char* id,float x,float& value,float direction,float lo,float hi){
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});panel(id,{x-2,shell.menu},{4,shell.body_height()},ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoNav);
            ImGui::InvisibleButton("drag",{4,std::max(1.f,shell.body_height())});if(ImGui::IsItemHovered()||ImGui::IsItemActive())ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if(ImGui::IsItemActive())value=std::clamp(value+ImGui::GetIO().MouseDelta.x*direction,lo,hi);ImGui::End();ImGui::PopStyleVar();
        };
        if(shell.left>0)splitter("Scene splitter",shell.left,scene_width,1,190,280);
        if(shell.right>0)splitter("Inspector splitter",shell.width-shell.right,inspector_width,-1,300,420);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{8,3});
        panel("Editor status",{0,shell.height-shell.status},{shell.width,shell.status},ImGuiWindowFlags_NoScrollbar);
        const char* state=!error.empty()?"操作未完成":ui_gpu_blocked?"GPU 配置不支持 · 预览暂停":!stats.scene_resources_error.empty()?"烘焙未完成":!stats.uploadError.empty()?"资源未替换":imported.valid()?"模型解析中":imported_texture.valid()?"纹理读取中":stats.uploadPending?"资源准备中":(scene_bake.valid()||stats.scene_resources_pending)?"场景烘焙中":lod_chain_build.valid()?"LOD 生成中":bg.busy()?"参考计算中":!stats.environment_ready?"IBL 准备中":"就绪";
        if(ImGui::SmallButton(state))ImGui::OpenPopup("Status details");
        help(!error.empty()?error.c_str():!stats.uploadError.empty()?stats.uploadError.c_str():notice.c_str());
        if(ImGui::BeginPopup("Status details")){
            ImGui::PushTextWrapPos(ImGui::GetFontSize()*36);ImGui::TextWrapped("%s",notice.c_str());
            if(ui_gpu_blocked){ImGui::Separator();for(const auto& why:ui_gpu_limitations)ImGui::TextWrapped("%s",gpu_issue_text(why).c_str());ImGui::TextWrapped("未启动 CPU 回退；请修改渲染设置。");}
            if(!error.empty()){ImGui::Separator();ImGui::TextWrapped("%s",error.c_str());if(ImGui::Button("关闭错误提示")){error.clear();ImGui::CloseCurrentPopup();}}
            if(!stats.uploadError.empty())ImGui::TextWrapped("%s",stats.uploadError.c_str());
            if(!stats.scene_resources_error.empty())ImGui::TextWrapped("%s",stats.scene_resources_error.c_str());
            if(stats.uploadPending){ImGui::Text("%s",stats.uploadStage.c_str());ImGui::ProgressBar(stats.total?float(double(stats.uploaded)/stats.total):0,{-1,0});}
            ImGui::PopTextWrapPos();ImGui::EndPopup();
        }
        if(shell.width>1050){ImGui::SameLine();ImGui::TextDisabled("%d x %d%s",settings.render_width,settings.render_height,resolution_limited?" (受限)":"");
            if(ImGui::IsItemHovered()){ImGui::BeginTooltip();ImGui::Text("物理视口 %d x %d",viewport_pixel_width,viewport_pixel_height);ImGui::Text("渲染比例 %.2fx  |  DPI %.0f%%",render_scale,stats.display_scale*100);ImGui::TextUnformatted("适应视口 + 1.00x：按原生像素渲染；鼠标操作见帮助菜单。");ImGui::EndTooltip();}}
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(),shell.width-490));
        if(stats.gpu_ms>=0)ImGui::TextDisabled("GPU %.2f ms",stats.gpu_ms);else ImGui::TextDisabled("GPU -- ms");
        help(stats.gpu_name.c_str());ImGui::SameLine();ImGui::TextDisabled("| %llu Draw | %llu tris | %.0f MiB",(unsigned long long)stats.draw_calls,(unsigned long long)stats.triangles,stats.allocated_bytes/1048576.);
        if(ImGui::IsItemHovered()){
            ImGui::BeginTooltip();ImGui::Text("可见 %llu / %llu · Pass %zu · Barrier %zu",(unsigned long long)stats.visible_objects,(unsigned long long)stats.candidate_objects,stats.graph_passes,stats.graph_barriers);
            ImGui::Text("CPU 参考 %.1f ms · %llu rays",reference.cpu_ms,(unsigned long long)reference.rays);
            if(settings.taa||settings.denoise||settings.svgf){ImGui::Text("GPU 历史 %s · %zu 次计算",stats.temporal_history_valid?"有效":"重建",stats.effect_dispatches);ImGui::Text("CPU 累计 %llu 帧 · 复用 %.0f%%",(unsigned long long)temporal_info.frames,temporal_info.reused_fraction*100);}ImGui::EndTooltip();
        }
        ImGui::End();ImGui::PopStyleVar();
        if(open_new_scene_popup){ImGui::OpenPopup("新建独立场景");open_new_scene_popup=false;}
        if(ImGui::BeginPopupModal("新建独立场景",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextUnformatted("当前未保存的修改将离开；原工程文件不会被覆盖。请先保存需要保留的内容。");
            ImGui::InputText("场景名称",new_scene_name.data(),new_scene_name.size());
            ImGui::Checkbox("添加地板（取消则为空场景）",&new_scene_floor);
            ImGui::Checkbox("添加一盏初始方向光",&new_scene_light);
            ImGui::TextUnformatted("新场景不会沿用旧工程路径，也不会启动实验或 CPU 参考。");
            if(ImGui::Button("先保存当前工程"))save();ImGui::SameLine();
            if(ImGui::Button("创建新场景（放弃未保存修改）")){
                try{new_document();ImGui::CloseCurrentPopup();}catch(const std::exception& e){error=e.what();}
            }
            ImGui::SameLine();if(ImGui::Button("取消"))ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
        if(open_replace_popup){ImGui::OpenPopup("替换场景？");open_replace_popup=false;}
        if(ImGui::BeginPopupModal("替换场景？",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextUnformatted("当前节点、材质和光源会被替换。该操作可撤销，但建议先保存工程。");
            ImGui::TextWrapped("%s",path_to_utf8(replace_request).c_str());
            ImGui::BeginDisabled(imported.valid());
            if(ImGui::Button("确认替换")){import_asset(replace_request,true);ImGui::CloseCurrentPopup();}ImGui::EndDisabled();
            ImGui::SameLine();if(ImGui::Button("取消"))ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
        if(open_texture_popup){ImGui::OpenPopup("导入纹理到材质");open_texture_popup=false;}
        if(ImGui::BeginPopupModal("导入纹理到材质",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::PushTextWrapPos(ImGui::GetFontSize()*35);ImGui::TextWrapped("%s",path_to_utf8(texture_request).c_str());ImGui::PopTextWrapPos();
            if(!scene.materials.empty()){
                texture_material=std::clamp(texture_material,0,int(scene.materials.size())-1);
                if(ImGui::BeginCombo("目标材质",scene.materials[texture_material].name.c_str())){
                    for(int i=0;i<int(scene.materials.size());++i){ImGui::PushID(i);if(ImGui::Selectable(scene.materials[i].name.c_str(),i==texture_material))texture_material=i;ImGui::PopID();}ImGui::EndCombo();
                }
                int role=int(texture_role);if(ImGui::Combo("用途",&role,"基础颜色\0金属 / 粗糙度\0法线\0环境遮蔽 AO\0自发光\0"))texture_role=TextureRole(role);
                ImGui::TextDisabled(texture_role==TextureRole::base_color||texture_role==TextureRole::emissive?"颜色贴图：sRGB 解码后参与计算。":"数据贴图：线性读取，不进行 sRGB 解码。");
                ImGui::TextDisabled("绑定到共享材质；使用该材质的对象都会更新。");
                ImGui::BeginDisabled(imported_texture.valid());
                if(ImGui::Button("导入并绑定")){
                    const auto path=texture_request;const auto role=texture_role;
                    texture_epoch=document_epoch;imported_texture_material=texture_material;imported_texture_role=role;
                    imported_texture=std::async(std::launch::async,[path,role](){return load_editor_texture(path,role);});
                    notice="正在后台读取纹理："+path_to_utf8(path);error.clear();ImGui::CloseCurrentPopup();
                }ImGui::EndDisabled();
            }else ImGui::TextUnformatted("场景没有材质，请先导入模型。");
            ImGui::SameLine();if(ImGui::Button("取消"))ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
        // 绘制在实际图像矩形内；弹窗优先，工具不遮挡菜单和属性检查器。
        if(show_move_tool&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)){
            try{if(const auto origin=selected_position()){
                const auto rect=viewport_rect();const auto center=editor::project_handle(*origin,camera,rect,settings.reversed_z);
                if(center){auto* draw=ImGui::GetForegroundDrawList();draw->PushClipRect({rect.x,rect.y},{rect.x+rect.width,rect.y+rect.height},true);
                    const float length=handle_length(*origin,rect);const glm::vec3 colors[]={{.95f,.28f,.24f},{.35f,.85f,.38f},{.32f,.58f,1}};
                    const auto tool=selected_light>=0?TransformTool::translate:transform_tool;
                    const auto world=selected_node>=0?scene_world_transforms(scene).at(selected_node):glm::mat4(1);
                    for(int a=0;a<3;++a){
                        const auto c=colors[a];const auto ink=ImGui::ColorConvertFloat4ToU32({srgb_to_linear(c.x),srgb_to_linear(c.y),srgb_to_linear(c.z),1});
                        const float thickness=translation_drag.active&&translation_drag.axis==a?4.f:2.f;
                        if(tool==TransformTool::rotate){
                            for(int segment=0;segment<64;++segment){
                                const auto p=project_handle(ring_point(*origin,a,length,2*pi*segment/64),camera,rect,settings.reversed_z);
                                const auto q=project_handle(ring_point(*origin,a,length,2*pi*(segment+1)/64),camera,rect,settings.reversed_z);
                                if(p&&q)draw->AddLine({p->x,p->y},{q->x,q->y},ink,thickness);
                            }
                            if(const auto label_position=project_handle(ring_point(*origin,a,length,0),camera,rect,settings.reversed_z)){
                                const char label[2]={char('X'+a),0};draw->AddText({label_position->x+5,label_position->y+3},ink,label);
                            }continue;
                        }
                        const auto axis=tool==TransformTool::scale?safe_normalize(glm::vec3(world[a]),world_axis(a)):world_axis(a);
                        const auto end=project_handle(*origin+axis*length,camera,rect,settings.reversed_z);
                        if(!end||glm::length(*end-*center)<12)continue;
                        const auto dir=glm::normalize(*end-*center),side=glm::vec2(-dir.y,dir.x);
                        draw->AddLine({center->x,center->y},{end->x,end->y},ink,thickness);
                        if(tool==TransformTool::scale)draw->AddRectFilled({end->x-5,end->y-5},{end->x+5,end->y+5},ink);
                        else{const auto b=*end-dir*9.f;draw->AddTriangleFilled({end->x,end->y},{b.x+side.x*4,b.y+side.y*4},{b.x-side.x*4,b.y-side.y*4},ink);}
                        const char label[2]={char('X'+a),0};draw->AddText({end->x+5,end->y+3},ink,label);
                    }
                    if(tool==TransformTool::scale)draw->AddRectFilled({center->x-5,center->y-5},{center->x+5,center->y+5},ImGui::GetColorU32(ImGuiCol_Text));
                    else draw->AddCircleFilled({center->x,center->y},3,ImGui::GetColorU32(ImGuiCol_Text));draw->PopClipRect();
                }
            }}catch(const std::exception& e){error=e.what();}
        }
        if(show_profiler){
            ImGui::SetNextWindowSize({680,510},ImGuiCond_FirstUseEver);
            if(ImGui::Begin("性能分析",&show_profiler,ImGuiWindowFlags_NoCollapse)){
                const auto& profile=gpu.stats().completed_profile;
                if(!profile.completed)ImGui::TextUnformatted("等待第一份 GPU 已完成帧分析…");
                else{
                    ImGui::Text("已完成提交 #%llu · %d x %d",(unsigned long long)profile.serial,profile.width,profile.height);
                    ImGui::Text("CPU %.3f ms · 资源 %.1f MiB",profile.cpu_total_ms,profile.allocated_bytes/1048576.);
                    ImGui::SameLine();if(profile.gpu_ms>=0)ImGui::Text("· GPU %.3f ms",profile.gpu_ms);else ImGui::TextDisabled("· GPU 计时不可用");
                    ImGui::Text("Fence 等待 %.3f · UI %.3f · 准备 %.3f · Acquire %.3f ms",profile.cpu_wait_ms,profile.cpu_ui_ms,profile.cpu_prepare_ms,profile.cpu_acquire_ms);
                    ImGui::Text("几何准备 %.3f · 命令录制 %.3f · 提交/呈现 %.3f ms",profile.cpu_geometry_ms,profile.cpu_record_ms,profile.cpu_submit_ms);
                    ImGui::Text("本次场景上传 %.1f KiB · 包围球 %s",profile.upload_bytes/1024.,profile.bounds_cached?"缓存":"逐帧计算");
                    ImGui::TextWrapped("GPU 行是包含子步骤的区间，嵌套行不能相加；CPU 录制时间不是 GPU 执行时间。整帧 CPU 包含等待，不等于算法计算成本。");
                    if(ImGui::Button("导出这份已完成帧 JSON")){try{save_frame_profile(profile,output_dir/"profiles"/("frame-"+std::to_string(profile.serial)+".json"));notice="已完成帧分析已保存到 output/profiles。";}catch(const std::exception& e){error=e.what();}}
                    if(ImGui::BeginTable("Pass profile",3,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_ScrollY,{0,300})){
                        ImGui::TableSetupColumn("实际 Pass");ImGui::TableSetupColumn("CPU 录制 / ms");ImGui::TableSetupColumn("GPU / ms");ImGui::TableHeadersRow();
                        for(const auto& pass:profile.passes){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%*s%s",int(pass.depth*2),"",pass.name.c_str());ImGui::TableNextColumn();ImGui::Text("%.4f",pass.cpu_record_ms);ImGui::TableNextColumn();if(pass.gpu_ms>=0)ImGui::Text("%.4f",pass.gpu_ms);else ImGui::TextUnformatted("不可用");}
                        ImGui::EndTable();
                    }
                }
            }ImGui::End();
        }
        if(edit_before&&!translation_drag.active&&!object_rotation_active&&!ImGui::IsAnyItemActive()){try{finish_edit();}catch(const std::exception& e){error=e.what();}}
    };
    bool editor_verified=false,editor_pose_started=false;int editor_ready_frames=0,editor_pose_frames=0,capture_ready_frames=0;
    auto verify_editor_actions=[&](){
        if(scene.lights.empty()||scene.lights[0].kind!=LightKind::rectangle||scene.lights[0].linked_node<0)
            throw std::runtime_error("--verify-editor requires the colored room (--preset 1)");
        auto require=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
        select_light(0);const auto before=scene.lights[0].position;
        const auto rect=viewport_rect();const float length=handle_length(before,rect);
        const auto grab=editor::project_handle(before+glm::vec3(length*.7f,0,0),camera,rect,settings.reversed_z);
        require(grab&&start_translation(*grab),"Editor XYZ arrow hit test failed");
        const auto destination=editor::project_handle(before+glm::vec3(length*.7f+.4f,0,0),camera,rect,settings.reversed_z);
        require(bool(destination),"Editor move destination was not projectable");move_translation(*destination,false);translation_drag.active=false;finish_edit();
        require(std::abs(scene.lights[0].position.x-before.x-.4f)<.0002f,"Editor drag did not move the light");
        const auto moved=scene.lights[0].position;
        require(glm::length(glm::vec3(scene_world_transforms(scene).at(scene.lights[0].linked_node)[3])-moved)<.0002f,"Emitter did not follow the light");
        require(undo.undo()&&glm::length(scene.lights[0].position-before)<.0002f,"Light drag undo failed");stamp_assets();
        require(undo.redo()&&glm::length(scene.lights[0].position-moved)<.0002f,"Light drag redo failed");stamp_assets();
        std::cout<<"PASS editor XYZ ray hit/drag, emitter follow, undo/redo\n";
        const auto nodes=scene.nodes.size(),materials=scene.materials.size();
        auto imported_model=load_editor_model(asset_root()/"assets/software_renderer/textured_cube.obj");
        editor_change("验证 OBJ 追加",[&](){append_scene_asset(scene,std::move(imported_model.scene));});
        require(scene.nodes.size()>nodes&&scene.materials.size()>materials,"OBJ append did not preserve room and add assets");
        const auto appended_nodes=scene.nodes.size();require(undo.undo()&&scene.nodes.size()==nodes,"Append undo failed");stamp_assets();
        require(undo.redo()&&scene.nodes.size()==appended_nodes,"Append redo failed");stamp_assets();
        editor_change("验证节点平移",[&](){auto& node=scene.nodes.at(nodes);node.local=glm::translate(glm::mat4(1),glm::vec3(-.7f,.6f,1))*glm::scale(glm::mat4(1),glm::vec3(.3f));++scene.revision;});
        const auto texture_count=scene.textures.size();auto texture=load_editor_texture(asset_root()/"assets/branding/emberframe-icon.png",TextureRole::base_color);
        editor_change("验证颜色纹理绑定",[&](){bind_material_texture(scene,materials,std::move(texture),TextureRole::base_color);});
        require(scene.materials.at(materials).base_texture>=int(texture_count),"Imported texture was not bound");
        require(undo.undo()&&scene.textures.size()==texture_count,"Texture undo failed");stamp_assets();require(undo.redo()&&scene.textures.size()==texture_count+1,"Texture redo failed");stamp_assets();
        std::cout<<"PASS editor OBJ append and independent PNG binding, undo/redo\n";
        // 调用和右键菜单、检查器及鼠标拖动共用的编辑函数；这不是人工点击验收。
        const auto before_create=scene.nodes.size();add_object(editor::NewObject::cube,-1);
        require(scene.nodes.size()==before_create+1&&property_target==editor::PropertyTarget::node,"Create cube did not select instance inspector");
        const int created=selected_node,created_mesh=scene.nodes[created].mesh;
        const auto initial=scene_world_transforms(scene).at(created);
        const auto center=editor::project_handle(glm::vec3(initial[3]),camera,viewport_rect(),settings.reversed_z);
        require(center&&start_object_rotation(*center),"Ctrl rotation entry did not start");
        move_object_rotation(*center+glm::vec2(40,-25),false);finish_edit();
        const auto rotated=scene_world_transforms(scene).at(created);
        require(glm::length(glm::vec3(rotated[3])-glm::vec3(initial[3]))<.0001f&&glm::length(glm::vec3(rotated[0])-glm::vec3(initial[0]))>.01f,"Ctrl rotation did not preserve pivot");
        require(undo.undo()&&glm::length(glm::vec3(scene_world_transforms(scene).at(created)[0])-glm::vec3(initial[0]))<.0001f,"Ctrl rotation undo failed");stamp_assets();
        require(undo.redo(),"Ctrl rotation redo failed");stamp_assets();
        select_resource(editor::PropertyTarget::mesh,created_mesh);
        require(selected_node<0&&selected_light<0&&property_target==editor::PropertyTarget::mesh,"Resource selection left stale scene gizmo target");
        select_scene_node(created);
        require(property_target==editor::PropertyTarget::node&&selected_node==created,"Node selection did not restore instance inspector");
        std::cout<<"PASS object/resource selection isolation, primitive creation and Ctrl rotation undo/redo\n";
        const auto parent_dir=!options.output.empty()?options.output.parent_path():output_dir;
        const auto path=parent_dir/("editor-check-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".ember");
        save_project(path,scene,camera,settings);Scene restored;Camera restored_camera;Settings restored_settings;load_project(path,restored,restored_camera,restored_settings);
        require(restored.nodes.size()==scene.nodes.size()&&restored.textures.size()==scene.textures.size()&&restored.lights[0].linked_node==scene.lights[0].linked_node,"Editor project round trip failed");
        require(restored.materials.at(materials).base_texture==scene.materials.at(materials).base_texture,"Project texture binding was lost");
        std::cout<<"PASS editor scene/material/texture/light-binding project persistence: "<<path_to_utf8(path)<<'\n';
        // 只影响诊断进程：便于查看新增模型和工具，正式启动不会自动改场景。
        selected_node=int(nodes);selected_light=-1;property_target=editor::PropertyTarget::node;selected_material=int(materials);requested_panel=1;mark_dirty();
    };
    while(running){
        const auto frame_started=std::chrono::steady_clock::now();
        SDL_Event event;
        while(SDL_PollEvent(&event)){
            gpu.process_event(event);
            if(event.type==SDL_QUIT)running=false;
            if(event.type==SDL_WINDOWEVENT&&(event.window.event==SDL_WINDOWEVENT_SIZE_CHANGED||event.window.event==SDL_WINDOWEVENT_RESTORED))gpu.resize();
            if(event.type==SDL_DROPFILE){
                auto p=path_from_utf8(event.drop.file);SDL_free(event.drop.file);auto ext=path_to_utf8(p.extension());
                std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
                if(ext==".glb"||ext==".gltf"||ext==".obj")request_model(std::move(p),false);
                else if(ext==".png"||ext==".jpg"||ext==".jpeg"||ext==".tga"||ext==".bmp")request_texture(std::move(p),TextureRole::base_color);
                else if(ext==".hdr")request_environment(std::move(p));
                else error="该文件类型暂不支持。模型使用 GLB / glTF / OBJ；FBX 请先转换为 GLB。";
            }
            if(event.type==SDL_MOUSEBUTTONUP&&event.button.button==SDL_BUTTON_LEFT&&object_rotation_active){
                try{finish_edit();}catch(const std::exception& e){error=e.what();}continue;
            }
            if(event.type==SDL_MOUSEMOTION&&object_rotation_active){
                try{move_object_rotation({event.motion.x,event.motion.y},(SDL_GetModState()&KMOD_SHIFT)!=0);}
                catch(const std::exception& e){error=e.what();object_rotation_active=false;if(edit_before){editor::restore_snapshot(*edit_before,scene,camera,settings);stamp_assets();edit_before.reset();mark_dirty();}}
                continue;
            }
            if(event.type==SDL_MOUSEBUTTONUP&&event.button.button==SDL_BUTTON_LEFT&&translation_drag.active){
                translation_drag.active=false;try{finish_edit();}catch(const std::exception& e){error=e.what();}continue;
            }
            if(event.type==SDL_MOUSEMOTION&&translation_drag.active){
                try{move_translation({event.motion.x,event.motion.y},(SDL_GetModState()&KMOD_SHIFT)!=0);}
                catch(const std::exception& e){error=e.what();translation_drag.active=false;if(edit_before){editor::restore_snapshot(*edit_before,scene,camera,settings);stamp_assets();edit_before.reset();mark_dirty();}}continue;
            }
            if(event.type==SDL_WINDOWEVENT&&event.window.event==SDL_WINDOWEVENT_FOCUS_LOST&&(translation_drag.active||object_rotation_active)){try{finish_edit();}catch(const std::exception& e){error=e.what();}}
            if(event.type==SDL_KEYDOWN&&!event.key.repeat&&!ImGui::GetIO().WantTextInput){
                auto key=event.key.keysym.sym;const bool ctrl=(event.key.keysym.mod&KMOD_CTRL)!=0;
                if(key==SDLK_ESCAPE){if((translation_drag.active||object_rotation_active)&&edit_before){editor::restore_snapshot(*edit_before,scene,camera,settings);stamp_assets();edit_before.reset();translation_drag.active=object_rotation_active=false;mark_dirty();}
                    else if(!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)){if(topic_return){try{return_from_topic();}catch(const std::exception& e){error=e.what();}}else running=false;}}
                if(!ctrl&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)){
                    if(key==SDLK_w)choose_transform_tool(editor::TransformTool::translate);
                    if(key==SDLK_e)choose_transform_tool(editor::TransformTool::rotate);
                    if(key==SDLK_r)choose_transform_tool(editor::TransformTool::scale);
                }
                if(key==SDLK_f){frame_camera(scene,camera);mark_dirty();}
                if(key==SDLK_F11){fullscreen=!fullscreen;SDL_SetWindowFullscreen(window.native_handle(),fullscreen?SDL_WINDOW_FULLSCREEN_DESKTOP:0);gpu.resize();}
                if(ctrl&&key==SDLK_n)open_new_scene_popup=true;
                if(ctrl&&key==SDLK_s){if(event.key.keysym.mod&KMOD_SHIFT)save_as();else save();}
                if(ctrl&&(key==SDLK_z||key==SDLK_y))history_step(key==SDLK_y);
            }
            if(!ImGui::GetIO().WantCaptureMouse&&event.type==SDL_MOUSEBUTTONDOWN&&event.button.button==SDL_BUTTON_LEFT) {
                if(SDL_GetModState()&KMOD_ALT)continue; // Alt 左键留给相机，不改变对象选择。
                if(SDL_GetModState()&KMOD_CTRL){try{if(start_object_rotation({event.button.x,event.button.y}))continue;}catch(const std::exception& e){error=e.what();continue;}}
                try{if(start_translation({event.button.x,event.button.y}))continue;}catch(const std::exception& e){error=e.what();continue;}
                // 同一场景空间索引用于视口选物；屏幕坐标先去掉工具栏/面板与留白。
                const auto display=ImGui::GetIO().DisplaySize;
                const auto shell=emberframe::lab::editor::layout(display.x,display.y,show_scene,show_inspector,scene_width,inspector_width);
                const float scale=std::min(shell.viewport_width()/settings.render_width,shell.viewport_height()/settings.render_height);
                const float w=settings.render_width*scale,h=settings.render_height*scale;
                const float left=shell.left+(shell.viewport_width()-w)*.5f,top=shell.menu+shell.toolbar+(shell.viewport_height()-h)*.5f;
                const float x=float(event.button.x),y=float(event.button.y);
                if(w>0&&h>0&&x>=left&&x<left+w&&y>=top&&y<top+h) {
                    try {
                        scene_geometry.prepare(scene,camera,settings);
                        const glm::vec2 ndc{2*(x-left)/w-1,1-2*(y-top)/h};
                        auto near_world=glm::inverse(camera.projection(float(settings.render_width)/settings.render_height,settings.reversed_z)*camera.view())*glm::vec4(ndc,settings.reversed_z?1.f:0.f,1.f);
                        const Ray ray{camera.position,safe_normalize(glm::vec3(near_world)/near_world.w-camera.position),.0001f,camera.far_plane};
                        TraversalStatistics traversal;const auto picked=scene_geometry.spatial_index().pick(ray,&traversal);
                        if(picked&&picked.node>=0&&picked.node<int(scene.nodes.size())) {
                            finish_edit();selected_node=picked.node;selected_light=selected_texture=-1;property_target=editor::PropertyTarget::node;selected_mesh=scene.nodes[selected_node].mesh;
                            if(picked.material>=0&&picked.material<int(scene.materials.size()))selected_material=picked.material;
                            requested_panel=1;show_inspector=true;
                            notice=std::string(settings.spatial_structure==SpatialStructure::bvh?"BVH":"八叉树")+" 选中 "+scene.nodes[selected_node].name+"；访问 "+std::to_string(traversal.nodes_visited)+" 个节点、测试 "+std::to_string(traversal.triangle_tests)+" 个三角形。";
                            if(const int light=light_for_node(picked.node);light>=0)select_light(light);
                        }
                    }catch(const std::exception& e){error=e.what();}
                }
            }
            if(!ImGui::GetIO().WantCaptureMouse&&event.type==SDL_MOUSEMOTION&&
                ((event.motion.state&SDL_BUTTON_RMASK)||((event.motion.state&SDL_BUTTON_LMASK)&&(SDL_GetModState()&KMOD_ALT)))){
                auto offset=camera.position-camera.target;const float distance=glm::length(offset);
                if(SDL_GetModState()&KMOD_SHIFT){
                    auto forward=safe_normalize(-offset),right=safe_normalize(glm::cross(forward,{0,1,0}));
                    auto move=(-right*float(event.motion.xrel)+glm::vec3(0,1,0)*float(event.motion.yrel))*distance*.0015f;
                    camera.position+=move;camera.target+=move;
                }else{
                    float yaw=std::atan2(offset.z,offset.x)-event.motion.xrel*.006f;
                    float elevation=std::clamp(std::asin(std::clamp(offset.y/std::max(distance,.001f),-1.f,1.f))+event.motion.yrel*.006f,-1.48f,1.48f);
                    camera.position=camera.target+distance*glm::vec3(std::cos(elevation)*std::cos(yaw),std::sin(elevation),std::cos(elevation)*std::sin(yaw));
                }replay_running=false;mark_dirty();
            }
            if(!ImGui::GetIO().WantCaptureMouse&&event.type==SDL_MOUSEWHEEL){replay_running=false;camera.position=camera.target+(camera.position-camera.target)*std::exp(-event.wheel.y*.12f);mark_dirty();}
        }
        if(!running)break;
        // 相机移动不取消静态烘焙；只在内容/质量键变化时废弃旧任务。
        const bool gpu_configuration=visual_topic<0&&(settings.path==RenderPath::forward||settings.path==RenderPath::deferred)
            &&VulkanWorkbench::unsupported_modes(scene,settings).empty();
        const bool needs_bake=gpu_configuration&&(settings.environment_diffuse!=EnvironmentDiffuse::ibl||settings.sdf_shadows);
        // 独立 SH/PRT/SDF 对照已有自己的计算流程，不同时再为不可见的 GPU 场景烘焙。
        if(scene_bake.valid()&&(!needs_bake||bake_key()!=bake_running_key))scene_bake_cancel=true;
        if(scene_bake.valid()&&scene_bake.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            try{auto baked=scene_bake.get();if(baked&&!scene_bake_cancel&&bake_epoch==document_epoch&&bake_running_key==bake_key()) {
                scene.baked_resources=std::move(baked);scene.revision=++generation;stamp_assets();mark_dirty();notice="场景烘焙完成；资源可随工程保存。";
            }}catch(const std::exception& e){error=e.what();}
        }
        if(needs_bake&&!scene_bake.valid()&&bake_key()!=bake_request_key)start_bake();
        if(lod_chain_build.valid()&&lod_chain_build.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            try{auto mesh=lod_chain_build.get();if(lod_epoch==document_epoch&&lod_source_hash==geometry_fingerprint(scene)&&lod_mesh_index<int(scene.meshes.size())&&same_lod_input(mesh,scene.meshes[lod_mesh_index])) {
                if(mesh.lods.empty())notice="没有生成可用 LOD：输入预算、边界或接缝约束阻止了减面。";
                else {auto before=scene.meshes[lod_mesh_index];auto after=before;after.lods=std::move(mesh.lods);auto index=lod_mesh_index;
                    scene.meshes[index]=after;scene.revision=++generation;stamp_assets();
                    undo.push_applied("生成 LOD 链",[&scene,index,before](){scene.meshes[index]=before;++scene.revision;},[&scene,index,after](){scene.meshes[index]=after;++scene.revision;});
                    mark_dirty();notice="LOD 链已保存到网格；启用自动 LOD 后按屏幕误差选择。";
                }
            }else notice="模型已变化，未提交旧 LOD 结果。";}catch(const std::exception& e){error=e.what();}
        }
        if(system_benchmark.valid()&&system_benchmark.wait_for(std::chrono::seconds(0))==std::future_status::ready){try{auto report=system_benchmark.get();notice=report.all_outputs_match?"基准完成：所有线程配置输出一致，报告见 output/benchmark。":"基准失败：请检查报告。";}catch(const std::exception& e){error=e.what();}}
        if(shader_build.valid()&&shader_build.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            try{
                auto build=shader_build.get();
                if(!build.success)throw std::runtime_error(build.error+"\n日志："+path_to_utf8(build.log_directory));
                if(!build.compatibility.compatible){std::string reason="Shader 资源布局不兼容，保留旧版本。";for(const auto& why:build.compatibility.reasons)reason+="\n"+why;throw std::runtime_error(reason);}
                std::string reason;if(!gpu.reload_pipelines(build.version->directory,&reason))throw std::runtime_error(reason);
                shader_library->activate(build.version);notice=build.cache_hit?"Shader 缓存命中，已安全替换。":"Shader 编译通过，已安全替换。";
            }catch(const std::exception& e){error=e.what();if(options.reload_shaders)throw;}
        }
        if(imported_environment.valid()&&imported_environment.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            try{auto environment=imported_environment.get();
                if(environment_epoch!=document_epoch)notice="工程已切换，旧 HDR 导入结果未提交。";
                else{editor_change("导入 HDR 环境",[&]{set_scene_environment(scene,std::move(environment),1,0);});notice="HDR 已导入；IBL 正在后台预计算，工程保存包含环境像素。";}
            }catch(const std::exception& e){error=e.what();}
        }
        if(imported.valid()&&imported.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            try{auto loaded=imported.get();if(import_epoch!=document_epoch){notice="工程已切换，旧模型导入结果未提交。";}
                else{
                    const int root=int(scene.nodes.size());
                    if(imported_replace){reset_document();editor_change("替换模型场景",[&](){scene=std::move(loaded.scene);scene.revision=++generation;frame_camera(scene,camera);});selected_material=0;selected_node=selected_light=-1;property_target=editor::PropertyTarget::none;selected_texture=-1;}
                    else{const bool first_model=scene.meshes.empty();editor_change("追加导入模型",[&](){append_scene_asset(scene,std::move(loaded.scene));});
                        if(first_model)frame_camera(scene,camera);
                        selected_node=root;selected_light=selected_texture=-1;property_target=editor::PropertyTarget::node;requested_panel=1;show_inspector=true;
                        for(int n=root+1;n<int(scene.nodes.size());++n){const int mesh=scene.nodes[n].mesh;if(mesh>=0&&mesh<int(scene.meshes.size())&&!scene.meshes[mesh].primitives.empty()){
                            selected_mesh=mesh;selected_material=int(scene.meshes[mesh].primitives.front().material);break;
                        }}
                    }
                    notice=imported_replace?"已替换场景，可 Ctrl+Z 撤销。":"模型已追加到当前场景，可 Ctrl+Z 撤销。";
                }
                for(const auto& warning:loaded.warnings)notice+="\n"+warning;
            }catch(const std::exception& e){error=e.what();}
        }
        if(!imported.valid()&&!model_queue.empty()){const auto path=model_queue.front();model_queue.pop_front();import_asset(path);}
        if(imported_texture.valid()&&imported_texture.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            try{auto texture=imported_texture.get();
                if(texture_epoch!=document_epoch)notice="工程已切换，旧纹理导入结果未提交。";
                else{editor_change("导入并绑定纹理",[&](){bind_material_texture(scene,imported_texture_material,std::move(texture),imported_texture_role);});
                    selected_material=imported_texture_material;selected_light=selected_node=selected_texture=-1;property_target=editor::PropertyTarget::material;requested_panel=1;show_inspector=true;
                    notice="已绑定到 "+scene.materials.at(selected_material).name+" · "+texture_role_name(imported_texture_role)+"；可 Ctrl+Z 撤销。";
                }
            }catch(const std::exception& e){error=e.what();}
        }
        if(checks.valid()&&checks.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            try{test_results=checks.get();notice="实验检查结束，详细结果见专题面板。";}catch(const std::exception& e){error=e.what();}
        }
        if(simplified.valid()&&simplified.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            try{auto result=simplified.get();
                if(simplification_epoch!=document_epoch||simplification_generation!=generation)notice="场景已变化，丢弃过期的 LOD 结果。";
                else if(result.output_triangles<result.input_triangles){
                    if(undo.transaction_active())undo.commit();undo.begin(scene,camera,settings,"QEM LOD");
                    scene.meshes.at(simplification_mesh)=std::move(result.mesh);++scene.revision;stamp_assets();undo.commit();mark_dirty();
                    notice="QEM："+std::to_string(result.input_triangles)+" → "+std::to_string(result.output_triangles)+" 三角形。";
                }else notice="约束下无法安全缩减："+result.reason;
            }catch(const std::exception& e){error=e.what();}
        }
        if(bg.render.valid()&&bg.render.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            try{
                auto result=bg.render.get();
                if(bg.generation==generation&&!bg.cancel){
                    reference=std::move(result);
                    if(!bg.visual)temporal_info=reference_session.info();reference_generation=generation;
                    gpu.set_reference_image(reference.color);notice=bg.visual?"独立 CPU 对照完成；保持当前结果，改参数后再计算。":"参考渲染完成，可继续改参数或保存画面。";
                }
            }catch(const std::exception& e){error=e.what();reference_failed_generation=bg.generation;}
        }
        if(replay_running){
            const auto count=sequence_capture?std::size_t(options.sequence_frames):std::size_t(240);
            const auto index=sequence_capture?sequence_index:replay_index%count;
            if(index<count){camera=replay_demo_camera(replay_origin,options.replay,index,count);dirty=true;}
        }
        if(options.object_replay&&!motion_replay.active){start_object_replay();options.object_replay=false;}
        if(motion_replay.active&&!gpu.stats().uploadPending&&gpu.stats().environment_ready){
            const float phase=float(motion_replay.tick++)*.035f;
            const auto delta=glm::translate(glm::mat4(1),glm::vec3(.5f*std::sin(phase),0,0))*glm::rotate(glm::mat4(1),.35f*std::sin(phase),glm::vec3(0,1,0));
            scene.nodes.at(motion_replay.node).local=delta*motion_replay.original;
            ++scene.revision;scene.baked_resources.reset();dirty=true;
        }
        Settings actual=settings;
        if(visual_topic>=0)actual.path=RenderPath::cpu_raster;
        const auto unsupported=VulkanWorkbench::unsupported_modes(scene,actual);
        const bool realtime_blocked=(actual.path==RenderPath::forward||actual.path==RenderPath::deferred)&&!unsupported.empty();
        if(realtime_blocked&&(options.frames>0||sequence_capture||options.benchmark_frames))
            throw std::runtime_error("Unsupported real-time configuration (no CPU fallback): "+unsupported.front());
        const bool cpu=actual.path==RenderPath::cpu_raster||actual.path==RenderPath::path_trace;
        if(sequence_capture&&cpu)throw std::runtime_error("Sequence capture refuses CPU fallback");
        if(sequence_capture&&std::chrono::steady_clock::now()-sequence_started>std::chrono::seconds(120))throw std::runtime_error("Sequence capture timed out");
        if(options.benchmark_frames&&cpu)throw std::runtime_error("GPU benchmark requires a supported Vulkan configuration; no CPU fallback is benchmarked");
        // 独立实验不更新 ReferenceSession::info()；沿用其 frames 会无限重算同一张图。
        // 只有真正的时序参考继续累积；失败也不逐帧重试，修改参数或“重新渲染”后重试。
        const bool accumulate=visual_topic<0&&(settings.taa||settings.denoise||settings.svgf)
            &&temporal_info.frames<32&&reference_failed_generation!=generation;
        if(reference_snapshot&&(!cpu||reference_snapshot_generation!=generation))reference_snapshot.reset();
        if(reference_source&&(!cpu||reference_source_generation!=generation))reference_source.reset();
        if(cpu&&(dirty||accumulate)&&!bg.busy()&&std::chrono::steady_clock::now()-last_edit>std::chrono::milliseconds(180)){
            try{
                std::shared_ptr<const Scene> snapshot=reference_source;
                const int experiment=visual_topic;
                if(!snapshot&&(experiment==10||experiment==11)&&!experiment_current_scene){
                    // 轻量对照只需天空与灯；不要先深拷贝整套模型/纹理再 clear。
                    auto light_scene=std::make_shared<Scene>();light_scene->name=scene.name;light_scene->sky_top=scene.sky_top;
                    light_scene->sky_bottom=scene.sky_bottom;light_scene->lights=scene.lights;light_scene->revision=scene.revision;
                    snapshot=std::move(light_scene);
                }else if(!snapshot){
                    if(!reference_snapshot){reference_snapshot=std::make_unique<SceneUploadSnapshot>(scene);reference_snapshot_generation=generation;}
                    // 复用已有的有界快照底座；准备不完仍继续正常显示/处理输入，不在这里等待。
                    if(reference_snapshot->advance(scene,4*1024*1024)){
                        snapshot=std::make_shared<Scene>(reference_snapshot->take());reference_snapshot.reset();
                    }
                }
                if(snapshot){
                    // 时序参考的下一帧复用不可变源；相机/参数编辑换 generation 才另建快照。
                    reference_source=snapshot;reference_source_generation=generation;
                    bg.cancel=false;bg.generation=generation;bg.visual=experiment>=0;dirty=false;
                    Camera view=camera;Settings quality=actual;
                    bg.render=std::async(std::launch::async,[snapshot=std::move(snapshot),view,quality,experiment,&bg,&reference_session](){
                        return experiment>=0?run_visual_experiment(experiment,*snapshot,view,quality,&bg.cancel):reference_session.render(*snapshot,view,quality,&bg.cancel);
                    });
                }
            }catch(const std::exception& e){
                error=e.what();reference_snapshot.reset();reference_failed_generation=generation;dirty=false;
            }
        }
        if(options.frames>0&&!gpu.stats().uploadError.empty())throw std::runtime_error(gpu.stats().uploadError);
        if(options.frames>0&&!gpu.stats().scene_resources_error.empty())throw std::runtime_error(gpu.stats().scene_resources_error);
        // 诊断连续 6 帧同步移动带纹理模型与灯板，不能等鼠标停下才更新画面。
        bool verify_pose_frame=false;
        if(options.verify_editor&&editor_verified&&!cpu&&editor_pose_frames<6&&(editor_pose_started||editor_ready_frames>=8)){
            editor_pose_started=true;verify_pose_frame=true;
            auto position=glm::vec3(scene_world_transforms(scene).at(selected_node)[3]);position.x+=.01f;set_node_position(selected_node,position);
            auto light_position=scene.lights[0].position;light_position.x+=.01f;set_editor_light_position(scene,0,light_position);mark_dirty();
        }
        // CPU 图像的呈现不需要每帧重扫网格/BVH；选物操作会按需准备空间索引。
        const Scene& render_scene=cpu||realtime_blocked?scene:scene_geometry.prepare(scene,camera,actual);
        // 姿态每帧递增时，上一提交的姿态版本天然落后一帧；静态资产已匹配即可继续采样。
        // 场景/材质替换仍必须等待完整资产版本，不能把上传中的旧画面当成完成。
        const bool gpu_ready=cpu||(!realtime_blocked&&!gpu.stats().uploadPending&&
            (gpu.stats().displayedRevision==render_scene.revision||(motion_replay.active&&render_scene.asset_revision&&gpu.stats().displayedAssetRevision==render_scene.asset_revision))&&gpu.stats().environment_ready&&gpu.stats().scene_resources_ready);
        if((options.frames>0||options.benchmark_frames)&&std::chrono::steady_clock::now()>next_progress){
            const auto& progress=gpu.stats();std::cerr<<"[Progress] frames="<<presented<<" revision="<<progress.displayedRevision<<'/'<<render_scene.revision
                <<" upload="<<progress.uploadStage<<" env="<<progress.environment_ready<<" bake="<<progress.scene_resources_ready<<'\n';
            next_progress=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            if(std::chrono::steady_clock::now()-sequence_started>std::chrono::seconds(120))throw std::runtime_error("Batch render timed out; inspect resource-readiness progress log");
        }
        const bool resources_ready=(!cpu||reference_generation==generation)&&!shader_build.valid()&&!scene_bake.valid()&&!lod_chain_build.valid()&&gpu_ready;
        if(options.verify_editor&&!editor_verified&&resources_ready&&presented>=5){
            if(cpu)throw std::runtime_error("Editor integration check refuses CPU fallback");
            verify_editor_actions();editor_verified=true;continue;
        }
        if(options.verify_editor&&editor_verified){if(resources_ready)++editor_ready_frames;else editor_ready_frames=0;}
        // GPU 统计来自已退休的 Frame Slot，晚于新资源的首次呈现；验收必须等两槽都稳定。
        // 大模型上传可能比 --frames 指定的帧数更久，不能在上传刚完成时就截取
        // 旧背景或写出尚未退休的零统计。所有自动捕获都等待连续四帧就绪。
        capture_ready_frames=resources_ready?std::min(capture_ready_frames+1,4):0;
        const bool ready_for_capture=resources_ready&&capture_ready_frames>=4&&(!options.verify_editor||(editor_pose_frames==6&&editor_ready_frames>=8));
        if(sequence_capture&&!sequence_pending&&ready_for_capture&&sequence_warmup>=8&&sequence_index<std::size_t(options.sequence_frames)){
            gpu.request_frame_readback();sequence_pending=true;
        }
        if(options.compare&&!readback_requested&&ready_for_capture&&presented>=2){
            if(cpu)throw std::runtime_error("--compare requires a supported Vulkan configuration");
            gpu.request_frame_readback(DebugView::albedo);readback_requested=true;
        }
        if(!options.output.empty()&&!output_requested&&ready_for_capture&&!cpu&&presented>=std::uint64_t(std::max(2,options.frames-2))){if(options.compare)throw std::invalid_argument("--compare and GPU --output must run separately");gpu.request_frame_readback();output_requested=true;}
        if(!options.screenshot.empty()&&!screenshot_requested&&ready_for_capture&&presented>=std::uint64_t(std::max(0,options.frames-2))){gpu.request_screenshot(options.screenshot);screenshot_requested=true;}
        try{
            if(gpu.draw(render_scene,camera,actual,[&](){
                if(!options.no_ui)ui();actual=settings;if(visual_topic>=0)actual.path=RenderPath::cpu_raster;
                // 不修改用户选择的实时路径；不支持时由 Workbench 保留旧 GPU 结果和可操作 UI。
            }))++presented;
            executed_path=actual.path;
            if(verify_pose_frame){const auto& pose=gpu.stats();
                if(pose.uploadPending||pose.uploadSubmitted||pose.snapshotCopied||pose.displayedRevision!=render_scene.revision)
                    throw std::runtime_error("Continuous editor dragging reuploaded static assets or displayed a stale pose");
                ++editor_pose_frames;if(editor_pose_frames==6)std::cout<<"PASS GPU continuous editor model/light dragging: 6 frames, zero static upload bytes\n";
            }
            if(auto readback=gpu.take_frame_readback()){
                capture_center=readback->image.at(readback->image.width/2,readback->image.height/2);capture_center_valid=true;
                if(sequence_capture){
                    if(!sequence_pending||readback->path==RenderPath::cpu_raster||readback->path==RenderPath::path_trace)throw std::runtime_error("Unexpected sequence readback");
                    for(const auto& pixel:readback->image.pixels)for(int k=0;k<3;++k)if(!std::isfinite(pixel[k]))throw std::runtime_error("Non-finite sequence HDR");
                    std::ostringstream name;name<<"frame-"<<std::setw(4)<<std::setfill('0')<<sequence_index;
                    save_bmp(options.sequence_directory/(name.str()+".bmp"),readback->image,settings.exposure);
                    if(sequence_index==0||sequence_index+1==std::size_t(options.sequence_frames))save_pfm(options.sequence_directory/(name.str()+".pfm"),readback->image);
                    const auto& gs=scene_geometry.stats();const auto& completed=gpu.stats();
                    if(sequence_index)sequence_report<<",\n";
                    sequence_report<<"{\"index\":"<<sequence_index<<",\"serial\":"<<readback->serial<<",\"path\":"<<int(readback->path)
                        <<",\"camera\":["<<camera.position.x<<','<<camera.position.y<<','<<camera.position.z<<"]"
                        <<",\"sourceTriangles\":"<<gs.source_triangles<<",\"selectedTriangles\":"<<gs.selected_triangles
                        <<",\"historyValid\":"<<(completed.temporal_history_valid?"true":"false")
                        <<",\"completedStatsSerial\":"<<completed.gpu_sample_serial<<",\"visibleObjects\":"<<completed.visible_objects
                        <<",\"candidateObjects\":"<<completed.candidate_objects<<",\"gpuMillisecondsWithReadback\":"<<completed.gpu_ms
                        <<",\"centerLinearRgb\":["<<capture_center.x<<','<<capture_center.y<<','<<capture_center.z<<"]}";
                    ++sequence_index;sequence_pending=false;
                }else if(options.compare)comparison_frame=std::move(readback);else output_frame=std::move(readback);
            }
            if(gpu_ready){++sequence_warmup;if(replay_running&&!sequence_capture)++replay_index;}
            const auto& completed=gpu.stats();if(options.benchmark_frames&&gpu_ready&&!cpu&&completed.gpu_sample_serial>last_timing_serial&&completed.gpu_ms>=0){last_timing_serial=completed.gpu_sample_serial;++eligible_samples;if(eligible_samples>gpu_timings.warmup_frames){gpu_timings.samples.push_back({completed.gpu_sample_serial,completed.gpu_ms});measured_profiles.push_back(completed.completed_profile);}}
            if(!options.profile_output.empty()&&!options.benchmark_frames&&gpu_ready&&!cpu&&completed.completed_profile.completed&&completed.completed_profile.serial>last_profile_serial){
                last_profile_serial=completed.completed_profile.serial;if(measured_profiles.size()<1000)measured_profiles.push_back(completed.completed_profile);}
        }catch(const std::exception& e){error=e.what();std::cerr<<"[Workbench] "<<error<<"\n";throw;}
        if(options.smoke&&presented==4){SDL_SetWindowSize(window.native_handle(),1280,800);gpu.resize();}
        if(options.benchmark_frames){if(gpu_timings.samples.size()>=std::size_t(options.benchmark_frames)||(!gpu.stats().timestamp_available&&presented>=std::uint64_t(options.benchmark_frames+30)))running=false;}
        else if(sequence_capture){if(sequence_index>=std::size_t(options.sequence_frames))running=false;}
        else if(options.frames>0&&presented>=std::uint64_t(options.frames)&&ready_for_capture&&!shader_build.valid()&&(options.output.empty()||cpu||output_frame))running=false;
        // 交互模式限制到约 60Hz，避免静止界面在 Mailbox 模式持续占满显卡。
        if(options.frames==0)std::this_thread::sleep_until(frame_started+std::chrono::microseconds(16667));
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bg.cancel=true;if(bg.render.valid())bg.render.wait();
    scene_bake_cancel=true;if(scene_bake.valid())scene_bake.wait();if(lod_chain_build.valid())lod_chain_build.wait();
    if(imported.valid())imported.wait();if(checks.valid())checks.wait();if(simplified.valid())simplified.wait();
    if(shader_build.valid())shader_build.wait();
    if(system_benchmark.valid())system_benchmark.wait();
    gpu.wait_idle();
    if(sequence_capture){
        if(sequence_index!=std::size_t(options.sequence_frames))throw std::runtime_error("Sequence cancelled before all frames completed");
        sequence_report<<"\n],\"capturedFrames\":"<<sequence_index<<"}\n";sequence_report.close();
        std::filesystem::rename(options.sequence_directory/"sequence.partial.json",options.sequence_directory/"sequence.json");
    }
    const auto& stats=gpu.stats();std::cout<<"[Workbench] presented="<<presented<<" gpuMs="<<stats.gpu_ms<<" triangles="<<stats.triangles<<" draws="<<stats.draw_calls<<" allocationBytes="<<stats.allocated_bytes
        <<" render="<<stats.render_width<<"x"<<stats.render_height<<" viewport="<<viewport_pixel_width<<"x"<<viewport_pixel_height<<" drawable="<<stats.present_width<<"x"<<stats.present_height<<" dpi="<<stats.display_scale<<" fontRaster="<<stats.font_raster_scale<<"\n";
    std::ofstream metrics(output_dir/"last-run.json");
    metrics<<"{\"gpu\":"<<std::quoted(stats.gpu_name)<<",\"presentedFrames\":"<<presented<<",\"gpuMilliseconds\":"<<stats.gpu_ms
        <<",\"drawCalls\":"<<stats.draw_calls<<",\"triangles\":"<<stats.triangles<<",\"allocatedBytes\":"<<stats.allocated_bytes
        <<",\"visibleObjects\":"<<stats.visible_objects<<",\"candidateObjects\":"<<stats.candidate_objects
        <<",\"renderWidth\":"<<stats.render_width<<",\"renderHeight\":"<<stats.render_height<<",\"viewportWidth\":"<<viewport_pixel_width<<",\"viewportHeight\":"<<viewport_pixel_height
        <<",\"drawableWidth\":"<<stats.present_width<<",\"drawableHeight\":"<<stats.present_height<<",\"displayScale\":"<<stats.display_scale<<",\"fontRasterScale\":"<<stats.font_raster_scale
        <<",\"graphPasses\":"<<stats.graph_passes<<",\"graphBarriers\":"<<stats.graph_barriers<<",\"actualPath\":"<<int(executed_path)
        <<",\"giMode\":"<<int(settings.gi)<<",\"debugView\":"<<int(settings.debug)
        <<",\"effectDispatches\":"<<stats.effect_dispatches<<",\"environmentReady\":"<<(stats.environment_ready?"true":"false")<<"}\n";
    if(!stats.screenshot_error.empty())throw std::runtime_error(stats.screenshot_error);
    if(!options.screenshot.empty()&&!std::filesystem::exists(options.screenshot))throw std::runtime_error("Screenshot was not produced");
    if(options.compare){if(!comparison_frame)throw std::runtime_error("GPU readback was not produced");validate_gpu_reference(*comparison_frame,scene,camera,settings,output_dir);}
    if(!options.output.empty()){if(!output_frame)throw std::runtime_error("GPU --output needs a supported Vulkan route; use --headless for CPU reference");save_bmp(options.output,output_frame->image,settings.exposure);auto hdr=options.output;hdr.replace_extension(".pfm");save_pfm(hdr,output_frame->image);}
    if(!options.graph_output.empty())save_render_graph_snapshot(gpu.render_graph(),options.graph_output,{stats.graph_serial,stats.graph_revision,"Vulkan","completed"});
    if(options.benchmark_frames){gpu_timings.gpu_name=stats.gpu_name;gpu_timings.render_path=settings.path==RenderPath::deferred?"deferred":"forward";auto dir=options.benchmark_output.empty()?output_dir/"benchmark":options.benchmark_output;std::filesystem::create_directories(dir);save_gpu_timings(gpu_timings,dir/"gpu-timings.json");}
    if(!options.profile_output.empty()){
        if(std::filesystem::exists(options.profile_output))throw std::runtime_error("Profile output must be a NEW directory");
        std::filesystem::create_directories(options.profile_output);std::ofstream manifest(options.profile_output/"index.json");
        manifest<<"{\"schema\":\"emberframe.profile-series.v1\",\"gpu\":"<<std::quoted(stats.gpu_name)<<",\"readback_included\":"<<((sequence_capture||output_frame||options.compare)?"true":"false")<<",\"profiles\":[";
        for(std::size_t i=0;i<measured_profiles.size();++i){const auto name="frame-"+std::to_string(measured_profiles[i].serial)+".json";save_frame_profile(measured_profiles[i],options.profile_output/name);if(i)manifest<<',';manifest<<std::quoted(name);}
        manifest<<"]}\n";if(!manifest)throw std::runtime_error("Could not save profile series manifest");
    }
    if(!options.demo_stats.empty()){
        if(!options.demo_stats.parent_path().empty())std::filesystem::create_directories(options.demo_stats.parent_path());std::ofstream report(options.demo_stats);
        const auto& geometry=scene_geometry.stats();
        report<<"{\"schema\":\"emberframe.resume-demo.v1\",\"profile\":"<<std::quoted(options.resume_demo)
              <<",\"gpu\":"<<std::quoted(stats.gpu_name)<<",\"actualPath\":"<<int(executed_path)
              <<",\"captureIncludesReadback\":"<<((sequence_capture||output_frame)?"true":"false")
              <<",\"visibleObjects\":"<<stats.visible_objects<<",\"candidateObjects\":"<<stats.candidate_objects
              <<",\"sourceTriangles\":"<<geometry.source_triangles<<",\"selectedTriangles\":"<<geometry.selected_triangles
              <<",\"spatialNodes\":"<<geometry.spatial_nodes<<",\"centerLinearRgb\":";
        if(capture_center_valid)report<<'['<<capture_center.x<<','<<capture_center.y<<','<<capture_center.z<<']';else report<<"null";
        if(options.resume_demo=="geometry"){
            const auto probe=probe_demo_bvh(scene,camera);
            report<<",\"bvhProbe\":{\"rays\":"<<probe.rays<<",\"hits\":"<<probe.hits<<",\"matchesBrute\":"<<(probe.matches?"true":"false")
                  <<",\"bruteTriangleTests\":"<<probe.brute_triangle_tests<<",\"bvhTriangleTests\":"<<probe.bvh_triangle_tests
                  <<",\"bruteMilliseconds\":"<<probe.brute_ms<<",\"bvhMilliseconds\":"<<probe.bvh_ms<<",\"buildMilliseconds\":"<<probe.build_ms<<'}';
        }
        report<<"}\n";if(!report)throw std::runtime_error("Failed to write demo statistics");
    }
    if(options.verify_editor&&!editor_verified)throw std::runtime_error("Editor integration check did not run");
    if(options.verify_editor){
        std::size_t expected=0;for(const auto& node:scene.nodes)if(node.mesh>=0)expected+=std::max<std::size_t>(1,scene.meshes.at(node.mesh).primitives.size());
        if(editor_pose_frames!=6||editor_ready_frames<8||gpu.stats().candidate_objects!=expected)throw std::runtime_error("Imported model was not confirmed in retired GPU indirect commands");
        std::cout<<"PASS GPU imported primitives confirmed: "<<expected<<" candidates, "<<gpu.stats().triangles<<" camera+shadow triangles\n";
    }
    return 0;
}
}
int run_application(int argc,char** argv){
    try{
        auto options=parse(argc,argv);
        if(!options.profile_output.empty()&&std::filesystem::exists(options.profile_output))
            throw std::invalid_argument("Profile directory exists; choose a new directory");
        if(!options.make_import_project.empty()){
            const auto destination=std::filesystem::absolute(options.make_import_project);
            if(destination.extension()!=".ember"||std::filesystem::exists(destination))
                throw std::invalid_argument("Choose a NEW .ember path; sample creation never overwrites an existing project");
            auto sample=editor::make_import_sample_scene(asset_root()/"assets/import_samples");
            Camera camera;camera.position={7,5,9};camera.target={0,1,0};camera.fov=45;
            ProjectDocument document;document.scene=std::move(sample);document.camera=camera;document.settings=options.settings;
            document.settings.path=RenderPath::forward;document.settings.debug=DebugView::final_color;
            document.settings.render_width=1280;document.settings.render_height=720;
            std::filesystem::create_directories(destination.parent_path());save_project_document(destination,document);
            std::cout<<"[Import project] models="<<document.scene.meshes.size()<<" textures="<<document.scene.textures.size()
                <<" lights="<<document.scene.lights.size()<<" path="<<path_to_utf8(destination)<<"\n";return 0;
        }
        if(options.benchmark){auto folder=options.benchmark_output.empty()?runtime_output_directory()/"benchmark":options.benchmark_output;auto report=run_system_benchmark(folder);std::cout<<system_benchmark_markdown(report);return report.all_outputs_match?0:1;}
        Scene scene;
        const bool default_showcase=options.showcase.empty()&&!options.preset_explicit&&!options.new_scene&&options.asset.empty()&&options.project.empty()&&options.resume_demo.empty()&&options.preset==0&&options.visual_topic<0&&!options.compare&&!options.smoke&&!options.verify_editor&&!options.headless;
        if(default_showcase)options.showcase="materials";
        if(!options.showcase.empty()){} // 下方同时应用代表场景的相机与默认参数。
        else if(options.new_scene){scene=editor::make_new_scene("Untitled scene",false,true);options.preset=-1;}
        else if(options.asset.empty())scene=make_demo_scene(options.preset);
        else{auto loaded=load_editor_model(options.asset);for(const auto& warning:loaded.warnings)std::cerr<<"[Import] "<<warning<<"\n";scene=std::move(loaded.scene);}
        Camera camera;frame_camera(scene,camera);
        if(!options.showcase.empty()){auto project=make_showcase(options.showcase);scene=std::move(project.scene);camera=project.camera;options.settings=project.settings;for(const auto& override:options.setting_overrides)override(options.settings);options.preset=-1;}
        if(!options.resume_demo.empty()){auto d=make_resume_demo(options.resume_demo);scene=std::move(d.scene);camera=d.camera;}
        const bool explicit_cpu=options.headless||options.visual_topic>=0||options.settings.path==RenderPath::cpu_raster||options.settings.path==RenderPath::path_trace;
        if(!options.project.empty()){
            load_project(options.project,scene,camera,options.settings);
            for(const auto& apply:options.setting_overrides)apply(options.settings);
            options.preset=-1;
            if(!explicit_cpu&&(options.settings.path==RenderPath::cpu_raster||options.settings.path==RenderPath::path_trace)){
                options.settings.path=RenderPath::forward;
                std::cerr<<"[Project] Saved CPU reference mode not started; select it explicitly in Developer Tools.\n";
            }
        }
        if(!options.environment_path.empty())set_scene_environment(scene,load_environment_hdr(options.environment_path));
        // 离线/批处理允许启动前明确生成；交互编辑器使用后台按钮，避免主线程做 QEM。
        if(options.settings.auto_lod&&(options.headless||options.frames>0||options.gpu_hidden)) {
            for(std::size_t i=0;i<scene.meshes.size();++i)if(scene.meshes[i].lods.empty()) {
                const auto reports=build_scene_lods(scene,i);
                for(const auto& report:reports)std::cout<<"[LOD] mesh="<<report.mesh<<" levels="<<report.stored_levels<<" "<<report.reason<<'\n';
            }
        }
        if(options.headless){
            if((options.visual_topic==10||options.visual_topic==11)&&options.asset.empty()){
                scene.meshes.clear();scene.nodes.clear();scene.materials.clear();scene.textures.clear();
            }
            if(options.settings.path==RenderPath::forward||options.settings.path==RenderPath::deferred)options.settings.path=RenderPath::cpu_raster;
            ReferenceSession session;RenderOutput result;
            const bool temporal=options.settings.taa||options.settings.denoise||options.settings.svgf;
            const int count=options.frames>0?options.frames:(temporal?8:1);
            for(int frame=0;frame<count;++frame)result=options.visual_topic>=0?run_visual_experiment(options.visual_topic,scene,camera,options.settings):session.render(scene,camera,options.settings);
            const auto path=options.output.empty()?runtime_output_directory()/"reference.bmp":options.output;
            save_bmp(path,result.color,options.settings.exposure);
            auto hdr=path;hdr.replace_extension(".pfm");save_pfm(hdr,result.color);
            std::cout<<"[Reference] scene="<<scene.name<<" resolution="<<result.color.width<<"x"<<result.color.height<<" triangles="<<result.triangles<<" rays="<<result.rays<<" cpuMs="<<result.cpu_ms<<" output="<<path_to_utf8(path)<<"\n";
            return 0;
        }
        return run_window(options,std::move(scene),camera);
    }catch(const std::exception& e){
        std::cerr<<"[EmberFrame] "<<e.what()<<"\n";
        // 双击启动时也能看到失败原因；命令行/headless 不额外弹窗。
        if(argc==1)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"EmberFrame",e.what(),nullptr);
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc,wchar_t** wide_argv){
    // 宽字符入口保留完整命令行，再统一为应用内部 UTF-8；不依赖用户的控制台代码页。
    std::vector<std::string> utf8;utf8.reserve(argc);
    for(int i=0;i<argc;++i)utf8.push_back(path_to_utf8(std::filesystem::path(wide_argv[i])));
    std::vector<char*> argv;argv.reserve(argc);
    for(auto& argument:utf8)argv.push_back(argument.data());
    return run_application(argc,argv.data());
}
#else
int main(int argc,char** argv){return run_application(argc,argv);}
#endif

