#include "diagnostics.h"
#include <bit>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <thread>

namespace emberframe::lab {
namespace {
using Clock = std::chrono::steady_clock;
std::string path_utf8(const std::filesystem::path& path) {
    const auto s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}
std::ostringstream stream() {
    std::ostringstream out; out.imbue(std::locale::classic());
    out << std::setprecision(17); return out;
}
std::string json_quote(std::string_view text) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
            else out += char(c);
        }
    }
    return out + '"';
}
std::string dot_quote(std::string_view text) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "    ";
        else if (c < 32 || c == 127) { out += "<0x"; out += hex[c >> 4]; out += hex[c & 15]; out += '>'; }
        else out += char(c);
    }
    return out + '"';
}
std::string_view state_name(ResourceState state) {
    switch (state) {
    case ResourceState::undefined: return "undefined";
    case ResourceState::shader_read: return "shader_read";
    case ResourceState::color_attachment: return "color_attachment";
    case ResourceState::depth_attachment: return "depth_attachment";
    case ResourceState::storage: return "storage";
    case ResourceState::transfer_src: return "transfer_src";
    case ResourceState::transfer_dst: return "transfer_dst";
    case ResourceState::present: return "present";
    case ResourceState::host_read: return "host_read";
    }
    throw std::invalid_argument("Unknown graph resource state");
}
std::string_view access_name(ResourceAccess access) {
    switch (access) {
    case ResourceAccess::read: return "read";
    case ResourceAccess::write: return "write";
    case ResourceAccess::read_write: return "read_write";
    }
    throw std::invalid_argument("Unknown graph resource access");
}
void write_text(const std::filesystem::path& path, const std::string& text) {
    if (path.empty()) throw std::invalid_argument("Empty diagnostics output path");
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    // 二进制写 UTF-8；不受中文文件名、系统代码页、区域小数逗号影响。
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("Cannot open diagnostics output: " + path_utf8(path));
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    if (!file) throw std::runtime_error("Cannot write diagnostics output: " + path_utf8(path));
    file.close();
    if (file.fail()) throw std::runtime_error("Cannot close diagnostics output: " + path_utf8(path));
}
void validate_plan(const GraphPlan& plan) {
    const auto n = plan.order.size();
    if (plan.pass_names.size() != n) throw std::invalid_argument("Graph order/name count differs");
    std::set<std::size_t> ids;
    std::set<std::string> names;
    for (std::size_t i = 0; i < n; ++i) {
        if (!ids.insert(plan.order[i]).second || plan.pass_names[i].empty() || !names.insert(plan.pass_names[i]).second)
            throw std::invalid_argument("Duplicate/empty graph pass");
    }
    std::set<std::pair<std::size_t, std::size_t>> dependencies;
    for (const auto& edge : plan.dependencies)
        if (edge.first >= n || edge.second >= n || edge.first >= edge.second || !dependencies.insert(edge).second)
            throw std::invalid_argument("Invalid/duplicate graph dependency in topological order");
    std::map<std::string, ResourceLifetime> resources;
    for (const auto& life : plan.lifetimes) {
        if (life.resource.empty() || !resources.emplace(life.resource, life).second || life.first > life.last ||
            (life.exported ? life.last != n : life.last >= n) || (n ? life.first >= n : !life.imported || !life.exported || life.first != 0))
            throw std::invalid_argument("Invalid graph resource lifetime");
    }
    std::size_t previous_position = 0;
    std::map<std::string, ResourceState> prior_state;
    for (const auto& barrier : plan.barriers) {
        const auto it = resources.find(barrier.resource);
        if (it == resources.end() || barrier.before_pass >= n || barrier.before_pass < previous_position ||
            barrier.before_pass < it->second.first || barrier.before_pass > it->second.last || barrier.after == ResourceState::undefined)
            throw std::invalid_argument("Invalid graph barrier position/resource");
        (void)state_name(barrier.before); (void)state_name(barrier.after);
        (void)access_name(barrier.previous_access); (void)access_name(barrier.next_access);
        if (auto previous = prior_state.find(barrier.resource); previous != prior_state.end() && previous->second != barrier.before)
            throw std::invalid_argument("Inconsistent graph barrier state chain");
        prior_state[barrier.resource] = barrier.after;
        previous_position = barrier.before_pass;
    }
}
std::string barrier_text(const ResourceBarrier& b) {
    auto out = stream();
    out << b.resource << ": " << state_name(b.before) << " -> " << state_name(b.after)
        << " | " << access_name(b.previous_access) << " -> " << access_name(b.next_access)
        << " | memory_dependency=" << (b.memory_dependency ? "true" : "false");
    return out.str();
}
void append_summary(std::ostream& out, const TimingSummary& timing) {
    out << "{\"sample_count\":" << timing.sample_count << ",\"minimum_ms\":" << timing.minimum_ms
        << ",\"median_ms\":" << timing.median_ms << ",\"p95_ms\":" << timing.p95_ms
        << ",\"maximum_ms\":" << timing.maximum_ms << '}';
}
std::string checksum_text(std::uint64_t checksum) {
    auto out = stream(); out << std::hex << std::setw(16) << std::setfill('0') << checksum; return out.str();
}

struct SphereObject { glm::mat4 model{1}; glm::vec4 local_sphere{0, 0, 0, 1}; };
struct VisibilityResult {
    glm::vec4 world_center{0}, clip_center{0};
    float world_radius = 0;
    std::uint32_t outside_mask = 0;
};
struct VisibilityWorkload {
    std::vector<SphereObject> objects;
    glm::mat4 view_projection{1};
    std::array<glm::vec4, 6> planes;
};
class FixedRandom {
public:
    explicit FixedRandom(std::uint32_t seed) : state_(seed) {}
    float range(float lower, float upper) {
        state_ = 1664525u * state_ + 1013904223u;
        // 保留 24 位后缩放，规避各标准库 uniform_real_distribution 的实现差异。
        return lower + (upper - lower) * (float(state_ >> 8) * (1.f / 16777216.f));
    }
private:
    std::uint32_t state_;
};
VisibilityWorkload make_workload(std::size_t count, std::uint32_t seed) {
    VisibilityWorkload work;
    Camera camera; camera.position = {0, 4, 12}; camera.target = {0, 0, -25};
    camera.near_plane = .1f; camera.far_plane = 180; camera.fov = 65;
    work.view_projection = camera.projection(16.f / 9.f, false) * camera.view();
    const auto& m = work.view_projection;
    const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
    const glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
    const glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
    const glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);
    // 与 Camera 的 RH / 深度 [0,1] 约定一致；近平面是 row2，不是 row3+row2。
    work.planes = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2};
    for (auto& plane : work.planes) plane /= glm::length(glm::vec3(plane));
    work.objects.reserve(count);
    FixedRandom random(seed);
    for (std::size_t i = 0; i < count; ++i) {
        const glm::vec3 position{random.range(-130, 130), random.range(-80, 80), random.range(-210, 45)};
        const glm::vec3 scale{random.range(.3f, 3.f), random.range(.3f, 3.f), random.range(.3f, 3.f)};
        const float angle = random.range(-pi, pi);
        SphereObject object;
        object.model = glm::translate(glm::mat4(1), position) * glm::rotate(glm::mat4(1), angle, glm::vec3(0, 1, 0)) * glm::scale(glm::mat4(1), scale);
        object.local_sphere = {random.range(-.5f, .5f), random.range(-.5f, .5f), random.range(-.5f, .5f), random.range(.05f, 1.25f)};
        work.objects.push_back(object);
    }
    return work;
}
void evaluate_visibility(const VisibilityWorkload& work, std::size_t i, VisibilityResult& out) {
    const auto& object = work.objects[i];
    const glm::vec4 local_center(glm::vec3(object.local_sphere), 1);
    out.world_center = object.model * local_center;
    const glm::mat4 clip_from_local = work.view_projection * object.model;
    out.clip_center = clip_from_local * local_center;
    out.world_radius = object.local_sphere.w * std::max({glm::length(glm::vec3(object.model[0])),
                                                      glm::length(glm::vec3(object.model[1])), glm::length(glm::vec3(object.model[2]))});
    out.outside_mask = 0;
    // 每个对象六个平面都计算；不同可见性不改变此工作量，也不制造空任务。
    for (std::size_t p = 0; p < work.planes.size(); ++p)
        if (glm::dot(work.planes[p], out.world_center) < -out.world_radius) out.outside_mask |= 1u << p;
}
void validate_visibility_math() {
    // 独立已知答案：单位立方体六面、球切面、非均匀/负缩放。
    // 不能只让串行和并行共同复现一个错误后仍宣布校验通过；此检查不进入计时。
    VisibilityWorkload cube; cube.objects.resize(1);
    cube.planes = {glm::vec4(1, 0, 0, 1), glm::vec4(-1, 0, 0, 1), glm::vec4(0, 1, 0, 1),
                   glm::vec4(0, -1, 0, 1), glm::vec4(0, 0, 1, 1), glm::vec4(0, 0, -1, 1)};
    cube.objects[0].local_sphere = {0, 0, 0, .25f};
    const std::array<glm::vec3, 6> outside{glm::vec3(-2, 0, 0), glm::vec3(2, 0, 0), glm::vec3(0, -2, 0),
                                         glm::vec3(0, 2, 0), glm::vec3(0, 0, -2), glm::vec3(0, 0, 2)};
    VisibilityResult result;
    evaluate_visibility(cube, 0, result);
    if (result.outside_mask != 0 || result.world_center != glm::vec4(0, 0, 0, 1) || result.clip_center != result.world_center || result.world_radius != .25f)
        throw std::logic_error("Visibility preflight: identity/inside sphere failed");
    for (std::size_t p = 0; p < 6; ++p) {
        cube.objects[0].model = glm::translate(glm::mat4(1), outside[p]); evaluate_visibility(cube, 0, result);
        if (result.outside_mask != (1u << p)) throw std::logic_error("Visibility preflight: six-plane outside mask failed");
    }
    cube.objects[0].model = glm::translate(glm::mat4(1), glm::vec3(1.25f, 0, 0)); evaluate_visibility(cube, 0, result);
    if (result.outside_mask != 0) throw std::logic_error("Visibility preflight: touching sphere rejected");
    cube.objects[0].model = glm::scale(glm::mat4(1), glm::vec3(-2, 3, -4));
    cube.objects[0].local_sphere = {.25f, -.25f, .25f, .125f}; evaluate_visibility(cube, 0, result);
    if (result.world_center != glm::vec4(-.5f, -.75f, -1, 1) || result.clip_center != result.world_center || result.world_radius != .5f || result.outside_mask != 0)
        throw std::logic_error("Visibility preflight: transformed sphere failed");
    // 零半径点在 Vulkan [0,1] 视锥内，须与独立齐次 clip 判定一致。
    auto camera_work = make_workload(1, 1);
    const std::array<glm::vec3, 8> points{glm::vec3(0, 0, -25), glm::vec3(0, 4, 13), glm::vec3(0, 4, -300), glm::vec3(200, 0, -25),
                                       glm::vec3(-200, 0, -25), glm::vec3(0, 200, -25), glm::vec3(0, -200, -25), glm::vec3(1, 1, -20)};
    for (const auto point : points) {
        camera_work.objects[0].model = glm::translate(glm::mat4(1), point); camera_work.objects[0].local_sphere = {0, 0, 0, 0};
        evaluate_visibility(camera_work, 0, result); const auto c = result.clip_center;
        const bool inside = -c.w <= c.x && c.x <= c.w && -c.w <= c.y && c.y <= c.w && 0 <= c.z && c.z <= c.w;
        if ((result.outside_mask == 0) != inside) throw std::logic_error("Visibility preflight: Vulkan clip/plane conventions differ");
    }
}
std::pair<std::size_t, std::uint64_t> fingerprint(std::span<const VisibilityResult> output) {
    std::uint64_t checksum = 14695981039346656037ull;
    std::size_t visible = 0;
    auto word = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) { checksum ^= (value >> shift) & 255u; checksum *= 1099511628211ull; }
    };
    for (const auto& result : output) {
        visible += result.outside_mask == 0;
        for (int c = 0; c < 4; ++c) word(std::bit_cast<std::uint32_t>(result.world_center[c]));
        for (int c = 0; c < 4; ++c) word(std::bit_cast<std::uint32_t>(result.clip_center[c]));
        word(std::bit_cast<std::uint32_t>(result.world_radius)); word(result.outside_mask);
    }
    return {visible, checksum};
}
void poison(std::span<VisibilityResult> results) {
    for (auto& result : results) {
        result.world_center = result.clip_center = glm::vec4(std::numeric_limits<float>::quiet_NaN());
        result.world_radius = std::numeric_limits<float>::quiet_NaN(); result.outside_mask = ~0u;
    }
}
void check_output(std::span<const VisibilityResult> expected, std::span<const VisibilityResult> actual) {
    if (expected.size() != actual.size()) throw std::runtime_error("Visibility output size differs");
    auto same = [](float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); };
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto& a = actual[i]; const auto& e = expected[i];
        bool matches = a.outside_mask == e.outside_mask && same(a.world_radius, e.world_radius);
        for (int c = 0; c < 4; ++c) matches = matches && same(a.world_center[c], e.world_center[c]) && same(a.clip_center[c], e.clip_center[c]);
        if (!matches) throw std::runtime_error("Visibility result differs at object " + std::to_string(i));
    }
}
void validate_options(const SystemBenchmarkOptions& o) {
    if (o.object_count < 1 || o.object_count > 1048576 || o.warmup_iterations < 1 || o.warmup_iterations > 32 ||
        o.measured_iterations < 3 || o.measured_iterations > 100 || o.worker_counts.empty() || o.worker_counts.size() > 8 ||
        o.grains.empty() || o.grains.size() > 8 || o.worker_counts.size() * o.grains.size() > 32)
        throw std::invalid_argument("System benchmark exceeds bounded options (objects 1..1048576, warmup 1..32, samples 3..100, modes <=32)");
    std::set<std::size_t> workers, grains;
    for (auto n : o.worker_counts) if (n < 1 || n > 64 || !workers.insert(n).second) throw std::invalid_argument("Invalid/duplicate benchmark worker count");
    for (auto n : o.grains) if (n < 1 || n > 1048576 || !grains.insert(n).second) throw std::invalid_argument("Invalid/duplicate benchmark grain");
}
void validate_report(const SystemBenchmarkReport& report) {
    validate_options(report.options);
    if (report.rows.empty()) throw std::invalid_argument("Benchmark report has no rows");
    for (const auto& row : report.rows) {
        if (row.samples_ms.size() != report.options.measured_iterations || !std::isfinite(row.speedup_over_serial) || row.speedup_over_serial < 0)
            throw std::invalid_argument("Invalid benchmark samples/speedup");
        const auto summary = summarize_timings(row.samples_ms);
        if (summary.sample_count != row.timing.sample_count || summary.minimum_ms != row.timing.minimum_ms ||
            summary.median_ms != row.timing.median_ms || summary.p95_ms != row.timing.p95_ms || summary.maximum_ms != row.timing.maximum_ms)
            throw std::invalid_argument("Benchmark summary does not match raw samples");
    }
}
} // namespace

std::string render_graph_json(const GraphPlan& plan, const GraphSnapshotInfo& info) {
    validate_plan(plan); auto out = stream(); const auto n = plan.order.size();
    out << "{\n  \"schema\": \"emberframe.render_graph.v1\",\n  \"frame_serial\": " << info.frame_serial
        << ",\n  \"scene_revision\": " << info.scene_revision << ",\n  \"backend\": " << json_quote(info.backend)
        << ",\n  \"status\": " << json_quote(info.status)
        << ",\n  \"original_dependencies_available\": true,\n"
           "  \"dependency_coverage\": \"compiler_edges_between_active_passes\",\n"
           "  \"dependency_types_available\": false,\n"
           "  \"scope\": \"GraphPlan only; no Vulkan masks, subresources, alias allocation, post/UI/present outside this plan\",\n"
           "  \"lifetime_index_semantics\": \"first/last index compiled order; exported last=pass_count denotes external consumer boundary\",\n"
           "  \"passes\": [";
    for (std::size_t i = 0; i < n; ++i) {
        out << (i ? ",\n" : "\n") << "    {\"id\":\"p" << i << "\",\"order_index\":" << i << ",\"declaration_index\":" << plan.order[i]
            << ",\"name\":" << json_quote(plan.pass_names[i]) << ",\"barrier_ids\":[";
        bool first = true;
        for (std::size_t b = 0; b < plan.barriers.size(); ++b) if (plan.barriers[b].before_pass == i) {
            out << (first ? "" : ",") << "\"b" << b << '"'; first = false;
        }
        out << "]}";
    }
    out << "\n  ],\n  \"culled_passes\": [";
    for (std::size_t i = 0; i < plan.culled_passes.size(); ++i) out << (i ? "," : "") << json_quote(plan.culled_passes[i]);
    out << "],\n  \"resources\": [";
    for (std::size_t i = 0; i < plan.lifetimes.size(); ++i) {
        const auto& life = plan.lifetimes[i];
        out << (i ? ",\n" : "\n") << "    {\"id\":\"r" << i << "\",\"name\":" << json_quote(life.resource)
            << ",\"first\":" << life.first << ",\"last\":" << life.last << ",\"imported\":" << (life.imported ? "true" : "false")
            << ",\"exported\":" << (life.exported ? "true" : "false") << ",\"last_is_external_boundary\":" << (life.last == n ? "true" : "false") << '}';
    }
    out << "\n  ],\n  \"barriers\": [";
    for (std::size_t i = 0; i < plan.barriers.size(); ++i) {
        const auto& b = plan.barriers[i];
        out << (i ? ",\n" : "\n") << "    {\"id\":\"b" << i << "\",\"resource\":" << json_quote(b.resource) << ",\"before_pass\":" << b.before_pass
            << ",\"pass_name\":" << json_quote(plan.pass_names[b.before_pass]) << ",\"before\":" << json_quote(state_name(b.before))
            << ",\"after\":" << json_quote(state_name(b.after)) << ",\"previous_access\":" << json_quote(access_name(b.previous_access))
            << ",\"next_access\":" << json_quote(access_name(b.next_access)) << ",\"memory_dependency\":" << (b.memory_dependency ? "true" : "false") << '}';
    }
    out << "\n  ],\n  \"dependencies\": [";
    bool first = true;
    for (const auto& [from, to] : plan.dependencies) {
        out << (first ? "\n" : ",\n") << "    {\"from\":\"p" << from << "\",\"to\":\"p" << to
            << "\",\"from_order\":" << from << ",\"to_order\":" << to << ",\"kind\":\"compiled_dependency\"}";
        first = false;
    }
    out << "\n  ],\n  \"barrier_applications\":[";
    for (std::size_t i = 0; i < plan.barriers.size(); ++i) {
        const auto& b = plan.barriers[i];
        out << (i ? ",\n" : "\n") << "    {\"barrier\":\"b" << i << "\",\"pass\":\"p" << b.before_pass << "\"}";
    }
    out << "\n  ]\n}\n"; return out.str();
}

std::string render_graph_dot(const GraphPlan& plan, const GraphSnapshotInfo& info) {
    validate_plan(plan); auto out = stream(); const auto n = plan.order.size();
    out << "digraph EmberFrameRenderGraph {\n  graph [rankdir=LR, charset=\"UTF-8\", label="
        << dot_quote("Compiled GraphPlan | frame=" + std::to_string(info.frame_serial) + " | " + info.backend + " | " + info.status +
                     "\nSolid black: real compiled dependencies. Blue: barrier application. Dotted: lifetime endpoints. No synthetic pass chain.")
        << "];\n  node [fontname=\"sans-serif\"];\n";
    for (std::size_t i = 0; i < n; ++i)
        out << "  p" << i << " [shape=box, label=" << dot_quote("#" + std::to_string(i) + " " + plan.pass_names[i] + "\ndeclaration=" + std::to_string(plan.order[i])) << "];\n";
    for (const auto& [from, to] : plan.dependencies)
        out << "  p" << from << " -> p" << to << " [color=black, label=\"compiled dependency\"];\n";
    if (std::any_of(plan.lifetimes.begin(), plan.lifetimes.end(), [](const auto& life) { return life.exported; }))
        out << "  external [shape=oval, label=\"external consumer boundary\"];\n";
    for (std::size_t i = 0; i < plan.lifetimes.size(); ++i) {
        const auto& life = plan.lifetimes[i];
        out << "  r" << i << " [shape=note, label=" << dot_quote(life.resource + "\nlifetime [" + std::to_string(life.first) + "," +
            std::to_string(life.last) + "]" + (life.imported ? " imported" : "") + (life.exported ? " exported" : "")) << "];\n";
        if (n) out << "  r" << i << " -> p" << life.first << " [style=dotted, color=gray, constraint=false, label=\"lifetime start\"];\n";
        if (life.exported) out << "  r" << i << " -> external [style=dotted, color=gray, label=\"lifetime end\"];\n";
        else out << "  p" << life.last << " -> r" << i << " [style=dotted, color=gray, constraint=false, label=\"lifetime end\"];\n";
    }
    for (std::size_t i = 0; i < plan.barriers.size(); ++i) {
        const auto& b = plan.barriers[i];
        out << "  b" << i << " [shape=ellipse, color=blue, label=" << dot_quote("barrier #" + std::to_string(i) + "\n" + barrier_text(b)) << "];\n"
            << "  b" << i << " -> p" << b.before_pass << " [color=blue, label=\"applies before\"];\n";
    }
    for (std::size_t i = 0; i < plan.culled_passes.size(); ++i)
        out << "  c" << i << " [shape=box, color=gray, style=dashed, label=" << dot_quote("culled: " + plan.culled_passes[i]) << "];\n";
    out << "}\n"; return out.str();
}
std::vector<std::string> render_graph_pass_list(const GraphPlan& plan) {
    validate_plan(plan); std::vector<std::string> rows;
    for (std::size_t i = 0; i < plan.order.size(); ++i) {
        const auto count = std::count_if(plan.barriers.begin(), plan.barriers.end(), [i](const auto& b) { return b.before_pass == i; });
        rows.push_back("#" + std::to_string(i) + " " + plan.pass_names[i] + " | declaration=" + std::to_string(plan.order[i]) + " | barriers=" + std::to_string(count));
    }
    return rows;
}
std::vector<std::string> render_graph_barrier_list(const GraphPlan& plan) {
    validate_plan(plan); std::vector<std::string> rows;
    for (const auto& b : plan.barriers)
        rows.push_back("before #" + std::to_string(b.before_pass) + " " + plan.pass_names[b.before_pass] + " | " + barrier_text(b));
    return rows;
}
void save_render_graph(const GraphPlan& plan, const std::filesystem::path& path, const GraphSnapshotInfo& info) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c); });
    if (extension == ".json") write_text(path, render_graph_json(plan, info));
    else if (extension == ".dot") write_text(path, render_graph_dot(plan, info));
    else throw std::invalid_argument("Render graph output must have .json or .dot suffix");
}
void save_render_graph(const RenderGraph& graph, const std::filesystem::path& path, const GraphSnapshotInfo& info) {
    save_render_graph(graph.compile(), path, info);
}
GraphSnapshotFiles save_render_graph_snapshot(const GraphPlan& plan, const std::filesystem::path& directory, const GraphSnapshotInfo& info) {
    if (directory.empty()) throw std::invalid_argument("Empty graph snapshot directory");
    auto name = stream(); name << "frame-" << std::setw(12) << std::setfill('0') << info.frame_serial;
    const GraphSnapshotFiles paths{directory / (name.str() + ".json"), directory / (name.str() + ".dot")};
    // 两个字符串先完成校验/生成，再写文件；失败通过异常上报，不声称整体原子保存。
    const auto json = render_graph_json(plan, info), dot = render_graph_dot(plan, info);
    write_text(paths.json, json); write_text(paths.dot, dot); return paths;
}

TimingSummary summarize_timings(std::span<const double> milliseconds) {
    if (milliseconds.empty()) return {};
    std::vector<double> values(milliseconds.begin(), milliseconds.end());
    for (const auto value : values) if (!std::isfinite(value) || value < 0) throw std::invalid_argument("Timing sample is negative or non-finite");
    std::sort(values.begin(), values.end()); const auto n = values.size();
    const double median = n % 2 ? values[n / 2] : values[n / 2 - 1] * .5 + values[n / 2] * .5;
    const auto rank = static_cast<std::size_t>(std::ceil(.95 * double(n)));
    return {n, values.front(), median, values[rank - 1], values.back()};
}

SystemBenchmarkReport run_system_benchmark(const std::filesystem::path& output_directory, const SystemBenchmarkOptions& options) {
    validate_options(options);
    validate_visibility_math();
    SystemBenchmarkReport report; report.options = options; report.hardware_threads = std::thread::hardware_concurrency();
#ifdef _MSC_VER
    report.compiler = "MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    report.compiler = "Clang " __clang_version__;
#elif defined(__GNUC__)
    report.compiler = "GCC " __VERSION__;
#else
    report.compiler = "unknown";
#endif
#ifdef NDEBUG
    report.build = "NDEBUG (record optimization flags separately)";
#else
    report.build = "assertions enabled (record optimization flags separately)";
#endif
    const auto work = make_workload(options.object_count, options.seed);
    std::vector<VisibilityResult> reference(options.object_count), output(options.object_count);
    for (std::size_t i = 0; i < reference.size(); ++i) evaluate_visibility(work, i, reference[i]);
    const auto [visible, checksum] = fingerprint(reference);
    report.reference_visible_count = visible; report.reference_checksum = checksum;
    report.input_bytes = work.objects.size() * sizeof(SphereObject); report.output_bytes = output.size() * sizeof(VisibilityResult);
    auto measure = [&](SystemBenchmarkRow row, auto&& iteration) {
        row.samples_ms.reserve(options.measured_iterations);
        for (std::size_t round = 0; round < options.warmup_iterations + options.measured_iterations; ++round) {
            poison(output); const auto start = Clock::now(); iteration(); const auto end = Clock::now();
            check_output(reference, output);
            if (round >= options.warmup_iterations) row.samples_ms.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        }
        const auto [actual_visible, actual_checksum] = fingerprint(output);
        row.visible_count = actual_visible; row.checksum = actual_checksum; row.output_matches = visible == actual_visible && checksum == actual_checksum;
        if (!row.output_matches) throw std::runtime_error("Visibility benchmark fingerprint mismatch");
        row.timing = summarize_timings(row.samples_ms); report.rows.push_back(std::move(row));
    };
    SystemBenchmarkRow serial; serial.mode = "serial";
    measure(serial, [&] { for (std::size_t i = 0; i < output.size(); ++i) evaluate_visibility(work, i, output[i]); });
    for (const auto workers : options.worker_counts) {
        JobSystem jobs(workers, 4096); // 建池和销毁不计时，每个 grain 有独立预热。
        for (const auto grain : options.grains) {
            SystemBenchmarkRow row; row.mode = "jobs"; row.workers = jobs.thread_count(); row.grain = grain;
            row.tasks_per_iteration = (options.object_count - 1) / grain + 1;
            std::vector<JobSystem::Task> tasks; tasks.reserve(row.tasks_per_iteration);
            measure(row, [&] {
                tasks.clear(); std::exception_ptr failure;
                try {
                    for (std::size_t begin = 0; begin < output.size();) {
                        const auto end = begin + std::min(grain, output.size() - begin);
                        tasks.push_back(jobs.submit([&work, &output, begin, end] {
                            for (auto i = begin; i < end; ++i) evaluate_visibility(work, i, output[i]);
                        }));
                        begin = end;
                    }
                } catch (...) { failure = std::current_exception(); }
                // 无论提交或执行是否失败，都等已接受任务结束，避免访问离开作用域的数据。
                for (const auto& task : tasks) try { task.get(); } catch (...) { if (!failure) failure = std::current_exception(); }
                if (failure) std::rethrow_exception(failure);
            });
        }
        jobs.wait_idle();
    }
    const auto baseline = report.rows.front().timing.median_ms;
    for (auto& row : report.rows) row.speedup_over_serial = row.timing.median_ms > 0 ? baseline / row.timing.median_ms : 0;
    report.all_outputs_match = std::all_of(report.rows.begin(), report.rows.end(), [](const auto& row) { return row.output_matches; });
    if (!output_directory.empty()) save_system_benchmark(report, output_directory);
    return report;
}

std::string system_benchmark_json(const SystemBenchmarkReport& report) {
    validate_report(report); auto out = stream(); const auto& o = report.options;
    out << "{\n  \"schema\":\"emberframe.system_benchmark.v1\",\n  \"workload\":\"transformed_spheres_six_plane_visibility_and_clip_matrices\",\n"
           "  \"timer\":\"steady_clock_wall_time_submit_compute_wait\",\n"
           "  \"excluded\":[\"generation\",\"thread_creation\",\"output_poison\",\"full_output_validation\",\"file_io\"],\n"
           "  \"validation\":\"every_warmup_and_sample_all_output_fields_float_bits_match_serial\",\n"
           "  \"preflight\":\"known_six_plane_masks_touching_scaled_spheres_and_vulkan_clip_point_oracle\",\n"
           "  \"scheduling\":\"submit_exact_grain_chunks_bounded_pending_4096\",\n"
           "  \"p95_method\":\"nearest_rank\",\n  \"object_count\":" << o.object_count << ",\n  \"seed\":" << o.seed
        << ",\n  \"warmup_iterations\":" << o.warmup_iterations << ",\n  \"measured_iterations\":" << o.measured_iterations
        << ",\n  \"hardware_threads\":" << report.hardware_threads << ",\n  \"compiler\":" << json_quote(report.compiler)
        << ",\n  \"build\":" << json_quote(report.build) << ",\n  \"input_bytes\":" << report.input_bytes << ",\n  \"output_bytes\":" << report.output_bytes
        << ",\n  \"reference_visible_count\":" << report.reference_visible_count << ",\n  \"reference_checksum_hex\":" << json_quote(checksum_text(report.reference_checksum))
        << ",\n  \"all_outputs_match\":" << (report.all_outputs_match ? "true" : "false") << ",\n  \"rows\":[";
    for (std::size_t i = 0; i < report.rows.size(); ++i) {
        const auto& row = report.rows[i];
        out << (i ? ",\n" : "\n") << "    {\"mode\":" << json_quote(row.mode) << ",\"workers\":" << row.workers << ",\"grain\":" << row.grain
            << ",\"tasks_per_iteration\":" << row.tasks_per_iteration << ",\"timing\":"; append_summary(out, row.timing);
        out << ",\"speedup_over_serial\":" << row.speedup_over_serial << ",\"visible_count\":" << row.visible_count
            << ",\"checksum_hex\":" << json_quote(checksum_text(row.checksum)) << ",\"output_matches\":" << (row.output_matches ? "true" : "false") << ",\"samples_ms\":[";
        for (std::size_t s = 0; s < row.samples_ms.size(); ++s) out << (s ? "," : "") << row.samples_ms[s];
        out << "]}";
    }
    out << "\n  ]\n}\n"; return out.str();
}
std::string system_benchmark_csv(const SystemBenchmarkReport& report) {
    validate_report(report); auto out = stream();
    out << "mode,workers,grain,tasks_per_iteration,object_count,seed,warmup_iterations,sample_count,minimum_ms,median_ms,p95_ms,maximum_ms,speedup_over_serial,visible_count,checksum_hex,output_matches\n";
    for (const auto& row : report.rows) {
        if (row.mode != "serial" && row.mode != "jobs") throw std::invalid_argument("Unknown benchmark mode");
        out << row.mode << ',' << row.workers << ',' << row.grain << ',' << row.tasks_per_iteration << ',' << report.options.object_count << ',' << report.options.seed << ','
            << report.options.warmup_iterations << ',' << row.timing.sample_count << ',' << row.timing.minimum_ms << ',' << row.timing.median_ms << ','
            << row.timing.p95_ms << ',' << row.timing.maximum_ms << ',' << row.speedup_over_serial << ',' << row.visible_count << ',' << checksum_text(row.checksum) << ','
            << (row.output_matches ? "true" : "false") << '\n';
    }
    return out.str();
}
std::string system_benchmark_markdown(const SystemBenchmarkReport& report) {
    validate_report(report); auto out = stream(); out << std::fixed << std::setprecision(3);
    out << "# EmberFrame 可见性工作基准\n\n对象数 " << report.options.object_count << "，固定种子 " << report.options.seed << "；每模式预热 " << report.options.warmup_iterations
        << " 轮，实测 " << report.options.measured_iterations << " 轮。硬件逻辑线程数 " << report.hardware_threads << "。\n\n"
           "每个对象执行世界矩阵与 clip 矩阵运算、非均匀缩放包围球和六平面距离测试。计时包含 JobSystem 提交、实际计算和等待，"
           "不包含造场景、建线程、校验和写文件。每轮开始清毒输出，每轮结束逐字段逐 float 位与串行参考比较。校验覆盖预热轮。\n\n"
           "| 模式 | 工作线程 | grain | 任务/轮 | 中位数 ms | p95 ms | 串行/本模式 | 可见对象 | 校验 |\n"
           "|---|---:|---:|---:|---:|---:|---:|---:|---|\n";
    for (const auto& row : report.rows)
        out << "| " << row.mode << " | " << row.workers << " | " << row.grain << " | " << row.tasks_per_iteration << " | " << row.timing.median_ms << " | " << row.timing.p95_ms
            << " | " << row.speedup_over_serial << " | " << row.visible_count << " | " << (row.output_matches ? "一致" : "失败") << " |\n";
    out << "\n参考 checksum（64 位 FNV-1a，十六进制）`" << checksum_text(report.reference_checksum) << "`，可见对象 " << report.reference_visible_count
        << "。全模式校验：" << (report.all_outputs_match ? "一致" : "失败") << "。\n\n"
           "中位数取中心值（偶数取两个中心值的均值）；p95 使用 nearest-rank，即排序后第 ceil(0.95*N) 个样本。"
           "少量样本的 p95 会接近或等于最大值，15 个样本不是稳定的长期尾延迟估计。JSON 保留全部计时样本。\n\n"
           "grain 通过 submit 按对象分块，末块可能较短；不用现有 parallel_for 的 workers*4 任务上限。"
           "待执行任务上限为 4096，背压和句柄管理属于实测开销。工作线程数不包含调用线程。\n\n"
           "加速比小于 1 表示本次并行比串行慢，不能删掉这些结果。细粒度可能由调度成本主导；粗粒度和线程数也受缓存、内存带宽、"
           "后台负载与笔记本功耗影响。此项是 CPU 工作/调度基准，不是 GPU 剔除吞吐或整机 FPS。\n\n"
           "编译配置：" << report.compiler << "；" << report.build << "。复现时还应记录优化选项、CPU 型号和电源状态。\n";
    return out.str();
}
void save_system_benchmark(const SystemBenchmarkReport& report, const std::filesystem::path& directory) {
    if (directory.empty()) throw std::invalid_argument("Empty system benchmark directory");
    const auto json = system_benchmark_json(report), csv = system_benchmark_csv(report), markdown = system_benchmark_markdown(report);
    write_text(directory / "system-benchmark.json", json); write_text(directory / "system-benchmark.csv", csv); write_text(directory / "system-benchmark.md", markdown);
}

std::string gpu_timings_json(const GpuTimingReport& report) {
    auto out = stream(); std::vector<double> values; values.reserve(report.samples.size());
    std::set<std::uint64_t> serials;
    for (const auto& sample : report.samples) {
        if (!sample.frame_serial || !serials.insert(sample.frame_serial).second || !std::isfinite(sample.gpu_ms) || sample.gpu_ms < 0)
            throw std::invalid_argument("GPU timings need distinct nonzero completed frame serials and finite nonnegative samples");
        values.push_back(sample.gpu_ms);
    }
    out << "{\n  \"schema\":\"emberframe.gpu_timings.v1\",\n  \"gpu\":" << json_quote(report.gpu_name) << ",\n  \"render_path\":" << json_quote(report.render_path)
        << ",\n  \"warmup_frames\":" << report.warmup_frames
        << ",\n  \"sample_source\":\"unique_fence_completed_timestamp_intervals\",\n"
           "  \"scope\":\"scene_postprocess_ui_gpu_interval_excludes_screenshot_transfer_not_cpu_frame_time\",\n"
           "  \"p95_method\":\"nearest_rank\",\n  \"available\":" << (values.empty() ? "false" : "true") << ",\n  \"summary\":";
    if (values.empty()) out << "null"; else append_summary(out, summarize_timings(values));
    out << ",\n  \"samples\":[";
    for (std::size_t i = 0; i < report.samples.size(); ++i) out << (i ? ",\n" : "\n") << "    {\"frame_serial\":" << report.samples[i].frame_serial << ",\"gpu_ms\":" << report.samples[i].gpu_ms << '}';
    out << "\n  ]\n}\n"; return out.str();
}
void save_gpu_timings(const GpuTimingReport& report, const std::filesystem::path& path) { write_text(path, gpu_timings_json(report)); }
std::string diagnostics_json(std::span<const DiagnosticRecord> records, std::string_view session) {
    auto out = stream();
    out << "{\n  \"schema\":\"emberframe.diagnostics.v1\",\n  \"session\":" << json_quote(session) << ",\n  \"records\":[";
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        out << (i ? ",\n" : "\n") << "    {\"category\":" << json_quote(r.category) << ",\"name\":" << json_quote(r.name) << ",\"severity\":" << json_quote(r.severity)
            << ",\"message\":" << json_quote(r.message) << ",\"artifact\":" << json_quote(path_utf8(r.artifact)) << ",\"frame_serial\":" << r.frame_serial << '}';
    }
    out << "\n  ]\n}\n"; return out.str();
}
void export_diagnostics(std::span<const DiagnosticRecord> records, const std::filesystem::path& path, std::string_view session) { write_text(path, diagnostics_json(records, session)); }

} // namespace emberframe::lab
