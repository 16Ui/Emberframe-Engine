#include "shader_assets.h"
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>

namespace emberframe::lab {
namespace {
namespace fs=std::filesystem;
void ensure(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
template<class F> void expect_error(F&& f,const char* message){bool caught=false;try{f();}catch(const std::exception&){caught=true;}ensure(caught,message);}
void write_test_file(const fs::path& path,std::string_view text) {
    fs::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(text.data(),std::streamsize(text.size()));file.close();ensure(bool(file),"Cannot write shader test fixture");
}
std::string test_file(const fs::path& path){std::ifstream file(path,std::ios::binary);ensure(bool(file),"Cannot read shader test fixture");return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};}
struct ShaderFixture {
    fs::path root,source,cache;
    explicit ShaderFixture() {
        static std::atomic<unsigned> serial{0};
        for(unsigned attempt=0;attempt<100;++attempt){auto candidate=fs::temp_directory_path()/("emberframe-shader-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial.fetch_add(1)));
            if(fs::create_directory(candidate)){root=std::move(candidate);break;}}
        ensure(!root.empty(),"Cannot create isolated shader test directory");source=root/fs::path(u8"source 空格 & symbols");cache=root/fs::path(u8"cache 空格 & symbols");fs::create_directories(source);
    }
    ~ShaderFixture(){if(!root.empty()){std::error_code ignored;fs::remove_all(root,ignored);}}
    ShaderFixture(const ShaderFixture&)=delete;
    static std::string fragment() {
        return R"(#version 450
#extension GL_GOOGLE_include_directive : require
#include "shared.glslinclude"
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=1,binding=1) uniform sampler2D textures[2];
layout(push_constant,std430) uniform Push { mat4 transform; vec4 offset; } pushData;
void main(){ color=(texture(textures[0],uv)+texture(textures[1],uv))*u.tint*vec4(TINT,1.0)+pushData.offset; }
)";
    }
    ShaderLibraryConfig config(const fs::path& compiler,bool full_fixture=true) const {
        ShaderLibraryConfig c;c.source_dir=source;c.cache_dir=cache;c.glslang=compiler;
        c.sources={{"quad.vert",ShaderStage::vertex},{"shade & look.frag",ShaderStage::fragment},{"bins/work.comp",ShaderStage::compute}};
        if(full_fixture) {
            write_test_file(source/"nested/colors.glslinclude","const vec3 TINT=vec3(0.2,0.4,0.6);\n");
            write_test_file(source/"shared.glslinclude",R"(#ifndef TEST_SHARED
#define TEST_SHARED
// #include "ignored_comment.glsl"
/* #include "also_ignored.glsl" */
#include "nested/colors.glslinclude"
layout(std140,set=0,binding=0) uniform Params { vec4 tint; } u;
#endif
)");
            write_test_file(source/"quad.vert",R"(#version 450
#extension GL_GOOGLE_include_directive : require
#include "shared.glslinclude"
layout(location=0) out vec2 uv;
layout(push_constant,std430) uniform Push { mat4 transform; vec4 offset; } pushData;
void main(){ uv=vec2(float(gl_VertexIndex&1),float((gl_VertexIndex>>1)&1)); gl_Position=pushData.transform*vec4(uv*2-1,0,1)+pushData.offset*u.tint.x; }
)");
            write_test_file(source/"shade & look.frag",fragment());
            write_test_file(source/"bins/work.comp",R"(#version 450
#extension GL_GOOGLE_include_directive : require
#include "../shared.glslinclude"
layout(local_size_x=1) in;
layout(std430,set=0,binding=2) buffer Output { float values[]; } outputData;
void main(){ outputData.values[gl_GlobalInvocationID.x]=u.tint.x*TINT.x; }
)");
        }
        return c;
    }
};
std::string shader_test_environment(const char* name) {
#ifdef _MSC_VER
    char* data=nullptr;std::size_t size=0;
    if(_dupenv_s(&data,&size,name)!=0)return {};
    std::unique_ptr<char,decltype(&std::free)> owned(data,std::free);
    return data?std::string(data):std::string{};
#else
    const auto* data=std::getenv(name);return data?std::string(data):std::string{};
#endif
}
fs::path compiler_path(const fs::path& supplied) {
    if(!supplied.empty())return supplied;
#ifdef EMBERFRAME_LAB_GLSLANG
    if(fs::is_regular_file(fs::path(EMBERFRAME_LAB_GLSLANG)))return fs::path(EMBERFRAME_LAB_GLSLANG);
#endif
    const auto configured=shader_test_environment("EMBERFRAME_LAB_GLSLANG");if(!configured.empty())return fs::path(configured);
    const auto sdk=shader_test_environment("VULKAN_SDK");if(!sdk.empty()) {
#ifdef _WIN32
        return fs::path(sdk)/"Bin/glslangValidator.exe";
#else
        return fs::path(sdk)/"bin/glslangValidator";
#endif
    }
    return {};
}
void good_build(const ShaderBuildResult& result) {
    std::string detail=result.error;
    if(!result.success)for(const auto& d:result.diagnostics)detail+="\n"+d.stdout_text+"\n"+d.stderr_text;
    ensure(result.success&&result.version,"Real glslang build failed: "+detail);
}
const CompiledShader& named_shader(const ShaderVersion& version,const fs::path& path) {
    for(const auto& shader:version.shaders)if(shader.source.file==path)return shader;throw std::runtime_error("Compiled shader record missing");
}
const ShaderDescriptor& binding(const SpirvReflection& r,std::uint32_t set,std::uint32_t id) {
    for(const auto& d:r.descriptors)if(d.set==set&&d.binding==id)return d;throw std::runtime_error("Reflected descriptor missing");
}
}

TestResults test_shader_assets(const fs::path& supplied_compiler) {
    TestResults results;
    auto test=[&](std::string name,auto&& fn){TestResult result;result.name=std::move(name);
#ifdef EMBERFRAME_SHADER_ASSETS_TEST_MAIN
        std::cerr<<"RUN "<<result.name<<std::endl;
#endif
        try{fn();result.passed=true;result.detail="All deterministic assertions passed";}catch(const std::exception& error){result.detail=error.what();}catch(...){result.detail="Non-standard exception";}
#ifdef EMBERFRAME_SHADER_ASSETS_TEST_MAIN
        std::cerr<<(result.passed?"OK ":"ERROR ")<<result.detail<<std::endl;
#endif
        results.push_back(std::move(result));};
    test("Shader assets: SHA-256 known vectors and exact workbench shader set",[]{
        ensure(shader_content_hash("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","Wrong SHA-256 empty vector");
        ensure(shader_content_hash("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","Wrong SHA-256 abc vector");
        ensure(shader_content_hash(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","Wrong SHA-256 multi-block vector");
        const std::vector<std::string> expected={"mesh.vert","fullscreen.vert","forward.frag","gbuffer.frag","shadow.frag","sky.frag","deferred.frag","post.frag","cull.comp","object_cull.comp","gpu_shadow.vert","gpu_shadow_check.comp","gpu_shadow_moments.comp","gpu_shadow_sat.comp","effects_bloom.comp","effects_compose.comp","effects_debug.comp","effects_filter.comp","effects_npr.comp","effects_occlusion.comp","effects_snapshot.comp","effects_temporal.comp","effects_trace.comp","effects_copy.comp","volume_inject.comp","volume_mip.comp","volume_propagate.comp","volume_rsm.comp","volume_trace.comp","volume_voxelize.comp","volume_sparse.comp"};
        const auto actual=workbench_shader_sources();ensure(actual.size()==expected.size(),"VULKAN_BUILD shader list size changed");
        for(std::size_t i=0;i<actual.size();++i)ensure(actual[i].file.generic_string()==expected[i],"VULKAN_BUILD shader list differs");
    });
    const auto compiler=compiler_path(supplied_compiler);
    if(compiler.empty()||!fs::is_regular_file(compiler)) {
        results.push_back({"Shader assets: required real glslang compiler",false,"Supply existing glslang path, define EMBERFRAME_LAB_GLSLANG, or set VULKAN_SDK; actual compilation was not run."});return results;
    }
    test("Shader assets: real vertex/fragment/compute compile, descriptors, logs and cache hit",[&]{
        ShaderFixture fixture;ShaderLibrary library(fixture.config(compiler));const auto first=library.build();good_build(first);
        ensure(!first.cache_hit&&first.compiled_files==3&&first.version->shaders.size()==3,"First build did not compile all stages");
        ensure(!library.active(),"Candidate build switched active GPU version");library.activate(first.version);
        const auto& fragment=named_shader(*first.version,"shade & look.frag");const auto& r=fragment.reflection;
        ensure(r.complete&&r.entry_points.size()==1&&r.entry_points[0].name=="main"&&r.entry_points[0].stage==ShaderStage::fragment,"Entry-point reflection incorrect");
        ensure(binding(r,0,0).kind==ShaderDescriptorKind::uniform_buffer&&binding(r,0,0).minimum_buffer_bytes==16,"UBO reflection incorrect");
        ensure(binding(r,1,1).kind==ShaderDescriptorKind::combined_image_sampler&&binding(r,1,1).count==2,"Sampler array reflection incorrect");
        ensure(r.push_constants.size()==1&&r.push_constants[0].offset==0&&r.push_constants[0].size==80,"Push constant mat4+vec4 range incorrect");
        const auto& compute=named_shader(*first.version,"bins/work.comp");ensure(binding(compute.reflection,0,2).kind==ShaderDescriptorKind::storage_buffer&&compute.reflection.complete,"Runtime data array confused with runtime descriptor array");
        for(const auto& d:first.diagnostics){ensure(d.exit_code==0&&fs::is_regular_file(d.stdout_file)&&fs::is_regular_file(d.stderr_file),"Compiler stdout/stderr not retained");}
        const auto second=library.build();good_build(second);ensure(second.cache_hit&&second.compiled_files==0&&second.version==first.version,"Identical contents missed stable version/cache identity");
        library.activate(first.version);ensure(library.active()==first.version,"Cache-hit validation invalidated known version handle");
        ShaderLibrary restored(fixture.config(compiler,false));const auto persisted=restored.build();good_build(persisted);
        ensure(persisted.cache_hit&&persisted.compiled_files==0&&persisted.version->id==first.version->id,"Cache was not reusable across library instances");
    });
    test("Shader assets: broken shader preserves old SPV, diagnostics, include hot reload and rollback",[&]{
        ShaderFixture fixture;ShaderLibrary library(fixture.config(compiler));const auto first=library.build();good_build(first);library.activate(first.version);
        const auto old_file=named_shader(*first.version,"shade & look.frag").spirv_file;const auto old_bytes=test_file(old_file);
        write_test_file(fixture.source/"shade & look.frag",ShaderFixture::fragment()+"\nthis is a deliberate syntax error;\n");
        const auto broken=library.build();ensure(!broken.success&&!broken.version&&library.active()==first.version,"Broken compilation changed active version");
        ensure(test_file(old_file)==old_bytes,"Failed build overwrote previously valid SPV");bool actual_error=false;
        for(const auto& d:broken.diagnostics){ensure(fs::is_regular_file(d.stdout_file)&&fs::is_regular_file(d.stderr_file),"Failed compiler logs were lost during version relocation");
            if(d.source=="shade & look.frag"&&d.exit_code!=0&&(d.stdout_text.find("ERROR")!=std::string::npos||d.stderr_text.find("ERROR")!=std::string::npos))actual_error=true;}
        ensure(actual_error,"Broken GLSL was not actually compiled/reported by glslang");
        ensure(broken.log_directory.parent_path().parent_path().filename()=="failures","Failed attempt not retained separately");
        write_test_file(fixture.source/"shade & look.frag",ShaderFixture::fragment());const auto recovered=library.build();good_build(recovered);ensure(recovered.cache_hit&&recovered.version==first.version,"Restoring source did not recover old immutable cache version");
        write_test_file(fixture.source/"nested/colors.glslinclude","const vec3 TINT=vec3(0.8,0.3,0.1);\n");
        const auto second=library.build();good_build(second);ensure(!second.cache_hit&&second.version->id!=first.version->id&&second.compatibility.compatible,"Transitive glslinclude edit not recompiled compatibly");
        ensure(library.active()==first.version,"Successful candidate auto-activated before GPU accepted it");library.activate(second.version);
        ensure(library.previous()==first.version&&test_file(old_file)==old_bytes,"New version destroyed rollback data");
        library.activate(library.previous());ensure(library.active()==first.version&&library.previous()==second.version,"Explicit metadata rollback failed");
    });
    test("Shader assets: options/compiler identity and conservative include hashes",[&]{
        ShaderFixture fixture;auto config=fixture.config(compiler);ShaderLibrary base(config);const auto first=base.build();good_build(first);
        config.options.defines={{"FIXTURE_VARIANT","2"}};ShaderLibrary option_change(config);const auto changed=option_change.build();good_build(changed);
        ensure(!changed.cache_hit&&changed.version->id!=first.version->id&&changed.version->compiler_hash==first.version->compiler_hash,"Compiler options are missing from cache key");
        const auto binary_hash=shader_content_hash(test_file(compiler));ensure(changed.version->compiler_hash==binary_hash,"Compiler identity is not based on actual executable contents");
        write_test_file(fixture.source/"unused.glslinclude","// Conservatively invalidated future include\n");
        const auto include=base.build();good_build(include);ensure(!include.cache_hit&&include.version->source_hash!=first.version->source_hash,"glslinclude contents missing from source fingerprint");
    });
    test("Shader assets: descriptor and buffer-layout edits block incompatible activation",[&]{
        ShaderFixture fixture;ShaderLibrary library(fixture.config(compiler));const auto first=library.build();good_build(first);library.activate(first.version);
        auto text=ShaderFixture::fragment();const auto at=text.find("binding=1");ensure(at!=std::string::npos,"Fixture binding token absent");text.replace(at,9,"binding=5");write_test_file(fixture.source/"shade & look.frag",text);
        const auto binding_change=library.build();good_build(binding_change);ensure(!binding_change.compatibility.compatible&&!binding_change.compatibility.reasons.empty(),"Changed binding was considered compatible");
        expect_error([&]{library.activate(binding_change.version);},"Incompatible shader was activated");ensure(library.active()==first.version,"Rejected activation modified active version");
        write_test_file(fixture.source/"shade & look.frag",ShaderFixture::fragment());auto shared=test_file(fixture.source/"shared.glslinclude");const auto member=shared.find("vec4 tint;");shared.replace(member,10,"vec4 padding; vec4 tint;");write_test_file(fixture.source/"shared.glslinclude",shared);
        const auto block_change=library.build();good_build(block_change);ensure(!block_change.compatibility.compatible,"Changed UBO member offsets escaped compatibility check");
        const auto& modified=named_shader(*block_change.version,"shade & look.frag").reflection;ensure(binding(modified,0,0).minimum_buffer_bytes==32,"Reflected expanded UBO size incorrect");
    });
    test("Shader assets: structural SPIR-V rejection and cache corruption detection",[&]{
        ShaderFixture fixture;ShaderLibrary library(fixture.config(compiler));const auto first=library.build();good_build(first);library.activate(first.version);
        const auto path=named_shader(*first.version,"shade & look.frag").spirv_file;const auto module=read_spirv(path,ShaderStage::fragment);auto bad=module.words;
        bad[0]=0;expect_error([&]{reflect_spirv(bad,ShaderStage::fragment);},"Invalid magic accepted");bad=module.words;bad[5]&=65535;
        expect_error([&]{reflect_spirv(bad,ShaderStage::fragment);},"Zero instruction length accepted");
        expect_error([&]{reflect_spirv(module.words,ShaderStage::vertex);},"Wrong entry-point execution model accepted");
        expect_error([&]{reflect_spirv(module.words,ShaderStage::fragment,"not_main");},"Missing entry-point name accepted");
        write_test_file(fixture.root/"unaligned.spv",test_file(path)+"x");expect_error([&]{read_spirv(fixture.root/"unaligned.spv",ShaderStage::fragment);},"Non-word-aligned SPIR-V accepted");
        // Only this temporary test cache is intentionally corrupted; real assets are never edited.
        auto bytes=test_file(path);bytes[0]=0;write_test_file(path,bytes);const auto corrupt=library.build();
        ensure(!corrupt.success&&!corrupt.cache_hit&&corrupt.error.find("checksum")!=std::string::npos,"Corrupt cached module was silently reused");
        ensure(library.active()==first.version&&test_file(path)==bytes,"Corrupt cache was overwritten or active metadata replaced");
    });
    test("Shader assets: runtime descriptor array marks reflection incomplete",[&]{
        ShaderFixture fixture;auto config=fixture.config(compiler,false);config.sources={{"runtime.frag",ShaderStage::fragment}};
        write_test_file(fixture.source/"runtime.frag",R"(#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set=0,binding=0) uniform sampler2D textures[];
layout(location=0) out vec4 color;
void main(){color=texture(textures[nonuniformEXT(int(gl_FragCoord.x))],vec2(0.5));}
)");
        ShaderLibrary library(config);const auto result=library.build();good_build(result);const auto& reflection=result.version->shaders[0].reflection;
        ensure(!reflection.complete&&binding(reflection,0,0).runtime_array&&!result.compatibility.compatible,"Unsupported descriptor array was falsely marked compatible");
        expect_error([&]{library.activate(result.version);},"Incomplete reflection was activated");ensure(!library.active(),"Incomplete candidate changed active metadata");
    });
    test("Shader assets: async cache serialization and baseline adoption",[&]{
        ShaderFixture fixture;const auto config=fixture.config(compiler);ShaderLibrary first(config),second(config);
        auto a=std::async(std::launch::async,[&]{return first.build();});auto b=std::async(std::launch::async,[&]{return second.build();});
        const auto ar=a.get(),br=b.get();good_build(ar);good_build(br);
        ensure(ar.version->id==br.version->id&&ar.cache_hit!=br.cache_hit&&ar.compiled_files+br.compiled_files==3,"Concurrent cache writers did not serialize/deduplicate");
        ShaderLibrary baseline(config);baseline.adopt_baseline(ar.version->directory);const auto active=baseline.active();ensure(active&&active->id.starts_with("baseline-"),"CMake-style external directory was not adopted");
        const auto candidate=baseline.build();good_build(candidate);ensure(candidate.compatibility.compatible,"Equivalent external baseline layout was rejected");
        baseline.activate(candidate.version);ensure(baseline.previous()==active,"Initial running shaders were not retained for rollback");
        expect_error([&]{baseline.activate(ar.version);},"Foreign library's candidate handle accepted");
    });
    test("Shader assets: input boundaries, missing include/compiler and output budget",[&]{
        ShaderFixture fixture;auto config=fixture.config(compiler);ShaderLibrary library(config);const auto first=library.build();good_build(first);library.activate(first.version);
        write_test_file(fixture.source/"nested/colors.glslinclude","#include \"../../outside.glslinclude\"\n");
        const auto outside=library.build();ensure(!outside.success&&outside.compiled_files==0&&library.active()==first.version,"Escaping include was accepted");
        write_test_file(fixture.source/"nested/colors.glslinclude","#include \"missing.glslinclude\"\n");const auto missing=library.build();ensure(!missing.success&&missing.error.find("not found")!=std::string::npos,"Missing include not diagnosed");
        config.sources[0].file="../escape.vert";expect_error([&]{ShaderLibrary invalid(config);},"Traversal source path accepted");
        config=fixture.config(compiler);config.glslang=fixture.root/"missing-compiler.exe";ShaderLibrary absent(config);const auto no_compiler=absent.build();ensure(!no_compiler.success&&no_compiler.compiled_files==0&&no_compiler.error.find("missing")!=std::string::npos,"Missing compiler produced fake success");
        config=fixture.config(compiler);config.cache_dir=fixture.root/"bounded-cache";config.max_log_bytes=1;ShaderLibrary bounded(config);bounded.adopt_baseline(first.version->directory);
        const auto before=bounded.active();const auto noisy=bounded.build();ensure(!noisy.success&&bounded.active()==before,"Compiler output budget did not preserve baseline");
        ensure(!noisy.diagnostics.empty()&&noisy.diagnostics[0].output_limit_exceeded,"Output limit failure not explicitly reported");
    });
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_SHADER_ASSETS_TEST_MAIN
int main(int argc,char** argv) {
    const auto results=emberframe::lab::test_shader_assets(argc>1?std::filesystem::path(argv[1]):std::filesystem::path{});int failures=0;
    for(const auto& r:results){std::cout<<(r.passed?"PASS ":"FAIL ")<<r.name<<": "<<r.detail<<'\n';if(!r.passed)++failures;}
    if(argc>2)try {
        const auto cache=std::filesystem::temp_directory_path()/("emberframe-workbench-shader-validation-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        emberframe::lab::ShaderLibrary library(argv[2],cache,argv[1]);const auto build=library.build();
        if(!build.success){std::cerr<<"FAIL actual VULKAN_BUILD bundle: "<<build.error<<'\n';for(const auto& d:build.diagnostics)std::cerr<<d.stdout_text<<d.stderr_text;++failures;}
        else {library.activate(build.version);std::cout<<"WORKBENCH compiled="<<build.compiled_files<<" version="<<build.version->id<<" directory="<<build.version->directory.string()<<'\n';}
    }catch(const std::exception& error){std::cerr<<"FAIL actual VULKAN_BUILD bundle: "<<error.what()<<'\n';++failures;}
    return failures?1:0;
}
#endif
