#include "showcase.h"
#include "environment.h"
#include "editor_assets.h"
#include "editor_lights.h"
#include "geometry.h"
#include "scene_resources.h"
#include <limits>
#include <sstream>

namespace emberframe::lab {
namespace {
void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
float error(glm::vec3 a,glm::vec3 b) {const auto e=glm::abs(a-b);return std::max({e.x,e.y,e.z});}
template<class Function> bool rejected(Function&& f) {try{f();return false;}catch(const std::exception&){return true;}}
std::vector<std::byte> hdr(bool rle) {
    const std::string header="#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X "+std::string(rle?"8":"2")+"\n";
    std::vector<std::byte> bytes;for(unsigned char c:header)bytes.push_back(std::byte(c));
    if(rle)for(int row=0;row<2;++row) {
        for(int value:{2,2,0,8})bytes.push_back(std::byte(value));
        for(int value:{128,64,32,130}){bytes.push_back(std::byte(136));bytes.push_back(std::byte(value));}
    }else for(int i=0;i<4;++i)for(int value:{128,64,32,130})bytes.push_back(std::byte(value));
    return bytes;
}
IblOptions small_ibl() {IblOptions options;options.width=8;options.height=4;options.roughness_levels=4;options.lut_resolution=4;options.samples=32;return options;}
}
TestResults test_showcase() {
    TestResults results;
    auto test=[&](const char* name,auto&& body) {
        try{results.push_back({name,true,body()});}
        catch(const std::exception& e){results.push_back({name,false,e.what()});}
        catch(...){results.push_back({name,false,"Unknown exception"});}
    };
    test("HDR flat and scanline-RLE decode to linear radiance",[] {
        for(bool rle:{false,true}) {
            const auto map=load_environment_hdr(hdr(rle),"fixture.hdr");
            require(map->image().width==(rle?8:2)&&map->image().height==2,"Incorrect decoded size");
            for(const auto pixel:map->image().pixels)require(error(pixel,{2,1,.5f})<1e-7f,"HDR decoded as sRGB/LDR or incorrect RGBE exponent");
        }
        return std::string("Actual Radiance RGBE stbi_loadf path; values >1 preserved");
    });
    test("HDR rejects truncation, zero RLE, huge dimensions and invalid floats",[] {
        auto flat=hdr(false);flat.pop_back();require(rejected([&]{(void)load_environment_hdr(flat);}),"Truncated flat HDR accepted");
        auto rle=hdr(true);rle[rle.size()-8]=std::byte{0};require(rejected([&]{(void)load_environment_hdr(rle);}),"Zero RLE accepted (would loop at EOF)");
        auto nan=Image<glm::vec3>(1,1,glm::vec3(1));nan.pixels[0].x=std::numeric_limits<float>::quiet_NaN();
        require(rejected([&]{(void)make_environment_map(nan);}),"NaN HDR accepted");
        nan.pixels[0]={-1,0,0};require(rejected([&]{(void)make_environment_map(nan);}),"Negative HDR accepted");
        nan.pixels[0]={65505,0,0};require(rejected([&]{(void)make_environment_map(nan);}),"Unsupported HDR range silently clipped");
        const std::string huge="#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 8192 +X 8192\n";
        const auto data=std::as_bytes(std::span(huge.data(),huge.size()));
        require(rejected([&]{(void)load_environment_hdr(data);}),"8M pixel budget not enforced");
        require(rejected([&]{(void)load_environment_hdr(std::as_bytes(std::span("not hdr",7)));}),"LDR/non-HDR accepted");
        return std::string("Bounded validation before float allocation and RGBE decoding");
    });
    test("HDR latlong seam, orientation, rotation, intensity and analytic fallback",[] {
        Image<glm::vec3> image(4,2);
        for(int y=0;y<2;++y)for(int x=0;x<4;++x)image.at(x,y)=glm::vec3(float(x+1));
        Scene scene;scene.environment_map=make_environment_map(std::move(image));
        const glm::vec3 d{std::sqrt(.5f),0,std::sqrt(.5f)};
        require(error(environment(scene,d),glm::vec3(1))<2e-6f,"Incorrect atan2(z,x) orientation");
        scene.environment_rotation=90;scene.environment_intensity=2;
        require(error(environment(scene,d),glm::vec3(4))<2e-6f,"World Y inverse rotation or intensity incorrect");
        require(error(environment(scene,{1,0,1e-6f}),environment(scene,{1,0,-1e-6f}))<1e-4f,"Longitude seam does not wrap");
        scene.environment_map.reset();scene.sky_top={2,4,6};scene.sky_bottom={0,0,0};
        require(error(environment(scene,{0,1,0}),{4,8,12})==0,"Analytic sky/intensity fallback broken");
        return std::string("HDR lookup is linear; Y-north and periodic longitude");
    });
    test("HDR immutable snapshot and self-contained project round-trip",[] {
        ProjectDocument source;source.scene.environment_map=load_environment_hdr(hdr(false),"portable.hdr");
        source.scene.environment_rotation=-31.5f;source.scene.environment_intensity=.75f;
        const auto map=source.scene.environment_map;
        const auto snapshot=capture_environment_project_snapshot(source.scene,source.camera,source.settings);
        require(snapshot.environment.map==map&&snapshot.project.find("scene.environment_map")==std::string::npos,"Snapshot duplicates embedded HDR");
        const auto text=serialize_project(source);const auto restored=deserialize_project(text);
        require(restored.scene.environment_map&&restored.scene.environment_map->fingerprint()==map->fingerprint(),"Portable HDR lost on project reload");
        require(restored.scene.environment_map->name()=="portable.hdr"&&restored.scene.environment_rotation==-31.5f&&restored.scene.environment_intensity==.75f,"Environment metadata lost");
        auto bad=serialize_environment(*map);bad.back()^=1;
        require(rejected([&]{(void)deserialize_environment(bad);}),"Corrupt embedded HDR accepted");
        Scene scene;Camera camera;Settings settings;restore_environment_project_snapshot(snapshot,scene,camera,settings);
        require(scene.environment_map==map,"Shared HDR identity lost during undo/preview restore");
        const auto previous=scene.environment_intensity;
        require(rejected([&]{set_scene_environment(scene,map,-1);})&&scene.environment_intensity==previous&&scene.environment_map==map,"Failed environment transaction changed scene");
        const auto legacy=deserialize_project("EmberFrame scene format 1\nscene.name 6f6c64\n");
        require(!legacy.scene.environment_map&&legacy.scene.environment_intensity==1,"Old analytic-sky document migration broken");
        return std::string("Persistence embeds linear pixels; memory snapshots retain the same immutable pointer");
    });
    test("HDR constant IBL, cache keys and SH/PRT environment invalidation",[] {
        Scene scene;scene.environment_map=make_environment_map(Image<glm::vec3>(4,2,{2,1,.5f}));
        const auto first=cached_scene_ibl(scene,small_ibl());
        require(error(sample_ibl_diffuse(*first,{0,1,0}),glm::vec3(2,1,.5f)*pi)<1e-5f,"Diffuse irradiance missing pi or HDR scale");
        require(error(sample_ibl_specular(*first,{1,0,0},.7f),{2,1,.5f})<1e-5f,"Specular constant environment incorrect");
        require(cached_scene_ibl(scene,small_ibl())==first,"Identical environment cache missed");
        ++scene.revision;require(cached_scene_ibl(scene,small_ibl())==first,"Geometry-only revision rebuilt IBL");
        const auto hash=environment_fingerprint(scene);scene.environment_intensity=2;
        const auto second=cached_scene_ibl(scene,small_ibl());
        require(second!=first&&environment_fingerprint(scene)!=hash,"Environment intensity absent from CPU cache key");
        require(second->shared_maps==first->shared_maps,"Intensity edit re-integrated/copied the environment maps");
        require(error(sample_ibl_specular(*second,{1,0,0},.5f),{4,2,1})<1e-5f,"CPU IBL ignored intensity");
        Image<glm::vec3> gradient(16,8);
        for(int y=0;y<8;++y)for(int x=0;x<16;++x)gradient.at(x,y)={.1f+x*.1f,.2f+y*.1f,2};
        Scene rotated;rotated.environment_map=make_environment_map(std::move(gradient));rotated.environment_intensity=.65f;rotated.environment_rotation=67;
        Scene raw=rotated;raw.environment_intensity=1;raw.environment_rotation=0;
        const auto raw_data=precompute_scene_ibl(raw,small_ibl()),rotated_data=precompute_scene_ibl(rotated,small_ibl());
        const glm::vec3 d=safe_normalize(glm::vec3(.6f,.3f,-.4f));
        require(error(sample_ibl_diffuse(rotated_data,d),sample_ibl_diffuse(raw_data,environment_lookup_direction(d,67))*.65f)<1e-6f,
                "CPU scene IBL rotates integration grid instead of sharing GPU lookup convention");
        Settings settings;settings.environment_diffuse=EnvironmentDiffuse::sh;
        const auto resources=prepare_scene_resources(scene,settings);
        require(resources->environment_hash==environment_fingerprint(scene),"Scene bake still hashes analytic sky only");
        require(error(evaluate_environment_sh(resources->environment_sh9,{0,1,0}),glm::vec3(4,2,1)*pi)<.02f,"HDR SH irradiance incorrect");
        return std::string("One shared CPU/GPU prefilter algorithm; runtime GPU parity still needs Vulkan test");
    });
    test("Three offline representative scenes have valid resources and usable cameras",[] {
        require(showcase_catalog().size()==3,"Showcase catalog incomplete");
        require(rejected([]{(void)parse_showcase("unsupported");}),"Unknown showcase silently replaced");
        std::size_t triangles=0;
        for(const auto& entry:showcase_catalog()) {
            auto project=make_showcase(entry.kind);const auto& scene=project.scene;
            require(!scene.meshes.empty()&&!scene.materials.empty()&&!scene.nodes.empty(),"Showcase has no real geometry");
            require(!scene.textures.empty()&&!scene.lights.empty(),"Showcase has no material textures/lights");
            require(project.settings.render_width==1280&&project.settings.render_height==720,"Wrong showcase default resolution");
            const auto world=resolve_world_transforms(scene);
            for(std::size_t i=0;i<world.size();++i)for(int c=0;c<4;++c)
                require(glm::all(glm::lessThanEqual(glm::abs(world[i][c]-scene.nodes[i].previous_world[c]),glm::vec4(1e-6f))),"Initial history is not world-space transform");
            for(std::size_t i=0;i<scene.lights.size();++i)if(scene.lights[i].linked_node>=0)validate_light_binding(scene,i);
            for(const auto& mesh:scene.meshes) {
                require(mesh.indices.size()%3==0,"Incomplete triangle indices");
                for(const auto& primitive:mesh.primitives)require(primitive.material<scene.materials.size(),"Missing material binding");
                for(std::size_t i=0;i<mesh.indices.size();i+=3) {
                    const auto& a=mesh.vertices.at(mesh.indices[i]);const auto& b=mesh.vertices.at(mesh.indices[i+1]);const auto& c=mesh.vertices.at(mesh.indices[i+2]);
                    const auto geometric=glm::cross(b.position-a.position,c.position-a.position);
                    require(glm::dot(geometric,geometric)>1e-18f,"Procedural mesh contains degenerate triangles");
                    require(glm::dot(geometric,a.normal+b.normal+c.normal)>0,"Procedural winding differs from outward normals");
                }
                triangles+=mesh.indices.size()/3;
            }
            // 只做未来执行的 CPU 画面存在性断言；不把它冒充实际 Vulkan/UI 验收。
            auto raster_settings=project.settings;raster_settings.render_width=96;raster_settings.render_height=54;
            const auto surfaces=rasterize(scene,project.camera,raster_settings);
            const auto visible=std::count_if(surfaces.pixels.begin(),surfaces.pixels.end(),[](const auto& s){return s.valid;});
            require(visible>96*54/8,"Default camera sees almost no scene");
            const auto saved=serialize_project(project);
            const auto loaded=deserialize_project(saved);
            require(loaded.scene.nodes.size()==scene.nodes.size()&&loaded.scene.textures.size()==scene.textures.size(),"Showcase project round-trip lost resources");
        }
        return std::string("Fine meshes, procedural material maps, fixed cameras; unique-mesh triangles=")+std::to_string(triangles);
    });
    test("Many-object scene genuinely shares meshes and contains >25 lights",[] {
        const auto project=make_showcase("many-objects");const auto& scene=project.scene;
        require(scene.nodes.size()>120&&scene.lights.size()==33,"Complex scene does not exercise instances/many lights");
        std::vector<int> counts(scene.meshes.size());for(const auto& node:scene.nodes)if(node.mesh>=0)++counts.at(node.mesh);
        require(*std::max_element(counts.begin(),counts.end())>10,"Shared meshes duplicated per instance");
        require(project.settings.path==RenderPath::deferred&&project.settings.culling==LightCulling::clustered,"Wrong multi-light default");
        return std::string("100 central instances + architectural occluders; shared resources and 32 local point lights");
    });
    return results;
}
} // namespace emberframe::lab
