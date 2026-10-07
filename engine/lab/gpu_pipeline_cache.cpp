#include "gpu_pipeline_cache.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace emberframe::lab {
namespace {
constexpr std::size_t header_size=64,max_payload=64*1024*1024;
constexpr char magic[8]={'E','F','P','C','C','0','0','1'};
std::uint64_t get(std::span<const std::byte> bytes,std::size_t offset,int count=4){std::uint64_t value=0;for(int i=0;i<count;++i)value|=std::uint64_t(std::to_integer<unsigned char>(bytes[offset+i]))<<(i*8);return value;}
void put(std::span<std::byte> bytes,std::size_t offset,std::uint64_t value,int count=4){for(int i=0;i<count;++i)bytes[offset+i]=std::byte(value>>(i*8));}
std::uint64_t checksum(std::span<const std::byte> bytes){std::uint64_t h=14695981039346656037ull;for(auto b:bytes){h^=std::to_integer<unsigned char>(b);h*=1099511628211ull;}return h;}
bool native_header(std::span<const std::byte> data,const VkPhysicalDeviceProperties& p){
    // Vulkan v1 原生头固定为 32 字节；不得把别的设备或未知版本的数据交给驱动。
    return data.size()>=32&&get(data,0)==32&&get(data,4)==VK_PIPELINE_CACHE_HEADER_VERSION_ONE&&get(data,8)==p.vendorID&&get(data,12)==p.deviceID&&std::memcmp(data.data()+16,p.pipelineCacheUUID,VK_UUID_SIZE)==0;
}
std::vector<std::byte> encode(std::span<const std::byte> payload,const VkPhysicalDeviceProperties& p){
    std::vector<std::byte> bytes(header_size+payload.size());std::memcpy(bytes.data(),magic,8);put(bytes,8,1);put(bytes,12,p.vendorID);put(bytes,16,p.deviceID);put(bytes,20,p.driverVersion);std::memcpy(bytes.data()+24,p.pipelineCacheUUID,VK_UUID_SIZE);put(bytes,40,payload.size(),8);put(bytes,48,checksum(payload),8);put(bytes,56,sizeof(void*));std::memcpy(bytes.data()+header_size,payload.data(),payload.size());return bytes;
}
std::span<const std::byte> decode(std::span<const std::byte> bytes,const VkPhysicalDeviceProperties& p,std::string& reason){
    if(bytes.size()<header_size||bytes.size()>header_size+max_payload){reason="invalid file size";return {};}
    if(std::memcmp(bytes.data(),magic,8)||get(bytes,8)!=1){reason="unknown envelope version";return {};}
    if(get(bytes,12)!=p.vendorID||get(bytes,16)!=p.deviceID||get(bytes,20)!=p.driverVersion||std::memcmp(bytes.data()+24,p.pipelineCacheUUID,VK_UUID_SIZE)||get(bytes,56)!=sizeof(void*)){reason="device, driver, UUID or ABI mismatch";return {};}
    if(get(bytes,40,8)!=bytes.size()-header_size){reason="truncated payload";return {};}
    auto payload=bytes.subspan(header_size);if(get(bytes,48,8)!=checksum(payload)){reason="payload checksum mismatch";return {};}
    if(!native_header(payload,p)){reason="invalid native Vulkan cache header";return {};}
    reason="loaded validated driver cache";return payload;
}
std::vector<std::byte> read_file(const std::filesystem::path& path){
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)return {};
    auto size=file.tellg();if(size<std::streamoff(header_size)||size>std::streamoff(header_size+max_payload))return {};
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));if(!file)return {};return bytes;
}
std::string unique_suffix(){
    static std::atomic_uint64_t sequence{0};
#ifdef _WIN32
    auto process=GetCurrentProcessId();
#else
    auto process=getpid();
#endif
    return std::to_string(process)+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(sequence++);
}
void write_atomic(const std::filesystem::path& destination,std::span<const std::byte> bytes){
    if(!destination.parent_path().empty())std::filesystem::create_directories(destination.parent_path());
    auto temporary=destination;temporary+=".tmp-"+unique_suffix();
    try{
        {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);if(!file)throw std::runtime_error("Cannot open pipeline cache temporary file");file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));file.flush();if(!file)throw std::runtime_error("Cannot write pipeline cache temporary file");file.close();if(!file)throw std::runtime_error("Cannot close pipeline cache temporary file");}
        // 同目录临时文件写完才替换；退出中断或写入失败不能破坏上一次完整缓存。
#ifdef _WIN32
        if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Pipeline cache atomic replace failed: "+std::to_string(GetLastError()));
#else
        std::filesystem::rename(temporary,destination);
#endif
    }catch(...){std::error_code ignored;std::filesystem::remove(temporary,ignored);throw;}
}
}
std::filesystem::path GpuPipelineCache::default_path(const std::filesystem::path& base,const VkPhysicalDeviceProperties& p){
    std::ostringstream name;name<<"emberframe-"<<std::hex<<p.vendorID<<'-'<<p.deviceID<<'-'<<p.driverVersion<<'-';for(auto v:p.pipelineCacheUUID){constexpr char digits[]="0123456789abcdef";name<<digits[v>>4]<<digits[v&15];}name<<".vkcache";return base/"cache"/name.str();
}
GpuPipelineCache::GpuPipelineCache(VkDevice device,const VkPhysicalDeviceProperties& properties,std::filesystem::path path):device_(device),properties_(properties),path_(std::move(path)){
    if(!device_)throw std::invalid_argument("Pipeline cache requires a live device");
    try{
        std::vector<std::byte> bytes;std::span<const std::byte> payload;
        try{bytes=read_file(path_);status_="cache absent or invalid size";if(!bytes.empty())payload=decode(bytes,properties_,status_);}catch(const std::exception& e){status_=std::string("cache read ignored: ")+e.what();}
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};ci.initialDataSize=payload.size();ci.pInitialData=payload.data();auto result=vkCreatePipelineCache(device_,&ci,nullptr,&cache_);
        if(result!=VK_SUCCESS&&!payload.empty()){
            if(cache_)vkDestroyPipelineCache(device_,cache_,nullptr);cache_={};ci.initialDataSize=0;ci.pInitialData=nullptr;status_="driver rejected cache; rebuilt empty";payload={};result=vkCreatePipelineCache(device_,&ci,nullptr,&cache_);
        }
        if(result!=VK_SUCCESS)throw std::runtime_error("vkCreatePipelineCache failed: "+std::to_string(result));loaded_=!payload.empty();
    }catch(...){if(cache_)vkDestroyPipelineCache(device_,cache_,nullptr);cache_={};throw;}
}
GpuPipelineCache::~GpuPipelineCache(){
    if(cache_){try{std::string error;if(!save(&error))std::fprintf(stderr,"[Vulkan pipeline cache] save skipped: %s\n",error.c_str());}catch(...){std::fputs("[Vulkan pipeline cache] save skipped after exception\n",stderr);}vkDestroyPipelineCache(device_,cache_,nullptr);}
}
bool GpuPipelineCache::save(std::string* error) const{
    try{
        std::size_t size=0;auto result=vkGetPipelineCacheData(device_,cache_,&size,nullptr);if(result!=VK_SUCCESS||size>max_payload||size<32)throw std::runtime_error("Invalid driver pipeline cache size/result");
        std::vector<std::byte> payload(size);result=vkGetPipelineCacheData(device_,cache_,&size,payload.data());if(result!=VK_SUCCESS)throw std::runtime_error("Driver pipeline cache changed or could not be exported");payload.resize(size);
        if(!native_header(payload,properties_))throw std::runtime_error("Driver returned unsupported pipeline cache header");write_atomic(path_,encode(payload,properties_));if(error)error->clear();return true;
    }catch(const std::exception& e){if(error)*error=e.what();return false;}
}
TestResults test_pipeline_cache_format(){
    TestResults results;VkPhysicalDeviceProperties p{};p.vendorID=0x10de;p.deviceID=123;p.driverVersion=456;p.pipelineCacheUUID[0]=17;
    std::vector<std::byte> payload(48);put(payload,0,32);put(payload,4,VK_PIPELINE_CACHE_HEADER_VERSION_ONE);put(payload,8,p.vendorID);put(payload,12,p.deviceID);std::memcpy(payload.data()+16,p.pipelineCacheUUID,16);payload[40]=std::byte(0xaa);auto valid=encode(payload,p);
    auto run=[&](const char* name,const std::function<bool()>& fn){try{results.push_back({name,fn(),"Checked before passing any file bytes to vkCreatePipelineCache"});}catch(const std::exception& e){results.push_back({name,false,e.what()});}};
    run("Pipeline cache validated envelope roundtrip",[&]{std::string why;auto data=decode(valid,p,why);return data.size()==payload.size()&&std::memcmp(data.data(),payload.data(),payload.size())==0;});
    run("Pipeline cache rejects device/driver/UUID/ABI mismatch",[&]{for(int field=0;field<5;++field){auto other=p;auto bytes=valid;if(field==0)++other.vendorID;if(field==1)++other.deviceID;if(field==2)++other.driverVersion;if(field==3)++other.pipelineCacheUUID[0];if(field==4)put(bytes,56,sizeof(void*)==8?4:8);std::string why;if(!decode(bytes,other,why).empty())return false;}return true;});
    run("Pipeline cache rejects truncation/checksum/native-header corruption",[&]{for(int field=0;field<4;++field){auto bytes=valid;if(field==0)bytes.resize(12);if(field==1)bytes.pop_back();if(field==2)bytes.back()^=std::byte(1);if(field==3){put(bytes,header_size+4,999);put(bytes,48,checksum(std::span<const std::byte>(bytes).subspan(header_size)),8);}std::string why;if(!decode(bytes,p,why).empty())return false;}return true;});return results;
}
TestResults test_gpu_pipeline_cache(VkDevice device,const VkPhysicalDeviceProperties& properties,const std::filesystem::path& directory,VkPipelineCache seed){
    TestResults results;auto path=directory/("pipeline-cache-test-"+unique_suffix()+".vkcache");
    try{
        {GpuPipelineCache cold(device,properties,path);if(seed&&vkMergePipelineCaches(device,cold.handle(),1,&seed)!=VK_SUCCESS)throw std::runtime_error("Merge rendered workbench pipeline cache failed");std::string error;bool okay=cold.handle()&&!cold.loaded_from_disk()&&cold.save(&error);results.push_back({"GPU native pipeline cache export",okay,error.empty()?(seed?"Exported real rendered graphics/compute/ImGui driver data":"Exported native driver payload"):error});}
        {GpuPipelineCache warm(device,properties,path);results.push_back({"GPU native pipeline cache warm import",warm.handle()&&warm.loaded_from_disk(),warm.load_status()});}
        auto bytes=read_file(path);if(bytes.empty())throw std::runtime_error("Cache diagnostic file absent");bytes.back()^=std::byte(1);write_atomic(path,bytes);
        {GpuPipelineCache repaired(device,properties,path);results.push_back({"GPU native pipeline cache corruption recovery",repaired.handle()&&!repaired.loaded_from_disk(),repaired.load_status()});}
        {GpuPipelineCache repaired(device,properties,path);results.push_back({"GPU rebuilt pipeline cache readable",repaired.loaded_from_disk(),repaired.load_status()});}
    }catch(const std::exception& e){results.push_back({"GPU native pipeline cache diagnostic",false,e.what()});}
    std::error_code error;std::filesystem::remove(path,error);if(error)results.push_back({"GPU cache diagnostic cleanup",false,error.message()});return results;
}
} // namespace emberframe::lab
