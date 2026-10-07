#include "editor_lights.h"
#include "systems.h"
#include "config_applicability.h"
#include <bit>
#include <sstream>
#include <utility>

namespace emberframe::lab {
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void throws(F&& f, const char* message) {
    bool caught=false; try { f(); } catch (const std::exception&) { caught=true; } check(caught,message);
}
bool close(float a, float b, float tolerance=2e-4f) { return std::abs(a-b)<=tolerance; }
bool close(glm::vec3 a, glm::vec3 b, float tolerance=2e-4f) { return glm::length(a-b)<=tolerance; }
bool same(float a, float b) { return std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b); }
template<glm::length_t N, glm::qualifier Q> bool same(const glm::vec<N,float,Q>& a, const glm::vec<N,float,Q>& b) {
    for (glm::length_t i=0;i<N;++i) if (!same(a[i],b[i])) return false;
    return true;
}
bool same(const glm::mat4& a, const glm::mat4& b) {
    for (int i=0;i<4;++i) if (!same(a[i],b[i])) return false;
    return true;
}
bool same(const Light& a, const Light& b) {
    return a.kind==b.kind && same(a.position,b.position) && same(a.direction,b.direction) && same(a.color,b.color) &&
           same(a.intensity,b.intensity) && same(a.range,b.range) && same(a.size,b.size) && a.linked_node==b.linked_node;
}
bool same(const Vertex& a, const Vertex& b) {
    return same(a.position,b.position) && same(a.normal,b.normal) && same(a.uv,b.uv) && same(a.tangent,b.tangent) &&
           same(a.color,b.color) && same(a.baked_irradiance,b.baked_irradiance);
}
template<class Geometry> bool same_geometry(const Geometry& a, const Geometry& b) {
    if (a.indices!=b.indices || a.vertices.size()!=b.vertices.size() || a.primitives.size()!=b.primitives.size()) return false;
    for (std::size_t i=0;i<a.vertices.size();++i) if (!same(a.vertices[i],b.vertices[i])) return false;
    for (std::size_t i=0;i<a.primitives.size();++i) {
        const auto& x=a.primitives[i]; const auto& y=b.primitives[i];
        if (x.first_index!=y.first_index || x.index_count!=y.index_count || x.material!=y.material) return false;
    }
    return true;
}
// This comparison also works on deliberately invalid fixtures, including NaNs.
void unchanged(const Scene& scene, const Scene& before) {
    check(scene.revision==before.revision && scene.name==before.name && scene.sky_top==before.sky_top &&
          scene.sky_bottom==before.sky_bottom && scene.baked_resources==before.baked_resources,
          "Rejected edit changed scene metadata");
    check(scene.lights.size()==before.lights.size() && scene.nodes.size()==before.nodes.size() &&
          scene.meshes.size()==before.meshes.size() && scene.materials.size()==before.materials.size() &&
          scene.textures.size()==before.textures.size(), "Rejected edit changed resource counts");
    for (std::size_t i=0;i<scene.lights.size();++i) check(same(scene.lights[i],before.lights[i]), "Rejected edit changed a light");
    for (std::size_t i=0;i<scene.nodes.size();++i) {
        const auto& a=scene.nodes[i]; const auto& b=before.nodes[i];
        check(a.name==b.name && a.parent==b.parent && a.mesh==b.mesh && same(a.local,b.local) &&
              same(a.previous_world,b.previous_world), "Rejected edit changed a node");
    }
    for (std::size_t i=0;i<scene.materials.size();++i) {
        const auto& a=scene.materials[i]; const auto& b=before.materials[i];
        check(a.name==b.name && a.base_color==b.base_color && a.emissive==b.emissive &&
              a.emissive_texture==b.emissive_texture, "Rejected edit changed a material");
    }
    for (std::size_t i=0;i<scene.meshes.size();++i) {
        const auto& a=scene.meshes[i]; const auto& b=before.meshes[i];
        check(a.name==b.name && same_geometry(a,b) && a.lods.size()==b.lods.size(), "Rejected edit changed a mesh");
        for (std::size_t j=0;j<a.lods.size();++j)
            check(same_geometry(a.lods[j],b.lods[j]) && a.lods[j].geometric_error==b.lods[j].geometric_error,
                  "Rejected edit changed a mesh LOD");
    }
}
std::string project_state(const Scene& scene) {
    ProjectDocument p; p.scene=scene; p.scene.revision=1; return serialize_project(p);
}
void check_emitter(const Scene& scene, std::size_t index) {
    const auto& light=scene.lights.at(index);
    check(light.linked_node>=0, "Rectangle light lost its binding");
    const auto& node=scene.nodes.at(std::size_t(light.linked_node));
    const auto& mesh=scene.meshes.at(std::size_t(node.mesh));
    const auto world=scene_world_transforms(scene).at(std::size_t(light.linked_node));
    const auto n=safe_normalize(light.direction,{0,-1,0});
    const auto u=safe_normalize(glm::cross(std::abs(n.y)<.95f ? glm::vec3(0,1,0) : glm::vec3(1,0,0),n));
    const auto v=glm::cross(n,u);
    glm::vec2 lo(std::numeric_limits<float>::max()),hi(-std::numeric_limits<float>::max());
    for (const auto& vertex:mesh.vertices) {
        const auto point=glm::vec3(world*glm::vec4(vertex.position,1))-light.position;
        check(close(glm::dot(point,n),0), "Visible emitter does not lie on the light plane");
        const glm::vec2 projected(glm::dot(point,u),glm::dot(point,v));
        lo=glm::min(lo,projected); hi=glm::max(hi,projected);
        check(close(safe_normalize(glm::transpose(glm::inverse(glm::mat3(world)))*vertex.normal),n),
              "Emitter normal disagrees with GPU rectangle direction");
    }
    check(close(lo.x,-light.size.x*.5f) && close(hi.x,light.size.x*.5f) &&
          close(lo.y,-light.size.y*.5f) && close(hi.y,light.size.y*.5f), "Emitter center/size disagrees with light");
    for (std::size_t i=0;i<mesh.indices.size();i+=3) {
        const auto a=glm::vec3(world*glm::vec4(mesh.vertices[mesh.indices[i]].position,1));
        const auto b=glm::vec3(world*glm::vec4(mesh.vertices[mesh.indices[i+1]].position,1));
        const auto c=glm::vec3(world*glm::vec4(mesh.vertices[mesh.indices[i+2]].position,1));
        check(glm::dot(glm::cross(b-a,c-a),n)>0, "Emitter winding faces away from its light");
    }
    auto emission=[&](const auto& geometry) {
        for (const auto& primitive:geometry.primitives) {
            const auto& material=scene.materials.at(primitive.material);
            check(close(material.emissive,light.color*light.intensity) && material.emissive_texture==-1,
                  "Visible emitter radiance disagrees with light");
        }
    };
    emission(mesh); for (const auto& lod:mesh.lods) emission(lod);
}
Scene rectangle_scene() { Scene scene; add_editor_light(scene,LightKind::rectangle); return scene; }
std::string without_bindings(const std::string& text) {
    std::istringstream in(text); std::string line,result;
    while (std::getline(in,line)) if (!line.starts_with("light_binding ")) result+=line+'\n';
    return result;
}
} // namespace

TestResults test_editor_lights() {
    TestResults results;
    auto test=[&](const char* name, auto&& f) {
        try { f(); results.push_back({name,true,{}}); }
        catch (const std::exception& e) { results.push_back({name,false,e.what()}); }
    };
    test("Editor lights: room position, direction, size and radiance agree with visible emitter",[] {
        auto scene=make_demo_scene(1);
        check(scene.lights.size()==1 && scene.lights[0].linked_node==int(scene.nodes.size()-1), "Room emitter binding missing");
        check_emitter(scene,0);
        const auto node_id=std::size_t(scene.lights[0].linked_node);
        check(scene.nodes[node_id].previous_world==scene.nodes[node_id].local, "Preset emitter has spurious motion history");
        check(close(scene.lights[0].size.x,1.2f), "Preset emitter dimensions changed");
        const auto history=scene.nodes[node_id].previous_world;
        const auto original_mesh=scene.meshes[std::size_t(scene.nodes[node_id].mesh)];
        const auto revision=scene.revision;
        set_editor_light_position(scene,0,{.7f,2.3f,-.4f}); check_emitter(scene,0);
        for (const auto direction:{glm::vec3(.3f,-.8f,.7f),glm::vec3(0,1,0),glm::vec3(1,0,0),glm::vec3(0,-4,0)}) {
            auto light=scene.lights[0]; light.direction=direction; light.size={2.1f,.65f};
            light.color={.2f,.7f,.9f}; light.intensity=7; update_editor_light(scene,0,light); check_emitter(scene,0);
        }
        check(scene.revision>revision && scene.nodes[node_id].previous_world==history, "Edit lost revision or previous frame history");
        check(same_geometry(original_mesh,scene.meshes[std::size_t(scene.nodes[node_id].mesh)]), "Simple light edit rebuilt geometry");
        const auto before=scene; update_editor_light(scene,0,scene.lights[0]); unchanged(scene,before);
    });
    test("Editor lights: non-unit off-center plane and nested affine parents",[] {
        auto scene=rectangle_scene();
        auto& mesh=scene.meshes[0];
        for (auto& vertex:mesh.vertices) vertex.position={4*vertex.position.x+3,7,6*vertex.position.z-2};
        Node root; root.name="Rotated, mirrored parent";
        root.local=glm::translate(glm::mat4(1),{-3,1,-2})*glm::rotate(glm::mat4(1),.65f,glm::vec3(0,1,0))*
                   glm::scale(glm::mat4(1),{2,.6f,-1.2f});
        scene.nodes.push_back(root);
        Node child; child.name="Sheared parent"; child.parent=1; child.local[1].x=.25f; child.local[2].y=.15f;
        scene.nodes.push_back(child); scene.nodes[0].parent=2;
        const auto before=scene;
        auto light=scene.lights[0]; light.position={.3f,3.2f,-.8f}; light.direction={.3f,-.4f,1.2f}; light.size={2.4f,.7f};
        validate_light_binding(scene,0,light); unchanged(scene,before);
        update_editor_light(scene,0,light); check_emitter(scene,0);
        check(scene.nodes[1].local==root.local && scene.nodes[2].local==child.local &&
              scene.nodes[0].previous_world==before.nodes[0].previous_world, "Edit changed parents/history");
        check(same_geometry(scene.meshes[0],before.meshes[0]), "Plane normalization rewrote vertices");
        scene.nodes[1].local=glm::scale(glm::mat4(1),glm::vec3(1e-6f));
        update_editor_light(scene,0,scene.lights[0]); check_emitter(scene,0);
    });
    test("Editor lights: shared mesh/material isolation, LODs and texture storage",[] {
        auto scene=rectangle_scene();
        MeshLod lod; lod.vertices=scene.meshes[0].vertices; lod.indices={0,1,2}; lod.primitives={{0,3,0}}; lod.geometric_error=.1;
        scene.meshes[0].lods.push_back(lod);
        Node instance=scene.nodes[0]; instance.name="Unrelated instance";
        instance.local=glm::translate(glm::mat4(1),{-5,0,0}); scene.nodes.push_back(instance);
        scene.meshes.push_back(scene.meshes[0]);
        Texture texture; texture.name="Retained emission texture"; texture.levels.emplace_back(2,2,glm::vec4(.5f));
        scene.textures.push_back(texture); scene.materials[0].emissive_texture=0;
        const auto* pixels=scene.textures[0].levels[0].pixels.data();
        const auto* vertices=scene.meshes[0].vertices.data();
        const auto before=scene;
        Camera camera; Settings settings; UndoStack undo;
        undo.begin(scene,camera,settings,"Isolate shared emitter resources");
        auto light=scene.lights[0]; light.color={1,.1f,.3f}; light.intensity=12;
        update_editor_light(scene,0,light); check_emitter(scene,0);
        check(scene.meshes.size()==3 && scene.materials.size()==2 && scene.nodes[0].mesh==2,
              "Shared emitter resources were not isolated");
        check(scene.nodes[1].mesh==0 && scene.nodes[1].local==before.nodes[1].local &&
              scene.materials[0].emissive==before.materials[0].emissive && scene.materials[0].emissive_texture==0 &&
              same_geometry(scene.meshes[0],before.meshes[0]) && same_geometry(scene.meshes[1],before.meshes[1]),
              "Emitter edit affected unrelated instances/material users");
        check(scene.textures[0].levels[0].pixels.data()==pixels && scene.meshes[0].vertices.data()==vertices,
              "Emitter edit deep-copied unrelated pixels/mesh storage");
        light=scene.lights[0]; light.intensity=4; update_editor_light(scene,0,light); check_emitter(scene,0);
        check(scene.meshes.size()==3 && scene.materials.size()==2, "Repeated edit allocated redundant emitter resources");
        const auto edited=project_state(scene);
        check(undo.commit() && undo.undo() && project_state(scene)==project_state(before),
              "Isolation undo left cloned resources or material remaps behind");
        check(undo.redo() && project_state(scene)==edited, "Isolation redo lost cloned resources or LOD remaps");
        check_emitter(scene,0);
    });
    test("Editor lights: material shared through another mesh LOD only",[] {
        auto scene=rectangle_scene();
        Material unrelated; scene.materials.push_back(unrelated);
        Mesh other=scene.meshes[0]; other.primitives[0].material=1;
        MeshLod lod; lod.vertices=other.vertices; lod.indices={0,1,2}; lod.primitives={{0,3,0}}; lod.geometric_error=.1;
        other.lods.push_back(lod); scene.meshes.push_back(other);
        const auto original=scene.materials[0].emissive;
        auto light=scene.lights[0]; light.intensity=9; update_editor_light(scene,0,light); check_emitter(scene,0);
        check(scene.meshes.size()==2 && scene.nodes[0].mesh==0 && scene.materials.size()==3,
              "Private emitter mesh was needlessly duplicated or LOD sharing missed");
        check(scene.meshes[1].lods[0].primitives[0].material==0 && scene.materials[0].emissive==original,
              "Edit changed another mesh's LOD emission");
    });
    test("Editor lights: binding persistence and older projects remain unbound",[] {
        ProjectDocument p; p.scene=make_demo_scene(1); p.unknown_lines={"future.light_data preserved"};
        const auto text=serialize_project(p);
        check(text.find("light_binding 0 ")!=std::string::npos, "Binding record was not written");
        auto restored=deserialize_project(text);
        check(restored.scene.lights[0].linked_node==p.scene.lights[0].linked_node &&
              restored.unknown_lines==p.unknown_lines && serialize_project(restored)==text, "Binding round trip is not lossless");
        check_emitter(restored.scene,0);
        set_editor_light_position(restored.scene,0,{.5f,2,1}); check_emitter(restored.scene,0);
        const auto old=without_bindings(text);
        auto legacy=deserialize_project(old);
        check(legacy.scene.lights[0].linked_node==-1 && legacy.unknown_lines==p.unknown_lines, "Legacy emitter was implicitly bound");
        const auto emitter=legacy.scene.nodes.back().local;
        set_editor_light_position(legacy.scene,0,{1,2,3});
        check(legacy.scene.nodes.back().local==emitter, "Unbound old project changed geometry");
        auto v0=old; v0.replace(0,std::string("EmberFrame scene format 1").size(),"EmberFrame scene format 0");
        check(deserialize_project(v0).scene.lights[0].linked_node==-1, "Version 0 binding default changed");
        const auto record="light_binding 0 "+std::to_string(p.scene.lights[0].linked_node)+'\n';
        const auto reordered=old.substr(0,old.find('\n')+1)+record+old.substr(old.find('\n')+1);
        check(deserialize_project(reordered).scene.lights[0].linked_node==p.scene.lights[0].linked_node,
              "Forward binding references were rejected");
    });
    test("Editor lights: malformed and duplicate persisted bindings rejected",[] {
        ProjectDocument p; p.scene=rectangle_scene(); const auto text=serialize_project(p),old=without_bindings(text);
        throws([&] { deserialize_project(text+"light_binding 0 0\n"); }, "Duplicate binding record accepted");
        for (const auto* record:{"light_binding 9 0\n","light_binding 0 99\n","light_binding 0 -2\n",
                                 "light_binding 0 0 trailing\n"})
            throws([&] { deserialize_project(old+record); }, "Invalid binding reference accepted");
        add_editor_light(p.scene,LightKind::rectangle);
        auto duplicate=without_bindings(serialize_project(p))+"light_binding 0 0\nlight_binding 1 0\n";
        throws([&] { deserialize_project(duplicate); }, "Two lights bound to one node accepted");
        p.scene.lights[1].linked_node=0;
        throws([&] { serialize_project(p); }, "Duplicate live binding serialized");
        p.scene.lights[1].linked_node=1; p.scene.lights[0].kind=LightKind::point;
        throws([&] { serialize_project(p); }, "Unsupported punctual binding serialized");
        ProjectDocument valid; valid.scene=rectangle_scene();
        // Build the invalid record from a valid project, then replace its node matrix.
        auto broken=serialize_project(valid);
        const auto begin=broken.find("node 0 "),end=broken.find('\n',begin);
        broken.replace(begin,end-begin,"node 0 -1 0 - 0 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1");
        throws([&] { deserialize_project(broken); }, "Singular persisted emitter transform accepted");
    });
    test("Editor lights: UndoStack drag, undo/redo, cancel and creation",[] {
        auto scene=make_demo_scene(1); Camera camera; Settings settings; UndoStack undo;
        const auto initial=project_state(scene);
        undo.begin(scene,camera,settings,"Move and resize emitter");
        for (int i=0;i<4;++i) set_editor_light_position(scene,0,{.1f*float(i),2.3f,.2f});
        auto light=scene.lights[0]; light.direction={.2f,-1,.4f}; light.size={2,.5f}; light.intensity=5;
        update_editor_light(scene,0,light); const auto edited=project_state(scene);
        check(undo.commit() && undo.undo_label()=="Move and resize emitter", "Light drag did not form one transaction");
        auto revision=scene.revision;
        check(undo.undo() && project_state(scene)==initial && scene.revision>revision, "Undo missed emitter/light/material/binding state");
        check_emitter(scene,0); revision=scene.revision;
        check(undo.redo() && project_state(scene)==edited && scene.revision>revision, "Redo missed complete light state");
        check_emitter(scene,0);
        undo.begin(scene,camera,settings); set_editor_light_position(scene,0,{9,9,9}); undo.cancel();
        check(project_state(scene)==edited, "Cancel did not restore emitter and light");
        undo.begin(scene,camera,settings); update_editor_light(scene,0,scene.lights[0]);
        check(!undo.commit(), "Unchanged light edit created an undo command");
        undo.begin(scene,camera,settings,"Create rectangle light"); const auto added=add_editor_light(scene,LightKind::rectangle);
        check(added==1 && undo.commit(), "Creation transaction failed"); const auto created=project_state(scene);
        check(undo.undo() && project_state(scene)==edited, "Creation undo left resources or binding behind");
        check(undo.redo() && project_state(scene)==created, "Creation redo lost resources or binding"); check_emitter(scene,1);
    });
    test("Editor lights: invalid proposed edits leave scene unchanged",[] {
        auto scene=rectangle_scene(); const auto original=scene.lights[0];
        auto rejected=[&](Light light) {
            const auto before=scene;
            throws([&] { validate_light_binding(scene,0,light); }, "Invalid light validation succeeded"); unchanged(scene,before);
            throws([&] { update_editor_light(scene,0,light); }, "Invalid light edit succeeded"); unchanged(scene,before);
        };
        Light light=original; light.size.x=0; rejected(light);
        light=original; light.size.y=-1; rejected(light);
        light=original; light.direction=glm::vec3(0); rejected(light);
        light=original; light.direction.x=std::numeric_limits<float>::max(); rejected(light);
        light=original; light.position.x=std::numeric_limits<float>::quiet_NaN(); rejected(light);
        light=original; light.intensity=-1; rejected(light);
        light=original; light.intensity=std::numeric_limits<float>::infinity(); rejected(light);
        light=original; light.color.x=-1; rejected(light);
        light=original; light.color.x=std::numeric_limits<float>::max(); rejected(light);
        light=original; light.range=0; rejected(light);
        light=original; light.linked_node=-2; rejected(light);
        light=original; light.linked_node=99; rejected(light);
        light=original; light.kind=LightKind::point; rejected(light);
        light=original; light.kind=static_cast<LightKind>(99); rejected(light);
        const auto before=scene;
        throws([&] { set_editor_light_position(scene,scene.lights.size(),{1,2,3}); }, "Out-of-range edit succeeded"); unchanged(scene,before);
        scene.revision=std::numeric_limits<std::uint64_t>::max(); const auto exhausted=scene;
        throws([&] { set_editor_light_position(scene,0,{1,2,3}); }, "Revision overflow succeeded"); unchanged(scene,exhausted);
        throws([&] { add_editor_light(scene,LightKind::rectangle); }, "Revision overflow creation succeeded"); unchanged(scene,exhausted);
    });
    test("Editor lights: invalid hierarchy, geometry and duplicate binding are atomic",[] {
        auto invalid=[](auto&& corrupt) {
            auto scene=rectangle_scene(); corrupt(scene); const auto before=scene;
            throws([&] { validate_light_binding(scene,0); }, "Invalid bound emitter validated"); unchanged(scene,before);
            throws([&] { set_editor_light_position(scene,0,{.1f,2,.3f}); }, "Invalid bound emitter edited"); unchanged(scene,before);
        };
        invalid([](Scene& s) { s.nodes[0].parent=99; });
        invalid([](Scene& s) { s.nodes[0].parent=0; });
        invalid([](Scene& s) { s.nodes[0].local[0]=glm::vec4(0); });
        invalid([](Scene& s) { Node parent; parent.local=glm::scale(glm::mat4(1),{0,1,1}); s.nodes.push_back(parent); s.nodes[0].parent=1; });
        invalid([](Scene& s) { Node parent; parent.parent=0; s.nodes.push_back(parent); s.nodes[0].parent=1; });
        invalid([](Scene& s) { Node parent; parent.local[0].w=.1f; s.nodes.push_back(parent); s.nodes[0].parent=1; });
        invalid([](Scene& s) { Node parent; parent.local[3].x=std::numeric_limits<float>::infinity(); s.nodes.push_back(parent); s.nodes[0].parent=1; });
        invalid([](Scene& s) { s.nodes[0].mesh=99; });
        invalid([](Scene& s) { s.meshes[0].vertices.clear(); });
        invalid([](Scene& s) { for (auto& v:s.meshes[0].vertices) v.position.x=0; });
        invalid([](Scene& s) { s.meshes[0].vertices[0].position.y=.2f; });
        invalid([](Scene& s) { s.meshes[0].vertices[0].normal={0,1,0}; });
        invalid([](Scene& s) { s.meshes[0].primitives[0].material=99; });
        invalid([](Scene& s) { s.meshes[0].indices[0]=99; });
        invalid([](Scene& s) { s.lights.push_back(s.lights[0]); });
    });
    test("Editor lights: creation, independent punctual lights and 64-light limit",[] {
        Scene scene;
        check(add_editor_light(scene,LightKind::directional)==0 && scene.nodes.empty() && scene.meshes.empty() &&
              scene.materials.empty() && scene.lights[0].linked_node==-1, "Directional creation allocated an emitter");
        check(add_editor_light(scene,LightKind::point)==1 && scene.nodes.empty() && scene.lights[1].linked_node==-1,
              "Point creation claimed an unsupported binding");
        check(add_editor_light(scene,LightKind::rectangle)==2 && scene.nodes.size()==1 && scene.meshes.size()==1 &&
              scene.materials.size()==1 && scene.lights[2].linked_node==0, "Rectangle creation missed dedicated resources");
        check_emitter(scene,2);
        check(scene.nodes[0].local==scene.nodes[0].previous_world, "New emitter history was not initialized");
        const auto emitter=scene.nodes[0].local;
        set_editor_light_position(scene,1,{4,5,6});
        check(scene.nodes[0].local==emitter && scene.lights[1].position==glm::vec3(4,5,6), "Punctual edit affected emitter geometry");
        const auto before=scene;
        throws([&] { add_editor_light(scene,static_cast<LightKind>(99)); }, "Unsupported creation succeeded"); unchanged(scene,before);
        while (scene.lights.size()<64) add_editor_light(scene,LightKind::point);
        const auto full=scene;
        for (auto kind:{LightKind::directional,LightKind::point,LightKind::rectangle}) {
            throws([&] { add_editor_light(scene,kind); }, "65th editor light was accepted"); unchanged(scene,full);
        }
        set_editor_light_position(scene,2,{0,3,0}); check_emitter(scene,2);
        check(scene.lights.size()==64, "Limit prevented editing an existing light");
    });
    test("Editor lights: pose does not invalidate static assets, emission does",[] {
        Scene scene;add_editor_light(scene,LightKind::rectangle);scene.asset_revision=7;
        set_editor_light_position(scene,0,{.5f,3,0});check(scene.asset_revision==7,"Light movement invalidated static resources");
        auto light=scene.lights[0];light.intensity+=1;update_editor_light(scene,0,light);
        check(scene.asset_revision==8,"Emitter material change did not invalidate static resources");
        scene.asset_revision=UINT64_MAX;const auto before=scene;light.intensity+=1;
        throws([&]{update_editor_light(scene,0,light);},"Asset revision overflow accepted");unchanged(scene,before);
    });
    test("Volume GI applicability: area-only and high-poly scenes are not direction/4096 gated",[] {
        Scene scene;scene.meshes.emplace_back();scene.meshes[0].vertices.resize(3);
        scene.meshes[0].indices.resize(120972*3,0);
        Light area;area.kind=LightKind::rectangle;scene.lights.push_back(area);
        Settings settings;settings.shadows=ShadowMode::pcf;
        for(auto mode:{GiMode::rsm,GiMode::lpv,GiMode::voxel}) {
            settings.gi=mode;const auto facts=inspect_config(scene,settings);
            check(facts.instanced_triangles==120972&&!facts.gpu_volume_missing_directional&&
                !facts.gpu_volume_triangle_limit_exceeded&&!facts.gpu_fallback_expected,
                "Valid large area scene still blocked by old teaching limits");
            check(facts.gpu_volume_light_index==0&&facts.gpu_volume_area_center_approximate&&
                !facts.gpu_volume_primary_source_only,"Area center approximation not reported accurately");
        }
    });
    test("Volume GI applicability: point-only source reports cube coverage",[] {
        Scene scene;Light point;point.kind=LightKind::point;scene.lights.push_back(point);
        Settings settings;settings.gi=GiMode::voxel;
        const auto facts=inspect_config(scene,settings);
        check(facts.gpu_volume_light_index==0&&facts.gpu_volume_point_cubemap&&
            !facts.gpu_volume_missing_directional&&!facts.gpu_fallback_expected,
            "Point-only GI was rejected or still described as a single frustum");
    });
    test("Volume GI applicability: no fabricated source and honest multi-light approximation",[] {
        Scene scene;Settings settings;settings.gi=GiMode::lpv;
        const auto empty=inspect_config(scene,settings);
        check(empty.gpu_volume_no_emitting_light&&empty.gpu_volume_light_index==-1&&!empty.gpu_fallback_expected,
            "Empty scene should be a valid zero-GI configuration");
        Light dark;dark.intensity=0;scene.lights.push_back(dark);
        Light point;point.kind=LightKind::point;scene.lights.push_back(point);
        Light area;area.kind=LightKind::rectangle;scene.lights.push_back(area);
        const auto facts=inspect_config(scene,settings);
        check(facts.gpu_volume_light_index==2&&facts.gpu_volume_primary_source_only&&
            facts.gpu_volume_area_center_approximate&&!facts.gpu_volume_no_emitting_light,
            "Dark directional light must not hide emitting sources; multiple lights need an explicit primary-source notice");
        settings.voxel_resolution=24;
        check(inspect_config(scene,settings).gpu_volume_resolution_unsupported&&inspect_config(scene,settings).gpu_fallback_expected,
            "Removing the geometry/light restrictions must not invent unsupported volume resolutions");
    });
    return results;
}
} // namespace emberframe::lab
