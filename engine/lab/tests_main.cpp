#include "geometry.h"
#include "shading.h"
#include "shadow_gi.h"
#include "postprocess.h"
#include "systems.h"
#include "reference_renderer.h"
#include "temporal_session.h"
#include "mesh_lod.h"
#include "visual_experiments.h"
#include "shader_assets.h"
#include "diagnostics.h"
#include "frame_profile.h"
#include "effects_motion_tests.h"
#include "showcase.h"
#include "scene_resources.h"
#include "scene_geometry.h"
#include "resume_demo.h"
#include "editor_assets.h"
#include "editor_lights.h"
#include <chrono>
#include <iostream>
#include <string_view>
namespace emberframe::lab { TestResults test_editor_gizmo(); }
int main(int argc,char** argv) {
    using namespace emberframe::lab;
    const std::string_view filter=argc>1?argv[1]:"all";
    struct Group {const char* name;TestResults(*function)();};
    const Group groups[]={{"frame_profile",test_frame_profile},{"geometry",test_geometry},{"shading",test_shading},{"shadow_gi",test_shadow_gi},{"postprocess",test_postprocess},{"systems",test_systems},{"reference",test_reference},{"temporal_session",test_temporal_session},{"mesh_lod",test_mesh_lod},{"visual_experiments",test_visual_experiments},{"shader_assets",+[](){return test_shader_assets(EMBERFRAME_GLSLANG_PATH);}},{"diagnostics",test_diagnostics},{"scene_resources",test_scene_resources},{"scene_geometry",test_scene_geometry},{"resume_demo",test_resume_demo},{"editor_assets",test_editor_assets},{"editor_lights",test_editor_lights},{"editor_gizmo",test_editor_gizmo}};
    int passed=0,failed=0;bool found=false;
    if(filter=="all"||filter=="showcase"){
        found=true;for(const auto& result:test_showcase()){
            (result.passed?++passed:++failed);std::cout<<(result.passed?"PASS ":"FAIL ")<<result.name<<": "<<result.detail<<'\n';
        }
    }
    if(filter=="all"||filter=="effects_motion"){
        found=true;for(const auto& result:test_effects_motion_cpu()){
            if(result.passed)++passed;else ++failed;
            std::cout<<(result.passed?"PASS ":"FAIL ")<<result.name<<": "<<result.detail<<'\n';
        }
    }
    for(const auto& group:groups){
        if(filter!="all"&&filter!=group.name)continue;
        found=true;const auto start=std::chrono::steady_clock::now();
        try{
            const auto results=group.function();
            if(results.empty()){++failed;std::cerr<<"FAIL "<<group.name<<": no tests returned\n";}
            for(const auto& test:results){
                (test.passed?++passed:++failed);
                std::cout<<(test.passed?"PASS ":"FAIL ")<<group.name<<" / "<<test.name<<" | "<<test.detail<<"\n";
            }
        }catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<group.name<<": "<<e.what()<<"\n";}
        std::cout<<"TIME "<<group.name<<" "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<" ms\n";
    }
    if(!found){std::cerr<<"Unknown group. Use all";for(const auto& group:groups)std::cerr<<", "<<group.name;std::cerr<<".\n";return 2;}
    std::cout<<"RESULT passed="<<passed<<" failed="<<failed<<"\n";return failed?1:0;
}

