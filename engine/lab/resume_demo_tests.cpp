#include "resume_demo.h"
#include "scene_geometry.h"
#include "systems.h"

namespace emberframe::lab {
TestResults test_resume_demo() {
    TestResults out;
    auto test=[&](const char* name,auto&& f){try{out.push_back({name,f(),"Fixed real scene/camera contract"});}catch(const std::exception& e){out.push_back({name,false,e.what()});}};
    test("Resume profiles have real meshes and finite cameras",[]{for(auto name:{"pbr","kc","lights","shadows","temporal","geometry"}){auto d=make_resume_demo(name);if(flatten_scene(d.scene).empty()||!std::isfinite(d.camera.view()[0][0]))return false;}return true;});
    test("KC controlled furnace has no lights/bloom and rough white metal",[]{auto d=make_resume_demo("kc");return d.scene.lights.empty()&&!d.settings.bloom&&d.scene.sky_top==glm::vec3(1)&&d.scene.sky_bottom==glm::vec3(1)&&d.scene.materials[0].metallic==1&&d.scene.materials[0].roughness==1;});
    test("Replay is deterministic and does not rotate models",[]{auto d=make_resume_demo("temporal");auto a=replay_demo_camera(d.camera,"pan",12,48),b=replay_demo_camera(d.camera,"pan",12,48);return a.position==b.position&&glm::length(a.position-d.camera.position)>.1f&&glm::length((a.target-a.position)-(d.camera.target-d.camera.position))<1e-5f;});
    test("Replay validates ranges and returns to origin",[]{auto d=make_resume_demo("shadows");auto c=replay_demo_camera(d.camera,"pan",47,48);bool rejected=false;try{replay_demo_camera(d.camera,"bad",0,48);}catch(const std::invalid_argument&){rejected=true;}return rejected&&glm::length(c.position-d.camera.position)<1e-5f;});
    test("Light profile has actual offscreen objects",[]{auto d=make_resume_demo("lights");SceneSpatialIndex index;index.prepare(d.scene);return index.visible_objects(d.camera,640,360).size()<d.scene.nodes.size();});
    test("Geometry replay consumes genuine persistent QEM chain",[]{auto d=make_resume_demo("geometry");build_scene_lods(d.scene,0);if(d.scene.meshes[0].lods.empty())return false;d.settings.auto_lod=true;SceneGeometryRuntime runtime;runtime.prepare(d.scene,d.camera,d.settings);const auto near=runtime.stats().selected_triangles;auto far=replay_demo_camera(d.camera,"dolly",24,49);runtime.prepare(d.scene,far,d.settings);return runtime.stats().selected_triangles<near;});
    test("SAH BVH probe accumulates every ray and matches brute force",[]{auto d=make_resume_demo("geometry");const auto p=probe_demo_bvh(d.scene,d.camera);return p.matches&&p.hits>0&&p.rays==192&&p.brute_triangle_tests==p.rays*flatten_scene(d.scene).size()&&p.bvh_triangle_tests>0&&p.bvh_triangle_tests<p.brute_triangle_tests;});
    return out;
}
}
