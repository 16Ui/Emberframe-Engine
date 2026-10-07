#include "diagnostics.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <set>
#include <sstream>

namespace emberframe::lab {
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void throws(F&& function, const char* message) {
    bool caught = false; try { function(); } catch (const std::exception&) { caught = true; }
    check(caught, message);
}
struct TemporaryFiles {
    std::filesystem::path root;
    TemporaryFiles() {
        static std::atomic<unsigned> serial{0};
        const auto base = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = base / ("emberframe-diagnostics-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial.fetch_add(1)));
            if (std::filesystem::create_directory(candidate)) { root = candidate; return; }
        }
        throw std::runtime_error("Cannot create isolated diagnostics test directory");
    }
    ~TemporaryFiles() { if (!root.empty()) { std::error_code ignored; std::filesystem::remove_all(root, ignored); } }
};
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary); check(bool(file), "Saved file missing");
    std::ostringstream out; out << file.rdbuf(); return out.str();
}
GraphPlan graph_fixture() {
    // 三条真实并行分支 -> join。root 声明在子 pass 后，强制执行索引映射。
    RenderGraph graph; graph.add_resource("a"); graph.add_resource("b"); graph.add_resource("unused");
    graph.add_pass("branch-a", {{"a", ResourceAccess::write, ResourceState::storage}}, [] {}, false, {"root"});
    graph.add_pass("branch-b", {{"b", ResourceAccess::write, ResourceState::storage}}, [] {}, false, {"root"});
    graph.add_pass("independent", {}, [] {}, true);
    graph.add_pass("join", {{"a", ResourceAccess::read, ResourceState::shader_read}, {"b", ResourceAccess::read, ResourceState::shader_read}}, [] {}, true);
    graph.add_pass("root", {}, [] {});
    graph.add_pass("dead-write", {{"unused", ResourceAccess::write, ResourceState::storage}}, [] {});
    graph.export_resource("a");
    // 返回 plan 后 graph 已析构；诊断导出只读 value 字段，不访问 owner_/回调。
    return graph.compile();
}
SystemBenchmarkOptions small_benchmark() {
    SystemBenchmarkOptions options; options.object_count = 257; options.warmup_iterations = 1; options.measured_iterations = 3;
    options.worker_counts = {1, 2, 4, 8}; options.grains = {64, 512, 4096}; return options;
}
} // namespace

TestResults test_diagnostics() {
    TestResults results;
    auto test = [&](std::string name, auto&& function) {
        TestResult result; result.name = std::move(name);
        try { function(); result.passed = true; result.detail = "All deterministic assertions passed (no speed threshold)"; }
        catch (const std::exception& e) { result.detail = e.what(); }
        catch (...) { result.detail = "Non-standard exception"; }
        results.push_back(std::move(result));
    };
    test("real DAG edges use execution indices, branches stay independent", [] {
        const auto plan = graph_fixture();
        check(plan.order == std::vector<std::size_t>({2, 4, 0, 1, 3}), "Fixture did not reorder declaration indices");
        const std::set<std::pair<std::size_t, std::size_t>> actual(plan.dependencies.begin(), plan.dependencies.end());
        check(actual == std::set<std::pair<std::size_t, std::size_t>>({{1, 2}, {1, 3}, {2, 4}, {3, 4}}), "Compiler DAG edges differ");
        const auto dot = render_graph_dot(plan);
        check(dot.find("p1 -> p2 [color=black") != std::string::npos && dot.find("p1 -> p3 [color=black") != std::string::npos, "DAG branch edges missing");
        check(dot.find("p0 -> p1") == std::string::npos && dot.find("p2 -> p3") == std::string::npos, "Independent passes falsely chained");
        const auto json = render_graph_json(plan);
        check(json.find("\"from_order\":1,\"to_order\":2") != std::string::npos && json.find("\"declaration_index\":4") != std::string::npos, "JSON index semantics lost");
        check(json.find("\"original_dependencies_available\": true") != std::string::npos, "Actual edge coverage missing");
        check(plan.culled_passes == std::vector<std::string>({"dead-write"}) && json.find("dead-write") != std::string::npos, "Culled passes lost");
    });
    test("automatic barriers include same-state hazards and exported boundary", [] {
        RenderGraph graph; graph.add_resource("image");
        graph.add_pass("write", {{"image", ResourceAccess::write, ResourceState::storage}}, [] {});
        graph.add_pass("rw", {{"image", ResourceAccess::read_write, ResourceState::storage}}, [] {});
        graph.add_pass("read", {{"image", ResourceAccess::read, ResourceState::shader_read}}, [] {}, true);
        graph.export_resource("image"); const auto plan = graph.compile();
        check(plan.barriers.size() == 3 && plan.barriers[1].before == plan.barriers[1].after && plan.barriers[1].memory_dependency, "Same-state hazard not retained");
        check(plan.lifetimes[0].last == plan.order.size(), "Exported lifetime boundary differs");
        const auto json = render_graph_json(plan);
        check(json.find("\"previous_access\":\"write\",\"next_access\":\"read_write\",\"memory_dependency\":true") != std::string::npos, "Barrier access metadata lost");
        check(json.find("\"last_is_external_boundary\":true") != std::string::npos, "External boundary not marked");
        check(render_graph_pass_list(plan).size() == 3 && render_graph_barrier_list(plan).size() == 3, "UI row counts differ");
        check(render_graph_barrier_list(plan)[1].find("before #1 rw") != std::string::npos, "UI barrier associated with wrong pass");
    });
    test("JSON/DOT escape arbitrary labels and preserve UTF-8", [] {
        RenderGraph graph;
        const std::string name = "中文 %s \"; p999 -> p0; //\\n\n\t\r" + std::string(1, '\1');
        graph.add_pass(name, {}, [] {}, true); const auto plan = graph.compile();
        const auto json = render_graph_json(plan), dot = render_graph_dot(plan);
        check(json.find("中文 %s") != std::string::npos && json.find("\\u0001") != std::string::npos && json.find("\\t\\r") != std::string::npos, "JSON control escaping failed");
        check(json.find("\\\"") != std::string::npos && dot.find("\\\"; p999") != std::string::npos, "Quoted label could escape the string");
        check(dot.find("<0x01>") != std::string::npos && dot.find("\\\\n") != std::string::npos, "DOT control/backslash escaping failed");
    });
    test("empty and imported export-only plans serialize without fake passes", [] {
        RenderGraph empty; check(render_graph_json(empty.compile()).find("\"passes\": [\n  ]") != std::string::npos, "Empty plan failed");
        RenderGraph imported; imported.add_resource("external-image", true, ResourceState::shader_read); imported.export_resource("external-image");
        const auto plan = imported.compile();
        check(plan.order.empty() && plan.lifetimes.size() == 1 && plan.lifetimes[0].last == 0, "Export-only plan fixture differs");
        check(render_graph_dot(plan).find("r0 -> external") != std::string::npos, "Export-only lifetime missing");
        check(render_graph_dot(plan).find("-> p0") == std::string::npos, "Fake pass added to export-only graph");
    });
    test("malformed graph snapshots and suffixes fail explicitly", [] {
        auto plan = graph_fixture(); plan.pass_names.pop_back();
        throws([&] { render_graph_json(plan); }, "Mismatched pass names accepted");
        plan = graph_fixture(); plan.dependencies.emplace_back(4, 0);
        throws([&] { render_graph_dot(plan); }, "Backward dependency accepted");
        plan = graph_fixture(); plan.dependencies.push_back(plan.dependencies.front());
        throws([&] { render_graph_json(plan); }, "Duplicate edge accepted");
        plan = graph_fixture(); plan.barriers[0].before_pass = plan.order.size();
        throws([&] { render_graph_json(plan); }, "Out-of-range barrier accepted");
        plan = graph_fixture(); plan.lifetimes[0].last = plan.order.size() + 1;
        throws([&] { render_graph_json(plan); }, "Bad lifetime accepted");
        throws([&] { save_render_graph(graph_fixture(), "unsupported.svg"); }, "Unknown graph file suffix accepted");
    });
    test("snapshot files retain serial/revision and native Unicode paths", [] {
        TemporaryFiles files; const auto directory = files.root / std::filesystem::path(u8"中文 空格 🎮");
        const GraphSnapshotInfo info{123, 77, "Vulkan deferred", "recorded"};
        const auto paths = save_render_graph_snapshot(graph_fixture(), directory, info);
        check(paths.json.filename() == "frame-000000000123.json" && paths.dot.filename() == "frame-000000000123.dot", "Frame filenames differ");
        check(read(paths.json).find("\"scene_revision\": 77") != std::string::npos && read(paths.dot).find("frame=123") != std::string::npos, "Snapshot metadata not saved");
        RenderGraph graph; graph.add_pass("only", {}, [] {}, true);
        save_render_graph(graph, files.root / "overload.JSON"); check(read(files.root / "overload.JSON").find("only") != std::string::npos, "Graph overload or mixed suffix failed");
        throws([&] { save_render_graph_snapshot(graph_fixture(), {}, info); }, "Empty snapshot directory accepted");
        throws([&] { save_render_graph(graph_fixture(), directory / "frame-000000000123.json" / "child.json"); }, "Write failure hidden");
    });
    test("timing median/nearest-rank p95 and invalid inputs", [] {
        const auto timing = summarize_timings(std::vector<double>{9, 1, 5, 3});
        check(timing.sample_count == 4 && timing.minimum_ms == 1 && timing.median_ms == 4 && timing.p95_ms == 9 && timing.maximum_ms == 9, "Even median/p95 failed");
        std::vector<double> samples(20); std::iota(samples.begin(), samples.end(), 1.0);
        check(summarize_timings(samples).p95_ms == 19 && summarize_timings(std::vector<double>{1, 7, 3}).median_ms == 3, "Quantile indexing failed");
        check(summarize_timings({}).sample_count == 0, "Empty summary not handled");
        throws([] { summarize_timings(std::vector<double>{-1}); }, "Negative timing accepted");
        throws([] { summarize_timings(std::vector<double>{std::numeric_limits<double>::infinity()}); }, "Infinite timing accepted");
        throws([] { summarize_timings(std::vector<double>{std::numeric_limits<double>::quiet_NaN()}); }, "NaN timing accepted");
    });
    test("real visibility benchmark checks every mode, task tails, repeat determinism", [] {
        const auto options = small_benchmark(); const auto report = run_system_benchmark({}, options);
        check(report.rows.size() == 13 && report.all_outputs_match, "Required worker/grain matrix missing or failed");
        check(report.reference_visible_count > 0 && report.reference_visible_count < options.object_count, "Workload has trivial visibility");
        for (const auto& row : report.rows) {
            check(row.samples_ms.size() == 3 && row.timing.sample_count == 3 && row.output_matches, "Measured repetitions/checks missing");
            check(row.checksum == report.reference_checksum && row.visible_count == report.reference_visible_count, "Mode output differs");
            if (row.mode == "jobs") check(row.tasks_per_iteration == (options.object_count - 1) / row.grain + 1, "Grain not used as actual chunk size");
        }
        auto repeat = options; repeat.worker_counts = {1}; repeat.grains = {64};
        check(run_system_benchmark({}, repeat).reference_checksum == report.reference_checksum, "Fixed seed not deterministic");
        repeat.seed += 1; check(run_system_benchmark({}, repeat).reference_checksum != report.reference_checksum, "Seed did not alter real input/output");
        repeat.object_count = 1;
        const auto one = run_system_benchmark({}, repeat); check(one.all_outputs_match && one.rows[1].tasks_per_iteration == 1, "Small bounded workload or large-grain tail failed");
    });
    test("benchmark JSON/CSV/Chinese Markdown and bounds", [] {
        TemporaryFiles files; auto options = small_benchmark(); options.worker_counts = {2}; options.grains = {64};
        const auto report = run_system_benchmark(files.root, options);
        check(read(files.root / "system-benchmark.json").find("\"samples_ms\":[") != std::string::npos, "Raw benchmark samples not saved");
        check(read(files.root / "system-benchmark.csv").find("serial,0,0,0,257,") != std::string::npos, "CSV serial row missing");
        check(read(files.root / "system-benchmark.md").find("加速比小于 1") != std::string::npos, "Performance limitations not explained");
        auto bad = options; bad.object_count = 0; throws([&] { run_system_benchmark({}, bad); }, "Empty work accepted");
        bad = options; bad.object_count = 1048577; throws([&] { run_system_benchmark({}, bad); }, "Unbounded objects accepted");
        bad = options; bad.warmup_iterations = 0; throws([&] { run_system_benchmark({}, bad); }, "No warmup accepted");
        bad = options; bad.measured_iterations = 2; throws([&] { run_system_benchmark({}, bad); }, "Insufficient samples accepted");
        bad = options; bad.grains = {0}; throws([&] { run_system_benchmark({}, bad); }, "Zero grain accepted");
        bad = options; bad.worker_counts = {65}; throws([&] { run_system_benchmark({}, bad); }, "Out-of-bounds threads accepted");
        bad = options; bad.worker_counts = {1, 1}; throws([&] { run_system_benchmark({}, bad); }, "Duplicate modes accepted");
    });
    test("GPU sample serialization requires unique completed serials", [] {
        GpuTimingReport report; report.gpu_name = "GPU\n\"device\""; report.render_path = "deferred"; report.samples = {{31, 4}, {33, 2}, {32, 3}};
        const auto json = gpu_timings_json(report);
        check(json.find("\"median_ms\":3") != std::string::npos && json.find("\"p95_ms\":4") != std::string::npos, "GPU distribution wrong");
        check(json.find("\"frame_serial\":33") != std::string::npos && json.find("GPU\\n\\\"device\\\"") != std::string::npos, "GPU samples/name escaping missing");
        report.samples.push_back({31, 1}); throws([&] { gpu_timings_json(report); }, "Repeated stale timestamp accepted");
        report.samples = {{0, 1}}; throws([&] { gpu_timings_json(report); }, "Unidentified frame accepted");
        report.samples = {{1, -1}}; throws([&] { gpu_timings_json(report); }, "Unavailable timestamp fabricated into sample");
        report.samples.clear(); check(gpu_timings_json(report).find("\"summary\":null") != std::string::npos, "Unavailable GPU timings presented as zero");
    });
    test("failure diagnostics preserve multiline evidence and artifact path", [] {
        TemporaryFiles files;
        const std::vector<DiagnosticRecord> records{{"shader", "compile \"failure\"", "error", "line1\nline2\t\1", files.root / std::filesystem::path(u8"失败 shader.log"), 12}};
        export_diagnostics(records, files.root / "diagnostics.json", "test session"); const auto json = read(files.root / "diagnostics.json");
        check(json.find("line1\\nline2\\t\\u0001") != std::string::npos && json.find("失败 shader.log") != std::string::npos, "Failure evidence escaped incorrectly");
        check(json.find("\"frame_serial\":12") != std::string::npos && diagnostics_json({}).find("\"records\":[\n  ]") != std::string::npos, "Failure metadata/empty records failed");
    });
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_DIAGNOSTICS_TEST_MAIN
int main(int argc, char** argv) {
    using namespace emberframe::lab;
    try {
        if (argc > 1 && (std::string_view(argv[1]) == "benchmark" || std::string_view(argv[1]) == "export-fixtures")) {
            const std::string_view directory_text = argc > 2 ? argv[2] : "diagnostics-benchmark";
            const auto directory = std::filesystem::path(std::u8string(directory_text.begin(), directory_text.end()));
            if (std::string_view(argv[1]) == "export-fixtures") {
                save_render_graph_snapshot(graph_fixture(), directory / "graph", {24, 9, "test fixture", "recorded"});
                RenderGraph hostile; const std::string name = "中文 \"; p999 -> p0; //\\n\n\t\r" + std::string(1, '\1');
                hostile.add_resource(name); hostile.add_pass(name, {{name, ResourceAccess::write, ResourceState::storage}}, [] {});
                hostile.add_pass("finish", {{name, ResourceAccess::read, ResourceState::shader_read}}, [] {}, true);
                hostile.export_resource(name);
                save_render_graph_snapshot(hostile.compile(), directory / "graph", {25, 10, "escape fixture", "recorded"});
                GpuTimingReport gpu; gpu.gpu_name = "test fixture\n\"GPU\""; gpu.render_path = "deferred"; gpu.samples = {{31, 4}, {32, 3}, {33, 2}};
                save_gpu_timings(gpu, directory / "gpu-timings.json");
                const std::vector<DiagnosticRecord> errors{{"fixture", "quoted \"failure\"", "error", "line1\nline2\t\1", directory / "error.log", 25}};
                export_diagnostics(errors, directory / "diagnostics.json", "fixture export (not GPU measurement)");
                run_system_benchmark(directory, small_benchmark());
                std::cout << "Diagnostic schema fixtures exported\n"; return 0;
            }
            const auto report = run_system_benchmark(directory);
            std::cout << system_benchmark_markdown(report); return report.all_outputs_match ? 0 : 1;
        }
        std::size_t failed = 0; const auto results = test_diagnostics();
        for (const auto& result : results) {
            std::cout << (result.passed ? "PASS " : "FAIL ") << result.name << " | " << result.detail << '\n'; failed += !result.passed;
        }
        std::cout << "RESULT passed=" << results.size() - failed << " failed=" << failed << '\n'; return failed ? 1 : 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
#endif
