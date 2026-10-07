#pragma once
#include "types.h"
#include <chrono>
#include <memory>
#include <span>
#include <string_view>

namespace emberframe::lab {

enum class ShaderStage : std::uint32_t { vertex=0, tess_control=1, tess_evaluation=2, geometry=3, fragment=4, compute=5 };
enum class ShaderDescriptorKind : std::uint32_t {
    sampler=0, combined_image_sampler=1, sampled_image=2, storage_image=3,
    uniform_texel_buffer=4, storage_texel_buffer=5, uniform_buffer=6,
    storage_buffer=7, input_attachment=10, acceleration_structure=1000150000, unknown=0xffffffffu
};
struct ShaderEntryPoint { std::string name; ShaderStage stage=ShaderStage::vertex; std::uint32_t id=0; };
struct ShaderDescriptor {
    std::uint32_t set=0,binding=0,count=1;
    ShaderDescriptorKind kind=ShaderDescriptorKind::unknown;
    bool runtime_array=false;
    std::string name,type_layout_hash; // Structural types/offsets/strides; excludes debug names and IDs.
    std::uint32_t minimum_buffer_bytes=0;
};
struct ShaderPushConstant {
    std::uint32_t offset=0,size=0;
    std::string type_layout_hash;
};
struct SpirvReflection {
    std::vector<ShaderEntryPoint> entry_points;
    std::vector<ShaderDescriptor> descriptors; // Sorted by set, binding.
    std::vector<ShaderPushConstant> push_constants;
    bool complete=true;
    std::vector<std::string> limitations;
};
struct SpirvModule { std::vector<std::uint32_t> words; SpirvReflection reflection; };

// SHA-256 of bytes, also useful for integration cache keys. Not std::hash.
std::string shader_content_hash(std::string_view bytes);
// Validates byte alignment, magic/version/header, instruction lengths and an actual
// function-backed entry point of the requested stage. This is structural validation,
// not a replacement for spirv-val/device validation. Reflection supports ordinary
// UBO/SSBO, samplers/images, fixed descriptor arrays and explicit push-constant layouts.
// Runtime/spec-constant descriptor arrays and decoration groups are marked incomplete;
// compatibility then fails closed. All declared resources are reflected conservatively,
// including unused declarations. Stage IO linking remains the GPU pipeline's responsibility.
SpirvModule read_spirv(const std::filesystem::path&, ShaderStage, std::string_view entry_point="main");
SpirvReflection reflect_spirv(std::span<const std::uint32_t>, ShaderStage, std::string_view entry_point="main");

struct ShaderSource {
    std::filesystem::path file; // Relative to source_dir; output is <file>.spv. The source/ prefix is reserved.
    ShaderStage stage=ShaderStage::vertex;
    std::string entry_point="main";
};
// Exact current VULKAN_BUILD.cmake shader list, including object_cull.comp.
std::vector<ShaderSource> workbench_shader_sources();
struct ShaderCompileOptions {
    std::string vulkan_target="vulkan1.1",spirv_target="spirv1.3";
    std::vector<std::pair<std::string,std::string>> defines;
    bool debug_info=false,optimize_size=false,disable_optimization=false;
};
struct ShaderLibraryConfig {
    std::filesystem::path source_dir,cache_dir,glslang;
    std::vector<ShaderSource> sources=workbench_shader_sources();
    ShaderCompileOptions options;
    std::chrono::milliseconds compiler_timeout{30000};
    std::size_t max_log_bytes=16*1024*1024;
};
struct ShaderDiagnostic {
    std::filesystem::path source,stdout_file,stderr_file;
    std::string stdout_text,stderr_text; // First 1 MiB each; complete output is retained in the files.
    int exit_code=-1;
    bool timed_out=false,output_limit_exceeded=false;
    double elapsed_ms=0;
};
struct CompiledShader {
    ShaderSource source;
    std::filesystem::path spirv_file;
    std::string spirv_hash;
    SpirvReflection reflection;
};
struct ShaderVersion {
    std::string id,source_hash,compiler_hash;
    std::filesystem::path directory; // Pass this directly to VulkanWorkbench::reload_pipelines().
    std::vector<CompiledShader> shaders;
};
struct ShaderCompatibility {
    bool compatible=true;
    std::vector<std::string> reasons;
};
ShaderCompatibility compare_shader_layouts(const SpirvReflection&,const SpirvReflection&);
ShaderCompatibility compare_shader_versions(const ShaderVersion&,const ShaderVersion&);
struct ShaderBuildResult {
    bool success=false,cache_hit=false;
    std::shared_ptr<const ShaderVersion> version;
    ShaderCompatibility compatibility; // Compared with active() at build completion, when present.
    std::vector<ShaderDiagnostic> diagnostics;
    std::filesystem::path log_directory;
    std::string error;
    std::size_t compiled_files=0;
};

// Real glslang process, never a shell. Windows uses CREATE_NO_WINDOW + SW_HIDE,
// explicit executable, correctly quoted argv and only three inherited standard handles.
// Compiler timeout/output limits terminate the process; stdout/stderr are always retained.
// No tool download/discovery is performed: the executable path must be supplied.
//
// build() is synchronous and may run in an existing JobSystem/std::async worker. Builds
// are serialized per instance AND cache directory; active()/previous() do not wait on
// compilation. Keep the library alive until its callers finish. No GPU calls are made.
//
// Every build snapshots selected sources + literal transitive includes + all .glsl and
// .glslinclude files below source_dir, hashes their contents and the compiler binary,
// version output and options. Includes must remain within source_dir; macro includes
// and symlinks are rejected. Inactive literal includes are conservatively snapshotted.
// Shader/include edits during compilation affect the next build, not this snapshot.
//
// Successful bundles are immutable cache_dir/versions/<sha256>. Failed attempts and logs
// remain under cache_dir/failures. No published SPV is overwritten or automatically GC'd.
// Corrupt cached bundles fail validation and are not silently accepted or overwritten.
// Cache retention is caller-managed; source snapshots are limited to 64 MiB/4096 files.
class ShaderLibrary {
public:
    explicit ShaderLibrary(ShaderLibraryConfig);
    ShaderLibrary(std::filesystem::path source_dir,std::filesystem::path cache_dir,std::filesystem::path glslang);
    ~ShaderLibrary();
    ShaderLibrary(const ShaderLibrary&)=delete;
    ShaderLibrary& operator=(const ShaderLibrary&)=delete;
    ShaderBuildResult build(); // Compile/check candidate ONLY; failure never changes active().
    // Optional: register the initially CMake-built directory before the first reload,
    // so the first candidate is checked against the running pipeline's actual shaders.
    void adopt_baseline(const std::filesystem::path& shader_dir);
    std::shared_ptr<const ShaderVersion> active() const;
    std::shared_ptr<const ShaderVersion> previous() const;
    // Main-thread commit AFTER GPU reload_pipelines(version->directory) succeeds.
    // Only this library's known versions are accepted. Incompatible/incomplete layouts
    // throw and retain the active version. To roll back, reload previous()->directory
    // on the GPU first, then activate(previous). Activation itself never mutates files.
    void activate(const std::shared_ptr<const ShaderVersion>&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Runtime fixtures exclusively under temp_directory_path(). The compiler must exist;
// unavailable tools produce a FAILED test, never fake success. Empty argument checks
// EMBERFRAME_LAB_GLSLANG / VULKAN_SDK, otherwise fails with a precise diagnostic.
TestResults test_shader_assets(const std::filesystem::path& glslang={});
} // namespace emberframe::lab
