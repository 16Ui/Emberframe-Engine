#include "editor_lights.h"
#include <optional>
#include <set>
#include <type_traits>
#include <utility>

namespace emberframe::lab {
namespace {
void require_binding(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
template<glm::length_t N, typename T, glm::qualifier Q>
bool finite(const glm::vec<N,T,Q>& v) {
    for (glm::length_t i=0;i<N;++i) if (!std::isfinite(v[i])) return false;
    return true;
}
bool finite(const glm::dmat4& m) {
    for (int i=0;i<4;++i) if (!finite(m[i])) return false;
    return true;
}
void validate_affine(const glm::dmat4& m) {
    require_binding(finite(m), "Nonfinite emitter transform");
    require_binding(m[0].w==0 && m[1].w==0 && m[2].w==0 && m[3].w==1,
                    "Emitter transform must be affine");
    // Relative volume permits small valid scales without accepting parallel axes.
    const auto axes=glm::dmat3(m);
    const double scale=glm::length(axes[0])*glm::length(axes[1])*glm::length(axes[2]);
    const double determinant=glm::determinant(axes);
    require_binding(std::isfinite(scale) && scale>0 && std::isfinite(determinant) &&
                    std::abs(determinant)>scale*1e-12, "Singular emitter transform");
}
void validate_light(const Light& l) {
    require_binding(int(l.kind)>=0 && int(l.kind)<=2, "Unsupported editor light kind");
    require_binding(finite(l.position) && finite(l.direction) && finite(l.color) && finite(l.size) &&
                    std::isfinite(l.intensity) && l.intensity>=0 && std::isfinite(l.range) && l.range>0 &&
                    l.color.x>=0 && l.color.y>=0 && l.color.z>=0 && finite(l.color*l.intensity),
                    "Invalid editor light values");
    if (l.kind!=LightKind::point) {
        const float length_squared=glm::dot(l.direction,l.direction);
        require_binding(std::isfinite(length_squared) && length_squared>1e-20f,
                        "Invalid editor light direction");
    }
    if (l.kind==LightKind::rectangle)
        require_binding(l.size.x>0 && l.size.y>0, "Rectangle size must be positive");
    require_binding(l.linked_node>=-1, "Invalid light binding node");
}
glm::dmat4 parent_world(const Scene& scene, std::size_t node) {
    std::vector<std::size_t> path;
    std::vector<bool> seen(scene.nodes.size());
    int current=int(node);
    while (current>=0) {
        const auto id=std::size_t(current);
        require_binding(id<scene.nodes.size(), "Emitter parent index out of range");
        require_binding(!seen[id], "Emitter hierarchy cycle");
        seen[id]=true;
        validate_affine(glm::dmat4(scene.nodes[id].local));
        require_binding(finite(glm::dmat4(scene.nodes[id].previous_world)), "Nonfinite emitter history");
        if (id!=node) path.push_back(id);
        current=scene.nodes[id].parent;
        require_binding(current>=-1, "Emitter parent index out of range");
    }
    glm::mat4 world(1);
    for (auto it=path.rbegin();it!=path.rend();++it) {
        // Match scene_world_transforms' float accumulation before inverting in
        // double precision, so the resulting local matrix targets renderer space.
        world*=scene.nodes[*it].local;
        validate_affine(glm::dmat4(world));
    }
    return glm::dmat4(world);
}
template<class Geometry>
void validate_emitter_geometry(const Geometry& mesh, std::size_t material_count) {
    require_binding(!mesh.vertices.empty() && !mesh.indices.empty() && !mesh.primitives.empty(),
                    "Rectangle emitter has no geometry");
    for (const auto& v:mesh.vertices) {
        require_binding(finite(v.position) && finite(v.normal) && finite(v.uv) && finite(v.tangent) && finite(v.color),
                        "Nonfinite rectangle emitter vertex");
        require_binding(v.normal.y<0 && std::abs(v.normal.x)<=-v.normal.y*1e-5f &&
                        std::abs(v.normal.z)<=-v.normal.y*1e-5f, "Rectangle emitter normals must face local -Y");
    }
    for (auto index:mesh.indices) require_binding(index<mesh.vertices.size(), "Emitter vertex index out of range");
    for (const auto& p:mesh.primitives)
        require_binding(p.material<material_count && p.index_count>0 && p.index_count%3==0 &&
                        p.first_index<=mesh.indices.size() && p.index_count<=mesh.indices.size()-p.first_index,
                        "Invalid rectangle emitter primitive");
}
struct BindingPlan { int mesh=-1; glm::mat4 local{1}; };
BindingPlan binding_plan(const Scene& scene, std::size_t index, const Light& l) {
    if (index>=scene.lights.size()) throw std::out_of_range("Editor light index out of range");
    validate_light(l);
    if (l.linked_node<0) return {};
    require_binding(l.kind==LightKind::rectangle, "Only rectangle emitter bindings are supported");
    require_binding(std::size_t(l.linked_node)<scene.nodes.size(), "Light binding node out of range");
    for (std::size_t i=0;i<scene.lights.size();++i)
        require_binding(i==index || scene.lights[i].linked_node!=l.linked_node, "Duplicate light emitter binding");
    const auto& node=scene.nodes[std::size_t(l.linked_node)];
    require_binding(node.mesh>=0 && std::size_t(node.mesh)<scene.meshes.size(), "Light emitter mesh out of range");
    const auto parent=parent_world(scene,std::size_t(l.linked_node));
    const auto inverse=glm::inverse(parent);
    require_binding(finite(inverse), "Invalid inverse emitter parent transform");
    const auto& mesh=scene.meshes[std::size_t(node.mesh)];
    validate_emitter_geometry(mesh,scene.materials.size());
    glm::dvec3 lo(mesh.vertices.front().position),hi=lo;
    for (const auto& v:mesh.vertices) { lo=glm::min(lo,glm::dvec3(v.position)); hi=glm::max(hi,glm::dvec3(v.position)); }
    const auto extent=hi-lo,center=lo+extent*.5;
    require_binding(extent.x>0 && extent.z>0 && extent.y==0, "Emitter must be a nondegenerate XZ plane");
    for (const auto& lod:mesh.lods) {
        validate_emitter_geometry(lod,scene.materials.size());
        for (const auto& vertex:lod.vertices)
            require_binding(double(vertex.position.y)==center.y, "Emitter LOD must share the XZ plane");
    }
    // Same reference axis and .95 threshold as gpu_lighting.cpp::rectangle_frame.
    const auto n=safe_normalize(l.direction,{0,-1,0});
    const auto u=safe_normalize(glm::cross(std::abs(n.y)<.95f ? glm::vec3(0,1,0) : glm::vec3(1,0,0),n));
    const auto v=glm::cross(n,u);
    glm::dmat4 world(1);
    world[0]=glm::dvec4(glm::dvec3(u)*(double(l.size.x)/extent.x),0);
    world[1]=glm::dvec4(-glm::dvec3(n),0);
    world[2]=glm::dvec4(glm::dvec3(v)*(double(l.size.y)/extent.z),0);
    world[3]=glm::dvec4(glm::dvec3(l.position)-glm::dmat3(world)*center,1);
    validate_affine(world);
    const glm::mat4 local(inverse*world);
    validate_affine(glm::dmat4(local));
    const auto restored=parent*glm::dmat4(local);
    for (int col=0;col<4;++col) {
        const double tolerance=1e-4*std::max(1.0,glm::length(world[col]));
        require_binding(glm::length(restored[col]-world[col])<=tolerance,
                        "Emitter transform cannot be represented accurately");
    }
    return {node.mesh,local};
}
template<class Geometry> bool uses_material(const Geometry& mesh, std::uint32_t material) {
    for (const auto& p:mesh.primitives) if (p.material==material) return true;
    return false;
}
bool shared_material(const Scene& scene, int mesh, std::uint32_t material) {
    for (std::size_t i=0;i<scene.meshes.size();++i) if (i!=std::size_t(mesh)) {
        if (uses_material(scene.meshes[i],material)) return true;
        for (const auto& lod:scene.meshes[i].lods) if (uses_material(lod,material)) return true;
    }
    return false;
}
bool same_light(const Light& a, const Light& b) {
    return a.kind==b.kind && a.position==b.position && a.direction==b.direction && a.color==b.color &&
           a.intensity==b.intensity && a.range==b.range && a.size==b.size && a.linked_node==b.linked_node;
}
std::uint64_t edited_revision(const Scene& scene) {
    if (scene.revision==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Scene revision exhausted");
    return scene.revision+1;
}
int next_index(std::size_t size) {
    if (size>=std::size_t(std::numeric_limits<int>::max())) throw std::length_error("Editor scene index limit exceeded");
    return int(size);
}
// Allocate all destination storage before committing. Existing meshes/textures
// need no deep copy; reserved buffers and noexcept moves make the commit atomic.
template<class T> std::vector<T> append_buffer(const std::vector<T>& source) {
    std::vector<T> result; result.reserve(source.size()+1); return result;
}
template<class T> void commit_append(std::vector<T>& source, std::vector<T>& buffer, T value) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<T>);
    for (auto& item:source) buffer.push_back(std::move(item));
    buffer.push_back(std::move(value)); source.swap(buffer);
}
Mesh rectangle_mesh(std::uint32_t material) {
    Mesh mesh; mesh.name="Rectangle light emitter";
    for (auto uv:{glm::vec2(0,0),glm::vec2(1,0),glm::vec2(1,1),glm::vec2(0,1)}) {
        Vertex v; v.position={uv.x-.5f,0,uv.y-.5f}; v.normal={0,-1,0}; v.uv=uv;
        v.tangent={1,0,0,1}; mesh.vertices.push_back(v);
    }
    mesh.indices={0,1,2,0,2,3}; mesh.primitives.push_back({0,6,material}); return mesh;
}
} // namespace

void validate_light_binding(const Scene& scene, std::size_t index, const Light& replacement) {
    (void)binding_plan(scene,index,replacement);
}
void validate_light_binding(const Scene& scene, std::size_t index) {
    validate_light_binding(scene,index,scene.lights.at(index));
}
void update_editor_light(Scene& scene, std::size_t index, const Light& replacement) {
    // replacement may alias scene.lights[index]; copy before swapping any storage.
    const Light light=replacement;
    const auto plan=binding_plan(scene,index,light);
    std::optional<std::vector<Material>> materials;
    std::optional<Mesh> mesh;
    std::vector<Mesh> mesh_buffer;
    int target_mesh=plan.mesh;
    if (plan.mesh>=0) {
        const auto& source=scene.meshes[std::size_t(plan.mesh)];
        bool instanced=false;
        for (std::size_t i=0;i<scene.nodes.size();++i)
            if (i!=std::size_t(light.linked_node) && scene.nodes[i].mesh==plan.mesh) instanced=true;
        std::set<std::uint32_t> slots;
        for (const auto& p:source.primitives) slots.insert(p.material);
        for (const auto& lod:source.lods) for (const auto& p:lod.primitives) slots.insert(p.material);
        for (const auto slot:slots) {
            const auto& current=scene.materials[slot];
            if (current.emissive==light.color*light.intensity && current.emissive_texture==-1) continue;
            if (!materials) materials=scene.materials;
            if (instanced || shared_material(scene,plan.mesh,slot)) {
                const auto material=std::uint32_t(next_index(materials->size()));
                Material isolated=current; isolated.emissive=light.color*light.intensity; isolated.emissive_texture=-1;
                materials->push_back(std::move(isolated));
                if (!mesh) mesh=source;
                for (auto& p:mesh->primitives) if (p.material==slot) p.material=material;
                for (auto& lod:mesh->lods) for (auto& p:lod.primitives) if (p.material==slot) p.material=material;
            } else {
                (*materials)[slot].emissive=light.color*light.intensity;
                (*materials)[slot].emissive_texture=-1;
            }
        }
        if (mesh && instanced) {
            target_mesh=next_index(scene.meshes.size()); mesh_buffer=append_buffer(scene.meshes);
        }
    }
    if (same_light(scene.lights[index],light) && !materials &&
        (plan.mesh<0 || scene.nodes[std::size_t(light.linked_node)].local==plan.local)) return;
    const auto revision=edited_revision(scene);
    auto asset_revision=scene.asset_revision;
    if(asset_revision&&(materials||mesh)){
        if(asset_revision==UINT64_MAX)throw std::overflow_error("Asset revision exhausted");
        ++asset_revision;
    }
    // All throwing validation/copies/allocations are finished before this point.
    if (materials) scene.materials.swap(*materials);
    if (mesh) {
        static_assert(std::is_nothrow_swappable_v<Mesh>);
        if (target_mesh!=plan.mesh) commit_append(scene.meshes,mesh_buffer,std::move(*mesh));
        else std::swap(scene.meshes[std::size_t(plan.mesh)],*mesh);
    }
    if (plan.mesh>=0) {
        auto& node=scene.nodes[std::size_t(light.linked_node)]; node.local=plan.local; node.mesh=target_mesh;
    }
    scene.lights[index]=light; scene.revision=revision;scene.asset_revision=asset_revision;
}
void set_editor_light_position(Scene& scene, std::size_t index, glm::vec3 position) {
    Light light=scene.lights.at(index); light.position=position; update_editor_light(scene,index,light);
}
int add_editor_light(Scene& scene, LightKind kind) {
    if (scene.lights.size()>=64) throw std::length_error("Editor renderer supports at most 64 lights");
    const int index=next_index(scene.lights.size());
    Light light; light.kind=kind;
    if (kind==LightKind::rectangle) light.direction={0,-1,0};
    validate_light(light);
    const auto revision=edited_revision(scene);
    auto lights=append_buffer(scene.lights);
    if (kind==LightKind::rectangle) {
        const int node_index=next_index(scene.nodes.size()),mesh_index=next_index(scene.meshes.size());
        const auto material_index=std::uint32_t(next_index(scene.materials.size()));
        Material material; material.name="Rectangle light emission"; material.base_color=glm::vec4(1);
        material.emissive=light.color*light.intensity;
        // Prepare the same transform through the standard binding code on a tiny
        // scene; the destination's existing geometry and textures are not copied.
        Scene emitter; emitter.materials.push_back(material); emitter.meshes.push_back(rectangle_mesh(0));
        Node node; node.name="Rectangle light emitter"; node.mesh=0; emitter.nodes.push_back(node);
        Light bound=light; bound.linked_node=0; emitter.lights.push_back(bound);
        update_editor_light(emitter,0,bound);
        node.local=node.previous_world=emitter.nodes[0].local; node.mesh=mesh_index;
        Mesh mesh=std::move(emitter.meshes[0]); mesh.primitives[0].material=material_index;
        light.linked_node=node_index;
        auto materials=append_buffer(scene.materials);
        auto meshes=append_buffer(scene.meshes);
        auto nodes=append_buffer(scene.nodes);
        commit_append(scene.materials,materials,std::move(material));
        commit_append(scene.meshes,meshes,std::move(mesh));
        commit_append(scene.nodes,nodes,std::move(node));
    }
    commit_append(scene.lights,lights,light); scene.revision=revision;scene.asset_revision=0; return index;
}
} // namespace emberframe::lab
