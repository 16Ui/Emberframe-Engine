#include "systems.h"
#include "scene_resources.h"
#include "environment.h"
#include <fastgltf/parser.hpp>
#include <fastgltf/glm_element_traits.hpp>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201)
#endif
#include <glm/gtc/quaternion.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// Internal linkage avoids colliding with an upstream stb implementation if the
// integration target later gains another image-loading translation unit.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4505) // stb's private optional entry points are intentionally unused.
#endif
#include <stb_image.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace emberframe::lab {
namespace {
thread_local void* worker_core = nullptr;
thread_local void* current_task = nullptr;
thread_local std::size_t worker_index = 0;
thread_local std::vector<void*> task_stack;
[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }
void require(bool condition, const std::string& what) { if (!condition) fail(what); }
// 场景名称、日志和序列化文本统一使用 UTF-8，文件打开仍传原生 filesystem::path。
std::string path_text(const std::filesystem::path& path) {
    const auto bytes=path.u8string();return std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size());
}
std::string exception_text(std::exception_ptr error) {
    try { if (error) std::rethrow_exception(error); }
    catch (const std::exception& e) { return e.what(); }
    catch (...) { return "Non-standard exception"; }
    return {};
}
}

struct JobSystem::State {
    std::function<void()> function;
    std::vector<std::weak_ptr<State>> dependents, dependencies;
    std::weak_ptr<State> waiting_on;
    std::size_t remaining = 0, queue = 0;
    bool done = false;
    std::exception_ptr error;
};
struct JobSystem::Core {
    std::mutex mutex, join_mutex;
    std::condition_variable changed;
    std::vector<std::thread> workers;
    std::vector<std::deque<std::shared_ptr<State>>> queues;
    std::unordered_map<State*, std::shared_ptr<State>> live;
    std::size_t capacity, outstanding = 0, next_queue = 0;
    bool accepting = true;
    std::exception_ptr first_error;

    std::shared_ptr<State> take(std::size_t owner) {
        // 本队列从尾部取任务以保持缓存局部性；空闲线程从其他队列头部窃取较老任务。
        auto& local = queues[owner];
        if (!local.empty()) { auto task = local.back(); local.pop_back(); return task; }
        for (std::size_t i = 1; i < queues.size(); ++i) {
            auto& victim = queues[(owner + i) % queues.size()];
            if (!victim.empty()) { auto task = victim.front(); victim.pop_front(); return task; }
        }
        return {};
    }
    bool runnable() const {
        return std::any_of(queues.begin(), queues.end(), [](const auto& q) { return !q.empty(); });
    }
    void run(const std::shared_ptr<State>& task) {
        auto* previous = current_task;
        current_task = task.get();
        task_stack.push_back(task.get());
        std::exception_ptr error;
        try { task->function(); } catch (...) { error = std::current_exception(); }
        current_task = previous;
        task_stack.pop_back();
        // Destroy captures outside the scheduler lock (their destructors may call APIs).
        task->function = {};
        std::vector<std::function<void()>> discarded;
        std::unique_lock lock(mutex);
        std::deque<std::pair<std::shared_ptr<State>, std::exception_ptr>> completed;
        completed.emplace_back(task, error);
        while (!completed.empty()) {
            auto [finished, failure] = std::move(completed.front()); completed.pop_front();
            finished->error = failure; finished->done = true;
            if(finished->function)discarded.push_back(std::move(finished->function));
            if (failure && !first_error) first_error = failure;
            --outstanding;
            for (auto& weak : finished->dependents) if (auto dependent = weak.lock()) {
                if (failure && !dependent->error) dependent->error = failure;
                if (--dependent->remaining == 0) {
                    if (dependent->error) completed.emplace_back(dependent, dependent->error);
                    else queues[dependent->queue].push_back(dependent);
                }
            }
            live.erase(finished.get());
        }
        changed.notify_all();
        lock.unlock(); // Cancelled task captures are also destroyed outside the scheduler lock.
    }
    void await(const std::shared_ptr<State>& target) {
        std::unique_lock lock(mutex);
        const bool worker = worker_core == this;
        if (worker && !target->done) {
            std::vector<std::shared_ptr<State>> visit{target};
            std::unordered_set<State*> seen;
            while (!visit.empty()) {
                auto t = std::move(visit.back()); visit.pop_back();
                if (!seen.insert(t.get()).second || t->done) continue;
                if (std::find(task_stack.begin(),task_stack.end(),t.get())!=task_stack.end())
                    throw std::logic_error("Job wait would create a dependency cycle");
                for (auto& w : t->dependencies) if (auto d = w.lock()) visit.push_back(d);
                if(auto waiting=t->waiting_on.lock())visit.push_back(waiting);
            }
        }
        State* waiter=worker?static_cast<State*>(current_task):nullptr;
        if(waiter)waiter->waiting_on=target;
        while (!target->done) {
            if (worker) if (auto t = take(worker_index)) {
                lock.unlock(); run(t); lock.lock(); continue;
            }
            changed.wait(lock);
        }
        if(waiter)waiter->waiting_on.reset();
    }
};

JobSystem::JobSystem(std::size_t threads, std::size_t max_pending) : core_(std::make_shared<Core>()) {
    if (!max_pending) throw std::invalid_argument("Job capacity must be positive");
    if (!threads) threads = std::max(1u, std::thread::hardware_concurrency());
    threads = std::clamp<std::size_t>(threads, 1, 64);
    core_->capacity = max_pending;
    core_->queues.resize(threads);
    try {
        for (std::size_t i = 0; i < threads; ++i) core_->workers.emplace_back([core = core_, i] {
            worker_core = core.get(); worker_index = i;
            std::unique_lock lock(core->mutex);
            for (;;) {
                core->changed.wait(lock, [&] { return core->runnable() || (!core->accepting && !core->outstanding); });
                if (!core->accepting && !core->outstanding) break;
                if (auto t = core->take(i)) { lock.unlock(); core->run(t); lock.lock(); }
            }
            worker_core = nullptr;
        });
    } catch (...) { shutdown(); throw; }
}
JobSystem::~JobSystem() { shutdown(); }
std::size_t JobSystem::thread_count() const noexcept { return core_->queues.size(); }
std::size_t JobSystem::pending() const { std::lock_guard lock(core_->mutex); return core_->outstanding; }
bool JobSystem::Task::ready() const {
    if (!valid()) throw std::logic_error("Empty job handle");
    std::lock_guard lock(core_->mutex); return state_->done;
}
void JobSystem::Task::wait() const {
    if (!valid()) throw std::logic_error("Empty job handle");
    core_->await(state_);
}
void JobSystem::Task::get() const {
    wait(); std::exception_ptr e;
    { std::lock_guard lock(core_->mutex); e = state_->error; }
    if (e) std::rethrow_exception(e);
}
JobSystem::Task JobSystem::submit(std::function<void()> function, std::span<const Task> dependencies) {
    if (!function) throw std::invalid_argument("Empty job function");
    std::unique_lock lock(core_->mutex);
    for (const auto& dep : dependencies)
        if (!dep.valid() || dep.core_ != core_) throw std::invalid_argument("Foreign or empty dependency");
    while (core_->accepting && core_->outstanding >= core_->capacity) {
        if (worker_core == core_.get()) {
            auto t = core_->take(worker_index);
            if (!t) throw std::length_error("Nested job submission exhausted bounded capacity");
            lock.unlock(); core_->run(t); lock.lock();
        } else core_->changed.wait(lock);
    }
    if (!core_->accepting) throw std::logic_error("JobSystem has shut down");
    Task handle; handle.core_ = core_; handle.state_ = std::make_shared<State>();
    auto& task = *handle.state_; task.function = std::move(function);
    task.queue = worker_core == core_.get() ? worker_index : core_->next_queue++ % core_->queues.size();
    std::unordered_set<State*> unique;
    for (const auto& dep : dependencies) if (unique.insert(dep.state_.get()).second) {
        task.dependencies.emplace_back(dep.state_);
        if (dep.state_->done) { if (dep.state_->error && !task.error) task.error = dep.state_->error; }
        else { ++task.remaining; dep.state_->dependents.emplace_back(handle.state_); }
    }
    if (!task.remaining && task.error) { task.done = true; return handle; }
    ++core_->outstanding; core_->live.emplace(&task, handle.state_);
    if (!task.remaining) core_->queues[task.queue].push_back(handle.state_);
    core_->changed.notify_all(); return handle;
}
void JobSystem::parallel_for(std::size_t begin, std::size_t end,
                             const std::function<void(std::size_t)>& function, std::size_t grain) {
    if (end < begin || !grain || !function) throw std::invalid_argument("Invalid parallel_for range/function/grain");
    if (begin == end) return;
    const auto count = end - begin;
    const auto chunks = std::min((count - 1) / grain + 1, thread_count() * 4);
    const auto stride = (count - 1) / chunks + 1;
    std::vector<Task> tasks; tasks.reserve(chunks);
    std::exception_ptr failure;
    try {
        for (auto start = begin; start < end;) {
            const auto stop = start + std::min(stride, end - start);
            tasks.push_back(submit([start, stop, function] { for (auto i = start; i < stop; ++i) function(i); }));
            start = stop;
        }
    } catch (...) { failure = std::current_exception(); }
    for (const auto& t : tasks) try { t.get(); } catch (...) { if (!failure) failure = std::current_exception(); }
    if (failure) std::rethrow_exception(failure);
}
void JobSystem::wait_idle() {
    if (worker_core == core_.get()) throw std::logic_error("Worker cannot wait for itself to become idle");
    std::unique_lock lock(core_->mutex);
    core_->changed.wait(lock, [&] { return !core_->outstanding; });
    if (core_->first_error) std::rethrow_exception(core_->first_error);
}
void JobSystem::shutdown() {
    if (worker_core == core_.get()) throw std::logic_error("Worker cannot shut down its own JobSystem");
    std::lock_guard join_lock(core_->join_mutex);
    { std::lock_guard lock(core_->mutex); core_->accepting = false; }
    core_->changed.notify_all();
    for (auto& worker : core_->workers) if (worker.joinable()) worker.join();
}

void RenderGraph::add_resource(std::string name, bool imported, ResourceState initial) {
    if (name.empty() || std::any_of(resources_.begin(), resources_.end(), [&](const auto& r) { return r.name == name; }))
        throw std::invalid_argument("Duplicate/empty graph resource: " + name);
    if (imported && initial == ResourceState::undefined) throw std::invalid_argument("Imported resource needs a known state");
    resources_.push_back({std::move(name), imported, initial, false}); ++generation_;
}
void RenderGraph::add_pass(std::string name, std::vector<ResourceUse> uses, std::function<void()> callback,
                           bool side_effect, std::vector<std::string> dependencies) {
    if (name.empty() || !callback || std::any_of(passes_.begin(), passes_.end(), [&](const auto& p) { return p.name == name; }))
        throw std::invalid_argument("Duplicate/empty graph pass or callback: " + name);
    passes_.push_back({std::move(name), std::move(uses), std::move(callback), side_effect, std::move(dependencies)}); ++generation_;
}
void RenderGraph::export_resource(std::string_view name) {
    for (auto& r : resources_) if (r.name == name) { r.exported = true; ++generation_; return; }
    throw std::invalid_argument("Unknown graph export: " + std::string(name));
}
void RenderGraph::clear() { resources_.clear(); passes_.clear(); ++generation_; }
GraphPlan RenderGraph::compile() const {
    const auto n = passes_.size();
    std::unordered_map<std::string, std::size_t> resource_ids, pass_ids;
    for (std::size_t i = 0; i < resources_.size(); ++i) resource_ids.emplace(resources_[i].name, i);
    for (std::size_t i = 0; i < n; ++i) pass_ids.emplace(passes_[i].name, i);
    std::vector<std::set<std::size_t>> edges(n), needed(n);
    std::vector<std::optional<std::size_t>> last_writer(resources_.size());
    std::vector<std::set<std::size_t>> readers(resources_.size());
    auto edge = [&](std::size_t from, std::size_t to, bool data) {
        edges[from].insert(to); if (data) needed[to].insert(from);
    };
    for (std::size_t p = 0; p < n; ++p) {
        std::set<std::size_t> seen;
        for (const auto& name : passes_[p].dependencies) {
            auto it = pass_ids.find(name); require(it != pass_ids.end(), "Unknown graph dependency: " + name);
            edge(it->second, p, true);
        }
        for (const auto& use : passes_[p].uses) {
            auto it = resource_ids.find(use.resource); require(it != resource_ids.end(), "Unknown graph resource: " + use.resource);
            const auto r = it->second;
            require(seen.insert(r).second, "Use read_write for duplicate resource access in " + passes_[p].name);
            require(use.state != ResourceState::undefined, "Graph use cannot have undefined state");
            if (use.access != ResourceAccess::write) {
                require(last_writer[r].has_value() || resources_[r].imported,
                        "Read-before-write: " + use.resource + " in " + passes_[p].name);
                if (last_writer[r]) edge(*last_writer[r], p, true);
            }
            if (use.access != ResourceAccess::read) {
                // RAW 是数据依赖；WAW/WAR 只约束顺序，不让被覆盖的无用写入阻止裁剪。
                if (last_writer[r]) edge(*last_writer[r], p, false);
                for (auto reader : readers[r]) edge(reader, p, false);
                readers[r].clear(); last_writer[r] = p;
            } else readers[r].insert(p);
        }
    }
    auto topological = [&](const std::vector<bool>& active) {
        std::vector<std::size_t> degree(n), order;
        std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
        for (std::size_t i = 0; i < n; ++i) if (active[i]) for (auto j : edges[i]) if (active[j]) ++degree[j];
        for (std::size_t i = 0; i < n; ++i) if (active[i] && degree[i] == 0) ready.push(i);
        while (!ready.empty()) {
            const auto i = ready.top(); ready.pop(); order.push_back(i);
            for (auto j : edges[i]) if (active[j] && --degree[j] == 0) ready.push(j);
        }
        require(order.size() == std::size_t(std::count(active.begin(), active.end(), true)), "Render graph dependency cycle");
        return order;
    };
    topological(std::vector<bool>(n, true));
    std::vector<bool> active(n, false); std::vector<std::size_t> stack;
    for (std::size_t i = 0; i < n; ++i) if (passes_[i].side_effect) stack.push_back(i);
    for (std::size_t i = 0; i < resources_.size(); ++i) if (resources_[i].exported) {
        require(last_writer[i].has_value() || resources_[i].imported, "Export without contents: " + resources_[i].name);
        if (last_writer[i]) stack.push_back(*last_writer[i]);
    }
    while (!stack.empty()) {
        auto p = stack.back(); stack.pop_back(); if (active[p]) continue;
        active[p] = true; for (auto dep : needed[p]) stack.push_back(dep);
    }
    GraphPlan result; result.owner_ = this; result.generation_ = generation_; result.order = topological(active);
    std::vector<std::size_t> execution_index(n);
    for(std::size_t i=0;i<result.order.size();++i)execution_index[result.order[i]]=i;
    for(std::size_t from=0;from<n;++from)if(active[from])for(auto to:edges[from])if(active[to])
        result.dependencies.emplace_back(execution_index[from],execution_index[to]);
    for (std::size_t p = 0; p < n; ++p) if (!active[p]) result.culled_passes.push_back(passes_[p].name);
    std::vector<std::optional<ResourceLifetime>> life(resources_.size());
    std::vector<ResourceState> states; for (const auto& r : resources_) states.push_back(r.initial);
    std::vector<ResourceAccess> access(resources_.size(), ResourceAccess::read);
    for (std::size_t position = 0; position < result.order.size(); ++position) {
        const auto& pass = passes_[result.order[position]]; result.pass_names.push_back(pass.name);
        for (const auto& use : pass.uses) {
            const auto r = resource_ids.at(use.resource);
            const bool hazard = life[r].has_value() ? (access[r] != ResourceAccess::read || use.access != ResourceAccess::read) : resources_[r].imported;
            if (states[r] != use.state || hazard)
                result.barriers.push_back({use.resource, position, states[r], use.state, access[r], use.access, hazard});
            states[r] = use.state; access[r] = use.access;
            if (!life[r]) life[r] = ResourceLifetime{use.resource, position, position, resources_[r].imported,resources_[r].exported};
            life[r]->last = position;
        }
    }
    for(std::size_t r=0;r<life.size();++r) {
        if(!life[r]&&resources_[r].exported)life[r]=ResourceLifetime{resources_[r].name,0,result.order.size(),resources_[r].imported,true};
        if(life[r]){if(life[r]->exported)life[r]->last=result.order.size();result.lifetimes.push_back(std::move(*life[r]));}
    }
    return result;
}
void RenderGraph::execute(const GraphPlan& plan, const std::function<void(const ResourceBarrier&)>& barrier,
                          const std::function<void(std::string_view,bool)>& observer) const {
    if (plan.owner_ != this || plan.generation_ != generation_) throw std::logic_error("Stale or foreign render graph plan");
    for (std::size_t i = 0; i < plan.order.size(); ++i) {
        const auto& pass=passes_.at(plan.order[i]);
        if(observer)observer(pass.name,true);
        try {
            if (barrier) for (const auto& b : plan.barriers) if (b.before_pass == i) barrier(b);
            pass.callback();
        } catch(...) { if(observer)observer(pass.name,false);throw; }
        if(observer)observer(pass.name,false);
    }
}

std::vector<glm::mat4> scene_world_transforms(const Scene& scene) {
    std::vector<glm::mat4> world(scene.nodes.size(), glm::mat4(1));
    std::vector<unsigned char> visited(scene.nodes.size());
    for (std::size_t i = 0; i < scene.nodes.size(); ++i) {
        std::vector<std::size_t> path; int node = static_cast<int>(i);
        while (node >= 0 && visited[std::size_t(node)] != 2) {
            require(std::size_t(node) < scene.nodes.size(), "Node parent out of range");
            require(!visited[std::size_t(node)], "Scene node cycle");
            visited[std::size_t(node)] = 1; path.push_back(std::size_t(node));
            node = scene.nodes[std::size_t(node)].parent;
            require(node >= -1 && (node < 0 || std::size_t(node) < scene.nodes.size()), "Node parent out of range");
        }
        while (!path.empty()) {
            const auto id = path.back(); path.pop_back(); const auto& n = scene.nodes[id];
            world[id] = n.parent < 0 ? n.local : world[std::size_t(n.parent)] * n.local;
            visited[id] = 2;
        }
    }
    return world;
}

namespace {
Mesh box_mesh(std::uint32_t material) {
    Mesh mesh; mesh.name = "Unit cube";
    const glm::vec3 normals[] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (const auto n : normals) {
        const auto tangent = safe_normalize(glm::cross(std::abs(n.y) > .5f ? glm::vec3(0,0,1) : glm::vec3(0,1,0), n));
        const auto bitangent = glm::cross(n, tangent);
        const auto base = std::uint32_t(mesh.vertices.size());
        for (auto uv : {glm::vec2(0,0),glm::vec2(1,0),glm::vec2(1,1),glm::vec2(0,1)}) {
            Vertex v; v.position = (n + tangent * (2*uv.x-1) + bitangent * (2*uv.y-1)) * .5f;
            v.normal = n; v.uv = uv; v.tangent = glm::vec4(tangent,1); mesh.vertices.push_back(v);
        }
        for (auto j : {0u,1u,2u,0u,2u,3u}) mesh.indices.push_back(base+j);
    }
    mesh.primitives.push_back({0,std::uint32_t(mesh.indices.size()),material}); return mesh;
}
Mesh sphere_mesh(std::uint32_t material,int rows=32,int cols=48) {
    Mesh mesh; mesh.name = "UV sphere";
    mesh.vertices.reserve((rows-1)*(cols+1)+2*cols);
    mesh.indices.reserve(6*cols*(rows-1));
    for (int y=1; y<rows; ++y) for (int x=0; x<=cols; ++x) {
        const double theta = std::acos(-1.0)*y/rows;
        // 接缝两端共用角度，位置/法线/切线逐位一致，UV 保持 0/1 分离。
        const double phi = x==cols ? 0.0:2*std::acos(-1.0)*x/cols;
        Vertex v; v.normal = glm::vec3(std::sin(theta)*std::cos(phi),std::cos(theta),std::sin(theta)*std::sin(phi));
        v.position=v.normal*.5f; v.uv={float(x)/cols,float(y)/rows};
        v.tangent=glm::vec4(float(-std::sin(phi)),0,float(std::cos(phi)),1); mesh.vertices.push_back(v);
    }
    for (int y=0;y<rows-2;++y) for(int x=0;x<cols;++x) {
        const auto a=std::uint32_t(y*(cols+1)+x), b=a+cols+1;
        for(auto i:{a,a+1,b,a+1,b+1,b}) mesh.indices.push_back(i);
    }
    // 极点按扇区独立，U/切线居中；避免经线端点切线偏向一侧及 sin(pi) 的微小倒置环。
    for(int pole=0;pole<2;++pole) for(int x=0;x<cols;++x) {
        const float u=(float(x)+.5f)/cols;
        const double phi=2*std::acos(-1.0)*(double(x)+.5)/cols;
        Vertex v; v.normal={0,pole ? -1.f:1.f,0}; v.position=v.normal*.5f;
        v.uv={u,float(pole)}; v.tangent={float(-std::sin(phi)),0,float(std::cos(phi)),1};
        const auto tip=std::uint32_t(mesh.vertices.size()); mesh.vertices.push_back(v);
        const auto ring=std::uint32_t((pole ? rows-2:0)*(cols+1)+x);
        if(pole) for(auto i:{ring,ring+1,tip}) mesh.indices.push_back(i);
        else for(auto i:{tip,ring+1,ring}) mesh.indices.push_back(i);
    }
    mesh.primitives.push_back({0,std::uint32_t(mesh.indices.size()),material}); return mesh;
}
void add_object(Scene& s, Mesh mesh, glm::vec3 position, glm::vec3 scale, float rotation=0) {
    Node n; n.name=mesh.name; n.mesh=int(s.meshes.size());
    n.local=glm::translate(glm::mat4(1),position)*glm::rotate(glm::mat4(1),rotation,glm::vec3(0,1,0))*glm::scale(glm::mat4(1),scale);
    n.previous_world=n.local; s.meshes.push_back(std::move(mesh)); s.nodes.push_back(std::move(n));
}
}
Scene make_demo_scene(int preset) {
    if (preset<0 || preset>3) throw std::invalid_argument("Demo preset must be 0..3");
    Scene s; const char* names[]={"Material studio","Colored-box GI room","Shadow test","Many lights"}; s.name=names[preset];
    auto material=[&](std::string name,glm::vec3 color,float roughness=.5f,float metallic=0) {
        Material m; m.name=std::move(name); m.base_color=glm::vec4(color,1);m.roughness=roughness;m.metallic=metallic;
        s.materials.push_back(m); return std::uint32_t(s.materials.size()-1);
    };
    const auto white=material("Neutral plaster",glm::vec3(.65f),.75f);
    add_object(s,box_mesh(white),{0,-.1f,0},{9,.2f,9});
    Light key; key.direction=safe_normalize(glm::vec3(-.5f,-1,-.3f)); key.intensity=2.5f;
    s.lights.push_back(key);
    if(preset==0) {
        for(int row=0;row<2;++row) for(int col=0;col<5;++col) {
            auto m=material(row?"Metal":"Dielectric",row?glm::vec3(.95f,.58f,.2f):glm::vec3(.2f,.45f,.8f),.07f+.21f*float(col),float(row));
            // 平滑法线不会改变阴影图里的真实轮廓。材质工作室提高真实细分，减小明暗交界台阶；
            // 不关闭自阴影、不抬高 Bias。体积 GI 教学场景保留默认网格，遵守 4096 三角形预算。
            add_object(s,sphere_mesh(m,64,96),{(col-2)*1.25f,.6f,(row-.5f)*1.5f},{1.1f,1.1f,1.1f});
        }
    } else if(preset==1) {
        s.sky_top=s.sky_bottom=glm::vec3(0);s.lights.clear();
        auto red=material("Red wall",{.75f,.06f,.04f}),green=material("Green wall",{.08f,.65f,.12f});
        add_object(s,box_mesh(red),{-2,1.5f,0},{.1f,3,4});
        add_object(s,box_mesh(green),{2,1.5f,0},{.1f,3,4});
        add_object(s,box_mesh(white),{0,1.5f,-2},{4,3,.1f});
        add_object(s,box_mesh(white),{0,3,0},{4,.1f,4});
        add_object(s,box_mesh(white),{-.7f,.65f,0},{.9f,1.3f,.9f},.3f);
        auto metal=material("Copper sphere",{.95f,.55f,.3f},.18f,1);
        add_object(s,sphere_mesh(metal),{.8f,.6f,.3f},{1.2f,1.2f,1.2f});
        const auto emitter=material("Ceiling emitter",{1,1,1}); s.materials[emitter].emissive={12,11,9};
        Mesh panel;panel.name="One-sided ceiling emitter";
        for(auto uv:{glm::vec2(0,0),glm::vec2(1,0),glm::vec2(1,1),glm::vec2(0,1)}) {
            Vertex v;v.position={uv.x-.5f,0,uv.y-.5f};v.normal={0,-1,0};v.uv=uv;v.tangent={1,0,0,1};panel.vertices.push_back(v);
        }
        panel.indices={0,1,2,0,2,3};panel.primitives.push_back({0,6,emitter});
        add_object(s,std::move(panel),{0,2.92f,0},{1.2f,1,1.2f});
        Light area;area.kind=LightKind::rectangle;area.position={0,2.92f,0};area.direction={0,-1,0};
        area.color={1,11.f/12,9.f/12};area.intensity=12;area.size={1.2f,1.2f};
        area.linked_node=int(s.nodes.size()-1);s.lights.push_back(area);
        const auto revision=s.revision;update_editor_light(s,0,area);s.revision=revision;
        s.nodes[std::size_t(area.linked_node)].previous_world=s.nodes[std::size_t(area.linked_node)].local;
    } else if(preset==2) {
        for(int i=0;i<5;++i) {
            auto m=material("Shadow blocker",{.2f+.12f*i,.35f,.65f},.4f);
            add_object(s,box_mesh(m),{(i-2)*1.3f,.3f+.2f*i,0},{.55f,.6f+.4f*i,.65f});
        }
        add_object(s,sphere_mesh(white),{0,.6f,2},{1.2f,1.2f,1.2f});
    } else {
        s.lights[0].intensity=.25f;
        for(int y=0;y<5;++y) for(int x=0;x<5;++x) {
            add_object(s,sphere_mesh(white),{float(x-2)*1.3f,.45f,float(y-2)*1.3f},{.9f,.9f,.9f});
            Light l;l.kind=LightKind::point;l.position={float(x-2)*1.5f,1.3f,float(y-2)*1.5f};l.range=3.5f;l.intensity=2;
            l.color={.2f+.8f*float((x+2*y)%5)/4,.2f+.8f*float((2*x+y+1)%5)/4,.2f+.8f*float((x+y+3)%5)/4};s.lights.push_back(l);
        }
    }
    return s;
}

namespace {
constexpr std::size_t max_asset_elements = 16 * 1024 * 1024;
std::span<const std::byte> bytes_of(const fastgltf::DataSource& source) {
    if (const auto* v=std::get_if<fastgltf::sources::Vector>(&source))
        return {reinterpret_cast<const std::byte*>(v->bytes.data()),v->bytes.size()};
    if (const auto* v=std::get_if<fastgltf::sources::ByteView>(&source)) return {v->bytes.data(),v->bytes.size()};
    fail("Asset buffer/image was not loaded into CPU memory");
}
std::span<const std::byte> view_bytes(const fastgltf::Asset& asset,std::size_t id) {
    require(id<asset.bufferViews.size(),"Invalid bufferView index");
    const auto& view=asset.bufferViews[id];
    require(view.bufferIndex<asset.buffers.size(),"Invalid buffer index");
    require(!view.meshoptCompression,"Meshopt compression is not supported");
    const auto data=bytes_of(asset.buffers[view.bufferIndex].data);
    require(view.byteOffset<=data.size() && view.byteLength<=data.size()-view.byteOffset,"bufferView exceeds loaded buffer");
    require(view.byteOffset<=asset.buffers[view.bufferIndex].byteLength && view.byteLength<=asset.buffers[view.bufferIndex].byteLength-view.byteOffset,
            "bufferView exceeds declared buffer");
    return data.subspan(view.byteOffset,view.byteLength);
}
const fastgltf::Accessor& checked_accessor(const fastgltf::Asset& asset,std::size_t id,fastgltf::AccessorType type) {
    require(id<asset.accessors.size(),"Invalid accessor index");const auto& a=asset.accessors[id];
    require(a.type==type && a.count>0 && a.count<=max_asset_elements,"Invalid accessor type/count");
    const auto size=fastgltf::getElementByteSize(a.type,a.componentType);
    require(size>0,"Invalid accessor component type");
    auto range=[](std::size_t length,std::size_t offset,std::size_t count,std::size_t stride,std::size_t element) {
        require(stride>=element && offset<=length && element<=length-offset && (count-1)<=(length-offset-element)/stride,
                "Accessor exceeds its bufferView");
    };
    if(a.bufferViewIndex) {
        const auto data=view_bytes(asset,*a.bufferViewIndex);const auto& view=asset.bufferViews[*a.bufferViewIndex];
        range(data.size(),a.byteOffset,a.count,view.byteStride.value_or(size),size);
    } else require(a.byteOffset==0,"Unbacked accessor cannot have a byte offset");
    if(a.sparse) {
        const auto& sparse=*a.sparse;
        require(sparse.count>0 && sparse.count<=a.count,"Invalid sparse accessor count");
        require(sparse.indexComponentType==fastgltf::ComponentType::UnsignedByte || sparse.indexComponentType==fastgltf::ComponentType::UnsignedShort ||
                sparse.indexComponentType==fastgltf::ComponentType::UnsignedInt,"Invalid sparse index type");
        const auto indices=view_bytes(asset,sparse.indicesBufferView),values=view_bytes(asset,sparse.valuesBufferView);
        require(!asset.bufferViews[sparse.indicesBufferView].byteStride && !asset.bufferViews[sparse.valuesBufferView].byteStride,
                "Sparse buffer views cannot be interleaved");
        const auto step=fastgltf::getElementByteSize(fastgltf::AccessorType::Scalar,sparse.indexComponentType);
        range(indices.size(),sparse.indicesByteOffset,sparse.count,step,step);
        range(values.size(),sparse.valuesByteOffset,sparse.count,size,size);
        std::uint32_t previous=0;
        for(std::size_t i=0;i<sparse.count;++i) {
            std::uint32_t index=0;
            for(std::size_t b=0;b<step;++b) index|=std::uint32_t(std::to_integer<unsigned char>(indices[sparse.indicesByteOffset+i*step+b]))<<(b*8);
            require(index<a.count && (i==0 || index>previous),"Sparse indices must be increasing and in range");previous=index;
        }
    }
    return a;
}
template<class V> bool finite_vec(const V& v) {
    for(glm::length_t i=0;i<v.length();++i) if(!std::isfinite(v[i])) return false;return true;
}
float decode_srgb(float x) { return x<=.04045f ? x/12.92f : std::pow((x+.055f)/1.055f,2.4f); }
float encode_srgb(float x) { return x<=.0031308f ? 12.92f*x : 1.055f*std::pow(x,1.f/2.4f)-.055f; }
void build_asset_mips(Texture& texture) {
    // 颜色先转线性再平均，最后重新编码；alpha 从不参与 sRGB 变换。
    // 奇数尺寸按面积重采样，不丢弃最后一行/列。法线贴图这里保存数据平均，采样后需归一化。
    while(texture.levels.back().width>1 || texture.levels.back().height>1) {
        const auto& src=texture.levels.back();const int w=std::max(1,src.width/2),h=std::max(1,src.height/2);
        Image<glm::vec4> dst(w,h);
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
            const float x0=float(x)*src.width/w,x1=float(x+1)*src.width/w,y0=float(y)*src.height/h,y1=float(y+1)*src.height/h;
            glm::vec4 sum(0);float weight=0;
            for(int sy=int(y0);sy<std::min(src.height,int(std::ceil(y1)));++sy)
                for(int sx=int(x0);sx<std::min(src.width,int(std::ceil(x1)));++sx) {
                    const float a=(std::min(x1,float(sx+1))-std::max(x0,float(sx)))*(std::min(y1,float(sy+1))-std::max(y0,float(sy)));
                    auto c=src.at(sx,sy);if(texture.srgb) for(int k=0;k<3;++k)c[k]=decode_srgb(c[k]);sum+=a*c;weight+=a;
                }
            sum/=weight;if(texture.srgb)for(int k=0;k<3;++k)sum[k]=encode_srgb(sum[k]);dst.at(x,y)=sum;
        }
        texture.levels.push_back(std::move(dst));
    }
}
glm::mat4 gltf_transform(const fastgltf::Node& n) {
    glm::mat4 matrix(1);
    if(const auto* trs=std::get_if<fastgltf::Node::TRS>(&n.transform)) {
        const glm::quat q(trs->rotation[3],trs->rotation[0],trs->rotation[1],trs->rotation[2]);
        require(std::isfinite(glm::dot(q,q)) && glm::dot(q,q)>1e-12f,"Invalid node quaternion");
        // glTF 使用列向量，局部变换顺序固定为 T * R * S；顶点留在原网格坐标中。
        matrix=glm::translate(glm::mat4(1),glm::vec3(trs->translation[0],trs->translation[1],trs->translation[2]))*
               glm::mat4_cast(glm::normalize(q))*glm::scale(glm::mat4(1),glm::vec3(trs->scale[0],trs->scale[1],trs->scale[2]));
    } else {
        const auto& values=std::get<fastgltf::Node::TransformMatrix>(n.transform);
        for(int c=0;c<4;++c)for(int r=0;r<4;++r)matrix[c][r]=values[std::size_t(c*4+r)];
    }
    for(int c=0;c<4;++c)require(finite_vec(matrix[c]),"Nonfinite node transform");return matrix;
}
glm::vec3 geometry_unit(glm::dvec3 v,glm::vec3 fallback={0,1,0}) {
    const double q=glm::dot(v,v);
    return q>0 && std::isfinite(q) ? glm::vec3(v/std::sqrt(q)):fallback;
}
glm::vec3 geometry_tangent(glm::dvec3 t,glm::vec3 normal) {
    const glm::dvec3 n(normal);
    const auto projected=glm::cross(n,glm::cross(t,n));
    const auto fallback=geometry_unit(glm::cross(std::abs(n.y)<.9 ? glm::dvec3(0,1,0):glm::dvec3(1,0,0),n),{1,0,0});
    // 相对阈值只检测近乎平行的病态方向，不依赖模型或 UV 的绝对大小。
    return glm::dot(projected,projected)>glm::dot(t,t)*1e-24 ? geometry_unit(projected,fallback):fallback;
}
void generate_tangents(Mesh& mesh,std::size_t first_vertex,std::size_t first_index,std::size_t& total_vertices) {
    const auto count=mesh.vertices.size()-first_vertex;
    std::vector<glm::dvec3> tangent(count,glm::dvec3(0));
    std::vector<int> handedness(count,0);
    std::vector<std::uint32_t> mirrored(count,std::numeric_limits<std::uint32_t>::max());
    for(auto i=first_index;i<mesh.indices.size();i+=3) {
        const std::array<std::uint32_t,3> ids{mesh.indices[i],mesh.indices[i+1],mesh.indices[i+2]};
        const std::array<glm::dvec3,3> p{glm::dvec3(mesh.vertices[ids[0]].position),glm::dvec3(mesh.vertices[ids[1]].position),glm::dvec3(mesh.vertices[ids[2]].position)};
        const auto e1=p[1]-p[0],e2=p[2]-p[0];
        const auto uv1=glm::dvec2(mesh.vertices[ids[1]].uv)-glm::dvec2(mesh.vertices[ids[0]].uv);
        const auto uv2=glm::dvec2(mesh.vertices[ids[2]].uv)-glm::dvec2(mesh.vertices[ids[0]].uv);
        const double det=uv1.x*uv2.y-uv1.y*uv2.x;
        const double uv_scale=std::sqrt(glm::dot(uv1,uv1)*glm::dot(uv2,uv2));
        if(std::abs(det)<=8*std::numeric_limits<double>::epsilon()*uv_scale || glm::dot(glm::cross(e1,e2),glm::cross(e1,e2))==0)continue;
        auto t=(e1*uv2.y-e2*uv1.y)/det;
        const auto bn=(e2*uv1.x-e1*uv2.x)/det;
        t/=std::sqrt(glm::dot(t,t));
        for(int k=0;k<3;++k) {
            auto local=std::size_t(ids[k])-first_vertex;
            const glm::dvec3 normal(mesh.vertices[ids[k]].normal);
            const int sign=glm::dot(glm::cross(normal,t),bn)<0 ? -1:1;
            if(!handedness[local])handedness[local]=sign;
            if(handedness[local]!=sign) {
                // 镜像 UV 两侧不能共用 tangent.w，也不能让相反切线互相抵消。
                if(mirrored[local]==std::numeric_limits<std::uint32_t>::max()) {
                    require(total_vertices<max_asset_elements,"Tangent seam vertex budget exceeded");
                    const auto copy=mesh.vertices[ids[k]];
                    mirrored[local]=std::uint32_t(mesh.vertices.size()); mesh.vertices.push_back(copy); ++total_vertices;
                    tangent.emplace_back(0); handedness.push_back(sign);
                }
                mesh.indices[i+std::size_t(k)]=mirrored[local]; local=std::size_t(mirrored[local])-first_vertex;
            }
            const auto a=p[(k+1)%3]-p[k],b=p[(k+2)%3]-p[k];
            const double angle=std::atan2(glm::length(glm::cross(a,b)),glm::dot(a,b));
            tangent[local]+=t*angle;
        }
    }
    for(auto i=first_vertex;i<mesh.vertices.size();++i) {
        auto& v=mesh.vertices[i];
        v.tangent=glm::vec4(geometry_tangent(tangent[i-first_vertex],v.normal),handedness[i-first_vertex]<0 ? -1.f:1.f);
    }
}
}

LoadedSceneAsset load_scene_asset_detailed(const std::filesystem::path& input_path) {
    const auto path=std::filesystem::absolute(input_path).lexically_normal();
    require(std::filesystem::is_regular_file(path),"Scene asset does not exist: "+path_text(path));
    require(std::filesystem::file_size(path)<=512ull*1024*1024,"Scene asset exceeds 512 MiB import limit");
    fastgltf::GltfDataBuffer data;require(data.loadFromFile(path),"Cannot read scene asset: "+path_text(path));
    const auto extensions=fastgltf::Extensions::KHR_mesh_quantization|fastgltf::Extensions::KHR_materials_emissive_strength|
                          fastgltf::Extensions::KHR_lights_punctual|fastgltf::Extensions::KHR_materials_clearcoat|
                          fastgltf::Extensions::KHR_texture_transform;
    fastgltf::Parser parser(extensions);
    const auto options=fastgltf::Options::LoadGLBBuffers|fastgltf::Options::LoadExternalBuffers|fastgltf::Options::LoadExternalImages;
    const auto type=fastgltf::determineGltfFileType(&data);
    require(type==fastgltf::GltfType::glTF || type==fastgltf::GltfType::GLB,"Not a glTF/GLB asset");
    auto parsed=type==fastgltf::GltfType::glTF?parser.loadGLTF(&data,path.parent_path(),options):parser.loadBinaryGLTF(&data,path.parent_path(),options);
    require(parsed.error()==fastgltf::Error::None,"glTF parse failed: "+std::string(fastgltf::getErrorMessage(parsed.error())));
    auto asset=std::move(parsed.get());
    const auto validation=fastgltf::validate(asset);
    require(validation==fastgltf::Error::None,"glTF validation failed: "+std::string(fastgltf::getErrorMessage(validation)));
    LoadedSceneAsset result;auto& scene=result.scene;scene.name=path_text(path.stem());
    if(!asset.animations.empty())result.warnings.push_back("Animations are not evaluated; static node bind transforms were imported.");
    if(!asset.cameras.empty())result.warnings.push_back("glTF cameras are not applied; the viewer camera remains independent.");
    require(asset.skins.empty(),"Skinned glTF is unsupported by the shared static Scene");
    const std::set<std::string> implemented={"KHR_mesh_quantization","KHR_materials_emissive_strength","KHR_lights_punctual","KHR_materials_clearcoat","KHR_texture_transform"};
    for(const auto& extension:asset.extensionsUsed)if(!implemented.contains(std::string(extension)))
        result.warnings.push_back("Optional glTF extension ignored: "+std::string(extension));
    for(const auto& b:asset.buffers)require(bytes_of(b.data).size()>=b.byteLength,"Truncated glTF buffer");
    for(std::size_t i=0;i<asset.bufferViews.size();++i)view_bytes(asset,i);
    std::map<std::pair<std::size_t,bool>,int> texture_cache;
    std::size_t decoded_pixels=0;
    auto texture=[&](const auto& optional,bool srgb)->int {
        if(!optional)return -1;
        const auto& info=*optional;require(info.texCoordIndex==0,"Only TEXCOORD_0 is representable in Scene");
        if(info.transform) {
            const auto& t=*info.transform;
            require(t.rotation==0 && t.uvOffset[0]==0 && t.uvOffset[1]==0 && t.uvScale[0]==1 && t.uvScale[1]==1 &&
                    (!t.texCoordIndex || *t.texCoordIndex==0),"Non-identity KHR_texture_transform is not representable in Scene");
        }
        const auto key=std::make_pair(info.textureIndex,srgb);
        if(auto found=texture_cache.find(key);found!=texture_cache.end())return found->second;
        require(info.textureIndex<asset.textures.size(),"Invalid texture index");const auto& source=asset.textures[info.textureIndex];
        require(source.imageIndex.has_value() && *source.imageIndex<asset.images.size(),"Texture has no supported image source");
        const auto& image=asset.images[*source.imageIndex];
        const auto encoded=std::holds_alternative<fastgltf::sources::BufferView>(image.data)?
            view_bytes(asset,std::get<fastgltf::sources::BufferView>(image.data).bufferViewIndex):bytes_of(image.data);
        require(encoded.size()<=std::size_t(std::numeric_limits<int>::max()),"Encoded image too large");
        int w=0,h=0,components=0;
        require(stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(encoded.data()),int(encoded.size()),&w,&h,&components)!=0,
                "Cannot inspect image: "+std::string(stbi_failure_reason()?stbi_failure_reason():"unknown codec error"));
        require(w>0 && h>0 && std::uint64_t(w)*h<=max_asset_elements-decoded_pixels,"Decoded texture pixel budget exceeded");
        std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> pixels(stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(encoded.data()),int(encoded.size()),&w,&h,&components,4),stbi_image_free);
        require(bool(pixels),"Image decode failed: "+std::string(stbi_failure_reason()?stbi_failure_reason():"unknown codec error"));
        Texture target;target.name=std::string(image.name);target.srgb=srgb;target.levels.emplace_back(w,h);
        for(std::size_t i=0;i<target.levels[0].pixels.size();++i) {
            const auto* p=pixels.get()+4*i;target.levels[0].pixels[i]=glm::vec4(p[0],p[1],p[2],p[3])/255.f;
        }
        decoded_pixels+=std::size_t(w)*h;build_asset_mips(target);
        TextureSampler sampler;
        if(source.samplerIndex) {
            require(*source.samplerIndex<asset.samplers.size(),"Invalid sampler index");const auto& s=asset.samplers[*source.samplerIndex];
            sampler.mag_filter=int(s.magFilter.value_or(fastgltf::Filter::Linear));
            sampler.min_filter=int(s.minFilter.value_or(fastgltf::Filter::LinearMipMapLinear));
            sampler.wrap_s=int(s.wrapS);sampler.wrap_t=int(s.wrapT);
        }
        target.wrap_s=Texture::Wrap(sampler.wrap_s);target.wrap_t=Texture::Wrap(sampler.wrap_t);
        target.min_filter=Texture::Filter(sampler.min_filter);target.mag_filter=Texture::Filter(sampler.mag_filter);
        const int index=int(scene.textures.size());scene.textures.push_back(std::move(target));result.samplers.push_back(sampler);texture_cache.emplace(key,index);return index;
    };
    for(const auto& source:asset.materials) {
        Material m;m.name=std::string(source.name);const auto& p=source.pbrData;
        m.base_color={p.baseColorFactor[0],p.baseColorFactor[1],p.baseColorFactor[2],p.baseColorFactor[3]};
        m.metallic=p.metallicFactor;m.roughness=p.roughnessFactor;
        m.emissive=glm::vec3(source.emissiveFactor[0],source.emissiveFactor[1],source.emissiveFactor[2])*source.emissiveStrength.value_or(1.f);
        m.alpha_mode=source.alphaMode==fastgltf::AlphaMode::Mask?1:source.alphaMode==fastgltf::AlphaMode::Blend?2:0;
        m.alpha_cutoff=source.alphaCutoff;m.double_sided=source.doubleSided;
        m.base_texture=texture(p.baseColorTexture,true);m.mr_texture=texture(p.metallicRoughnessTexture,false);
        m.normal_texture=texture(source.normalTexture,false);m.ao_texture=texture(source.occlusionTexture,false);m.emissive_texture=texture(source.emissiveTexture,true);
        if(source.normalTexture)m.normal_scale=source.normalTexture->scale;
        if(source.occlusionTexture)m.ao_strength=source.occlusionTexture->strength;
        if(source.clearcoat) {
            require(!source.clearcoat->clearcoatTexture && !source.clearcoat->clearcoatRoughnessTexture && !source.clearcoat->clearcoatNormalTexture,
                    "Clearcoat textures cannot be represented in shared Material");
            m.clearcoat=source.clearcoat->clearcoatFactor;m.clearcoat_roughness=source.clearcoat->clearcoatRoughnessFactor;
        }
        require(finite_vec(m.base_color)&&finite_vec(m.emissive)&&std::isfinite(m.roughness)&&std::isfinite(m.metallic),"Nonfinite material");
        scene.materials.push_back(std::move(m));
    }
    const auto default_material=std::uint32_t(scene.materials.size());
    Material fallback;fallback.name="glTF default material";fallback.base_color=glm::vec4(1);fallback.metallic=1;fallback.roughness=1;scene.materials.push_back(fallback);
    std::size_t total_vertices=0,total_indices=0;
    for(const auto& source:asset.meshes) {
        require(source.weights.empty(),"Morph weights are unsupported");Mesh mesh;mesh.name=std::string(source.name);
        for(const auto& primitive:source.primitives) {
            require(primitive.targets.empty(),"Morph targets are unsupported");
            require(primitive.type==fastgltf::PrimitiveType::Triangles || primitive.type==fastgltf::PrimitiveType::TriangleStrip || primitive.type==fastgltf::PrimitiveType::TriangleFan,
                    "Only triangle geometry can be imported");
            const auto pos=primitive.findAttribute("POSITION");require(pos!=primitive.attributes.end(),"Primitive is missing POSITION");
            const auto& positions=checked_accessor(asset,pos->second,fastgltf::AccessorType::Vec3);
            require(positions.count<=max_asset_elements-total_vertices,"Vertex budget exceeded");total_vertices+=positions.count;
            const auto start=mesh.vertices.size(),first_index=mesh.indices.size();mesh.vertices.resize(start+positions.count);
            fastgltf::iterateAccessorWithIndex<glm::vec3>(asset,positions,[&](glm::vec3 v,std::size_t i) {
                require(finite_vec(v),"Nonfinite vertex position");mesh.vertices[start+i].position=v;
            });
            auto attribute=[&]<class V>(std::string_view name,fastgltf::AccessorType type,V Vertex::*member) {
                const auto a=primitive.findAttribute(name);if(a==primitive.attributes.end())return false;
                const auto& values=checked_accessor(asset,a->second,type);require(values.count==positions.count,"Attribute count differs from POSITION");
                fastgltf::iterateAccessorWithIndex<V>(asset,values,[&](V v,std::size_t i){ require(finite_vec(v),"Nonfinite vertex attribute");mesh.vertices[start+i].*member=v; });return true;
            };
            const bool normals=attribute("NORMAL",fastgltf::AccessorType::Vec3,&Vertex::normal);
            const bool uv=attribute("TEXCOORD_0",fastgltf::AccessorType::Vec2,&Vertex::uv);
            const bool tangents=attribute("TANGENT",fastgltf::AccessorType::Vec4,&Vertex::tangent);
            const auto color=primitive.findAttribute("COLOR_0");
            if(color!=primitive.attributes.end()) {
                require(color->second<asset.accessors.size(),"Invalid COLOR_0 accessor");
                if(asset.accessors[color->second].type==fastgltf::AccessorType::Vec3) {
                    const auto& values=checked_accessor(asset,color->second,fastgltf::AccessorType::Vec3);require(values.count==positions.count,"Color count mismatch");
                    fastgltf::iterateAccessorWithIndex<glm::vec3>(asset,values,[&](glm::vec3 v,std::size_t i){require(finite_vec(v),"Nonfinite color");mesh.vertices[start+i].color=glm::vec4(v,1);});
                } else attribute("COLOR_0",fastgltf::AccessorType::Vec4,&Vertex::color);
            }
            std::vector<std::uint32_t> raw;
            if(primitive.indicesAccessor) {
                const auto& indices=checked_accessor(asset,*primitive.indicesAccessor,fastgltf::AccessorType::Scalar);
                require(!indices.normalized && (indices.componentType==fastgltf::ComponentType::UnsignedByte || indices.componentType==fastgltf::ComponentType::UnsignedShort ||
                        indices.componentType==fastgltf::ComponentType::UnsignedInt),"Invalid index accessor type");
                raw.reserve(indices.count);fastgltf::iterateAccessor<std::uint32_t>(asset,indices,[&](std::uint32_t v){require(v<positions.count,"Primitive index exceeds vertices");raw.push_back(v);});
            } else { raw.resize(positions.count);std::iota(raw.begin(),raw.end(),0u); }
            auto triangle=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c) {
                require(total_indices+3<=max_asset_elements,"Index budget exceeded");total_indices+=3;
                for(auto index:{a,b,c})mesh.indices.push_back(std::uint32_t(start)+index);
            };
            if(primitive.type==fastgltf::PrimitiveType::Triangles) {
                require(raw.size()%3==0,"Triangle index count is not a multiple of three");
                for(std::size_t i=0;i<raw.size();i+=3)triangle(raw[i],raw[i+1],raw[i+2]);
            } else {
                require(raw.size()>=3,"Triangle strip/fan needs three vertices");
                for(std::size_t i=2;i<raw.size();++i) {
                    if(primitive.type==fastgltf::PrimitiveType::TriangleFan)triangle(raw[0],raw[i-1],raw[i]);
                    else if(i%2==0)triangle(raw[i-2],raw[i-1],raw[i]);else triangle(raw[i-1],raw[i-2],raw[i]);
                }
            }
            const auto material=primitive.materialIndex?std::uint32_t(*primitive.materialIndex):default_material;
            require(material<scene.materials.size() && (!primitive.materialIndex || *primitive.materialIndex<asset.materials.size()),"Invalid material index");
            const auto& m=scene.materials[material];
            require(uv || (m.base_texture<0 && m.normal_texture<0 && m.mr_texture<0 && m.ao_texture<0 && m.emissive_texture<0),"Textured primitive has no TEXCOORD_0");
            if(!normals) {
                // glTF 缺少 NORMAL 时要求平面法线；复制每个三角形的顶点，避免错误地跨硬边平滑。
                std::vector<Vertex> flat;flat.reserve(mesh.indices.size()-first_index);
                for(auto i=first_index;i<mesh.indices.size();i+=3) {
                    auto a=mesh.vertices[mesh.indices[i]],b=mesh.vertices[mesh.indices[i+1]],c=mesh.vertices[mesh.indices[i+2]];
                    const auto n=geometry_unit(glm::cross(glm::dvec3(b.position)-glm::dvec3(a.position),glm::dvec3(c.position)-glm::dvec3(a.position)));a.normal=b.normal=c.normal=n;
                    for(auto v:{a,b,c})flat.push_back(v);
                }
                total_vertices-=positions.count;require(flat.size()<=max_asset_elements-total_vertices,"Flat-normal vertex budget exceeded");total_vertices+=flat.size();
                mesh.vertices.resize(start);mesh.vertices.insert(mesh.vertices.end(),flat.begin(),flat.end());
                for(auto i=first_index;i<mesh.indices.size();++i)mesh.indices[i]=std::uint32_t(start+i-first_index);
            } else for(auto i=start;i<mesh.vertices.size();++i) {
                const glm::dvec3 n(mesh.vertices[i].normal);
                require(glm::dot(n,n)>0,"Zero glTF normal"); mesh.vertices[i].normal=geometry_unit(n);
            }
            if(!tangents || !normals)generate_tangents(mesh,start,first_index,total_vertices);
            else for(auto i=start;i<mesh.vertices.size();++i) {
                auto& v=mesh.vertices[i]; const glm::dvec3 t(v.tangent);
                require(glm::dot(t,t)>0 && (v.tangent.w==-1 || v.tangent.w==1),"Invalid glTF tangent direction/handedness");
                v.tangent=glm::vec4(geometry_tangent(t,v.normal),v.tangent.w);
            }
            mesh.primitives.push_back({std::uint32_t(first_index),std::uint32_t(mesh.indices.size()-first_index),material});
        }
        scene.meshes.push_back(std::move(mesh));
    }
    std::vector<int> parents(asset.nodes.size(),-1);
    for(std::size_t i=0;i<asset.nodes.size();++i)for(auto child:asset.nodes[i].children) {
        require(child<asset.nodes.size() && child!=i && parents[child]<0,"Invalid glTF node hierarchy");parents[child]=int(i);
    }
    // 先验证整个节点森林，再只保留默认场景可达节点；同一 mesh 可被多个节点实例化。
    Scene hierarchy;hierarchy.nodes.resize(asset.nodes.size());
    for(std::size_t i=0;i<asset.nodes.size();++i){hierarchy.nodes[i].parent=parents[i];hierarchy.nodes[i].local=gltf_transform(asset.nodes[i]);}
    scene_world_transforms(hierarchy);
    std::vector<std::size_t> roots;
    if(!asset.scenes.empty()) {
        const auto selected=asset.defaultScene.value_or(0);require(selected<asset.scenes.size(),"Invalid default scene index");
        if(!asset.scenes[selected].name.empty())scene.name=std::string(asset.scenes[selected].name);
        roots.assign(asset.scenes[selected].nodeIndices.begin(),asset.scenes[selected].nodeIndices.end());
    } else for(std::size_t i=0;i<parents.size();++i)if(parents[i]<0)roots.push_back(i);
    std::vector<int> remap(asset.nodes.size(),-1);
    std::vector<std::pair<std::size_t,int>> pending;
    for(auto it=roots.rbegin();it!=roots.rend();++it){require(*it<asset.nodes.size() && parents[*it]<0,"Scene root is invalid or has a parent");pending.emplace_back(*it,-1);}
    std::vector<std::pair<std::size_t,std::size_t>> light_nodes;
    while(!pending.empty()) {
        const auto [id,parent]=pending.back();pending.pop_back();require(remap[id]<0,"Repeated node in active scene");const auto& source=asset.nodes[id];
        require(!source.skinIndex && source.weights.empty(),"Skin/morph node is unsupported");
        Node n;n.name=std::string(source.name);n.parent=parent;n.local=hierarchy.nodes[id].local;
        if(source.meshIndex){require(*source.meshIndex<scene.meshes.size(),"Invalid node mesh index");n.mesh=int(*source.meshIndex);}
        remap[id]=int(scene.nodes.size());scene.nodes.push_back(n);
        if(source.lightIndex)light_nodes.emplace_back(std::size_t(remap[id]),*source.lightIndex);
        for(auto it=source.children.rbegin();it!=source.children.rend();++it)pending.emplace_back(*it,remap[id]);
    }
    const auto worlds=scene_world_transforms(scene);
    for(std::size_t i=0;i<scene.nodes.size();++i)scene.nodes[i].previous_world=worlds[i];
    for(auto [node,id]:light_nodes) {
        require(id<asset.lights.size(),"Invalid punctual light index");const auto& source=asset.lights[id];
        require(source.type!=fastgltf::LightType::Spot,"Spot lights cannot be represented by shared Light");
        Light l;l.kind=source.type==fastgltf::LightType::Directional?LightKind::directional:LightKind::point;
        l.color={source.color[0],source.color[1],source.color[2]};l.intensity=source.intensity;l.range=source.range.value_or(1e30f);
        l.position=glm::vec3(worlds[node]*glm::vec4(0,0,0,1));l.direction=safe_normalize(glm::vec3(worlds[node]*glm::vec4(0,0,-1,0)),{0,0,-1});scene.lights.push_back(l);
    }
    return result;
}
Scene load_scene_asset(const std::filesystem::path& path) {
    auto loaded=load_scene_asset_detailed(path);
    return std::move(loaded.scene);
}

struct AssetPipeline::Entry {
    mutable std::mutex mutex;
    std::condition_variable changed;
    AssetSnapshot snapshot;
    std::exception_ptr error;
    std::uint64_t touched=0;
    std::weak_ptr<int> owner;
};
struct AssetPipeline::Impl {
    JobSystem& jobs;std::size_t capacity;Loader loader;
    mutable std::mutex mutex;
    std::mutex upload_mutex;
    std::map<std::pair<std::filesystem::path,std::uint64_t>,std::shared_ptr<Entry>> entries;
    std::uint64_t clock=0;
    std::shared_ptr<int> identity=std::make_shared<int>(0);
    Impl(JobSystem& j,std::size_t n,Loader f):jobs(j),capacity(n),loader(std::move(f)){}
};
namespace {
std::size_t scene_upload_bytes(const Scene& s) {
    std::size_t bytes=0;
    auto add=[&](std::size_t count,std::size_t stride) {
        require(count<=(std::numeric_limits<std::size_t>::max()-bytes)/stride,"Upload size overflow");bytes+=count*stride;
    };
    for(const auto& mesh:s.meshes){add(mesh.vertices.size(),sizeof(Vertex));add(mesh.indices.size(),sizeof(std::uint32_t));}
    for(const auto& texture:s.textures)for(const auto& level:texture.levels)add(level.pixels.size(),sizeof(glm::vec4));
    // Virtual payload: packed material/light/node streams have a stable declared size;
    // strings, allocator padding and std::vector internals are never GPU payload.
    add(s.materials.size(),128);add(s.nodes.size(),sizeof(glm::mat4)*2);add(s.lights.size(),64);return bytes;
}
}
AssetPipeline::AssetPipeline(JobSystem& jobs,std::size_t cache_entries,Loader loader)
    :impl_(std::make_unique<Impl>(jobs,cache_entries,std::move(loader))) {
    if(!cache_entries || !impl_->loader)throw std::invalid_argument("Asset cache needs capacity and a loader");
}
AssetPipeline::~AssetPipeline()=default;
AssetSnapshot AssetPipeline::Ticket::snapshot() const {
    if(!entry_)throw std::logic_error("Empty asset ticket");std::lock_guard lock(entry_->mutex);return entry_->snapshot;
}
std::shared_ptr<const Scene> AssetPipeline::Ticket::get() const {
    if(!entry_)throw std::logic_error("Empty asset ticket");
    if(worker_core)throw std::logic_error("Asset Ticket::get is a host-thread wait");
    std::unique_lock lock(entry_->mutex);entry_->changed.wait(lock,[&]{return entry_->snapshot.stage!=AssetStage::pending;});
    if(entry_->error)std::rethrow_exception(entry_->error);return entry_->snapshot.scene;
}
AssetPipeline::Ticket AssetPipeline::request(const std::filesystem::path& path,std::uint64_t version) {
    // 词法规范化不要求文件已存在：自定义 Loader 可以使用过程资源名，真实缺失文件由异步 Loader 报错。
    const auto key=std::make_pair(std::filesystem::absolute(path).lexically_normal(),version);
    Ticket ticket;
    {
        std::lock_guard lock(impl_->mutex);
        if(const auto found=impl_->entries.find(key);found!=impl_->entries.end()) {
            found->second->touched=++impl_->clock;ticket.entry_=found->second;return ticket;
        }
        if(impl_->entries.size()>=impl_->capacity) {
            auto victim=impl_->entries.end();
            for(auto it=impl_->entries.begin();it!=impl_->entries.end();++it) {
                std::lock_guard entry_lock(it->second->mutex);
                const auto stage=it->second->snapshot.stage;
                if(stage!=AssetStage::pending && stage!=AssetStage::upload_needed &&
                   (victim==impl_->entries.end() || it->second->touched<victim->second->touched))victim=it;
            }
            if(victim==impl_->entries.end())throw std::length_error("Asset cache is full of in-flight work");
            impl_->entries.erase(victim);
        }
        ticket.entry_=std::make_shared<Entry>();ticket.entry_->snapshot.version=version;
        ticket.entry_->owner=impl_->identity;ticket.entry_->touched=++impl_->clock;impl_->entries.emplace(key,ticket.entry_);
    }
    try {
        impl_->jobs.submit([entry=ticket.entry_,loader=impl_->loader,path=key.first] {
            try {
                auto scene=std::make_shared<Scene>(loader(path));const auto bytes=scene_upload_bytes(*scene);
                {std::lock_guard lock(entry->mutex);entry->snapshot.scene=std::move(scene);entry->snapshot.upload_bytes=bytes;entry->snapshot.stage=AssetStage::ready;}
            } catch(...) {
                std::lock_guard lock(entry->mutex);entry->error=std::current_exception();entry->snapshot.error=exception_text(entry->error);entry->snapshot.stage=AssetStage::failed;
            }
            entry->changed.notify_all();
        });
    } catch(...) {
        std::lock_guard lock(ticket.entry_->mutex);ticket.entry_->error=std::current_exception();
        ticket.entry_->snapshot.error=exception_text(ticket.entry_->error);ticket.entry_->snapshot.stage=AssetStage::failed;ticket.entry_->changed.notify_all();
        throw;
    }
    return ticket;
}
void AssetPipeline::queue_upload(const Ticket& ticket) {
    if(!ticket.entry_ || ticket.entry_->owner.lock()!=impl_->identity)throw std::invalid_argument("Foreign asset ticket");
    std::lock_guard cache_lock(impl_->mutex);
    require(std::any_of(impl_->entries.begin(),impl_->entries.end(),[&](const auto& item){return item.second==ticket.entry_;}),
            "Asset ticket was evicted; request it again before uploading");
    std::lock_guard lock(ticket.entry_->mutex);auto& stage=ticket.entry_->snapshot.stage;
    if(stage==AssetStage::pending)throw std::logic_error("Asset is still parsing");
    if(stage==AssetStage::failed)std::rethrow_exception(ticket.entry_->error);
    if(stage==AssetStage::ready)stage=AssetStage::upload_needed;
}
std::size_t AssetPipeline::pump_uploads(std::size_t budget,const Uploader& uploader) {
    if(!uploader)throw std::invalid_argument("Missing upload callback");
    std::lock_guard serial(impl_->upload_mutex);
    std::vector<std::shared_ptr<Entry>> entries;
    {
        std::lock_guard lock(impl_->mutex);
        for(const auto& [key,entry]:impl_->entries) {
            std::lock_guard entry_lock(entry->mutex);
            if(entry->snapshot.stage==AssetStage::ready)entry->snapshot.stage=AssetStage::upload_needed;
            entries.push_back(entry);
        }
    }
    std::size_t used=0;
    for(const auto& entry:entries) {
        std::size_t offset=0,amount=0;std::shared_ptr<const Scene> scene;
        {
            std::lock_guard lock(entry->mutex);auto& s=entry->snapshot;
            if(s.stage==AssetStage::ready)s.stage=AssetStage::upload_needed;
            if(s.stage!=AssetStage::upload_needed)continue;
            offset=s.uploaded_bytes;amount=std::min(budget-used,s.upload_bytes-offset);scene=s.scene;
            if(offset==s.upload_bytes){s.stage=AssetStage::uploaded;continue;}
        }
        if(!amount)continue;
        Ticket ticket;ticket.entry_=entry;
        try {uploader(ticket,*scene,offset,amount);}
        catch(...) {
            std::lock_guard lock(entry->mutex);entry->error=std::current_exception();entry->snapshot.error=exception_text(entry->error);entry->snapshot.stage=AssetStage::failed;
            entry->changed.notify_all();throw;
        }
        {std::lock_guard lock(entry->mutex);auto& s=entry->snapshot;s.uploaded_bytes+=amount;if(s.uploaded_bytes==s.upload_bytes)s.stage=AssetStage::uploaded;}
        used+=amount;
    }
    return used;
}
std::size_t AssetPipeline::cached_count() const {std::lock_guard lock(impl_->mutex);return impl_->entries.size();}

namespace {
// Keep every current Settings field in one table: serialization, migration defaults,
// validation and deterministic round-trip tests share the public struct's field names.
#define EF_SETTING_ENUMS(X) \
 X(path,RenderPath,3) X(shadows,ShadowMode,6) X(gi,GiMode,6) X(ao,AoMode,2) \
 X(shading,ShadingMode,3) X(culling,LightCulling,2) X(filter,FilterMode,3) X(debug,DebugView,11) \
 X(environment_diffuse,EnvironmentDiffuse,2) X(spatial_structure,SpatialStructure,1)
#define EF_SETTING_BOOLS(X) \
 X(reversed_z) X(energy_compensation) X(bloom) X(taa) X(denoise) X(svgf) X(outline) X(hatching) X(sdf_shadows) X(auto_lod) X(sparse_voxels)
#define EF_SETTING_FLOATS(X) \
 X(exposure) X(bloom_strength) X(bloom_threshold) X(ao_radius) X(ao_strength) X(shadow_bias) X(light_size) X(temporal_weight) X(lod_error_pixels) X(sdf_softness)
#define EF_SETTING_INTS(X) \
 X(samples) X(max_bounces) X(shadow_resolution) X(voxel_resolution) X(propagation_steps) X(render_width) X(render_height) X(bake_samples) X(sdf_resolution)
#define EF_MATERIAL_FLOATS(X) \
 X(metallic) X(roughness) X(normal_scale) X(ao_strength) X(clearcoat) X(clearcoat_roughness) X(anisotropy) X(sheen) X(alpha_cutoff)
#define EF_MATERIAL_INTS(X) X(base_texture) X(mr_texture) X(normal_texture) X(ao_texture) X(emissive_texture) X(alpha_mode)
constexpr std::size_t max_project_bytes=256*1024*1024;
std::string hex_string(std::string_view s) {
    if(s.empty())return "-";const char* alphabet="0123456789abcdef";std::string out;out.reserve(s.size()*2);
    for(unsigned char c:s){out.push_back(alphabet[c>>4]);out.push_back(alphabet[c&15]);}return out;
}
std::string unhex(std::string_view s) {
    if(s=="-")return {};require(s.size()%2==0,"Invalid hexadecimal string");std::string out;out.reserve(s.size()/2);
    auto digit=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;fail("Invalid hex digit");};
    for(std::size_t i=0;i<s.size();i+=2)out.push_back(char(digit(s[i])*16+digit(s[i+1])));return out;
}
template<class T> void read_value(std::istream& in,T& value) {
    if(!(in>>value))fail("Missing/invalid record value");
    if constexpr(std::is_floating_point_v<T>)require(std::isfinite(value),"Nonfinite record value");
}
void read_value(std::istream& in,bool& value) {int n=0;read_value(in,n);require(n==0||n==1,"Boolean must be 0 or 1");value=n!=0;}
template<glm::length_t N,typename T,glm::qualifier Q> void read_value(std::istream& in,glm::vec<N,T,Q>& v) {for(glm::length_t i=0;i<N;++i)read_value(in,v[i]);}
void read_value(std::istream& in,glm::mat4& m) {for(int c=0;c<4;++c)read_value(in,m[c]);}
template<glm::length_t N,typename T,glm::qualifier Q> void emit(std::ostream& out,const glm::vec<N,T,Q>& v) {for(glm::length_t i=0;i<N;++i)out<<' '<<v[i];}
void emit(std::ostream& out,const glm::mat4& m) {for(int c=0;c<4;++c)emit(out,m[c]);}
void finish_line(std::istream& line) {line>>std::ws;require(line.eof(),"Unexpected extra data in record");}
std::size_t index_value(std::istream& line,std::size_t size,bool append=false) {
    std::uint64_t id=0;read_value(line,id);require(append?id==size:id<size,"Record index out of range/order");return std::size_t(id);
}
void validate_project(const ProjectDocument& p) {
    const auto& s=p.scene;const auto& c=p.camera;const auto& settings=p.settings;
    require(finite_vec(s.sky_top)&&finite_vec(s.sky_bottom),"Nonfinite sky");
    validate_environment_settings(s);
    require(finite_vec(c.position)&&finite_vec(c.target)&&std::isfinite(c.fov)&&c.fov>0&&c.fov<179&&std::isfinite(c.near_plane)&&std::isfinite(c.far_plane)&&c.near_plane>0&&c.far_plane>c.near_plane,"Invalid camera");
#define CHECK_ENUM(name,type,last) require(int(settings.name)>=0 && int(settings.name)<=last,"Invalid settings enum: " #name);
    EF_SETTING_ENUMS(CHECK_ENUM)
#undef CHECK_ENUM
#define CHECK_FLOAT(name) require(std::isfinite(settings.name),"Nonfinite setting: " #name);
    EF_SETTING_FLOATS(CHECK_FLOAT)
#undef CHECK_FLOAT
    require(settings.render_width>0&&settings.render_height>0&&std::uint64_t(settings.render_width)*settings.render_height<=268435456ull,"Invalid render dimensions");
    require(settings.samples>0&&settings.max_bounces>=0&&settings.shadow_resolution>0&&settings.voxel_resolution>0&&settings.propagation_steps>=0,"Invalid integer settings");
    require(settings.exposure>=0&&settings.temporal_weight>=0&&settings.temporal_weight<=1,"Invalid exposure/temporal weight");
    require(settings.lod_error_pixels>0&&settings.lod_error_pixels<=4096&&settings.sdf_softness>0&&settings.sdf_softness<=128,"Invalid LOD/SDF quality");
    require(settings.bake_samples>=16&&settings.bake_samples<=4096&&settings.sdf_resolution>=8&&settings.sdf_resolution<=64,"Invalid scene bake budget");
    for(const auto& m:s.materials) {
        require(finite_vec(m.base_color)&&finite_vec(m.emissive),"Nonfinite material color");
#define CHECK_MFLOAT(name) require(std::isfinite(m.name),"Nonfinite material field: " #name);
        EF_MATERIAL_FLOATS(CHECK_MFLOAT)
#undef CHECK_MFLOAT
        for(auto index:{m.base_texture,m.mr_texture,m.normal_texture,m.ao_texture,m.emissive_texture})require(index>=-1&&(index<0||std::size_t(index)<s.textures.size()),"Material texture index out of range");
        require(m.alpha_mode>=0&&m.alpha_mode<=2,"Invalid material alpha mode");
    }
    for(const auto& mesh:s.meshes) {
        for(const auto& v:mesh.vertices)require(finite_vec(v.position)&&finite_vec(v.normal)&&finite_vec(v.uv)&&finite_vec(v.tangent)&&finite_vec(v.color),"Nonfinite vertex");
        for(auto index:mesh.indices)require(index<mesh.vertices.size(),"Mesh index out of range");
        for(const auto& primitive:mesh.primitives)require(primitive.material<s.materials.size()&&primitive.index_count%3==0&&primitive.first_index<=mesh.indices.size()&&primitive.index_count<=mesh.indices.size()-primitive.first_index,"Invalid mesh primitive");
        require(mesh.lods.size()<=8,"Too many mesh LOD levels");
        double previous_error=0;std::size_t previous_indices=mesh.indices.size();
        for(const auto& lod:mesh.lods) {
            require(std::isfinite(lod.geometric_error)&&lod.geometric_error>=previous_error,"Invalid accumulated LOD error");
            require(!lod.indices.empty()&&lod.indices.size()%3==0&&lod.indices.size()<previous_indices,"LOD must be a real triangle reduction");
            for(const auto& v:lod.vertices)require(finite_vec(v.position)&&finite_vec(v.normal)&&finite_vec(v.uv)&&finite_vec(v.tangent)&&finite_vec(v.color),"Nonfinite LOD vertex");
            for(auto index:lod.indices)require(index<lod.vertices.size(),"LOD index out of range");
            for(const auto& primitive:lod.primitives)require(primitive.material<s.materials.size()&&primitive.index_count%3==0&&primitive.first_index<=lod.indices.size()&&primitive.index_count<=lod.indices.size()-primitive.first_index,"Invalid LOD primitive");
            previous_error=lod.geometric_error;previous_indices=lod.indices.size();
        }
    }
    for(const auto& t:s.textures) {
        const auto wrap=[](Texture::Wrap w){return w==Texture::Wrap::repeat||w==Texture::Wrap::clamp_to_edge||w==Texture::Wrap::mirrored_repeat;};
        const int filter=int(t.min_filter);
        require(wrap(t.wrap_s)&&wrap(t.wrap_t)&&(filter==9728||filter==9729||(filter>=9984&&filter<=9987))&&
                (t.mag_filter==Texture::Filter::nearest||t.mag_filter==Texture::Filter::linear),"Invalid texture sampler");
        require(!t.levels.empty(),"Texture has no levels");
        for(const auto& l:t.levels){require(l.width>0&&l.height>0&&std::uint64_t(l.width)*l.height==l.pixels.size(),"Invalid texture level");for(const auto v:l.pixels)require(finite_vec(v),"Nonfinite texture pixel");}
    }
    for(const auto& n:s.nodes) {
        require(n.mesh>=-1&&(n.mesh<0||std::size_t(n.mesh)<s.meshes.size()),"Node mesh out of range");
        for(int col=0;col<4;++col)require(finite_vec(n.local[col])&&finite_vec(n.previous_world[col]),"Nonfinite node matrix");
    }
    scene_world_transforms(s);
    for(const auto& l:s.lights)require(int(l.kind)>=0&&int(l.kind)<=2&&finite_vec(l.position)&&finite_vec(l.direction)&&finite_vec(l.color)&&finite_vec(l.size)&&
                                     std::isfinite(l.intensity)&&l.intensity>=0&&std::isfinite(l.range)&&l.range>0,"Invalid light");
    for(std::size_t i=0;i<s.lights.size();++i) {
        require(s.lights[i].linked_node>=-1,"Invalid light binding node");
        if(s.lights[i].linked_node>=0)validate_light_binding(s,i);
    }
}
}

std::string serialize_project(const ProjectDocument& p) {
    validate_project(p);std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(std::numeric_limits<float>::max_digits10);
    const auto& scene=p.scene;const auto& settings=p.settings;
    out<<"EmberFrame scene format 1\nscene.name "<<hex_string(scene.name)<<"\nscene.revision "<<scene.revision<<"\nscene.sky";
    emit(out,scene.sky_top);emit(out,scene.sky_bottom);
    out<<"\nscene.environment "<<scene.environment_intensity<<' '<<scene.environment_rotation<<'\n';
    // 数据与工程一同保存；原导入文件移走、改名或复制工程到另一台机器均不失效。
    if(scene.environment_map)out<<"scene.environment_map "<<hex_string(serialize_environment(*scene.environment_map))<<'\n';
    out<<"camera";emit(out,p.camera.position);emit(out,p.camera.target);
    out<<' '<<p.camera.fov<<' '<<p.camera.near_plane<<' '<<p.camera.far_plane<<'\n';
#define WRITE_ENUM(name,type,last) out<<"settings." #name " "<<int(settings.name)<<'\n';
    EF_SETTING_ENUMS(WRITE_ENUM)
#undef WRITE_ENUM
#define WRITE_FIELD(name) out<<"settings." #name " "<<settings.name<<'\n';
    EF_SETTING_BOOLS(WRITE_FIELD) EF_SETTING_FLOATS(WRITE_FIELD) EF_SETTING_INTS(WRITE_FIELD) WRITE_FIELD(seed)
#undef WRITE_FIELD
    for(std::size_t id=0;id<scene.materials.size();++id) {
        const auto& m=scene.materials[id];out<<"material "<<id<<' '<<hex_string(m.name)<<'\n';
        out<<"material.base_color "<<id;emit(out,m.base_color);out<<"\nmaterial.emissive "<<id;emit(out,m.emissive);out<<'\n';
#define WRITE_MAT(name) out<<"material." #name " "<<id<<' '<<m.name<<'\n';
        EF_MATERIAL_FLOATS(WRITE_MAT) EF_MATERIAL_INTS(WRITE_MAT) WRITE_MAT(double_sided)
#undef WRITE_MAT
    }
    for(std::size_t id=0;id<scene.meshes.size();++id) {
        const auto& m=scene.meshes[id];out<<"mesh "<<id<<' '<<hex_string(m.name)<<'\n';
        for(const auto& v:m.vertices){out<<"vertex "<<id;emit(out,v.position);emit(out,v.normal);emit(out,v.uv);emit(out,v.tangent);emit(out,v.color);out<<'\n';}
        out<<"indices "<<id<<' '<<m.indices.size();for(auto i:m.indices)out<<' '<<i;out<<'\n';
        for(const auto& primitive:m.primitives)out<<"primitive "<<id<<' '<<primitive.first_index<<' '<<primitive.index_count<<' '<<primitive.material<<'\n';
        for(std::size_t level=0;level<m.lods.size();++level) {
            const auto& lod=m.lods[level];
            out<<"lod "<<id<<' '<<level<<' '<<std::setprecision(std::numeric_limits<double>::max_digits10)<<lod.geometric_error<<std::setprecision(std::numeric_limits<float>::max_digits10)<<'\n';
            for(const auto& v:lod.vertices){out<<"lod.vertex "<<id<<' '<<level;emit(out,v.position);emit(out,v.normal);emit(out,v.uv);emit(out,v.tangent);emit(out,v.color);out<<'\n';}
            out<<"lod.indices "<<id<<' '<<level<<' '<<lod.indices.size();for(auto i:lod.indices)out<<' '<<i;out<<'\n';
            for(const auto& primitive:lod.primitives)out<<"lod.primitive "<<id<<' '<<level<<' '<<primitive.first_index<<' '<<primitive.index_count<<' '<<primitive.material<<'\n';
        }
    }
    for(std::size_t id=0;id<scene.textures.size();++id) {
        const auto& t=scene.textures[id];out<<"texture "<<id<<' '<<t.srgb<<' '<<hex_string(t.name)<<'\n';
        out<<"texture.sampler "<<id<<' '<<int(t.wrap_s)<<' '<<int(t.wrap_t)<<' '<<int(t.min_filter)<<' '<<int(t.mag_filter)<<'\n';
        for(std::size_t mip=0;mip<t.levels.size();++mip) {
            const auto& l=t.levels[mip];out<<"level "<<id<<' '<<mip<<' '<<l.width<<' '<<l.height<<'\n';
            for(const auto pixel:l.pixels){out<<"pixel "<<id<<' '<<mip;emit(out,pixel);out<<'\n';}
        }
    }
    for(std::size_t id=0;id<scene.nodes.size();++id) {
        const auto& n=scene.nodes[id];out<<"node "<<id<<' '<<n.parent<<' '<<n.mesh<<' '<<hex_string(n.name);emit(out,n.local);emit(out,n.previous_world);out<<'\n';
    }
    for(std::size_t id=0;id<scene.lights.size();++id) {
        const auto& l=scene.lights[id];out<<"light "<<id<<' '<<int(l.kind);emit(out,l.position);emit(out,l.direction);emit(out,l.color);
        out<<' '<<l.intensity<<' '<<l.range;emit(out,l.size);out<<'\n';
        if(l.linked_node>=0)out<<"light_binding "<<id<<' '<<l.linked_node<<'\n';
    }
    // 派生资源带内容指纹和校验和；仍原子地保存在同一个工程文件中。
    // 失效缓存不写入：重新打开后按当前几何烘焙，而不是使用旧场景的传输/距离。
    if(scene.baked_resources&&scene.baked_resources->geometry_hash==geometry_fingerprint(scene))
        out<<"scene.bake "<<hex_string(serialize_scene_resources(*scene.baked_resources))<<'\n';
    for(const auto& line:p.unknown_lines){require(line.find('\n')==std::string::npos&&line.find('\r')==std::string::npos,"Unknown record contains line breaks");out<<line<<'\n';}
    auto result=out.str();require(result.size()<=max_project_bytes,"Project exceeds 256 MiB teaching-format limit");return result;
}

ProjectDocument deserialize_project(std::string_view bytes) {
    require(bytes.size()<=max_project_bytes,"Project exceeds 256 MiB teaching-format limit");
    std::istringstream input{std::string(bytes)};input.imbue(std::locale::classic());
    std::string raw;require(bool(std::getline(input,raw)),"Empty project");if(!raw.empty()&&raw.back()=='\r')raw.pop_back();
    ProjectDocument p;std::istringstream header(raw);std::string a,b,c;header>>a>>b>>c>>p.source_version;
    require(header&&a=="EmberFrame"&&b=="scene"&&c=="format"&&p.source_version<=1,"Unsupported EmberFrame scene format/version");finish_line(header);
    auto& scene=p.scene;auto& settings=p.settings;
    std::unordered_set<std::string> unique;
    std::vector<std::vector<std::size_t>> pixel_counts;
    std::vector<std::pair<std::size_t,int>> light_bindings;
    std::size_t vertices=0,indices=0,pixels=0,line_number=1;
    while(std::getline(input,raw)) {
        ++line_number;if(!raw.empty()&&raw.back()=='\r')raw.pop_back();
        std::istringstream line(raw);line.imbue(std::locale::classic());std::string tag;line>>tag;
        if(tag.empty()||tag[0]=='#'){p.unknown_lines.push_back(raw);continue;}
        if(p.source_version==0){if(tag=="exposure")tag="settings.exposure";if(tag=="scene_name")tag="scene.name";}
        bool known=true,repeatable=false;std::string uniqueness=tag;
        try {
            if(tag=="scene.name"){std::string value;read_value(line,value);scene.name=unhex(value);}
            else if(tag=="scene.revision")read_value(line,scene.revision);
            else if(tag=="scene.sky"){read_value(line,scene.sky_top);read_value(line,scene.sky_bottom);}
            else if(tag=="scene.environment"){read_value(line,scene.environment_intensity);read_value(line,scene.environment_rotation);}
            else if(tag=="scene.environment_map"){
                std::string data;read_value(line,data);
                require(data.size()<=2*(40+4096+EnvironmentLimits::pixels*12),"Embedded HDR exceeds byte budget");
                scene.environment_map=deserialize_environment(unhex(data));
            }
            else if(tag=="scene.bake"){std::string data;read_value(line,data);scene.baked_resources=deserialize_scene_resources(unhex(data));}
            else if(tag=="camera"){read_value(line,p.camera.position);read_value(line,p.camera.target);read_value(line,p.camera.fov);read_value(line,p.camera.near_plane);read_value(line,p.camera.far_plane);}
#define READ_ENUM(name,type,last) else if(tag=="settings." #name){int value;read_value(line,value);require(value>=0&&value<=last,"Invalid settings enum");settings.name=type(value);}
            EF_SETTING_ENUMS(READ_ENUM)
#undef READ_ENUM
#define READ_FIELD(name) else if(tag=="settings." #name)read_value(line,settings.name);
            EF_SETTING_BOOLS(READ_FIELD) EF_SETTING_FLOATS(READ_FIELD) EF_SETTING_INTS(READ_FIELD) READ_FIELD(seed)
#undef READ_FIELD
            else if(tag=="material") {
                index_value(line,scene.materials.size(),true);require(scene.materials.size()<100000,"Material limit exceeded");
                Material m;std::string name;read_value(line,name);m.name=unhex(name);scene.materials.push_back(m);repeatable=true;
            }
            else if(tag=="material.base_color"||tag=="material.emissive") {
                auto id=index_value(line,scene.materials.size());uniqueness+=' '+std::to_string(id);
                if(tag=="material.base_color")read_value(line,scene.materials[id].base_color);else read_value(line,scene.materials[id].emissive);
            }
#define READ_MAT(name) else if(tag=="material." #name){const auto id=index_value(line,scene.materials.size());uniqueness+=' '+std::to_string(id);read_value(line,scene.materials[id].name);}
            EF_MATERIAL_FLOATS(READ_MAT) EF_MATERIAL_INTS(READ_MAT) READ_MAT(double_sided)
#undef READ_MAT
            else if(tag=="mesh") {
                index_value(line,scene.meshes.size(),true);require(scene.meshes.size()<100000,"Mesh limit exceeded");
                Mesh m;std::string name;read_value(line,name);m.name=unhex(name);scene.meshes.push_back(std::move(m));repeatable=true;
            } else if(tag=="vertex") {
                auto id=index_value(line,scene.meshes.size());require(++vertices<=max_asset_elements,"Vertex limit exceeded");Vertex v;
                read_value(line,v.position);read_value(line,v.normal);read_value(line,v.uv);read_value(line,v.tangent);read_value(line,v.color);
                scene.meshes[id].vertices.push_back(v);repeatable=true;
            } else if(tag=="indices") {
                const auto id=index_value(line,scene.meshes.size());uniqueness+=' '+std::to_string(id);std::uint64_t count;read_value(line,count);
                require(count<=max_asset_elements-indices,"Index limit exceeded");indices+=std::size_t(count);
                auto& values=scene.meshes[id].indices;values.resize(std::size_t(count));for(auto& v:values)read_value(line,v);
            } else if(tag=="primitive") {
                const auto id=index_value(line,scene.meshes.size());Primitive primitive;
                read_value(line,primitive.first_index);read_value(line,primitive.index_count);read_value(line,primitive.material);
                require(scene.meshes[id].primitives.size()<max_asset_elements,"Primitive limit exceeded");scene.meshes[id].primitives.push_back(primitive);repeatable=true;
            } else if(tag=="lod") {
                const auto id=index_value(line,scene.meshes.size());auto& lods=scene.meshes[id].lods;
                index_value(line,lods.size(),true);require(lods.size()<8,"LOD limit exceeded");MeshLod lod;read_value(line,lod.geometric_error);lods.push_back(std::move(lod));repeatable=true;
            } else if(tag=="lod.vertex"||tag=="lod.indices"||tag=="lod.primitive") {
                const auto id=index_value(line,scene.meshes.size()),level=index_value(line,scene.meshes[id].lods.size());auto& lod=scene.meshes[id].lods[level];
                uniqueness+=' '+std::to_string(id)+' '+std::to_string(level);
                if(tag=="lod.vertex") {
                    require(++vertices<=max_asset_elements,"LOD vertex limit exceeded");Vertex v;read_value(line,v.position);read_value(line,v.normal);read_value(line,v.uv);read_value(line,v.tangent);read_value(line,v.color);lod.vertices.push_back(v);repeatable=true;
                } else if(tag=="lod.indices") {
                    std::uint64_t count;read_value(line,count);require(count<=max_asset_elements-indices,"LOD index limit exceeded");indices+=std::size_t(count);lod.indices.resize(std::size_t(count));for(auto& index:lod.indices)read_value(line,index);
                } else {
                    Primitive primitive;read_value(line,primitive.first_index);read_value(line,primitive.index_count);read_value(line,primitive.material);require(lod.primitives.size()<max_asset_elements,"LOD primitive limit exceeded");lod.primitives.push_back(primitive);repeatable=true;
                }
            } else if(tag=="texture") {
                index_value(line,scene.textures.size(),true);require(scene.textures.size()<100000,"Texture limit exceeded");Texture t;std::string name;
                read_value(line,t.srgb);read_value(line,name);t.name=unhex(name);scene.textures.push_back(std::move(t));pixel_counts.emplace_back();repeatable=true;
            } else if(tag=="texture.sampler") {
                const auto id=index_value(line,scene.textures.size());uniqueness+=' '+std::to_string(id);
                int s,t,min,mag;read_value(line,s);read_value(line,t);read_value(line,min);read_value(line,mag);
                auto& texture=scene.textures[id];texture.wrap_s=Texture::Wrap(s);texture.wrap_t=Texture::Wrap(t);texture.min_filter=Texture::Filter(min);texture.mag_filter=Texture::Filter(mag);
            } else if(tag=="level") {
                const auto id=index_value(line,scene.textures.size());index_value(line,scene.textures[id].levels.size(),true);
                require(scene.textures[id].levels.size()<32,"Mip level limit exceeded");int width,height;read_value(line,width);read_value(line,height);
                require(width>0&&height>0&&std::uint64_t(width)*height<=max_asset_elements-pixels,"Texture pixel limit exceeded");
                pixels+=std::size_t(width)*height;scene.textures[id].levels.emplace_back(width,height);pixel_counts[id].push_back(0);repeatable=true;
            } else if(tag=="pixel") {
                const auto id=index_value(line,scene.textures.size()),mip=index_value(line,scene.textures[id].levels.size());auto& count=pixel_counts[id][mip];
                auto& level=scene.textures[id].levels[mip];require(count<level.pixels.size(),"Extra texture pixels");read_value(line,level.pixels[count++]);repeatable=true;
            } else if(tag=="node") {
                index_value(line,scene.nodes.size(),true);require(scene.nodes.size()<100000,"Node limit exceeded");Node n;std::string name;
                read_value(line,n.parent);read_value(line,n.mesh);read_value(line,name);n.name=unhex(name);read_value(line,n.local);read_value(line,n.previous_world);
                scene.nodes.push_back(std::move(n));repeatable=true;
            } else if(tag=="light_binding") {
                std::uint64_t id;int node;read_value(line,id);read_value(line,node);
                require(id<100000&&node>=-1,"Invalid light binding reference");
                uniqueness+=' '+std::to_string(id);light_bindings.emplace_back(std::size_t(id),node);
            } else if(tag=="light") {
                index_value(line,scene.lights.size(),true);require(scene.lights.size()<100000,"Light limit exceeded");Light l;int kind;read_value(line,kind);l.kind=LightKind(kind);
                read_value(line,l.position);read_value(line,l.direction);read_value(line,l.color);read_value(line,l.intensity);read_value(line,l.range);read_value(line,l.size);
                scene.lights.push_back(l);repeatable=true;
            } else known=false;
            if(known){finish_line(line);if(!repeatable)require(unique.insert(uniqueness).second,"Duplicate record: "+uniqueness);}
            else p.unknown_lines.push_back(raw);
        } catch(const std::exception& e){fail("Project line "+std::to_string(line_number)+": "+e.what());}
    }
    for(std::size_t t=0;t<scene.textures.size();++t)for(std::size_t l=0;l<scene.textures[t].levels.size();++l)
        require(pixel_counts[t][l]==scene.textures[t].levels[l].pixels.size(),"Missing texture pixel records");
    for(const auto& [light,node]:light_bindings) {
        require(light<scene.lights.size(),"Light binding light index out of range");
        scene.lights[light].linked_node=node;
    }
    validate_project(p);
    if(scene.baked_resources&&scene.baked_resources->geometry_hash!=geometry_fingerprint(scene))
        scene.baked_resources.reset(); // 几何被其他编辑器改动：旧缓存失效，源场景仍可打开。
    return p;
}

ProjectDocument load_project_document(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);require(bool(file),"Cannot open project: "+path_text(path));
    const auto size=file.tellg();require(size>=0&&std::uint64_t(size)<=max_project_bytes,"Project file too large");
    std::string text(std::size_t(size),'\0');file.seekg(0);file.read(text.data(),std::streamsize(text.size()));require(bool(file),"Project read failed");return deserialize_project(text);
}
void save_project_document(const std::filesystem::path& path,const ProjectDocument& p) {
    const auto bytes=serialize_project(p);static std::atomic<std::uint64_t> counter{0};
    auto temporary=path;temporary+=".tmp-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(counter.fetch_add(1));
    try {
        {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);require(bool(file),"Cannot create temporary project file");
         file.write(bytes.data(),std::streamsize(bytes.size()));file.flush();require(bool(file),"Project write failed");file.close();require(bool(file),"Project close failed");}
        // 同目录临时文件完成后替换：失败时保留旧项目，不先删除目标文件。
#ifdef _WIN32
        require(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"Atomic project replacement failed");
#else
        std::filesystem::rename(temporary,path);
#endif
    } catch(...) {std::error_code ignored;std::filesystem::remove(temporary,ignored);throw;}
}
void save_project(const std::filesystem::path& path,const Scene& scene,const Camera& camera,const Settings& settings) {
    ProjectDocument p;
    if(std::filesystem::exists(path))p.unknown_lines=load_project_document(path).unknown_lines;
    p.scene=scene;p.camera=camera;p.settings=settings;save_project_document(path,p);
}
void load_project(const std::filesystem::path& path,Scene& scene,Camera& camera,Settings& settings) {
    auto p=load_project_document(path);scene=std::move(p.scene);camera=p.camera;settings=p.settings;
}

namespace {
std::uint64_t next_revision(std::uint64_t revision) {
    if(revision==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Scene revision exhausted");return revision+1;
}
std::string snapshot_project(const Scene& scene,const Camera& camera,const Settings& settings) {
    ProjectDocument p;p.scene=scene;p.camera=camera;p.settings=settings;return serialize_project(p);
}
void restore_snapshot(std::string_view text,Scene& scene,Camera& camera,Settings& settings) {
    auto p=deserialize_project(text);p.scene.revision=next_revision(std::max(scene.revision,p.scene.revision));
    scene=std::move(p.scene);camera=p.camera;settings=p.settings;
}
}
UndoStack::UndoStack(std::size_t max_entries,std::size_t max_bytes):max_entries_(max_entries),max_bytes_(max_bytes) {
    if(!max_entries || !max_bytes)throw std::invalid_argument("Undo history limits must be positive");
}
void UndoStack::push_applied(std::string label,std::function<void()> undo,std::function<void()> redo,std::size_t retained_bytes) {
    if(transaction_active())throw std::logic_error("Commit/cancel the active inspector transaction first");
    if(!undo || !redo)throw std::invalid_argument("Undo command needs both callbacks");
    if(retained_bytes>max_bytes_)throw std::length_error("Undo command exceeds history byte budget");
    Command command{std::move(label),std::move(undo),std::move(redo),retained_bytes};
    commands_.reserve(std::max(commands_.size(),cursor_+1));
    for(std::size_t i=cursor_;i<commands_.size();++i)bytes_-=commands_[i].bytes;
    commands_.erase(commands_.begin()+std::ptrdiff_t(cursor_),commands_.end());
    while(!commands_.empty()&&(commands_.size()>=max_entries_||bytes_>max_bytes_-retained_bytes)) {
        bytes_-=commands_.front().bytes;commands_.erase(commands_.begin());--cursor_;
    }
    bytes_+=retained_bytes;commands_.push_back(std::move(command));cursor_=commands_.size();
}
void UndoStack::execute(std::string label,std::function<void()> undo,std::function<void()> redo,std::size_t retained_bytes) {
    if(transaction_active())throw std::logic_error("Cannot execute a command inside an inspector transaction");
    if(!undo||!redo)throw std::invalid_argument("Undo command needs both callbacks");
    if(retained_bytes>max_bytes_)throw std::length_error("Undo command exceeds history byte budget");
    redo();
    try {push_applied(std::move(label),undo,redo,retained_bytes);}
    catch(...){auto error=std::current_exception();undo();std::rethrow_exception(error);}
}
void UndoStack::set_material(Scene& scene,std::size_t material,Material replacement,std::string label) {
    if(material>=scene.materials.size())throw std::out_of_range("Material index out of range");
    const Material previous=scene.materials[material];
    auto apply=[&scene,material](const Material& value) {
        if(material>=scene.materials.size())throw std::out_of_range("Material no longer exists; clear history after replacing scene");
        auto revision=next_revision(scene.revision);scene.materials[material]=value;scene.revision=revision;
    };
    const auto bytes=2*sizeof(Material)+previous.name.size()+replacement.name.size();
    execute(std::move(label),[apply,previous]{apply(previous);},[apply,replacement=std::move(replacement)]{apply(replacement);},bytes);
}
void UndoStack::begin(Scene& scene,Camera& camera,Settings& settings,std::string label) {
    if(transaction_active())throw std::logic_error("Nested inspector transactions are not supported");
    auto snapshot=snapshot_project(scene,camera,settings);
    if(snapshot.size()>max_bytes_/2)throw std::length_error("Scene snapshot exceeds undo byte budget");
    before_=std::move(snapshot);transaction_label_=std::move(label);transaction_scene_=&scene;transaction_camera_=&camera;transaction_settings_=&settings;
}
bool UndoStack::commit() {
    if(!transaction_active())throw std::logic_error("No active inspector transaction");
    auto* scene=transaction_scene_;auto* camera=transaction_camera_;auto* settings=transaction_settings_;
    auto after=snapshot_project(*scene,*camera,*settings);
    if(after==before_){transaction_scene_=nullptr;transaction_camera_=nullptr;transaction_settings_=nullptr;before_.clear();transaction_label_.clear();return false;}
    if(after.size()>max_bytes_-before_.size())throw std::length_error("Inspector transaction exceeds history byte budget");
    const auto revision=next_revision(scene->revision);
    auto undo=[scene,camera,settings,before=before_]{restore_snapshot(before,*scene,*camera,*settings);};
    auto redo=[scene,camera,settings,after]{restore_snapshot(after,*scene,*camera,*settings);};
    transaction_scene_=nullptr;
    try {push_applied(transaction_label_,std::move(undo),std::move(redo),before_.size()+after.size());}
    catch(...){transaction_scene_=scene;throw;}
    scene->revision=revision;transaction_camera_=nullptr;transaction_settings_=nullptr;before_.clear();transaction_label_.clear();return true;
}
void UndoStack::cancel() {
    if(!transaction_active())throw std::logic_error("No active inspector transaction");
    restore_snapshot(before_,*transaction_scene_,*transaction_camera_,*transaction_settings_);
    transaction_scene_=nullptr;transaction_camera_=nullptr;transaction_settings_=nullptr;before_.clear();transaction_label_.clear();
}
bool UndoStack::can_undo() const noexcept {return !transaction_active()&&cursor_>0;}
bool UndoStack::can_redo() const noexcept {return !transaction_active()&&cursor_<commands_.size();}
bool UndoStack::undo() {if(transaction_active())throw std::logic_error("Active inspector transaction");if(!can_undo())return false;commands_[cursor_-1].undo();--cursor_;return true;}
bool UndoStack::redo() {if(transaction_active())throw std::logic_error("Active inspector transaction");if(!can_redo())return false;commands_[cursor_].redo();++cursor_;return true;}
std::string UndoStack::undo_label() const {return can_undo()?commands_[cursor_-1].label:std::string{};}
std::string UndoStack::redo_label() const {return can_redo()?commands_[cursor_].label:std::string{};}
void UndoStack::clear() {if(transaction_active())throw std::logic_error("Cancel/commit transaction before clearing history");commands_.clear();cursor_=bytes_=0;}

#undef EF_SETTING_ENUMS
#undef EF_SETTING_BOOLS
#undef EF_SETTING_FLOATS
#undef EF_SETTING_INTS
#undef EF_MATERIAL_FLOATS
#undef EF_MATERIAL_INTS
} // namespace emberframe::lab
