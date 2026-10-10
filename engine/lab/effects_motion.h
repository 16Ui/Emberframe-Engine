#pragma once
#include "gpu_effects.h"
#include <limits>

namespace emberframe::lab {
// metadata 使用 float 储存整数；超过 2^24-1 就无法稳定匹配历史身份。
inline constexpr std::uint32_t effects_exact_object_id_limit=16777215;
inline constexpr std::size_t effects_motion_slot_budget=64u*1024u*1024u;

inline bool effects_motion_affine(const glm::mat4& m) noexcept {
    for(int c=0;c<4;++c)for(int r=0;r<4;++r)if(!std::isfinite(m[c][r]))return false;
    if(std::abs(m[0][3])>1e-6f||std::abs(m[1][3])>1e-6f||
       std::abs(m[2][3])>1e-6f||std::abs(m[3][3]-1)>1e-6f)return false;
    const glm::mat3 linear(m);
    const float volume=glm::length(linear[0])*glm::length(linear[1])*glm::length(linear[2]);
    return std::isfinite(volume)&&volume>1e-20f&&
           std::abs(glm::determinant(linear))>volume*1e-6f;
}

// 输入是同一持久对象的两帧 world matrix，不能用当前排序冒充持久身份。
// 新对象、变形网格、奇异缩放都返回 history_valid=0，只拒绝自己的历史。
inline EffectsObjectMotion effects_make_object_motion(const glm::mat4& current_world,
    const glm::mat4& previous_world,std::uint32_t previous_id,bool known_previous=true) noexcept {
    EffectsObjectMotion result;
    if(!known_previous||previous_id>effects_exact_object_id_limit||
       !effects_motion_affine(current_world)||!effects_motion_affine(previous_world))return result;
    result.current_to_previous_world=previous_world*glm::inverse(current_world);
    if(!effects_motion_affine(result.current_to_previous_world))return EffectsObjectMotion{};
    result.previous_object_id=previous_id;result.history_valid=1;return result;
}

inline std::size_t effects_motion_upload_bytes(std::size_t count,std::size_t max_storage_range,
    std::size_t budget=effects_motion_slot_budget) {
    // 空表也绑定一个合法的 dummy 元素，shader 的 count=0 禁止索引它。
    if(count>std::size_t(effects_exact_object_id_limit)+1||
       std::max(count,std::size_t(1))>std::numeric_limits<std::size_t>::max()/sizeof(EffectsObjectMotion))
        throw std::length_error("Effects motion table index/size overflow");
    const auto bytes=std::max(count,std::size_t(1))*sizeof(EffectsObjectMotion);
    if(bytes>max_storage_range||bytes>budget)throw std::length_error("Effects motion exceeds SSBO/per-slot upload budget");
    return bytes;
}

inline bool effects_previous_surface(const EffectsObjectMotion& motion,glm::vec3 world,
    glm::vec3 normal,glm::vec3& previous_world,glm::vec3& previous_normal) noexcept {
    if(motion.history_valid!=1||!effects_motion_affine(motion.current_to_previous_world))return false;
    previous_world=glm::vec3(motion.current_to_previous_world*glm::vec4(world,1));
    // 位置乘 A；法线必须乘 inverse-transpose(A)，不能直接拿位置矩阵乘法线。
    const auto n=glm::transpose(glm::inverse(glm::mat3(motion.current_to_previous_world)))*normal;
    const auto length=glm::length(n);if(!std::isfinite(length)||length<1e-10f)return false;
    previous_normal=n/length;return true;
}
} // namespace emberframe::lab
