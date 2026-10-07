#pragma once
#include "mesh_lod.h"
#include <memory>

namespace emberframe::lab {

struct SceneObjectBounds {
    int node=-1,mesh=-1; // 无显式节点时，node 与隐式实例编号（mesh）一致。
    Aabb bounds;
};
struct SceneRayPick {
    Hit hit;
    int node=-1,mesh=-1,material=-1;
    glm::vec3 position{0},normal{0,1,0};
    explicit operator bool() const { return hit.triangle>=0; }
};

// 当前场景的 CPU 三角形空间索引；Octree 是三角形树，并非 GPU 稀疏体素。
// prepare 的内容指纹涵盖几何和世界变换；sky/revision/previous_world 不触发建树。
class SceneSpatialIndex {
public:
    SceneSpatialIndex();
    ~SceneSpatialIndex();
    SceneSpatialIndex(SceneSpatialIndex&&) noexcept;
    SceneSpatialIndex& operator=(SceneSpatialIndex&&) noexcept;
    SceneSpatialIndex(const SceneSpatialIndex&)=delete;
    SceneSpatialIndex& operator=(const SceneSpatialIndex&)=delete;
    void prepare(const Scene& scene,SpatialStructure structure=SpatialStructure::bvh);
    Hit intersect(const Ray& ray,TraversalStatistics* statistics=nullptr) const; // 渲染热路径只需命中，省略 UI 元数据计算。
    SceneRayPick pick(const Ray& ray,TraversalStatistics* statistics=nullptr) const;
    bool occluded(const Ray& ray,TraversalStatistics* statistics=nullptr) const;
    // 保守对象 AABB 重叠查询，返回排序且去重的原节点编号。
    std::vector<int> query_bounds(const Aabb& region) const;
    // Vulkan 0..w 深度视锥，保留与近平面相交的对象；不会修改渲染/阴影几何。
    std::vector<int> visible_objects(const Camera& camera,int width,int height) const;
    const std::vector<SceneObjectBounds>& objects() const;
    const HierarchyStatistics& statistics() const;
    std::uint64_t rebuilds() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct SceneGeometryStatistics {
    std::uint64_t source_triangles=0; // 所有实例在 LOD0 的实际三角形数。
    std::uint64_t selected_triangles=0; // 当前渲染快照提交的实际三角形数，含视野外阴影对象。
    std::vector<std::size_t> current_levels; // 节点顺序；无节点时按 mesh 顺序，LOD0=原网格。
    std::size_t candidates=0; // 当前视锥可见对象候选数，不代表已删除其他对象。
    std::size_t spatial_nodes=0; // 实际 BVH/三角形 Octree 节点数。
    std::size_t spatial_leaves=0; // 实际空间索引叶节点数。
    std::uint64_t lod_builds=0; // prepare 不执行 QEM，此计数保持 0；显式生成由后台调用方统计。
    std::uint64_t lod_cache_loads=0; // 实际接受持久化链的累计次数。
    std::uint64_t spatial_rebuilds=0; // 当前场景 BVH/Octree 的累计建树次数。
    std::uint64_t snapshot_changes=0; // 真实渲染内容或选级改变次数。
    std::size_t reduced_meshes=0; // 具有可用、真实减面持久化链的源网格数。
    std::string status; // 可直接显示的中文选级/预算状态。
    std::vector<MeshLodBuildReport> lod_reports; // 逐网格加载/缺链/失效/预算原因。
};

// prepare 返回 runtime 自有快照，直到下次 prepare 有效。可直接传给 CPU/GPU 渲染器。
// 源 Scene 不被改写；同一 mesh 的不同实例可引用不同快照变体，父子关系/材质/运动历史保留。
// 选择稳定时不会逐帧增加 revision；距离、FOV、视口、非均匀缩放及近平面均参与误差约束。
// prepare 仅消费 mesh.lods，不隐式执行耗时 QEM；缺链/失效时保持 LOD0 并报告后台生成提示。
// 默认 asset_revision==0：几何原地编辑由完整属性指纹发现；纹理像素原地编辑需增加源 revision。
// 编辑器使用非零 asset_revision 时，静态网格/纹理修改必须增加该版本，节点姿态修改增加 revision。
// 遵守此显式契约后，稳定帧复用网格指纹与空间索引，不逐帧扫描全部顶点/纹理像素。
class SceneGeometryRuntime {
public:
    SceneGeometryRuntime();
    ~SceneGeometryRuntime();
    SceneGeometryRuntime(SceneGeometryRuntime&&) noexcept;
    SceneGeometryRuntime& operator=(SceneGeometryRuntime&&) noexcept;
    SceneGeometryRuntime(const SceneGeometryRuntime&)=delete;
    SceneGeometryRuntime& operator=(const SceneGeometryRuntime&)=delete;
    const Scene& prepare(const Scene& source,const Camera& camera,const Settings& settings);
    const SceneGeometryStatistics& stats() const;
    std::uint64_t selectedTriangles() const;
    const std::string& status() const;
    const SceneSpatialIndex& spatial_index() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

TestResults test_scene_geometry();
} // namespace emberframe::lab
