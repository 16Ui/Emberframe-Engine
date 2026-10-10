#pragma once
#include "effects_motion.h"

namespace emberframe::lab {
// Header-only，供 CPU checks 直接调用，不强迫子任务修改主 CMake/tests_main。
inline TestResults test_effects_motion_cpu() {
    TestResults results;
    auto test=[&](const char* name,auto fn){
        try {const bool ok=fn();results.push_back({name,ok,ok ? "CPU rigid-motion/ABI assertion":"Motion assertion failed"});}
        catch(const std::exception& e){results.push_back({name,false,e.what()});}
    };
    test("Effects motion ABI is std430 mat4 plus four uints, stride 80",[]{
        return sizeof(EffectsObjectMotion)==80&&alignof(EffectsObjectMotion)==16&&
            offsetof(EffectsObjectMotion,previous_object_id)==64&&offsetof(EffectsObjectMotion,history_valid)==68;
    });
    test("Effects motion maps current world to previous world, including parent affine transform",[]{
        auto a=glm::translate(glm::mat4(1),glm::vec3(2,1,0))*glm::rotate(glm::mat4(1),.6f,glm::vec3(0,1,0))*glm::scale(glm::mat4(1),glm::vec3(2,1,3));
        auto b=glm::translate(glm::mat4(1),glm::vec3(-1,2,0))*glm::rotate(glm::mat4(1),-.3f,glm::vec3(0,1,0));
        const auto motion=effects_make_object_motion(a,b,7);const glm::vec4 local(.2f,.3f,.4f,1);
        return motion.history_valid==1&&motion.previous_object_id==7&&glm::length(motion.current_to_previous_world*a*local-b*local)<1e-5f;
    });
    test("Effects motion normal uses inverse transpose under nonuniform scale",[]{
        auto a=glm::rotate(glm::mat4(1),.4f,glm::vec3(0,0,1))*glm::scale(glm::mat4(1),glm::vec3(2,1,.5f));
        auto b=glm::rotate(glm::mat4(1),-.7f,glm::vec3(0,1,0));auto m=effects_make_object_motion(a,b,0);
        auto n=glm::normalize(glm::vec3(1,1,1));auto current_n=glm::normalize(glm::transpose(glm::inverse(glm::mat3(a)))*n);
        glm::vec3 p,previous_n;return effects_previous_surface(m,{},current_n,p,previous_n)&&
            glm::length(previous_n-glm::normalize(glm::transpose(glm::inverse(glm::mat3(b)))*n))<1e-5f;
    });
    test("Effects motion new object and singular/projective transforms fail closed",[]{
        auto singular=glm::scale(glm::mat4(1),glm::vec3(0,1,1));auto perspective=glm::mat4(1);perspective[0][3]=.1f;
        return !effects_make_object_motion(glm::mat4(1),glm::mat4(1),0,false).history_valid&&
            !effects_make_object_motion(singular,glm::mat4(1),0).history_valid&&
            !effects_make_object_motion(perspective,glm::mat4(1),0).history_valid&&
            !effects_make_object_motion(glm::mat4(1),glm::mat4(1),16777216).history_valid;
    });
    test("Effects motion upload validates empty table, per-slot budget and device range",[]{
        if(effects_motion_upload_bytes(0,80)!=80||effects_motion_upload_bytes(3,240)!=240)return false;
        for(auto range:{std::size_t(79),std::size_t(239)}){
            try {effects_motion_upload_bytes(3,range);return false;}catch(const std::length_error&){}
        }
        try {effects_motion_upload_bytes(3,1024,160);return false;}catch(const std::length_error&){}
        return true;
    });
    return results;
}
} // namespace emberframe::lab
