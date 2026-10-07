#include "systems.h"
#include "geometry.h"
#include "scene_resources.h"
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>

namespace emberframe::lab {
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class F> void throws(F&& f,const char* message) {bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught,message);}
bool close(float a,float b,float eps=1e-5f) {return std::abs(a-b)<=eps;}
struct TemporaryFiles {
    std::filesystem::path root;
    TemporaryFiles() {
        static std::atomic<unsigned> serial{0};const auto base=std::filesystem::temp_directory_path();
        for(unsigned tries=0;tries<100;++tries) {
            auto candidate=base/("emberframe-systems-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial.fetch_add(1)));
            if(std::filesystem::create_directory(candidate)){root=std::move(candidate);return;}
        }
        throw std::runtime_error("Cannot create isolated test directory");
    }
    ~TemporaryFiles() {if(!root.empty()){std::error_code ignored;std::filesystem::remove_all(root,ignored);}}
};
void write_bytes(const std::filesystem::path& path,const std::vector<unsigned char>& data) {
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()));check(bool(file),"Fixture write failed");
}
void write_text(const std::filesystem::path& path,const std::string& text) {write_bytes(path,{text.begin(),text.end()});}
void u32(std::vector<unsigned char>& b,std::uint32_t v) {for(int i=0;i<4;++i)b.push_back(static_cast<unsigned char>((v>>(8*i))&255));}
void be32(std::vector<unsigned char>& b,std::uint32_t v) {for(int i=3;i>=0;--i)b.push_back(static_cast<unsigned char>((v>>(8*i))&255));}
void f32(std::vector<unsigned char>& b,float v) {u32(b,std::bit_cast<std::uint32_t>(v));}
std::uint32_t crc32(const std::vector<unsigned char>& data,std::size_t start) {
    std::uint32_t crc=~0u;for(auto i=start;i<data.size();++i){crc^=data[i];for(int j=0;j<8;++j)crc=(crc>>1)^(0xedb88320u&std::uint32_t(-int(crc&1)));}return ~crc;
}
std::vector<unsigned char> tiny_png() {
    // 自己生成的 2x1 RGBA PNG：不依赖下载、固定外部文件或 stb 的编码器。
    std::vector<unsigned char> png={137,80,78,71,13,10,26,10};
    auto chunk=[&](const char* type,const std::vector<unsigned char>& payload) {
        be32(png,std::uint32_t(payload.size()));const auto start=png.size();for(int i=0;i<4;++i)png.push_back(type[i]);
        png.insert(png.end(),payload.begin(),payload.end());be32(png,crc32(png,start));
    };
    std::vector<unsigned char> ihdr;be32(ihdr,2);be32(ihdr,1);ihdr.insert(ihdr.end(),{8,6,0,0,0});chunk("IHDR",ihdr);
    const std::vector<unsigned char> raw={0,128,64,255,64,255,128,0,255};
    std::vector<unsigned char> zlib={0x78,0x01,0x01,9,0,246,255};zlib.insert(zlib.end(),raw.begin(),raw.end());
    std::uint32_t a=1,b=0;for(auto v:raw){a=(a+v)%65521;b=(b+a)%65521;}be32(zlib,(b<<16)|a);chunk("IDAT",zlib);chunk("IEND",{});return png;
}
std::filesystem::path gltf_fixture(const std::filesystem::path& root,bool glb,bool nondefault=false,bool bad_index=false) {
    std::vector<unsigned char> buffer;
    for(const auto v:std::array<std::array<float,5>,3>{{{{0,0,0,0,0}},{{1,0,0,1,0}},{{0,1,0,0,1}}}})for(auto f:v)f32(buffer,f);
    const auto colors=buffer.size();buffer.insert(buffer.end(),{255,128,64,128,64,255,128,255,128,64,255,64});
    const auto indices=buffer.size();for(auto index:{2u,0u,bad_index?99u:1u}){buffer.push_back(static_cast<unsigned char>(index));buffer.push_back(0);}
    while(buffer.size()%4)buffer.push_back(0);
    const auto normals=buffer.size();for(int i=0;i<3;++i)for(auto f:{0.f,0.f,1.f})f32(buffer,f);
    const auto image_offset=buffer.size();const auto png=tiny_png();if(glb)buffer.insert(buffer.end(),png.begin(),png.end());
    std::ostringstream j;
    j<<R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"name":"Fixture scene","nodes":[0,2]},{"nodes":[3]}],"buffers":[{"byteLength":)"<<buffer.size();
    if(!glb)j<<R"(,"uri":"mesh.bin")";
    j<<R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":60,"byteStride":20},{"buffer":0,"byteOffset":)"<<colors<<R"(,"byteLength":12},{"buffer":0,"byteOffset":)"<<indices
     <<R"(,"byteLength":6},{"buffer":0,"byteOffset":)"<<normals<<R"(,"byteLength":36})";
    if(glb)j<<R"(,{"buffer":0,"byteOffset":)"<<image_offset<<R"(,"byteLength":)"<<png.size()<<'}';
    j<<R"(],"accessors":[{"bufferView":0,"byteOffset":12,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"},{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5121,"normalized":true,"count":3,"type":"VEC4"},{"bufferView":3,"componentType":5126,"count":3,"type":"VEC3"}],"images":[)";
    j<<(glb?R"({"name":"RGBA test","bufferView":4,"mimeType":"image/png"})":R"({"name":"RGBA test","uri":"texture.png"})");
    j<<R"(],"textures":[{"source":0)";if(nondefault)j<<R"(,"sampler":0)";j<<"}]";
    if(nondefault)j<<R"(,"samplers":[{"magFilter":9728,"minFilter":9728,"wrapS":33071,"wrapT":33648}])";
    j<<R"(,"materials":[{"name":"Alpha material","alphaMode":"BLEND","alphaCutoff":0.4,"doubleSided":true,"pbrMetallicRoughness":{"baseColorFactor":[1,0.5,0.25,0.75],"metallicFactor":0.6,"roughnessFactor":0.3,"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":0}}}],"meshes":[{"name":"Indexed and nonindexed","primitives":[{"attributes":{"TEXCOORD_0":0,"COLOR_0":3,"POSITION":2,"NORMAL":4},"indices":1,"material":0},{"attributes":{"POSITION":2,"NORMAL":4,"TEXCOORD_0":0},"material":0}]}],"nodes":[{"name":"Parent","translation":[1,2,3],"rotation":[0,0.7071067811865475,0,0.7071067811865475],"children":[1]},{"name":"Child","mesh":0,"translation":[0,0,2],"scale":[2,1,1]},{"name":"Instance","mesh":0,"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,5,0,0,1]},{"name":"Other scene","mesh":0}]})";
    std::string json=j.str();const auto path=root/(glb?"fixture.glb":"fixture.gltf");
    if(glb) {
        while(json.size()%4)json+=' ';while(buffer.size()%4)buffer.push_back(0);
        std::vector<unsigned char> out;u32(out,0x46546c67);u32(out,2);u32(out,std::uint32_t(12+8+json.size()+8+buffer.size()));
        u32(out,std::uint32_t(json.size()));u32(out,0x4e4f534a);out.insert(out.end(),json.begin(),json.end());
        u32(out,std::uint32_t(buffer.size()));u32(out,0x004e4942);out.insert(out.end(),buffer.begin(),buffer.end());write_bytes(path,out);
    } else {write_text(path,json);write_bytes(root/"mesh.bin",buffer);write_bytes(root/"texture.png",png);}
    return path;
}
std::filesystem::path sparse_fixture(const std::filesystem::path& root,bool invalid) {
    std::vector<unsigned char> b={0,static_cast<unsigned char>(invalid?0:1),2,0};
    for(float v:{0.f,0.f,0.f,1.f,0.f,0.f,0.f,1.f,0.f})f32(b,v);write_bytes(root/"sparse.bin",b);
    const std::string json=R"({"asset":{"version":"2.0"},"buffers":[{"uri":"sparse.bin","byteLength":40}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":3},{"buffer":0,"byteOffset":4,"byteLength":36}],"accessors":[{"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0],"sparse":{"count":3,"indices":{"bufferView":0,"componentType":5121},"values":{"bufferView":1}}}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}]})";
    const auto path=root/"sparse.gltf";write_text(path,json);return path;
}
std::filesystem::path frame_fixture(const std::filesystem::path& root,const std::vector<Vertex>& vertices,
                                   const std::vector<std::uint32_t>& indices,bool normals=true,bool tangents=false,bool repeat=false) {
    std::vector<unsigned char> buffer;
    glm::vec3 minimum(std::numeric_limits<float>::max()),maximum(-std::numeric_limits<float>::max());
    for(const auto& v:vertices) {
        minimum=glm::min(minimum,v.position);maximum=glm::max(maximum,v.position);
        for(auto f:{v.position.x,v.position.y,v.position.z,v.normal.x,v.normal.y,v.normal.z,v.uv.x,v.uv.y,
                    v.tangent.x,v.tangent.y,v.tangent.z,v.tangent.w})f32(buffer,f);
    }
    const auto index_offset=buffer.size();for(auto id:indices)u32(buffer,id);
    std::ostringstream json;json<<std::setprecision(std::numeric_limits<float>::max_digits10);
    json<<R"({"asset":{"version":"2.0"},"buffers":[{"uri":"frame.bin","byteLength":)"<<buffer.size()
        <<R"(}],"bufferViews":[{"buffer":0,"byteLength":)"<<index_offset<<R"(,"byteStride":48},{"buffer":0,"byteOffset":)"<<index_offset
        <<R"(,"byteLength":)"<<indices.size()*4<<R"(}],"accessors":[{"bufferView":0,"componentType":5126,"count":)"<<vertices.size()
        <<R"(,"type":"VEC3","min":[)"<<minimum.x<<','<<minimum.y<<','<<minimum.z<<R"(],"max":[)"<<maximum.x<<','<<maximum.y<<','<<maximum.z
        <<R"(]},{"bufferView":0,"byteOffset":12,"componentType":5126,"count":)"<<vertices.size()<<R"(,"type":"VEC3"},)"
        <<R"({"bufferView":0,"byteOffset":24,"componentType":5126,"count":)"<<vertices.size()<<R"(,"type":"VEC2"},)"
        <<R"({"bufferView":0,"byteOffset":32,"componentType":5126,"count":)"<<vertices.size()<<R"(,"type":"VEC4"},)"
        <<R"({"bufferView":1,"componentType":5125,"count":)"<<indices.size()<<R"(,"type":"SCALAR"}],"meshes":[{"primitives":[)";
    for(int p=0;p<(repeat ? 2:1);++p) {
        if(p)json<<',';
        json<<R"({"attributes":{"POSITION":0,"TEXCOORD_0":2)";
        if(normals)json<<R"(,"NORMAL":1)";
        if(tangents)json<<R"(,"TANGENT":3)";
        json<<R"(},"indices":4})";
    }
    json<<R"(]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}]})";
    write_bytes(root/"frame.bin",buffer);const auto path=root/"frame.gltf";write_text(path,json.str());return path;
}
void check_frame(const Vertex& v) {
    check(close(glm::length(v.normal),1)&&close(glm::length(glm::vec3(v.tangent)),1),"Normal/tangent is not finite unit length");
    check(close(glm::dot(v.normal,glm::vec3(v.tangent)),0),"Normal/tangent is not orthogonal");
    check(v.tangent.w==-1 || v.tangent.w==1,"Tangent handedness is not a sign");
}
}

TestResults test_systems() {
    TestResults results;
    auto test=[&](std::string name,auto&& function) {
        TestResult r;r.name=std::move(name);
#ifdef EMBERFRAME_SYSTEMS_TEST_MAIN
        std::cerr<<"RUN "<<r.name<<std::endl;
#endif
        try{function();r.passed=true;r.detail="All deterministic assertions passed";}
        catch(const std::exception& e){r.detail=e.what();}catch(...){r.detail="Non-standard exception";}
#ifdef EMBERFRAME_SYSTEMS_TEST_MAIN
        std::cerr<<(r.passed?"OK ":"ERROR ")<<r.detail<<std::endl;
#endif
        results.push_back(std::move(r));
    };
    test("C9 jobs: exact row coverage and dependency diamond",[]{
        JobSystem jobs(3,64);std::vector<int> rows(257,-1);
        jobs.parallel_for(0,rows.size(),[&](std::size_t y){rows[y]=int(y*y);},7);
        for(std::size_t y=0;y<rows.size();++y)check(rows[y]==int(y*y),"Lost/duplicated row");
        int root=0,left=0,right=0,total=0;
        auto a=jobs.submit([&]{root=7;});std::array deps_a{a};
        auto b=jobs.submit([&]{left=root*2;},deps_a),c=jobs.submit([&]{right=root+3;},deps_a);
        std::array deps_bc{b,c};jobs.submit([&]{total=left+right;},deps_bc).get();check(total==24,"Dependency order broken");jobs.wait_idle();
    });
    test("C9 jobs: actual work stealing and nested wait",[]{
        JobSystem jobs(2,16);std::promise<bool> proof;auto result=proof.get_future();
        auto root=jobs.submit([&]{
            const auto owner=std::this_thread::get_id();
            auto child=jobs.submit([&,owner]{proof.set_value(std::this_thread::get_id()!=owner);});
            check(result.wait_for(std::chrono::seconds(5))==std::future_status::ready,"No worker stole the owner's queued child");
            check(result.get(),"Child ran on the blocked owner, not a thief");child.get();
            jobs.parallel_for(19,[](std::size_t i){check(i<19,"Nested range overflow");});
        });root.get();
    });
    test("C9 jobs: dependency errors, foreign handles and clean shutdown",[]{
        JobSystem jobs(2,32),other(1,4);std::atomic<int> ran{0};
        auto failed=jobs.submit([]{throw std::runtime_error("intentional failure");});std::array deps{failed};
        auto child=jobs.submit([&]{++ran;},deps);throws([&]{child.get();},"Dependency failure was swallowed");check(ran==0,"Failed prerequisite still ran child");
        throws([&]{other.submit([]{},deps);},"Foreign dependency accepted");
        for(int i=0;i<100;++i)jobs.submit([&]{++ran;});jobs.shutdown();check(ran==100&&jobs.pending()==0,"Shutdown did not drain");
        throws([&]{jobs.submit([]{});},"Submission after shutdown accepted");
    });
    test("C9 jobs: bounded nested saturation and self wait rejected",[]{
        JobSystem jobs(1,1);auto task=jobs.submit([&]{throws([&]{jobs.submit([]{});},"Saturated nested submit should fail, not hang");});task.get();
        JobSystem cycle_jobs(1,4);std::promise<void> release;auto gate=release.get_future().share();JobSystem::Task self;
        self=cycle_jobs.submit([&]{gate.wait();throws([&]{self.get();},"Self wait was not rejected");});release.set_value();self.get();
    });
    test("C9 jobs: cross-worker wait cycles and exception drain",[]{
        JobSystem jobs(2,32);std::promise<void> release;auto gate=release.get_future().share();JobSystem::Task a,b;
        a=jobs.submit([&]{gate.wait();b.get();});b=jobs.submit([&]{gate.wait();a.get();});release.set_value();
        throws([&]{a.get();},"Dynamic wait cycle was accepted");throws([&]{b.get();},"Cycle failure did not propagate");
        std::atomic<int> running{0};
        throws([&]{jobs.parallel_for(100,[&](std::size_t i){++running;--running;if(i==7)throw std::runtime_error("row failure");});},"Parallel row error swallowed");
        check(running==0&&jobs.pending()==0,"parallel_for returned while chunks still ran");
        JobSystem nested(1,8);std::promise<void> publish;auto published=publish.get_future().share();JobSystem::Task parent;
        parent=nested.submit([&]{published.wait();auto child=nested.submit([&]{throws([&]{parent.get();},"Nested child waited on executing ancestor");});child.get();});
        publish.set_value();parent.get();
    });
    test("C6 render graph: executable pipeline, culling, lifetimes and barriers",[]{
        RenderGraph graph;graph.add_resource("depth");graph.add_resource("lit");graph.add_resource("unused");
        int depth=0,color=0,display=0,dead=0;std::vector<std::string> events;
        graph.add_pass("depth",{{"depth",ResourceAccess::write,ResourceState::depth_attachment}},[&]{depth=4;events.push_back("depth");});
        graph.add_pass("dead",{{"unused",ResourceAccess::write,ResourceState::storage}},[&]{++dead;});
        graph.add_pass("light",{{"depth",ResourceAccess::read,ResourceState::shader_read},{"lit",ResourceAccess::write,ResourceState::storage}},[&]{color=depth*3;events.push_back("light");});
        graph.add_pass("display",{{"lit",ResourceAccess::read,ResourceState::shader_read}},[&]{display=color;events.push_back("display");},true);
        const auto plan=graph.compile();check(plan.culled_passes==std::vector<std::string>{"dead"},"Dead pass was retained");
        check(plan.pass_names==std::vector<std::string>({"depth","light","display"}),"Wrong topological order");
        std::size_t barriers=0;graph.execute(plan,[&](const ResourceBarrier&){++barriers;});
        check(display==12&&dead==0&&events==plan.pass_names,"Graph callbacks not executed correctly");check(barriers==4,"Missing write/read transitions");
        check(plan.lifetimes.size()==2&&plan.lifetimes[0].first==0&&plan.lifetimes[0].last==1&&plan.lifetimes[1].first==1&&plan.lifetimes[1].last==2,"Wrong resource lifetime intervals");
        graph.add_resource("new");throws([&]{graph.execute(plan);},"Stale plan accepted");
    });
    test("C6 render graph: overwrite cull, read_modify_write and same-state barrier",[]{
        RenderGraph graph;graph.add_resource("value");int value=0;
        graph.add_pass("overwritten",{{"value",ResourceAccess::write,ResourceState::storage}},[&]{value=100;});
        graph.add_pass("initialize",{{"value",ResourceAccess::write,ResourceState::storage}},[&]{value=2;});
        graph.add_pass("increment",{{"value",ResourceAccess::read_write,ResourceState::storage}},[&]{value+=3;});graph.export_resource("value");
        const auto plan=graph.compile();graph.execute(plan);check(value==5&&plan.culled_passes==std::vector<std::string>{"overwritten"},"Overwrite/data culling incorrect");
        check(plan.barriers.size()==2&&plan.barriers.back().before==ResourceState::storage&&plan.barriers.back().after==ResourceState::storage&&plan.barriers.back().memory_dependency,"Same-state RAW/WAW barrier missing");
        check(plan.lifetimes[0].exported&&plan.lifetimes[0].last==plan.order.size(),"Export lifetime does not cover external consumer");
    });
    test("C6 render graph: invalid reads, cycles, undefined export and imported data",[]{
        RenderGraph read;read.add_resource("r");read.add_pass("bad",{{"r",ResourceAccess::read}},[]{},true);
        read.add_pass("late",{{"r",ResourceAccess::write,ResourceState::storage}},[]{});throws([&]{read.compile();},"Read-before-write accepted");
        RenderGraph cycle;cycle.add_pass("a",{},[]{},true,{"b"});cycle.add_pass("b",{},[]{},false,{"a"});throws([&]{cycle.compile();},"Cycle accepted");
        RenderGraph empty;empty.add_resource("r");empty.export_resource("r");throws([&]{empty.compile();},"Unwritten export accepted");
        RenderGraph imported;imported.add_resource("history",true,ResourceState::shader_read);int calls=0;
        imported.add_pass("sample",{{"history",ResourceAccess::read}},[&]{++calls;},true);auto plan=imported.compile();imported.execute(plan);check(calls==1&&plan.lifetimes[0].imported,"Imported resource not available");
    });
    test("C7 assets: cache/version and bounded chunked upload",[]{
        JobSystem jobs(2,16);std::atomic<int> loads{0};
        AssetPipeline pipeline(jobs,2,[&](const std::filesystem::path&){++loads;Scene s;s.materials.emplace_back();s.nodes.emplace_back();return s;});
        auto first=pipeline.request("procedural-fixture",1);auto same=pipeline.request("procedural-fixture",1);
        check(first.get()==same.get()&&loads==1,"Cache did not deduplicate");check(first.snapshot().stage==AssetStage::ready,"CPU readiness missing");
        pipeline.queue_upload(first);check(first.snapshot().stage==AssetStage::upload_needed,"Upload-needed state missing");
        std::size_t expected=0;const auto total=first.snapshot().upload_bytes;
        while(first.snapshot().stage!=AssetStage::uploaded) {
            const auto used=pipeline.pump_uploads(37,[&](const AssetPipeline::Ticket&,const Scene&,std::size_t offset,std::size_t bytes){check(offset==expected&&bytes<=37,"Noncontiguous/overbudget upload");expected+=bytes;});
            check(used>0&&used<=37,"Upload exceeded frame budget/stalled");
        }
        check(expected==total&&total==256,"Wrong upload accounting");
        auto newer=pipeline.request("procedural-fixture",2);check(newer.get()!=first.get()&&loads==2,"Version did not invalidate cache");
        pipeline.request("another-fixture",1).get();check(pipeline.cached_count()==2,"Cache capacity exceeded");
    });
    test("C7 assets: in-flight bounds and parse/upload error propagation",[]{
        JobSystem jobs(2,16);std::promise<void> release;auto gate=release.get_future().share();
        AssetPipeline bounded(jobs,1,[gate](const std::filesystem::path&){gate.wait();return Scene{};});
        auto waiting=bounded.request("pending");bool rejected=false;try{bounded.request("overflow");}catch(const std::length_error&){rejected=true;}
        release.set_value();waiting.get();check(rejected,"In-flight cache was not bounded");
        bounded.pump_uploads(0,[](const auto&,const auto&,auto,auto){});check(waiting.snapshot().stage==AssetStage::uploaded,"Zero-byte asset never completed");
        AssetPipeline broken(jobs,2,[](const std::filesystem::path&)->Scene{throw std::runtime_error("bad fixture");});
        auto bad=broken.request("bad");throws([&]{bad.get();},"Parse error was swallowed");check(bad.snapshot().stage==AssetStage::failed&&bad.snapshot().error=="bad fixture","No parse failure status");
        AssetPipeline upload(jobs,1,[](const std::filesystem::path&){Scene s;s.nodes.emplace_back();return s;});auto ready=upload.request("upload");ready.get();
        throws([&]{upload.pump_uploads(10,[](const auto&,const auto&,auto,auto){throw std::runtime_error("GPU staging failure");});},"Upload error swallowed");
        check(ready.snapshot().stage==AssetStage::failed&&ready.snapshot().uploaded_bytes==0,"Failed upload advanced byte cursor");
    });
    test("C11 project: full roundtrip, unknown fields, save-as and atomic failure",[]{
        TemporaryFiles files;ProjectDocument p;p.scene=make_demo_scene(2);p.scene.name="Unicode \xe7\xa4\xba\xe4\xbe\x8b\nquoted \\\" name";
        Texture t;t.name="RGBA";t.srgb=true;t.levels.emplace_back(2,1,glm::vec4(.25f,.5f,.75f,.125f));t.levels.emplace_back(1,1,glm::vec4(.4f));p.scene.textures.push_back(t);
        p.scene.materials[0].base_texture=0;p.scene.materials[0].alpha_mode=1;p.scene.materials[0].clearcoat=.7f;p.scene.materials[0].anisotropy=.3f;
        p.camera.position={1.1f,2.2f,3.3f};p.camera.fov=63;p.settings.gi=GiMode::voxel;p.settings.debug=DebugView::light_count;
        p.settings.render_width=321;p.settings.seed=98765;p.settings.temporal_weight=.375f;p.settings.svgf=true;p.settings.hatching=true;
        p.settings.environment_diffuse=EnvironmentDiffuse::prt;p.settings.spatial_structure=SpatialStructure::octree;
        p.settings.sdf_shadows=true;p.settings.auto_lod=true;p.settings.lod_error_pixels=2.5f;p.settings.sdf_resolution=24;p.settings.bake_samples=96;
        p.unknown_lines={"# Future plugin data","settings.future_quality 17","material.vendor 0 preserved text"};
        const auto text=serialize_project(p);auto q=deserialize_project(text);check(serialize_project(q)==text,"Roundtrip is not deterministic/lossless");
        check(q.scene.name==p.scene.name&&q.scene.materials[0].base_texture==0&&close(q.scene.materials[0].clearcoat,.7f)&&close(q.scene.textures[0].levels[0].at(0,0).a,.125f),"Scene fields were lost");
        check(q.settings.seed==98765&&q.settings.render_width==321&&q.settings.gi==GiMode::voxel&&q.settings.debug==DebugView::light_count&&q.settings.svgf&&q.settings.hatching&&close(q.camera.fov,63),"Settings/camera fields were lost");
        check(q.settings.environment_diffuse==EnvironmentDiffuse::prt&&q.settings.spatial_structure==SpatialStructure::octree&&q.settings.sdf_shadows&&q.settings.auto_lod&&q.settings.bake_samples==96&&q.settings.sdf_resolution==24&&close(q.settings.lod_error_pixels,2.5f),"Scene resource settings lost");
        const auto path=files.root/"project.ember";save_project_document(path,p);q.settings.exposure=2;
        save_project(path,q.scene,q.camera,q.settings);q=load_project_document(path);check(q.unknown_lines==p.unknown_lines&&q.settings.exposure==2,"Convenience save dropped unknown fields");
        save_project_document(files.root/"save-as.ember",q);check(load_project_document(files.root/"save-as.ember").unknown_lines==p.unknown_lines,"Save-as lost unknown fields");
        q.camera.near_plane=-1;throws([&]{save_project_document(path,q);},"Invalid camera persisted");check(load_project_document(path).camera.near_plane>0,"Failed save damaged previous file");
    });
    test("C11 scene resources: baked data and non-destructive LOD persistence",[]{
        ProjectDocument p;p.scene=make_demo_scene(2);p.settings.environment_diffuse=EnvironmentDiffuse::prt;
        p.settings.sdf_shadows=true;p.settings.auto_lod=true;p.settings.bake_samples=16;p.settings.sdf_resolution=8;
        auto& source=p.scene.meshes[0];MeshLod lod;lod.vertices=source.vertices;lod.indices.assign(source.indices.begin(),source.indices.begin()+3);
        lod.primitives={{0,3,source.primitives.front().material}};lod.geometric_error=.012345678901234567;source.lods.push_back(lod);
        p.scene.baked_resources=prepare_scene_resources(p.scene,p.settings);
        const auto text=serialize_project(p);const auto restored=deserialize_project(text);
        check(restored.scene.meshes[0].indices==source.indices,"LOD replaced source mesh");
        check(restored.scene.meshes[0].lods.size()==1&&restored.scene.meshes[0].lods[0].indices==lod.indices&&restored.scene.meshes[0].lods[0].geometric_error==lod.geometric_error,"LOD data/error lost");
        check(restored.scene.baked_resources&&restored.scene.baked_resources->geometry_hash==geometry_fingerprint(restored.scene),"Bake persistence fingerprint mismatch");
        check(restored.scene.baked_resources->prt&&restored.scene.baked_resources->sdf,"Bake payload lost");
        check(serialize_project(restored)==text,"Baked project not deterministic");
        auto changed=p;changed.scene.meshes[0].vertices[0].position.x+=.5f;
        check(serialize_project(changed).find("scene.bake ")==std::string::npos,"Stale geometry bake persisted");
        auto invalid=p;invalid.scene.meshes[0].lods[0].indices[0]=UINT32_MAX;
        throws([&]{serialize_project(invalid);},"Invalid LOD index accepted");
    });
    test("C11 project: v0 migration and malformed-input rejection",[]{
        auto p=deserialize_project("EmberFrame scene format 0\nscene_name 4c6567616379\nexposure 2.5\nfuture.record abc\n");
        check(p.scene.name=="Legacy"&&p.settings.exposure==2.5f&&p.settings.render_width==640&&p.unknown_lines.size()==1,"v0 defaults/aliases not migrated");
        check(serialize_project(p).starts_with("EmberFrame scene format 1\n"),"Save did not upgrade v0");
        for(const auto bad:{"EmberFrame scene format 2\n","EmberFrame scene format 1\nsettings.samples 0\n","EmberFrame scene format 1\nsettings.bloom 3\n","EmberFrame scene format 1\nsettings.exposure 1\nsettings.exposure 2\n","EmberFrame scene format 1\ntexture 0 0 -\nlevel 0 0 2 1\npixel 0 0 1 1 1 1\n"})
            throws([&]{deserialize_project(bad);},"Malformed project accepted");
        TemporaryFiles files;write_text(files.root/"broken.ember","broken");Scene s=make_demo_scene(3);Camera c;Settings settings;
        throws([&]{load_project(files.root/"broken.ember",s,c,settings);},"Invalid load accepted");check(s.name=="Many lights"&&s.lights.size()==26,"Failed load mutated caller state");
    });
    test("C11 undo: material edits, drag transactions, cancel and redo invalidation",[]{
        Scene s=make_demo_scene(0);Camera camera;Settings settings;UndoStack undo;
        const auto original=s.materials[0].roughness;auto material=s.materials[0];material.roughness=.22f;auto revision=s.revision;
        undo.set_material(s,0,material);check(s.materials[0].roughness==.22f&&s.revision>revision,"Material edit/revision failed");revision=s.revision;
        check(undo.undo()&&s.materials[0].roughness==original&&s.revision>revision,"Material undo failed");check(undo.redo()&&s.materials[0].roughness==.22f,"Material redo failed");
        undo.begin(s,camera,settings,"Drag roughness");for(int i=0;i<10;++i)s.materials[0].roughness=.3f+.01f*i;settings.exposure=2;camera.position.x=9;
        check(undo.commit()&&undo.undo_label()=="Drag roughness","Transaction did not merge edits");check(undo.undo()&&settings.exposure==1&&camera.position.x==5&&close(s.materials[0].roughness,.22f),"Transaction undo missed state");
        check(undo.redo()&&settings.exposure==2&&camera.position.x==9&&close(s.materials[0].roughness,.39f),"Transaction redo missed state");
        undo.begin(s,camera,settings);settings.exposure=10;undo.cancel();check(settings.exposure==2,"Cancel failed");
        undo.begin(s,camera,settings);check(!undo.commit(),"Unchanged transaction created history");
        undo.undo();material=s.materials[0];material.roughness=.8f;undo.set_material(s,0,material);check(!undo.can_redo(),"New edit retained obsolete redo branch");
    });
    test("C11 undo: bounded history and failed undo retains cursor",[]{
        UndoStack stack(2,32);int value=0;
        for(int i=1;i<=3;++i)stack.execute(std::to_string(i),[&,i]{value=i-1;},[&,i]{value=i;},8);
        check(stack.undo()&&value==2&&stack.undo()&&value==1&&!stack.undo(),"History count limit failed");
        throws([&]{stack.execute("too big",[]{},[]{},33);},"Undo memory budget ignored");
        stack.clear();stack.push_applied("bad undo",[]{throw std::runtime_error("blocked");},[]{});
        throws([&]{stack.undo();},"Undo failure swallowed");check(stack.can_undo()&&!stack.can_redo(),"Failed undo moved cursor");
    });
    test("C7 glTF/GLB: indexed attributes, transforms, textures, alpha and default sampler",[]{
        TemporaryFiles files;
        for(bool binary:{false,true}) {
            const auto path=gltf_fixture(files.root,binary);auto loaded=load_scene_asset_detailed(path);const auto& s=loaded.scene;
            check(s.meshes.size()==1&&s.nodes.size()==3&&s.meshes[0].vertices.size()==6,"Mesh instancing/default scene selection incorrect");
            const auto& m=s.meshes[0];check(m.indices==std::vector<std::uint32_t>({2,0,1,3,4,5}),"Accessor indices/base-vertex offset lost");
            check(close(m.vertices[1].position.x,1)&&close(m.vertices[1].uv.x,1)&&close(m.vertices[0].color.g,128.f/255),"Interleaved/normalized attributes misread");
            check(close(m.vertices[0].color.a,128.f/255)&&close(m.vertices[0].tangent.x,1),"Vertex alpha/generated tangent incorrect");
            const auto worlds=scene_world_transforms(s);check(s.nodes[1].parent==0&&s.nodes[1].mesh==0&&s.nodes[2].mesh==0,"Node/mesh indices disagree");
            check(close(worlds[1][3].x,3)&&close(worlds[1][3].y,2)&&close(worlds[1][3].z,3)&&close(worlds[2][3].x,5),"TRS/matrix hierarchy incorrect");
            const auto& mat=s.materials[0];check(mat.alpha_mode==2&&mat.double_sided&&close(mat.base_color.a,.75f)&&close(mat.alpha_cutoff,.4f),"Material alpha not retained");
            check(mat.base_texture>=0&&mat.mr_texture>=0&&mat.base_texture!=mat.mr_texture,"Color/data texture view aliasing");
            const auto& color=s.textures[mat.base_texture];const auto& data=s.textures[mat.mr_texture];
            check(color.srgb&&!data.srgb&&color.levels.size()==2&&close(color.levels[0].at(0,0).r,128.f/255)&&close(color.levels[0].at(0,0).a,64.f/255),"RGB/sRGB flag/alpha interpretation incorrect");
            check(color.levels[1].at(0,0).r>data.levels[1].at(0,0).r+.04f&&close(color.levels[1].at(0,0).a,159.5f/255),"Mip filter did not average linear RGB/linear alpha");
            check(loaded.samplers.size()==s.textures.size()&&loaded.samplers[0]==TextureSampler{},"Default sampler absent");
            check(load_scene_asset(path).nodes.size()==3,"Simple loader failed default sampler");
        }
    });
    test("C7 glTF: nondefault sampler roundtrip and malformed asset failure",[]{
        TemporaryFiles files;const auto path=gltf_fixture(files.root,false,true);const auto detailed=load_scene_asset_detailed(path);
        check(detailed.samplers[0].wrap_s==33071&&detailed.samplers[0].wrap_t==33648&&detailed.samplers[0].min_filter==9728,"Explicit sampler settings lost");
        const auto simple=load_scene_asset(path);const auto& sampler=simple.textures.at(0);
        check(sampler.wrap_s==Texture::Wrap::clamp_to_edge&&sampler.wrap_t==Texture::Wrap::mirrored_repeat&&sampler.min_filter==Texture::Filter::nearest&&sampler.mag_filter==Texture::Filter::nearest,"Simple loader lost sampler fields");
        ProjectDocument p;p.scene=simple;const auto restored=deserialize_project(serialize_project(p));const auto& restored_sampler=restored.scene.textures.at(0);
        check(restored_sampler.wrap_s==sampler.wrap_s&&restored_sampler.wrap_t==sampler.wrap_t&&restored_sampler.min_filter==sampler.min_filter&&restored_sampler.mag_filter==sampler.mag_filter,"Sampler persistence failed");
        const auto bad=gltf_fixture(files.root,false,false,true);throws([&]{load_scene_asset(bad);},"Out-of-range index accepted");
        write_text(files.root/"invalid.glb","glTF truncated");throws([&]{load_scene_asset(files.root/"invalid.glb");},"Truncated GLB accepted");
        throws([&]{load_scene_asset(files.root/"missing.gltf");},"Missing asset did not fail");
    });
    test("C7/C11 Unicode asset filename and project roundtrip",[]{
        TemporaryFiles files;const auto original=gltf_fixture(files.root,true);
        const auto asset=files.root/u8"\u6A21\u578B \U0001F525.glb";
        std::filesystem::copy_file(original,asset);
        check(load_scene_asset(asset).name=="Fixture scene","Explicit glTF scene name must take priority over its filename");
        // 有显式场景名时不应使用文件名；另测没有场景名的文件，验证 UTF-8 缺省名称。
        const auto unnamed=sparse_fixture(files.root,false);const auto unicode_gltf=files.root/u8"\u6A21\u578B \U0001F525.gltf";
        std::filesystem::copy_file(unnamed,unicode_gltf);
        auto scene=load_scene_asset(unicode_gltf);const auto expected=unicode_gltf.stem().u8string();
        check(scene.name==std::string(reinterpret_cast<const char*>(expected.data()),expected.size()),"Scene name was not preserved as UTF-8");
        const auto project=files.root/u8"\u573A\u666F \U0001F525.ember";
        Camera camera;Settings settings;save_project(project,scene,camera,settings);
        Scene restored;load_project(project,restored,camera,settings);
        check(restored.name==scene.name&&restored.meshes.size()==scene.meshes.size(),"Unicode project roundtrip failed");
    });
    test("C7 glTF: sparse positions and generated flat normals",[]{
        TemporaryFiles files;const auto path=sparse_fixture(files.root,false);const auto scene=load_scene_asset(path);const auto& m=scene.meshes.at(0);
        check(m.vertices.size()==3&&m.indices==std::vector<std::uint32_t>({0,1,2})&&close(m.vertices[1].position.x,1)&&close(m.vertices[2].position.y,1),"Sparse positions not applied");
        for(const auto& v:m.vertices)check(close(v.normal.z,1),"Missing normal was not generated as face normal");
        sparse_fixture(files.root,true);throws([&]{load_scene_asset(path);},"Invalid sparse ordering accepted");
    });
    test("C7 glTF: triangle-strip/fan triangulation and missing-buffer failure",[]{
        TemporaryFiles files;
        for(int mode:{5,6}) {
            std::vector<unsigned char> buffer;
            const std::array<glm::vec3,4> strip={glm::vec3(0,0,0),glm::vec3(1,0,0),glm::vec3(0,1,0),glm::vec3(1,1,0)};
            const std::array<glm::vec3,4> fan={glm::vec3(0,0,0),glm::vec3(1,0,0),glm::vec3(1,1,0),glm::vec3(0,1,0)};
            for(const auto& v:(mode==5?strip:fan))for(int k=0;k<3;++k)f32(buffer,v[k]);write_bytes(files.root/"quad.bin",buffer);
            const auto json=std::string(R"({"asset":{"version":"2.0"},"buffers":[{"uri":"quad.bin","byteLength":48}],"bufferViews":[{"buffer":0,"byteLength":48}],"accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],"meshes":[{"primitives":[{"mode":)")+std::to_string(mode)+R"(,"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}]})";
            const auto path=files.root/"quad.gltf";write_text(path,json);const auto scene=load_scene_asset(path);const auto& mesh=scene.meshes.at(0);
            check(mesh.indices.size()==6&&mesh.vertices.size()==6,"Strip/fan did not yield two flat triangles");float area=0;
            for(std::size_t i=0;i<6;i+=3){const auto a=mesh.vertices[mesh.indices[i]].position,b=mesh.vertices[mesh.indices[i+1]].position,c=mesh.vertices[mesh.indices[i+2]].position;area+=glm::cross(b-a,c-a).z*.5f;}
            check(close(area,1),"Strip/fan winding or area incorrect");
        }
        std::filesystem::remove(files.root/"quad.bin");throws([&]{load_scene_asset(files.root/"quad.gltf");},"Missing external buffer silently substituted");
    });
    test("C7 glTF: mirrored UV splits, handedness and primitive isolation",[]{
        TemporaryFiles files;std::vector<Vertex> vertices(4);
        vertices[0].position={0,0,0};vertices[1].position={1,0,0};vertices[2].position={0,1,0};vertices[3].position={-1,0,0};
        vertices[0].uv={0,0};vertices[1].uv=vertices[3].uv={1,0};vertices[2].uv={0,1};
        for(auto& v:vertices)v.normal={0,0,1};
        const auto scene=load_scene_asset(frame_fixture(files.root,vertices,{0,1,2,0,2,3},true,false,true));const auto& mesh=scene.meshes.at(0);
        check(mesh.vertices.size()==12 && mesh.indices.size()==12 && mesh.primitives.size()==2,"Mirrored seam expansion/primitive count incorrect");
        for(std::size_t p=0;p<2;++p) {
            const auto first=p*6;check(mesh.primitives[p].first_index==first && mesh.primitives[p].index_count==6,"Tangent split changed primitive ranges");
            check(mesh.indices[first]!=mesh.indices[first+3] && mesh.indices[first+2]!=mesh.indices[first+4],"Opposite handedness still shares vertices");
            for(std::size_t k=0;k<6;++k) {
                const auto& v=mesh.vertices[mesh.indices[first+k]];check_frame(v);
                const auto& source=vertices[std::array<std::size_t,6>{0,1,2,0,2,3}[k]];
                check(v.position==source.position && v.uv==source.uv && v.normal==source.normal,"Tangent split lost vertex attributes");
                const float sign=k<3 ? 1.f:-1.f;
                check(close(v.tangent.x,sign)&&v.tangent.w==sign,"Mirrored tangent cancelled or handedness flipped");
                check(glm::length(glm::cross(v.normal,glm::vec3(v.tangent))*v.tangent.w-glm::vec3(0,1,0))<1e-5f,"Reconstructed bitangent opposes increasing V");
                if(p)check(mesh.indices[first+k]>=6,"Later primitive overwrote earlier primitive vertices");
            }
        }
    });
    test("C7 glTF: scale independent normals, tiny UVs and degenerate UV fallback",[]{
        TemporaryFiles files;
        for(float scale:{1e-12f,1.f,1e20f})for(float uv_scale:{1e-10f,1.f,1e10f})for(bool authored_normals:{false,true}) {
            std::vector<Vertex> vertices(3);vertices[1].position={scale,0,0};vertices[2].position={0,scale,0};
            vertices[1].uv={uv_scale,0};vertices[2].uv={0,uv_scale};
            for(auto& v:vertices)v.normal={0,0,scale};
            const auto scene=load_scene_asset(frame_fixture(files.root,vertices,{0,1,2},authored_normals));
            for(const auto& v:scene.meshes.at(0).vertices) {
                check_frame(v);check(close(v.normal.z,1)&&close(v.tangent.x,1)&&v.tangent.w==1,"Model/UV scale changed tangent frame");
            }
        }
        std::vector<Vertex> vertices(3);vertices[1].position={1,0,0};vertices[2].position={0,1,0};
        for(auto& v:vertices)v.normal=glm::normalize(glm::vec3(1,1,1));
        const auto scene=load_scene_asset(frame_fixture(files.root,vertices,{0,1,2}));
        for(const auto& v:scene.meshes.at(0).vertices)check_frame(v);
        check(scene.meshes[0].vertices.size()==3,"Degenerate UV needlessly expanded topology");
    });
    test("C7 glTF: authored tangent conditioning and malformed frame rejection",[]{
        TemporaryFiles files;std::vector<Vertex> vertices(3);
        vertices[1].position={1,0,0};vertices[2].position={0,1,0};vertices[1].uv={1,0};vertices[2].uv={0,1};
        for(auto& v:vertices){v.normal={0,0,1e30f};v.tangent={1e30f,0,1e30f,-1};}
        const auto scene=load_scene_asset(frame_fixture(files.root,vertices,{0,1,2},true,true));
        for(const auto& v:scene.meshes.at(0).vertices){check_frame(v);check(close(v.tangent.x,1)&&v.tangent.w==-1,"Authored tangent direction/sign was lost");}
        for(auto& v:vertices)v.tangent={0,1,0,-1};
        const auto flat=load_scene_asset(frame_fixture(files.root,vertices,{0,1,2},false,true));
        for(const auto& v:flat.meshes.at(0).vertices){check_frame(v);check(close(v.normal.z,1)&&close(v.tangent.x,1)&&v.tangent.w==1,"Tangents without source normals were not regenerated");}
        for(auto& v:vertices){v.normal={0,0,1};v.tangent={1,0,0,1};}
        vertices[0].normal={0,0,0};auto path=frame_fixture(files.root,vertices,{0,1,2},true,true);
        throws([&]{load_scene_asset(path);},"Zero normal was silently replaced");vertices[0].normal={0,0,1};
        for(auto tangent:{glm::vec4(0,0,0,1),glm::vec4(1,0,0,0),glm::vec4(1,0,0,2)}) {
            vertices[0].tangent=tangent;path=frame_fixture(files.root,vertices,{0,1,2},true,true);
            throws([&]{load_scene_asset(path);},"Invalid authored tangent was accepted");
        }
    });
    test("C11 scenes: sphere budget, exact seam, sector poles and UV frame orientation",[]{
        const auto scene=make_demo_scene(0);const auto& mesh=scene.meshes.at(1);
        check(mesh.name=="UV sphere","Expected demo sphere");
        check(mesh.vertices.size()==6303 && mesh.indices.size()/3==12096,"Default sphere subdivision budget changed");
        for(const auto& v:mesh.vertices) {
            check_frame(v);check(close(glm::length(v.position),.5f),"Sphere radius changed");
            check(v.uv.x>=0 && v.uv.x<=1 && v.uv.y>=0 && v.uv.y<=1,"Sphere UV outside atlas");
        }
        for(int y=0;y<63;++y) {
            const auto& a=mesh.vertices[std::size_t(y)*97];const auto& b=mesh.vertices[std::size_t(y)*97+96];
            check(a.position==b.position && a.normal==b.normal && a.tangent==b.tangent,"Sphere seam frame is not bit identical");
            check(a.uv.x==0 && b.uv.x==1 && a.uv.y==b.uv.y,"Sphere UV seam was welded");
        }
        std::size_t poles=0;std::vector<bool> used(mesh.vertices.size(),false);
        for(std::size_t i=0;i<mesh.indices.size();i+=3) {
            const auto& a=mesh.vertices[mesh.indices[i]];const auto& b=mesh.vertices[mesh.indices[i+1]];const auto& c=mesh.vertices[mesh.indices[i+2]];
            for(int k=0;k<3;++k)used[mesh.indices[i+std::size_t(k)]]=true;
            const auto e1=glm::dvec3(b.position)-glm::dvec3(a.position),e2=glm::dvec3(c.position)-glm::dvec3(a.position);
            check(glm::dot(glm::cross(e1,e2),glm::dvec3(a.normal+b.normal+c.normal))>0,"Sphere triangle is degenerate or inward");
            const auto d1=glm::dvec2(b.uv)-glm::dvec2(a.uv),d2=glm::dvec2(c.uv)-glm::dvec2(a.uv);
            const double determinant=d1.x*d2.y-d1.y*d2.x;check(std::abs(determinant)>0,"Sphere UV triangle collapsed");
            const auto tangent=(e1*d2.y-e2*d1.y)/determinant,bitangent=(e2*d1.x-e1*d2.x)/determinant;
            const float min_u=std::min({a.uv.x,b.uv.x,c.uv.x}),max_u=std::max({a.uv.x,b.uv.x,c.uv.x});
            check(max_u-min_u<=1.f/96+1e-6f,"Sphere triangle crosses UV wrap");
            for(const auto* v:{&a,&b,&c}) {
                check(glm::dot(glm::dvec3(v->tangent),tangent)>0,"Sphere tangent opposes increasing U");
                check(glm::dot(glm::dvec3(glm::cross(v->normal,glm::vec3(v->tangent))*v->tangent.w),bitangent)>0,"Sphere handedness opposes increasing V");
                if(v->uv.y==0 || v->uv.y==1) {
                    ++poles;check(v->position==glm::vec3(0,v->uv.y==0 ? .5f:-.5f,0),"Pole is a small inverted ring");
                    check(close(v->uv.x,(min_u+max_u)*.5f),"Pole U is not centered in its sector");
                    const double phi=2*std::acos(-1.0)*v->uv.x;
                    check(glm::dot(glm::dvec3(v->tangent),glm::dvec3(-std::sin(phi),0,std::cos(phi)))>1-1e-6,"Pole tangent is not sector centered");
                }
            }
        }
        check(poles==192 && std::all_of(used.begin(),used.end(),[](bool value){return value;}),"Sphere contains unused or shared pole sectors");
    });
    test("C11 scenes: presets, finite geometry, winding and rectangle radiance",[]{
        for(int preset=0;preset<4;++preset) {
            auto s=make_demo_scene(preset);check(!s.nodes.empty()&&!s.meshes.empty()&&!s.materials.empty()&&!s.lights.empty(),"Procedural preset is empty");
            // 材质球展示可提高细分，但体积 GI 场景不能因此越过实际 GPU 支持预算。
            if(preset==1||preset==2)check(flatten_scene(s).size()<=4096,"Default volume GI demo exceeds GPU triangle budget");
            ProjectDocument p;p.scene=s;deserialize_project(serialize_project(p));
            for(const auto& mesh:s.meshes)for(std::size_t i=0;i<mesh.indices.size();i+=3) {
                const auto& a=mesh.vertices[mesh.indices[i]];const auto& b=mesh.vertices[mesh.indices[i+1]];const auto& c=mesh.vertices[mesh.indices[i+2]];
                const auto normal=glm::cross(b.position-a.position,c.position-a.position);
                check(glm::dot(normal,a.normal+b.normal+c.normal)>-1e-6f,"Procedural winding opposes normals");
            }
            if(preset==1) {
                check(s.lights.size()==1&&s.lights[0].kind==LightKind::rectangle,"GI room has no rectangle source");
                const auto radiance=s.lights[0].color*s.lights[0].intensity;check(close(radiance.x,12)&&close(radiance.y,11)&&close(radiance.z,9),"Rectangle radiance does not match emissive material");
            }
            if(preset==3)check(s.lights.size()==26,"Many-lights scene is not procedural light grid");
        }
        throws([]{make_demo_scene(4);},"Out-of-range preset accepted");
        Scene cycle;cycle.nodes.resize(2);cycle.nodes[0].parent=1;cycle.nodes[1].parent=0;throws([&]{scene_world_transforms(cycle);},"Scene node cycle accepted");
    });
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_SYSTEMS_TEST_MAIN
int main(int argc,char** argv) {
    const auto results=emberframe::lab::test_systems();int failures=0;
    for(const auto& r:results){std::cout<<(r.passed?"PASS ":"FAIL ")<<r.name<<": "<<r.detail<<'\n';if(!r.passed)++failures;}
    if(argc>1)try {const auto s=emberframe::lab::load_scene_asset(argv[1]);std::cout<<"ASSET "<<s.name<<" meshes="<<s.meshes.size()<<" nodes="<<s.nodes.size()<<" textures="<<s.textures.size()<<'\n';}
    catch(const std::exception& e){std::cout<<"FAIL requested asset: "<<e.what()<<'\n';++failures;}
    return failures?1:0;
}
#endif
