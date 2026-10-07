#pragma once
#include "systems.h"
#include <span>

namespace emberframe::lab {

// 只读取公开的编译计划，不保留 Graph/回调/GPU 对象，也不执行任何 pass。
// dependencies 来自编译器的真实 edges，索引对应 pass_names 的执行顺序。
// 不凭 Barrier 推测 DAG；计划尚未保存边的 RAW/WAR/WAW/显式依赖分类。
struct GraphSnapshotInfo {
    std::uint64_t frame_serial = 0, scene_revision = 0;
    std::string backend = "unspecified";
    std::string status = "recorded"; // recorded/submitted/completed，由调用方准确填写。
};
std::string render_graph_json(const GraphPlan&, const GraphSnapshotInfo& = {});
std::string render_graph_dot(const GraphPlan&, const GraphSnapshotInfo& = {});
// UI 应用 TextUnformatted 或 Text("%s", row.c_str())，不可将名称当作格式串。
std::vector<std::string> render_graph_pass_list(const GraphPlan&);
std::vector<std::string> render_graph_barrier_list(const GraphPlan&);
// .json/.dot 由后缀选择。使用原生 filesystem::path，支持中文/空格路径。
void save_render_graph(const GraphPlan&, const std::filesystem::path&, const GraphSnapshotInfo& = {});
void save_render_graph(const RenderGraph&, const std::filesystem::path&, const GraphSnapshotInfo& = {});
struct GraphSnapshotFiles { std::filesystem::path json, dot; };
// 每帧独立编号；目录中同名文件会被更新。不要在正常性能采样期间写磁盘。
GraphSnapshotFiles save_render_graph_snapshot(const GraphPlan&, const std::filesystem::path& directory,
                                            const GraphSnapshotInfo&);

struct TimingSummary {
    std::size_t sample_count = 0;
    double minimum_ms = 0, median_ms = 0, p95_ms = 0, maximum_ms = 0;
};
// 中位数采用中心两个样本的均值；p95 采用 nearest-rank。拒绝 NaN/负数。
TimingSummary summarize_timings(std::span<const double> milliseconds);

struct SystemBenchmarkOptions {
    std::size_t object_count = 262144;
    std::size_t warmup_iterations = 3, measured_iterations = 15;
    std::uint32_t seed = 0x454d4245u;
    std::vector<std::size_t> worker_counts{1, 2, 4, 8};
    std::vector<std::size_t> grains{64, 512, 4096};
};
struct SystemBenchmarkRow {
    std::string mode; // serial / jobs
    std::size_t workers = 0, grain = 0, tasks_per_iteration = 0;
    TimingSummary timing;
    std::vector<double> samples_ms;
    std::size_t visible_count = 0;
    std::uint64_t checksum = 0;
    bool output_matches = false;
    double speedup_over_serial = 0;
};
struct SystemBenchmarkReport {
    SystemBenchmarkOptions options;
    unsigned hardware_threads = 0;
    std::string compiler, build;
    std::size_t input_bytes = 0, output_bytes = 0, reference_visible_count = 0;
    std::uint64_t reference_checksum = 0;
    bool all_outputs_match = false;
    std::vector<SystemBenchmarkRow> rows;
};
// 有界工作量：对象 1..1048576，预热 1..32，实测 3..100；线程 1..64。
// 固定种子生成非均匀缩放的局部包围球。每对象实际执行矩阵乘法、世界球变换、
// 六个归一化视锥平面距离测试及 clip 坐标输出。计时覆盖提交、计算、等待；
// 场景生成、线程创建、输出清毒/逐项校验与文件 IO 不进入计时。
// grain 通过 submit() 实际分块；不使用会将任务数压到 workers*4 的 parallel_for。
// 每轮（含预热）全量逐字段、逐 float 位比较；不一致立即抛异常。
// output_directory 为空时只返回 report，否则保存 JSON/CSV/中文 Markdown。
SystemBenchmarkReport run_system_benchmark(const std::filesystem::path& output_directory = {},
                                          const SystemBenchmarkOptions& = {});
std::string system_benchmark_json(const SystemBenchmarkReport&);
std::string system_benchmark_csv(const SystemBenchmarkReport&);
std::string system_benchmark_markdown(const SystemBenchmarkReport&);
void save_system_benchmark(const SystemBenchmarkReport&, const std::filesystem::path& directory);

// 主线程在对应 FrameSlot fence 完成且成功取得 timestamp 后追加一次。
// 用真实提交 serial 去重，不能每个 UI 帧重复采集 stats().gpu_ms。
struct GpuTimingSample {
    std::uint64_t frame_serial = 0;
    double gpu_ms = -1;
};
struct GpuTimingReport {
    std::string gpu_name, render_path;
    std::size_t warmup_frames = 30;
    std::vector<GpuTimingSample> samples;
};
std::string gpu_timings_json(const GpuTimingReport&);
void save_gpu_timings(const GpuTimingReport&, const std::filesystem::path&);

// 测试/导入/shader/运行失败统一导出；不假定已有标准诊断文件格式。
struct DiagnosticRecord {
    std::string category, name, severity = "error", message;
    std::filesystem::path artifact;
    std::uint64_t frame_serial = 0;
};
std::string diagnostics_json(std::span<const DiagnosticRecord>, std::string_view session = {});
void export_diagnostics(std::span<const DiagnosticRecord>, const std::filesystem::path&,
                        std::string_view session = {});

TestResults test_diagnostics();
} // namespace emberframe::lab
