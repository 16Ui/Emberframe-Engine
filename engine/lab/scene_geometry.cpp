#include "scene_geometry.h"
#include <bit>
#include <map>
#include <utility>

namespace emberframe::lab {
namespace {
// 不散列结构体内存，避免 padding 或 GLM 对齐差异导致逐帧误判变化。
struct Fingerprint {
    std::uint64_t value=14695981039346656037ull;
    void add(std::uint64_t v) {
        for(int i=0;i<8;++i) { value^=(v>>(i*8))&255; value*=1099511628211ull; }
    }
    void real(float v) { add(std::bit_cast<std::uint32_t>(v)); }
    void real(double v) { add(std::bit_cast<std::uint64_t>(v)); }
    template<glm::length_t N> void vector(const glm::vec<N,float>& v) { for(int i=0;i<N;++i) real(v[i]); }
    void matrix(const glm::mat4& v) { for(int i=0;i<4;++i) vector(v[i]); }
    void string(const std::string& v) { add(v.size()); for(unsigned char c:v) add(c); }
};
template<class Geometry> void hash_geometry(Fingerprint& h,const Geometry& mesh) {
    h.add(mesh.vertices.size()); h.add(mesh.indices.size()); h.add(mesh.primitives.size());
    for(const auto& v:mesh.vertices) {
        h.vector(v.position); h.vector(v.normal); h.vector(v.uv); h.vector(v.tangent);
        h.vector(v.color); h.vector(v.baked_irradiance);
    }
    for(const auto i:mesh.indices) h.add(i);
    for(const auto& p:mesh.primitives) { h.add(p.first_index); h.add(p.index_count); h.add(p.material); }
}
std::uint64_t base_fingerprint(const Mesh& mesh) { Fingerprint h; hash_geometry(h,mesh); return h.value; }
std::uint64_t lod_fingerprint(const Mesh& mesh) {
    Fingerprint h; h.add(mesh.lods.size());
    for(const auto& lod:mesh.lods) { hash_geometry(h,lod); h.real(lod.geometric_error); }
    return h.value;
}
std::uint64_t spatial_fingerprint(const Scene& scene) {
    Fingerprint h; h.add(scene.meshes.size()); h.add(scene.nodes.size());
    for(const auto& mesh:scene.meshes) hash_geometry(h,mesh);
    for(const auto& node:scene.nodes) { h.add(std::uint64_t(node.parent)); h.add(std::uint64_t(node.mesh)); h.matrix(node.local); }
    return h.value;
}
std::uint64_t texture_storage_fingerprint(const Scene& scene) {
    Fingerprint h; h.add(scene.textures.size());
    for(const auto& t:scene.textures) {
        h.add(t.levels.size());
        for(const auto& level:t.levels) {
            h.add(std::uint64_t(level.width)); h.add(std::uint64_t(level.height)); h.add(level.pixels.size());
            h.add(std::uint64_t(reinterpret_cast<std::uintptr_t>(level.pixels.data())));
        }
    }
    return h.value;
}
std::uint64_t texture_payload_fingerprint(const Scene& scene) {
    Fingerprint h;
    for(const auto& t:scene.textures) for(const auto& level:t.levels)
        for(const auto& pixel:level.pixels) h.vector(pixel);
    return h.value;
}
std::uint64_t texture_descriptor_fingerprint(const Scene& scene,std::uint64_t payload) {
    Fingerprint h; h.add(payload); h.add(scene.textures.size());
    for(const auto& t:scene.textures) {
        h.string(t.name); h.add(t.srgb); h.add(std::uint64_t(t.wrap_s)); h.add(std::uint64_t(t.wrap_t));
        h.add(std::uint64_t(t.min_filter)); h.add(std::uint64_t(t.mag_filter)); h.add(t.levels.size());
        for(const auto& level:t.levels) {
            h.add(std::uint64_t(level.width)); h.add(std::uint64_t(level.height)); h.add(level.pixels.size());
        }
    }
    return h.value;
}
std::uint64_t presentation_fingerprint(const Scene& scene,std::uint64_t texture_payload) {
    Fingerprint h; h.string(scene.name); h.vector(scene.sky_top); h.vector(scene.sky_bottom);
    h.add(texture_payload);
    h.add(scene.materials.size()); h.add(scene.textures.size()); h.add(scene.lights.size());
    for(const auto& m:scene.materials) {
        h.string(m.name); h.vector(m.base_color); h.vector(m.emissive);
        for(float v:{m.metallic,m.roughness,m.normal_scale,m.ao_strength,m.clearcoat,m.clearcoat_roughness,
                     m.anisotropy,m.sheen,m.alpha_cutoff}) h.real(v);
        for(int v:{m.base_texture,m.mr_texture,m.normal_texture,m.ao_texture,m.emissive_texture,m.alpha_mode}) h.add(std::uint64_t(v));
        h.add(m.double_sided);
    }
    for(const auto& t:scene.textures) {
        h.string(t.name); h.add(t.srgb); h.add(std::uint64_t(t.wrap_s)); h.add(std::uint64_t(t.wrap_t));
        h.add(std::uint64_t(t.min_filter)); h.add(std::uint64_t(t.mag_filter)); h.add(t.levels.size());
        for(const auto& level:t.levels) {
            h.add(std::uint64_t(level.width)); h.add(std::uint64_t(level.height)); h.add(level.pixels.size());
        }
    }
    for(const auto& l:scene.lights) {
        h.add(std::uint64_t(l.kind)); h.vector(l.position); h.vector(l.direction); h.vector(l.color);
        h.real(l.intensity); h.real(l.range); h.vector(l.size);
    }
    for(const auto& m:scene.meshes) h.string(m.name);
    for(const auto& n:scene.nodes) h.string(n.name);
    return h.value;
}
template<class Geometry> std::size_t triangle_count(const Geometry& mesh) {
    const std::size_t available=mesh.indices.empty()?mesh.vertices.size():mesh.indices.size();
    std::size_t count=0;
    auto range=[&](std::size_t first,std::size_t size) {
        if(size%3 || first>available || size>available-first) throw std::invalid_argument("Invalid LOD primitive range");
        count+=size/3;
        for(std::size_t i=first;i<first+size;++i) {
            const auto index=mesh.indices.empty()?i:mesh.indices[i];
            if(index>=mesh.vertices.size()) throw std::invalid_argument("LOD index out of range");
            const auto& v=mesh.vertices[index];
            auto finite=[](const auto& attr) {
                for(glm::length_t k=0;k<attr.length();++k) if(!std::isfinite(attr[k])) return false;
                return true;
            };
            if(!finite(v.position) || !finite(v.normal) || !finite(v.uv) || !finite(v.tangent) ||
               !finite(v.color) || !finite(v.baked_irradiance)) throw std::invalid_argument("Non-finite LOD vertex attribute");
        }
    };
    if(mesh.primitives.empty()) range(0,available);
    else for(const auto& p:mesh.primitives) range(p.first_index,p.index_count);
    return count;
}
// 仅使用实际被 primitive 引用的顶点构造原始包围盒，避免未引用的离群点影响选级。
Aabb mesh_bounds(const Mesh& mesh) {
    Aabb box;
    const auto available=mesh.indices.empty()?mesh.vertices.size():mesh.indices.size();
    auto append=[&](std::size_t first,std::size_t count) {
        for(std::size_t i=first;i<first+count;++i) box.expand(mesh.vertices[mesh.indices.empty()?i:mesh.indices[i]].position);
    };
    if(mesh.primitives.empty()) append(0,available);
    else for(const auto& p:mesh.primitives) append(p.first_index,p.index_count);
    return box;
}
Mesh variant(const Mesh& base,const MeshLod& level,std::size_t index) {
    Mesh mesh; mesh.name=base.name+" (LOD"+std::to_string(index)+")";
    mesh.vertices=level.vertices; mesh.indices=level.indices; mesh.primitives=level.primitives;
    return mesh;
}
bool visible(const Aabb& box,const glm::dmat4& vp) {
    if(box.empty()) return false;
    std::array<glm::dvec4,8> corners;
    for(int i=0;i<8;++i) corners[std::size_t(i)]=vp*glm::dvec4(
        (i&1)?box.max.x:box.min.x,(i&2)?box.max.y:box.min.y,(i&4)?box.max.z:box.min.z,1);
    // 只有全部八角都严格位于同一裁剪平面外时剔除，边界/近平面相交保守保留。
    for(int plane=0;plane<6;++plane) {
        bool all_out=true;
        for(const auto& p:corners) {
            const double d=plane==0?p.w+p.x:plane==1?p.w-p.x:plane==2?p.w+p.y:
                           plane==3?p.w-p.y:plane==4?p.z:p.w-p.z;
            const double tolerance=1e-6*std::max(1.0,std::abs(p.w));
            if(!std::isfinite(d) || d>=-tolerance) { all_out=false; break; }
        }
        if(all_out) return false;
    }
    return true;
}
} // namespace

struct SceneSpatialIndex::Impl {
    std::uint64_t fingerprint=0,rebuild_count=0;
    const Scene* source_identity=nullptr;
    std::uint64_t source_revision=0,source_assets=0;
    bool initialized=false;
    SpatialStructure structure=SpatialStructure::bvh;
    std::unique_ptr<Bvh> bvh;
    std::unique_ptr<Octree> octree;
    std::vector<SceneObjectBounds> objects;
    const std::vector<Triangle>& triangles() const {
        static const std::vector<Triangle> empty;
        return bvh?bvh->triangles():octree?octree->triangles():empty;
    }
};
SceneSpatialIndex::SceneSpatialIndex():impl_(std::make_unique<Impl>()) {}
SceneSpatialIndex::~SceneSpatialIndex()=default;
SceneSpatialIndex::SceneSpatialIndex(SceneSpatialIndex&&) noexcept=default;
SceneSpatialIndex& SceneSpatialIndex::operator=(SceneSpatialIndex&&) noexcept=default;
void SceneSpatialIndex::prepare(const Scene& scene,SpatialStructure structure) {
    if(structure!=SpatialStructure::bvh && structure!=SpatialStructure::octree) throw std::invalid_argument("Invalid spatial structure");
    // 编辑器显式提供资源/姿态版本时，静止帧不再为“树是否改变”扫描每个顶点。
    // 未使用版本契约的调用者仍走完整指纹，保留原地几何修改的检测能力。
    if(scene.asset_revision&&impl_->initialized&&impl_->source_identity==&scene&&
       impl_->source_revision==scene.revision&&impl_->source_assets==scene.asset_revision&&impl_->structure==structure)return;
    const auto fingerprint=spatial_fingerprint(scene);
    if(impl_->initialized && impl_->fingerprint==fingerprint && impl_->structure==structure){
        impl_->source_identity=&scene;impl_->source_revision=scene.revision;impl_->source_assets=scene.asset_revision;return;
    }
    const auto triangles=flatten_scene(scene);
    std::vector<SceneObjectBounds> objects;
    std::map<int,std::size_t> lookup;
    for(const auto& triangle:triangles) {
        const int object=triangle.object;
        const int mesh=scene.nodes.empty()?object:scene.nodes.at(std::size_t(object)).mesh;
        auto [it,inserted]=lookup.emplace(object,objects.size());
        if(inserted) objects.push_back({object,mesh,{}});
        objects[it->second].bounds.expand(triangle_bounds(triangle));
    }
    // 新树成功建好后才提交；失败时已有索引仍可查询。
    std::unique_ptr<Bvh> bvh;
    std::unique_ptr<Octree> octree;
    if(structure==SpatialStructure::bvh) bvh=std::make_unique<Bvh>(triangles);
    else octree=std::make_unique<Octree>(triangles);
    impl_->bvh=std::move(bvh); impl_->octree=std::move(octree); impl_->objects=std::move(objects);
    impl_->fingerprint=fingerprint; impl_->structure=structure; impl_->initialized=true; ++impl_->rebuild_count;
    impl_->source_identity=&scene;impl_->source_revision=scene.revision;impl_->source_assets=scene.asset_revision;
}
Hit SceneSpatialIndex::intersect(const Ray& ray,TraversalStatistics* statistics) const {
    return impl_->bvh?impl_->bvh->intersect(ray,statistics):impl_->octree?impl_->octree->intersect(ray,statistics):Hit{};
}
SceneRayPick SceneSpatialIndex::pick(const Ray& ray,TraversalStatistics* statistics) const {
    SceneRayPick result;
    result.hit=intersect(ray,statistics);
    if(!result) return result;
    const auto& t=impl_->triangles().at(std::size_t(result.hit.triangle));
    result.node=t.object; result.material=t.material;
    for(const auto& object:impl_->objects) if(object.node==result.node) { result.mesh=object.mesh; break; }
    result.position=ray.origin+ray.direction*result.hit.t;
    glm::vec3 normal(0);
    for(int i=0;i<3;++i) normal+=t.vertices[std::size_t(i)].normal*result.hit.barycentric[i];
    result.normal=safe_normalize(normal,safe_normalize(glm::cross(t.vertices[1].position-t.vertices[0].position,
                                                               t.vertices[2].position-t.vertices[0].position)));
    return result;
}
bool SceneSpatialIndex::occluded(const Ray& ray,TraversalStatistics* statistics) const {
    return impl_->bvh?impl_->bvh->occluded(ray,statistics):impl_->octree?impl_->octree->occluded(ray,statistics):false;
}
std::vector<int> SceneSpatialIndex::query_bounds(const Aabb& region) const {
    std::vector<int> ids;
    // 对象 bounds 包含不同三角形之间的空隙；按对象语义查询，不能仅用命中三角形替代。
    for(const auto& object:impl_->objects) if(object.bounds.overlaps(region)) ids.push_back(object.node);
    std::sort(ids.begin(),ids.end()); return ids;
}
std::vector<int> SceneSpatialIndex::visible_objects(const Camera& camera,int width,int height) const {
    if(width<=0 || height<=0 || !std::isfinite(camera.fov) || camera.fov<=0 || camera.fov>=180)
        throw std::invalid_argument("Invalid frustum viewport/camera");
    const glm::dmat4 vp=glm::dmat4(camera.projection(float(width)/height))*glm::dmat4(camera.view());
    std::vector<int> ids;
    for(const auto& object:impl_->objects) if(visible(object.bounds,vp)) ids.push_back(object.node);
    std::sort(ids.begin(),ids.end()); return ids;
}
const std::vector<SceneObjectBounds>& SceneSpatialIndex::objects() const { return impl_->objects; }
const HierarchyStatistics& SceneSpatialIndex::statistics() const {
    static const HierarchyStatistics empty;
    return impl_->bvh?impl_->bvh->statistics():impl_->octree?impl_->octree->statistics():empty;
}
std::uint64_t SceneSpatialIndex::rebuilds() const { return impl_->rebuild_count; }

struct SceneGeometryRuntime::Impl {
    struct Cache {
        bool initialized=false;
        std::uint64_t base_hash=0,lod_hash=0;
        std::size_t triangles=0;
        Aabb bounds;
        std::vector<MeshLod> levels;
        std::vector<std::size_t> counts;
        MeshLodBuildReport report;
    };
    Scene snapshot;
    SceneSpatialIndex spatial;
    SceneGeometryStatistics statistics;
    std::vector<Cache> cache;
    bool initialized=false;
    std::uint64_t source_geometry=0,source_presentation=0,source_revision=0,source_assets=0;
    std::uint64_t texture_storage=0,texture_payload=0,source_textures=0;
    const Scene* source_identity=nullptr;
};
SceneGeometryRuntime::SceneGeometryRuntime():impl_(std::make_unique<Impl>()) {}
SceneGeometryRuntime::~SceneGeometryRuntime()=default;
SceneGeometryRuntime::SceneGeometryRuntime(SceneGeometryRuntime&&) noexcept=default;
SceneGeometryRuntime& SceneGeometryRuntime::operator=(SceneGeometryRuntime&&) noexcept=default;
const Scene& SceneGeometryRuntime::prepare(const Scene& source,const Camera& camera,const Settings& settings) {
    if(settings.render_width<=0 || settings.render_height<=0 || !std::isfinite(settings.lod_error_pixels) || settings.lod_error_pixels<0)
        throw std::invalid_argument("Invalid automatic LOD viewport/pixel budget");
    const auto worlds=resolve_world_transforms(source);
    auto& state=*impl_; auto& stats=state.statistics;
    // 纹理像素遵循源 revision 契约；稳定帧只核对存储指纹，不每帧扫描大幅纹理。
    const auto texture_storage=texture_storage_fingerprint(source);
    const bool pose_only=state.initialized&&!settings.auto_lod&&source.asset_revision&&
        state.snapshot.asset_revision==source.asset_revision&&state.source_identity==&source;
    if(!state.initialized || (!pose_only&&state.source_revision!=source.revision) || state.texture_storage!=texture_storage || state.source_identity!=&source)
        state.texture_payload=texture_payload_fingerprint(source);
    // 编辑器资源版本不变时复用各网格指纹；不把 O(全部顶点数) 的工作放进每一帧。
    // 资源导入、材质/几何修改、撤销会增加 asset_revision；普通旋转/平移不会。
    const bool reuse_mesh_hashes=source.asset_revision&&state.initialized&&state.source_identity==&source&&
        state.source_assets==source.asset_revision&&state.cache.size()==source.meshes.size();
    Fingerprint content; content.add(source.meshes.size()); content.add(source.nodes.size());
    std::vector<std::uint64_t> base_hashes,lod_hashes;
    for(std::size_t i=0;i<source.meshes.size();++i) {
        const auto& mesh=source.meshes[i];
        base_hashes.push_back(reuse_mesh_hashes?state.cache[i].base_hash:base_fingerprint(mesh));
        lod_hashes.push_back(reuse_mesh_hashes?state.cache[i].lod_hash:lod_fingerprint(mesh));
        content.add(base_hashes.back()); content.add(lod_hashes.back());
    }
    for(const auto& node:source.nodes) { content.add(std::uint64_t(node.parent)); content.add(std::uint64_t(node.mesh)); content.matrix(node.local); }
    const auto geometry=content.value,presentation=presentation_fingerprint(source,state.texture_payload);
    const auto texture_descriptors=texture_descriptor_fingerprint(source,state.texture_payload);
    state.cache.resize(source.meshes.size());
    stats.lod_reports.clear(); stats.reduced_meshes=0;
    for(std::size_t i=0;i<source.meshes.size();++i) {
        const auto& mesh=source.meshes[i]; auto& cache=state.cache[i];
        const auto base_hash=base_hashes[i],lod_hash=lod_hashes[i];
        if(!cache.initialized || cache.base_hash!=base_hash || cache.lod_hash!=lod_hash) {
            // 原几何被原地修改而持久化 lod 没变时，旧 lod 已失效，等待显式后台重建。
            const bool stale=cache.initialized && cache.base_hash!=base_hash && cache.lod_hash==lod_hash;
            Impl::Cache next; next.initialized=true; next.base_hash=base_hash; next.lod_hash=lod_hash;
            next.triangles=triangle_count(mesh); next.bounds=mesh_bounds(mesh);
            next.report.mesh=i; next.report.source_triangles=next.triangles;
            next.report.reason=stale?"原几何已改变，旧 LOD 链失效；请后台重新生成，当前保留 LOD0":
                next.triangles>4096?"缺少 LOD 链；真实 QEM 输入预算为 4096 三角形，当前网格超预算，保留 LOD0":
                "缺少 LOD 链；请使用后台生成按钮，当前保留 LOD0";
            if(!stale && !mesh.lods.empty()) {
                std::size_t count=next.triangles; double error=0;
                try {
                    for(const auto& lod:mesh.lods) {
                        const auto reduced=triangle_count(lod);
                        if(reduced==0 || reduced>=count || !std::isfinite(lod.geometric_error) || lod.geometric_error<=0 || lod.geometric_error<error)
                            throw std::invalid_argument("Stored LOD must reduce triangles and have finite monotone geometric_error");
                        next.levels.push_back(lod); next.counts.push_back(reduced); count=reduced; error=lod.geometric_error;
                    }
                    next.report.reason="已读取持久化真实减面级别"; ++stats.lod_cache_loads;
                } catch(const std::invalid_argument& e) {
                    next.levels.clear(); next.counts.clear(); next.report.reason=std::string("持久化 LOD 无效，保留 LOD0，请后台重新生成：")+e.what();
                }
            }
            cache=std::move(next);
        }
        cache.report.stored_levels=cache.levels.size(); cache.report.reduced=!cache.levels.empty();
        if(cache.report.reduced) ++stats.reduced_meshes;
        stats.lod_reports.push_back(cache.report);
    }
    const std::size_t instances=source.nodes.empty()?source.meshes.size():source.nodes.size();
    std::vector<std::size_t> selected(instances,0);
    stats.source_triangles=0; stats.selected_triangles=0;
    for(std::size_t i=0;i<instances;++i) {
        const int mesh=source.nodes.empty()?int(i):source.nodes[i].mesh;
        if(mesh<0) continue;
        const auto& cache=state.cache[std::size_t(mesh)];
        const auto world=source.nodes.empty()?glm::mat4(1):worlds[i];
        if(settings.auto_lod && settings.lod_error_pixels>0) {
            for(std::size_t level=cache.levels.size();level>0;--level) {
                const auto error=projected_lod_error_pixels(cache.levels[level-1].geometric_error,
                    cache.bounds,world,camera,settings.render_height);
                if(error<=settings.lod_error_pixels) { selected[i]=level; break; }
            }
        }
        stats.source_triangles+=cache.triangles;
        stats.selected_triangles+=selected[i]?cache.counts[selected[i]-1]:cache.triangles;
    }
    // 全局 revision 可能因无关编辑增加；完整内容指纹和实际选级才决定是否复制/失效。
    const bool geometry_changed=!state.initialized || state.source_geometry!=geometry || stats.current_levels!=selected;
    const bool changed=geometry_changed || state.source_presentation!=presentation || state.snapshot.baked_resources!=source.baked_resources;
    if(geometry_changed&&!pose_only) {
        Scene next=source;
        if(source.nodes.empty()) {
            // 空节点场景由每个 mesh 隐式实例化一次；不能追加变体，否则会产生多余可见对象。
            for(std::size_t i=0;i<instances;++i) if(selected[i])
                next.meshes[i]=variant(source.meshes[i],state.cache[i].levels[selected[i]-1],selected[i]);
        } else {
            std::map<std::pair<int,std::size_t>,int> variants;
            for(std::size_t i=0;i<instances;++i) if(selected[i]) {
                const int mesh=source.nodes[i].mesh;
                const auto key=std::make_pair(mesh,selected[i]);
                auto [it,inserted]=variants.emplace(key,int(next.meshes.size()));
                if(inserted) next.meshes.push_back(variant(source.meshes[std::size_t(mesh)],
                    state.cache[std::size_t(mesh)].levels[selected[i]-1],selected[i]));
                next.nodes[i].mesh=it->second;
            }
        }
        next.revision=state.initialized?std::max(source.revision,state.snapshot.revision+1):source.revision;
        state.snapshot=std::move(next); ++stats.snapshot_changes;
    } else if(changed) {
        // 有资源版本契约时，纯平移/旋转只复制节点，不深拷贝数百万纹理像素。
        if(pose_only)state.snapshot.nodes=source.nodes;
        // sky/材质/光源更新不复制全部 mesh，也不复制内容未变的大幅纹理。
        state.snapshot.name=source.name; state.snapshot.sky_top=source.sky_top; state.snapshot.sky_bottom=source.sky_bottom;
        state.snapshot.materials=source.materials; state.snapshot.lights=source.lights;
        if(state.source_textures!=texture_descriptors) state.snapshot.textures=source.textures;
        state.snapshot.baked_resources=source.baked_resources;
        for(std::size_t i=0;i<source.meshes.size();++i)
            state.snapshot.meshes[i].name=source.meshes[i].name+(source.nodes.empty() && selected[i]?" (LOD"+std::to_string(selected[i])+")":"");
        for(std::size_t i=0;i<source.nodes.size();++i) {
            state.snapshot.nodes[i].name=source.nodes[i].name;
            if(selected[i]) state.snapshot.meshes[std::size_t(state.snapshot.nodes[i].mesh)].name=
                source.meshes[std::size_t(source.nodes[i].mesh)].name+" (LOD"+std::to_string(selected[i])+")";
        }
        state.snapshot.revision=std::max(source.revision,state.snapshot.revision+1); ++stats.snapshot_changes;
    }
    // previous_world 是帧间历史输入，更新它不意味几何改变或每帧重置累积历史。
    for(std::size_t i=0;i<source.nodes.size();++i) state.snapshot.nodes[i].previous_world=source.nodes[i].previous_world;
    state.source_geometry=geometry; state.source_presentation=presentation; state.source_revision=source.revision;
    state.source_assets=source.asset_revision;
    state.texture_storage=texture_storage; state.source_identity=&source;
    state.source_textures=texture_descriptors;
    stats.current_levels=std::move(selected); state.initialized=true;
    // 在空间索引消费快照之前发布资源版本，否则静止帧仍会走完整几何指纹。
    state.snapshot.asset_revision=settings.auto_lod?0:source.asset_revision;
    state.spatial.prepare(state.snapshot,settings.spatial_structure);
    stats.spatial_nodes=state.spatial.statistics().nodes; stats.spatial_leaves=state.spatial.statistics().leaves;
    stats.spatial_rebuilds=state.spatial.rebuilds();
    stats.candidates=state.spatial.visible_objects(camera,settings.render_width,settings.render_height).size();
    stats.status=std::string(settings.auto_lod?"自动 LOD：":"LOD0：")+std::to_string(stats.source_triangles)+" → "+
        std::to_string(stats.selected_triangles)+" 三角形；可见候选 "+std::to_string(stats.candidates)+
        "；"+(settings.spatial_structure==SpatialStructure::bvh?"BVH":"三角形 Octree");
    if(settings.auto_lod && stats.reduced_meshes==0 && !stats.lod_reports.empty())
        stats.status+="；"+stats.lod_reports.front().reason;
    return state.snapshot;
}
const SceneGeometryStatistics& SceneGeometryRuntime::stats() const { return impl_->statistics; }
std::uint64_t SceneGeometryRuntime::selectedTriangles() const { return impl_->statistics.selected_triangles; }
const std::string& SceneGeometryRuntime::status() const { return impl_->statistics.status; }
const SceneSpatialIndex& SceneGeometryRuntime::spatial_index() const { return impl_->spatial; }
} // namespace emberframe::lab
