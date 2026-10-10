#pragma once
#include "types.h"
#include <span>

namespace emberframe::lab {
// 子 Pass 的 GPU 时间是 inclusive 区间；有嵌套时不能把所有行直接相加。
struct PassTiming {
    std::string name;
    std::uint32_t depth=0;
    double cpu_record_ms=0,gpu_ms=-1;
};
struct FrameProfile {
    std::uint64_t serial=0,scene_revision=0;
    int width=0,height=0;
    bool completed=false,timestamps_available=false,bounds_cached=true;
    double cpu_wait_ms=0,cpu_ui_ms=0,cpu_prepare_ms=0,cpu_geometry_ms=0;
    double cpu_acquire_ms=0,cpu_record_ms=0,cpu_submit_ms=0,cpu_total_ms=0,gpu_ms=-1;
    std::size_t allocated_bytes=0,upload_bytes=0;
    std::vector<PassTiming> passes;
};
// query[0..1] 为整帧；之后每个 Pass 有两个 query。仅在对应帧 Fence 完成后调用。
void decode_frame_timestamps(FrameProfile&,std::span<const std::uint64_t>,
                             std::uint32_t valid_bits,double period_nanoseconds);
std::string frame_profile_json(const FrameProfile&);
void save_frame_profile(const FrameProfile&,const std::filesystem::path&);
TestResults test_frame_profile();
} // namespace emberframe::lab
