#pragma once
#include "types.h"
#include <vulkan/vulkan.h>
#include <filesystem>
#include <string>

namespace emberframe::lab {
// Native driver pipeline cache, not a SPIR-V content cache. Main-thread only: calls
// using handle(), save() and destruction must be externally serialized. The device
// outlives this object. Corrupt/incompatible files are ignored; Vulkan allocation
// failures still propagate. Save is best-effort and atomic; failure preserves old data.
// Envelope checks device/driver/UUID/ABI and a checksum (not a security signature).
// Bounded to 64 MiB; larger native caches are skipped rather than loaded unboundedly.
class GpuPipelineCache {
public:
    GpuPipelineCache(VkDevice,const VkPhysicalDeviceProperties&,std::filesystem::path);
    ~GpuPipelineCache();
    GpuPipelineCache(const GpuPipelineCache&)=delete;
    GpuPipelineCache& operator=(const GpuPipelineCache&)=delete;
    VkPipelineCache handle() const noexcept{return cache_;}
    bool loaded_from_disk() const noexcept{return loaded_;}
    const std::string& load_status() const noexcept{return status_;}
    bool save(std::string* error=nullptr) const;
    static std::filesystem::path default_path(const std::filesystem::path& executable_directory,const VkPhysicalDeviceProperties&);
private:
    VkDevice device_{};VkPipelineCache cache_{};VkPhysicalDeviceProperties properties_{};
    std::filesystem::path path_;bool loaded_=false;std::string status_;
};
TestResults test_pipeline_cache_format();
// Real Vulkan cache export/reimport and corrupt-file recovery; uses unique temporary
// files inside the supplied directory and removes only those files afterward.
TestResults test_gpu_pipeline_cache(VkDevice,const VkPhysicalDeviceProperties&,const std::filesystem::path& directory,VkPipelineCache seed=VK_NULL_HANDLE);
} // namespace emberframe::lab
