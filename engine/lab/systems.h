#pragma once
#include "types.h"
#include "editor_lights.h"
#include <exception>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace emberframe::lab {

// C9: fixed worker count (1..64), bounded outstanding tasks, local LIFO / stolen FIFO.
// Only previously submitted tasks may be dependencies; foreign handles are rejected.
// Worker waits help run ready work. Saturated nested submissions with no runnable work
// throw length_error rather than deadlock. Destroy/shutdown from the owning host thread.
class JobSystem {
    struct Core;
    struct State;
public:
    class Task {
    public:
        Task() = default;
        bool valid() const noexcept { return bool(state_); }
        bool ready() const;
        void wait() const; // Wait only; get rethrows this task / prerequisite failure.
        void get() const;
    private:
        std::shared_ptr<Core> core_;
        std::shared_ptr<State> state_;
        friend class JobSystem;
    };
    explicit JobSystem(std::size_t threads = 0, std::size_t max_pending = 4096);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    Task submit(std::function<void()> function, std::span<const Task> dependencies = {});
    // Synchronous; each index in [begin,end) executes exactly once unless an exception
    // occurs. All launched chunks finish before an exception reaches the caller.
    void parallel_for(std::size_t begin, std::size_t end,
                      const std::function<void(std::size_t)>& function, std::size_t grain = 1);
    void parallel_for(std::size_t count, const std::function<void(std::size_t)>& function,
                      std::size_t grain = 1) { parallel_for(0, count, function, grain); }
    void wait_idle(); // Host-only; drains, then rethrows the first recorded failure.
    void shutdown();  // Drains accepted work, rejects new submissions, joins workers.
    std::size_t thread_count() const noexcept;
    std::size_t pending() const;
private:
    std::shared_ptr<Core> core_;
};

enum class ResourceAccess { read, write, read_write };
enum class ResourceState { undefined, shader_read, color_attachment, depth_attachment,
                           storage, transfer_src, transfer_dst, present, host_read };
struct ResourceUse {
    std::string resource;
    ResourceAccess access = ResourceAccess::read;
    ResourceState state = ResourceState::shader_read;
};
// Exported resources survive through last == order.size(), the external-consumer boundary.
struct ResourceLifetime { std::string resource; std::size_t first = 0, last = 0; bool imported = false, exported = false; };
struct ResourceBarrier {
    std::string resource;
    std::size_t before_pass = 0; // Index into GraphPlan::order, not pass declaration index.
    ResourceState before = ResourceState::undefined, after = ResourceState::undefined;
    ResourceAccess previous_access = ResourceAccess::read, next_access = ResourceAccess::read;
    bool memory_dependency = false; // Includes same-state RAW/WAR/WAW hazards.
};
struct GraphPlan {
    std::vector<std::size_t> order;
    std::vector<std::string> pass_names, culled_passes;
    // 编译后仍保留真实依赖；两端索引对应 pass_names 的执行顺序，而非声明序号。
    // 调试图据此展示 DAG，不能仅凭 Barrier 推测或把所有 Pass 串成一条链。
    std::vector<std::pair<std::size_t,std::size_t>> dependencies;
    std::vector<ResourceLifetime> lifetimes;
    std::vector<ResourceBarrier> barriers;
private:
    const void* owner_ = nullptr;
    std::uint64_t generation_ = 0;
    friend class RenderGraph;
};
// C6: declaration order versions resources. A read consumes the latest preceding
// write (or imported contents); a later writer does not repair read-before-write.
// Whole-resource, single-queue plan: no subresources, alias allocation or Vulkan calls.
// Exported resources' last writers and side-effect passes are culling roots.
class RenderGraph {
public:
    void add_resource(std::string name, bool imported = false,
                      ResourceState initial_state = ResourceState::undefined);
    void add_pass(std::string name, std::vector<ResourceUse> uses,
                  std::function<void()> callback, bool side_effect = false,
                  std::vector<std::string> depends_on = {});
    void export_resource(std::string_view name);
    GraphPlan compile() const; // Throws on unknown resources, bad reads, or any DAG cycle.
    void execute(const GraphPlan&, const std::function<void(const ResourceBarrier&)>& barrier = {}) const;
    void clear();
private:
    struct Resource { std::string name; bool imported; ResourceState initial; bool exported = false; };
    struct Pass { std::string name; std::vector<ResourceUse> uses; std::function<void()> callback;
                  bool side_effect; std::vector<std::string> dependencies; };
    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
    std::uint64_t generation_ = 0;
};

struct TextureSampler {
    // glTF enum values. Absent sampler/filter uses repeat, linear magnification,
    // trilinear minification as this application's documented "auto" policy.
    int mag_filter = 9729, min_filter = 9987, wrap_s = 10497, wrap_t = 10497;
    bool operator==(const TextureSampler&) const = default;
};
struct LoadedSceneAsset {
    Scene scene;
    std::vector<TextureSampler> samplers; // Exactly parallel to scene.textures.
    std::vector<std::string> warnings;
};
// Four fully procedural presets: 0 material studio, 1 colored-box GI room,
// 2 shadow test, 3 many lights. Invalid preset throws. Rectangle intensity is
// emitted radiance; point is intensity/r^2; directional is incident radiance.
Scene make_demo_scene(int preset);
// Static glTF 2.0/GLB, selected/default scene, triangle/list/strip/fan geometry,
// interleaved/normalized/sparse accessors, unbaked node transforms, PNG/JPEG.
// RGB stays encoded in Texture; sRGB is set only for base/emissive roles; alpha
// remains linear. The same source in color/data roles gets distinct Texture views.
// UV0 only. Skinning/morphs/required unsupported extensions fail explicitly;
// animation is a static bind-pose import with a warning in the detailed result.
// Missing normals are flat; generated tangents use UV derivatives + Gram-Schmidt,
// not MikkTSpace. glTF punctual-light numbers are preserved without photometric-to-
// radiometric conversion; the laboratory treats them in its common relative units.
// Samplers are retained on each Texture (including default repeat/linear settings).
// The detailed API also exposes numeric glTF sampler metadata and import warnings.
// Imported punctual lights capture world transforms at import time; linked_node
// stays -1 because the editor does not support punctual-light node bindings.
LoadedSceneAsset load_scene_asset_detailed(const std::filesystem::path&);
Scene load_scene_asset(const std::filesystem::path&);
std::vector<glm::mat4> scene_world_transforms(const Scene&); // Validates parents/cycles.

enum class AssetStage { pending, ready, upload_needed, uploaded, failed };
struct AssetSnapshot {
    AssetStage stage = AssetStage::pending;
    std::shared_ptr<const Scene> scene;
    std::uint64_t version = 0;
    std::size_t upload_bytes = 0, uploaded_bytes = 0;
    std::string error;
};
// C7: cache key is normalized absolute path + caller version. Callers must bump the
// version when ANY source/buffer/image changes. Cache entry count is bounded; external
// tickets may keep evicted CPU data alive. Parse errors are cached for that version.
class AssetPipeline {
    struct Entry;
    struct Impl;
public:
    class Ticket {
    public:
        Ticket() = default;
        bool valid() const noexcept { return bool(entry_); }
        AssetSnapshot snapshot() const;
        std::shared_ptr<const Scene> get() const; // Host wait for CPU readiness; rethrows parse errors.
    private:
        std::shared_ptr<Entry> entry_;
        friend class AssetPipeline;
    };
    using Loader = std::function<Scene(const std::filesystem::path&)>;
    // Callback receives a virtual upload byte range; the backend owns staging/GPU
    // resources and mapping of that range. No implicit GPU submission/completion claim.
    using Uploader = std::function<void(const Ticket&, const Scene&, std::size_t offset, std::size_t bytes)>;
    explicit AssetPipeline(JobSystem&, std::size_t cache_entries = 32, Loader = load_scene_asset);
    ~AssetPipeline();
    AssetPipeline(const AssetPipeline&) = delete;
    AssetPipeline& operator=(const AssetPipeline&) = delete;
    Ticket request(const std::filesystem::path&, std::uint64_t version = 1);
    void queue_upload(const Ticket&); // ready -> upload_needed, idempotent thereafter.
    // Promotes ready entries, then uploads at most budget bytes, including partial
    // large assets. Callback runs on caller thread, outside entry locks. A throwing
    // callback marks that asset failed and rethrows; backend cleans partial GPU work.
    std::size_t pump_uploads(std::size_t budget, const Uploader&);
    std::size_t cached_count() const;
private:
    std::unique_ptr<Impl> impl_;
};

// C11: deterministic UTF-8 tagged "EmberFrame scene format", version 1.
// Names are byte-hex encoded; mesh/texture data is embedded losslessly as decimal
// float round trips. This is an editable teaching format, not a compressed archive.
// Unknown lines are preserved verbatim (relocated to the end on save). Version 0
// accepts the same records plus "exposure"/"scene_name" legacy aliases and defaults
// missing Settings fields. Newer versions fail instead of silently down-converting.
// Optional "light_binding <light index> <node index>" records preserve unique
// rectangle emitter bindings. Older documents omit them and remain unbound.
struct ProjectDocument {
    Scene scene;
    Camera camera;
    Settings settings;
    std::vector<std::string> unknown_lines;
    unsigned source_version = 1;
};
std::string serialize_project(const ProjectDocument&);
ProjectDocument deserialize_project(std::string_view);
void save_project_document(const std::filesystem::path&, const ProjectDocument&);
ProjectDocument load_project_document(const std::filesystem::path&);
// These convenience calls preserve unknown lines already in the destination file.
// For save-as/independent documents keep ProjectDocument and use the functions above.
void save_project(const std::filesystem::path&, const Scene&, const Camera&, const Settings&);
void load_project(const std::filesystem::path&, Scene&, Camera&, Settings&);

// C11 inspector history. Callbacks/transaction targets must outlive this stack.
// One drag = begin, arbitrary field edits, commit; cancel restores the snapshot.
// New commands discard redo. Failed undo/redo keeps the cursor unchanged.
// Scene revision increases on commit/undo/redo/cancel so render history is invalidated.
class UndoStack {
public:
    explicit UndoStack(std::size_t max_entries = 64, std::size_t max_bytes = 64 * 1024 * 1024);
    void execute(std::string label, std::function<void()> undo, std::function<void()> redo,
                 std::size_t retained_bytes = 0);
    void push_applied(std::string label, std::function<void()> undo, std::function<void()> redo,
                      std::size_t retained_bytes = 0);
    void set_material(Scene&, std::size_t material, Material replacement, std::string label = "Material edit");
    void begin(Scene&, Camera&, Settings&, std::string label = "Inspector edit");
    bool commit(); // Returns false for an unchanged transaction.
    void cancel();
    bool undo();
    bool redo();
    bool can_undo() const noexcept;
    bool can_redo() const noexcept;
    bool transaction_active() const noexcept { return transaction_scene_ != nullptr; }
    std::string undo_label() const;
    std::string redo_label() const;
    void clear(); // Active transaction must be committed/cancelled first.
private:
    struct Command { std::string label; std::function<void()> undo, redo; std::size_t bytes; };
    std::vector<Command> commands_;
    std::size_t cursor_ = 0, bytes_ = 0, max_entries_, max_bytes_;
    Scene* transaction_scene_ = nullptr;
    Camera* transaction_camera_ = nullptr;
    Settings* transaction_settings_ = nullptr;
    std::string before_, transaction_label_;
};

TestResults test_systems();
} // namespace emberframe::lab
