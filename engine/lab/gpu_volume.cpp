#include "gpu_volume.h"
#include "geometry.h"
#include "shader_assets.h"
#include "config_applicability.h"
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <utility>

namespace emberframe::lab {
namespace {
constexpr std::array<const char*,7> volumeShaders={"volume_rsm.comp","volume_voxelize.comp",
    "volume_inject.comp","volume_propagate.comp","volume_mip.comp","volume_trace.comp","volume_sparse.comp"};
constexpr VkFormat volumeFormat=VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkPipelineStageFlags2 compute=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkAccessFlags2 shaderRead=VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
constexpr VkAccessFlags2 shaderWrite=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
void checked(VkResult result,const char* action) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string("GpuVolumeGi: ")+action+" (VkResult "+std::to_string(result)+")");
}
template<glm::length_t N,class T,glm::qualifier Q> bool finite(const glm::vec<N,T,Q>& x) {
    for(glm::length_t i=0;i<N;++i) if(!std::isfinite(x[i])) return false;
    return true;
}
bool finite(const glm::mat4& m) { for(int c=0;c<4;++c) if(!finite(m[c])) return false; return true; }
struct alignas(16) VolumeParams {
    glm::mat4 lightVP{1},inverseLightVP{1};
    glm::vec4 boundsMinCell{},boundsMaxSide{},cameraPosition{};
    glm::vec4 lightPositionRange{},lightDirectionKind{},lightColorIntensity{};
    glm::ivec4 dimensions{},rsmDimensions{},modes{};
    glm::vec4 transport{};
    glm::uvec4 random{};
    glm::ivec4 geometry{}; // BVH 节点数、RSM 面数、每面宽高
    glm::vec4 lightSize{}; // 面积光物理尺寸；不是点光范围或软阴影参数
    std::array<glm::mat4,6> lightFaces{},inverseLightFaces{};
};
static_assert(sizeof(VolumeParams)==1104 && offsetof(VolumeParams,dimensions)==224);
struct alignas(16) WorldTriangle { glm::vec4 a{},b{},c{},diffuse{},emissive{}; };
static_assert(sizeof(WorldTriangle)==80);
struct alignas(16) GeometryNode {
    glm::vec4 minimum{},maximum{};
    glm::uvec4 range{}; // firstTriangle, triangleCount(0=branch), escapeIndex, reserved
};
static_assert(sizeof(GeometryNode)==48);
// 深度优先布局和 escapeIndex 让 GPU 无递归、无固定栈上限地跳过整个不相交子树。
// 中位数分割保证构建深度为 O(log N)，完整三角形只出现一次，不因分桶重叠复制几何。
std::vector<GeometryNode> geometryBvh(std::vector<WorldTriangle>& triangles) {
    std::vector<GeometryNode> nodes;
    const auto centroid=[](const WorldTriangle& t){return (glm::vec3(t.a)+glm::vec3(t.b)+glm::vec3(t.c))/3.f;};
    std::function<void(std::uint32_t,std::uint32_t)> build=[&](std::uint32_t first,std::uint32_t end) {
        const auto index=std::uint32_t(nodes.size()); nodes.emplace_back();
        glm::vec3 lo(std::numeric_limits<float>::infinity()),hi(-lo),clo(lo),chi(hi);
        for(auto i=first;i<end;++i) {
            const auto& t=triangles[i];
            lo=glm::min(lo,glm::min(glm::vec3(t.a),glm::min(glm::vec3(t.b),glm::vec3(t.c))));
            hi=glm::max(hi,glm::max(glm::vec3(t.a),glm::max(glm::vec3(t.b),glm::vec3(t.c))));
            auto c=centroid(t);clo=glm::min(clo,c);chi=glm::max(chi,c);
        }
        nodes[index].minimum={lo,0};nodes[index].maximum={hi,0};
        if(end-first<=8)nodes[index].range={first,end-first,0,0};
        else {
            auto size=chi-clo;int axis=size.y>size.x?1:0;if(size.z>size[axis])axis=2;
            const auto middle=first+(end-first)/2;
            std::nth_element(triangles.begin()+first,triangles.begin()+middle,triangles.begin()+end,
                [&](const auto& a,const auto& b){return centroid(a)[axis]<centroid(b)[axis];});
            build(first,middle);build(middle,end);
        }
        nodes[index].range.z=std::uint32_t(nodes.size());
    };
    if(!triangles.empty())build(0,std::uint32_t(triangles.size()));
    return nodes;
}
std::uint64_t geometryKey(const Scene& scene) {
    std::uint64_t key=1469598103934665603ull;
    const auto hash=[&](const auto& value) {
        const auto* bytes=reinterpret_cast<const unsigned char*>(&value);
        for(std::size_t i=0;i<sizeof(value);++i){key^=bytes[i];key*=1099511628211ull;}
    };
    hash(scene.asset_revision?scene.asset_revision:scene.revision);
    for(const auto& n:scene.nodes){hash(n.parent);hash(n.mesh);hash(n.local);}
    for(const auto& m:scene.materials){hash(m.base_color);hash(m.emissive);hash(m.metallic);hash(m.double_sided);}
    return key; // 遵守 asset_revision 契约时，纯灯光修改无需重新 flatten/BVH。
}
constexpr VkDeviceSize packetStride=48,shStride=48;
struct alignas(16) SparseCounters {
    glm::uvec4 counts{};
    std::array<glm::uvec4,2> levels{};
};
static_assert(sizeof(SparseCounters)==48 && offsetof(SparseCounters,levels)==16);
static_assert(GpuVolumeGi::sparse_node_capacity(16)==4681 && GpuVolumeGi::sparse_node_capacity(32)==37449);
struct Buffer {
    VmaAllocator allocator{}; VkBuffer handle{}; VmaAllocation allocation{};
    VkDeviceSize size=0; void* mapped=nullptr;
    ~Buffer() { if(handle) vmaDestroyBuffer(allocator,handle,allocation); }
    Buffer()=default; Buffer(const Buffer&)=delete; Buffer& operator=(const Buffer&)=delete;
};
struct VolumeImage {
    VkDevice device{}; VmaAllocator allocator{}; VkImage handle{}; VmaAllocation allocation{};
    VkImageView view{}; std::vector<VkImageView> levels; std::uint32_t mips=1;
    ~VolumeImage() {
        for(auto x:levels) if(x) vkDestroyImageView(device,x,nullptr);
        if(view) vkDestroyImageView(device,view,nullptr);
        if(handle) vmaDestroyImage(allocator,handle,allocation);
    }
    VolumeImage()=default; VolumeImage(const VolumeImage&)=delete; VolumeImage& operator=(const VolumeImage&)=delete;
};
// 同 family 同 queue 的图像屏障。队列所有权始终 IGNORED，绝不偷偷做外部 queue transfer。
void imageBarrier(VkCommandBuffer cmd,VkImage image,std::uint32_t mips,
                  VkImageLayout oldLayout,VkImageLayout newLayout,
                  VkPipelineStageFlags2 srcStage,VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage,VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask=srcStage; b.srcAccessMask=srcAccess; b.dstStageMask=dstStage; b.dstAccessMask=dstAccess;
    b.oldLayout=oldLayout; b.newLayout=newLayout; b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=image;
    b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount=1; d.pImageMemoryBarriers=&b;
    vkCmdPipelineBarrier2(cmd,&d);
}
void memoryBarrier(VkCommandBuffer cmd,VkPipelineStageFlags2 srcStage,VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage,VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask=srcStage; b.srcAccessMask=srcAccess; b.dstStageMask=dstStage; b.dstAccessMask=dstAccess;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount=1; d.pMemoryBarriers=&b;
    vkCmdPipelineBarrier2(cmd,&d);
}
// 稀疏结构采用显式 buffer barrier，reset 的 transfer 写和 compact/查询读写逐一同步。
void bufferBarrier(VkCommandBuffer cmd,const Buffer& buffer,
                   VkPipelineStageFlags2 srcStage,VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage,VkAccessFlags2 dstAccess) {
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask=srcStage; b.srcAccessMask=srcAccess; b.dstStageMask=dstStage; b.dstAccessMask=dstAccess;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.buffer=buffer.handle; b.offset=0; b.size=buffer.size;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.bufferMemoryBarrierCount=1; d.pBufferMemoryBarriers=&b;
    vkCmdPipelineBarrier2(cmd,&d);
}
std::uint64_t instanceTriangleCount(const Scene& scene) {
    std::uint64_t total=0;
    const auto countMesh=[&](const Mesh& mesh) {
        const std::uint64_t available=mesh.indices.empty()?mesh.vertices.size():mesh.indices.size();
        if(mesh.primitives.empty()) {
            if(available%3) throw std::invalid_argument("GpuVolumeGi: non-triangle mesh");
            total+=available/3;
        } else for(const auto& p:mesh.primitives) {
            if(p.index_count%3||p.first_index>available||p.index_count>available-p.first_index)
                throw std::invalid_argument("GpuVolumeGi: invalid primitive range");
            total+=p.index_count/3;
        }
        if(total>GpuVolumeGi::max_triangles) throw std::invalid_argument("GpuVolumeGi: world triangle count exceeds shader index range; no geometry was truncated");
    };
    if(scene.nodes.empty()) for(const auto& mesh:scene.meshes) countMesh(mesh);
    else for(const auto& node:scene.nodes) {
        if(node.mesh<-1||node.mesh>=int(scene.meshes.size())) throw std::invalid_argument("GpuVolumeGi: invalid mesh index");
        if(node.mesh>=0) countMesh(scene.meshes[std::size_t(node.mesh)]);
    }
    return total;
}
}

struct GpuVolumeGi::PreparedPipelines::State {
    VkDevice device{}; const void* owner=nullptr;
    std::array<VkPipeline,volumeShaders.size()> pipelines{};
    std::array<SpirvReflection,volumeShaders.size()> reflections;
    std::filesystem::path directory;
    ~State() { for(auto p:pipelines) if(p) vkDestroyPipeline(device,p,nullptr); }
};
GpuVolumeGi::PreparedPipelines::PreparedPipelines(std::unique_ptr<State> s):state_(std::move(s)) {}
GpuVolumeGi::PreparedPipelines::~PreparedPipelines()=default;

struct GpuVolumeGi::Impl {
    struct Frame {
        Buffer uniform,packets,sh,sparsePages,sparseNodes,sparseCounters;
        VolumeImage volume,output;
        std::vector<VkDescriptorSet> sets;
        bool initialized=false;
        std::uint64_t transportKey=0;bool transportBuilt=false;
    };
    struct Resources {
        VkDevice device{}; VkDescriptorPool pool{};
        Buffer triangles,bvh,triangleUpload,bvhUpload; std::array<Frame,frame_slots> frames;
        const Scene* scene=nullptr; std::uint64_t revision=0;
        VkExtent2D extent{}; std::uint32_t resolution=0,mips=0,triangleCount=0,bvhCount=0;
        std::uint64_t geometryKey=0; bool hasGeometry=false;
        std::uint64_t instancedTriangles=0;bool geometryUploaded=false;
        std::uint32_t sparseCapacity=0; // 所有 mip 格之和；仅诊断会故意调低，正常帧不会溢出。
        glm::vec3 minimum{},maximum{}; float side=0,cell=0;
        ~Resources() { if(pool) vkDestroyDescriptorPool(device,pool,nullptr); }
    };
    VkPhysicalDevice physical{}; VkDevice device{}; VmaAllocator allocator{}; VkPipelineCache cache{};
    VkPhysicalDeviceProperties properties{}; VkDescriptorSetLayout descriptors{};
    VkPipelineLayout layout{}; VkSampler sampler{};
    std::array<VkPipeline,volumeShaders.size()> pipelines{}; std::array<SpirvReflection,volumeShaders.size()> reflections;
    std::filesystem::path directory;
    std::unique_ptr<Resources> resources;
    Impl(VkPhysicalDevice p,VkDevice d,VmaAllocator a,VkPipelineCache c):physical(p),device(d),allocator(a),cache(c) {}
    ~Impl() {
        // caller 统一先 wait_idle；析构不隐式提交、不等待、不访问已销毁的 device。
        resources.reset();
        for(auto p:pipelines) if(p) vkDestroyPipeline(device,p,nullptr);
        if(sampler) vkDestroySampler(device,sampler,nullptr);
        if(layout) vkDestroyPipelineLayout(device,layout,nullptr);
        if(descriptors) vkDestroyDescriptorSetLayout(device,descriptors,nullptr);
    }
    void initialize() {
        if(!physical||!device||!allocator) throw std::invalid_argument("GpuVolumeGi: null device/allocator");
        vkGetPhysicalDeviceProperties(physical,&properties);
        const auto& limits=properties.limits;
        if(properties.apiVersion<VK_API_VERSION_1_3||limits.maxPerStageDescriptorStorageBuffers<7||limits.maxDescriptorSetStorageBuffers<7
           ||limits.maxPerStageDescriptorSampledImages<7||limits.maxPerStageDescriptorSamplers<7
           ||limits.maxPerStageDescriptorStorageImages<3||limits.maxPerStageResources<18
           ||limits.maxComputeWorkGroupInvocations<64||limits.maxComputeWorkGroupSize[0]<64
           ||limits.maxComputeWorkGroupSize[1]<8||limits.maxComputeWorkGroupSize[2]<4)
            throw std::runtime_error("GpuVolumeGi: insufficient Vulkan 1.3 compute/descriptor limits");
        VkFormatProperties format{}; vkGetPhysicalDeviceFormatProperties(physical,volumeFormat,&format);
        const auto required=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if((format.optimalTilingFeatures&required)!=required)
            throw std::runtime_error("GpuVolumeGi: rgba32f storage+sampled images unavailable");
        std::array<VkDescriptorSetLayoutBinding,18> bindings{};
        for(std::uint32_t i=0;i<bindings.size();++i) {
            const auto kind=i==0?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                ((i>=1&&i<=6)||i==11)?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                ((i>=7&&i<=9)||i>=14)?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i]={i,kind,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        }
        VkDescriptorSetLayoutCreateInfo dc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dc.bindingCount=std::uint32_t(bindings.size()); dc.pBindings=bindings.data();
        checked(vkCreateDescriptorSetLayout(device,&dc,nullptr,&descriptors),"create descriptor layout");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,16};
        VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lc.setLayoutCount=1; lc.pSetLayouts=&descriptors; lc.pushConstantRangeCount=1; lc.pPushConstantRanges=&push;
        checked(vkCreatePipelineLayout(device,&lc,nullptr,&layout),"create pipeline layout");
        VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sc.magFilter=sc.minFilter=VK_FILTER_NEAREST; sc.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sc.addressModeU=sc.addressModeV=sc.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sc.maxLod=32;
        checked(vkCreateSampler(device,&sc,nullptr,&sampler),"create nearest sampler");
    }
    void createBuffer(Buffer& b,VkDeviceSize bytes,VkBufferUsageFlags usage,bool host) const {
        if(bytes>properties.limits.maxStorageBufferRange&&(usage&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
            throw std::runtime_error("GpuVolumeGi: storage buffer range exceeded");
        b.allocator=allocator; b.size=std::max<VkDeviceSize>(bytes,16);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size=b.size; info.usage=usage;
        info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ac{}; ac.usage=VMA_MEMORY_USAGE_AUTO;
        if(host) ac.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
        else ac.requiredFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VmaAllocationInfo ai{};
        checked(vmaCreateBuffer(allocator,&info,&ac,&b.handle,&b.allocation,&ai),"allocate buffer");
        b.mapped=ai.pMappedData;
        if(host&&!b.mapped) throw std::runtime_error("GpuVolumeGi: mapped buffer unavailable");
    }
    void createImage(VolumeImage& image,VkExtent3D extent,bool threeD,std::uint32_t mips,VkImageUsageFlags extra=0) const {
        image.device=device; image.allocator=allocator; image.mips=mips;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType=threeD?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D; info.format=volumeFormat; info.extent=extent;
        info.mipLevels=mips; info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
        info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|extra;
        info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ac{}; ac.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        checked(vmaCreateImage(allocator,&info,&ac,&image.handle,&image.allocation,nullptr),"allocate image");
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image=image.handle; vc.viewType=threeD?VK_IMAGE_VIEW_TYPE_3D:VK_IMAGE_VIEW_TYPE_2D; vc.format=volumeFormat;
        vc.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,1};
        checked(vkCreateImageView(device,&vc,nullptr,&image.view),"create image view");
        image.levels.resize(mips);
        for(std::uint32_t level=0;level<mips;++level) {
            vc.subresourceRange.baseMipLevel=level; vc.subresourceRange.levelCount=1;
            checked(vkCreateImageView(device,&vc,nullptr,&image.levels[level]),"create mip view");
        }
    }
    std::unique_ptr<PreparedPipelines::State> prepare(const std::filesystem::path& path) const {
        auto state=std::make_unique<PreparedPipelines::State>();
        state->device=device; state->owner=this; state->directory=path;
        // 与 shader_assets 的结构签名一致：同时锁定 uint/vec4 类型、数组 stride 和计数器偏移。
        // 只比较 descriptor 种类/最小字节数不足以识别运行时数组的 stride 变化。
        const std::array<std::string,3> sparseHashes={
            shader_content_hash("struct{0:runtime:4<21:32:0>;}"),
            shader_content_hash("struct{0:runtime:16<vec4<22:32>>;}"),
            shader_content_hash("struct{0:vec4<21:32:0>;16:array2:16<vec4<21:32:0>>;}")};
        std::array<SpirvModule,volumeShaders.size()> modules;
        // 先核验整个候选包，再创建任何 Vulkan Pipeline。
        // 第七个文件不兼容时不应已经申请前六个候选 GPU 对象。
        for(std::size_t i=0;i<volumeShaders.size();++i) {
            auto& module=modules[i];module=read_spirv(path/(std::string(volumeShaders[i])+".spv"),ShaderStage::compute);
            if(!module.reflection.complete) throw std::runtime_error("GpuVolumeGi: incomplete shader reflection");
            // 核心 device 没启用任何 optional shader feature；候选不得引入额外 capability。
            for(std::size_t p=5;p<module.words.size();) {
                const auto length=module.words[p]>>16,op=module.words[p]&0xffff;
                if(op==17&&length==2&&module.words[p+1]!=1&&module.words[p+1]!=50) // Shader / core ImageQuery
                    throw std::runtime_error("GpuVolumeGi: shader requires non-core capability");
                p+=length;
            }
            for(const auto& d:module.reflection.descriptors) {
                const auto expected=d.binding==0?ShaderDescriptorKind::uniform_buffer:
                    ((d.binding>=1&&d.binding<=6)||d.binding==11)?ShaderDescriptorKind::combined_image_sampler:
                    ((d.binding>=7&&d.binding<=9)||d.binding>=14)?ShaderDescriptorKind::storage_buffer:ShaderDescriptorKind::storage_image;
                if(d.set!=0||d.binding>17||d.count!=1||d.runtime_array||d.kind!=expected)
                    throw std::runtime_error("GpuVolumeGi: shader descriptor ABI mismatch");
                if(d.binding==0&&d.minimum_buffer_bytes!=sizeof(VolumeParams))
                    throw std::runtime_error("GpuVolumeGi: uniform ABI mismatch");
                if(d.binding==16&&d.minimum_buffer_bytes!=sizeof(SparseCounters))
                    throw std::runtime_error("GpuVolumeGi: sparse counter ABI mismatch");
                if(d.binding>=14&&d.binding<=16&&d.type_layout_hash!=sparseHashes[d.binding-14])
                    throw std::runtime_error("GpuVolumeGi: sparse SSBO type/stride ABI mismatch");
                if(d.binding==17&&d.type_layout_hash!=shader_content_hash(
                    "struct{0:runtime:48<struct{0:vec4<22:32>;16:vec4<22:32>;32:vec4<21:32:0>;}>;}"))
                    throw std::runtime_error("GpuVolumeGi: geometry BVH SSBO type/stride ABI mismatch");
            }
            for(const auto& push:module.reflection.push_constants)
                if(push.offset!=0||push.size!=16) throw std::runtime_error("GpuVolumeGi: push constant ABI mismatch");
            if(pipelines[i]) {
                const auto compatibility=compare_shader_layouts(reflections[i],module.reflection);
                if(!compatibility.compatible) throw std::runtime_error("GpuVolumeGi: reload changes shader resource layout");
            }
            state->reflections[i]=std::move(module.reflection);
        }
        for(std::size_t i=0;i<volumeShaders.size();++i) {
            const auto& module=modules[i];
            VkShaderModuleCreateInfo mc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mc.codeSize=module.words.size()*sizeof(std::uint32_t); mc.pCode=module.words.data();
            VkShaderModule shader{}; checked(vkCreateShaderModule(device,&mc,nullptr,&shader),"create shader module");
            VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pc.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            pc.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; pc.stage.module=shader; pc.stage.pName="main"; pc.layout=layout;
            const auto result=vkCreateComputePipelines(device,cache,1,&pc,nullptr,&state->pipelines[i]);
            vkDestroyShaderModule(device,shader,nullptr); checked(result,"create compute pipeline");
        }
        return state;
    }
    void configure(const Scene& scene,VkExtent2D extent,std::uint32_t resolution,GiMode mode) {
        if(resolution!=16&&resolution!=32) throw std::invalid_argument("GpuVolumeGi: resolution must be 16 or 32");
        if(extent.width==0||extent.height==0||extent.width>4096||extent.height>4096
           ||extent.width>properties.limits.maxImageDimension2D||extent.height>properties.limits.maxImageDimension2D)
            throw std::invalid_argument("GpuVolumeGi: output extent outside [1,4096]");
        const bool needsGeometry=mode!=GiMode::rsm;
        const auto key=geometryKey(scene);
        if(resources&&resources->scene==&scene&&resources->geometryKey==key&&(!needsGeometry||resources->hasGeometry)
           &&resources->resolution==resolution&&resources->extent.width==extent.width&&resources->extent.height==extent.height) {
            resources->revision=scene.revision;return;
        }
        const auto expected=instanceTriangleCount(scene);
        if(needsGeometry&&expected>properties.limits.maxStorageBufferRange/sizeof(WorldTriangle))
            throw std::length_error("GpuVolumeGi: complete geometry exceeds this GPU's storage-buffer range; no truncation or CPU fallback");
        const auto triangles=needsGeometry?flatten_scene(scene):std::vector<Triangle>{};
        if(needsGeometry&&triangles.size()!=expected) throw std::runtime_error("GpuVolumeGi: triangle count preparation mismatch");
        std::vector<WorldTriangle> upload; upload.reserve(triangles.size());
        glm::vec3 lo(std::numeric_limits<float>::infinity()),hi(-std::numeric_limits<float>::infinity());
        for(const auto& t:triangles) {
            Material m;
            if(!scene.materials.empty()) {
                if(t.material<0||std::size_t(t.material)>=scene.materials.size()) throw std::invalid_argument("GpuVolumeGi: invalid material index");
                m=scene.materials[std::size_t(t.material)];
            } else if(t.material!=0) throw std::invalid_argument("GpuVolumeGi: invalid implicit material index");
            if(!finite(m.base_color)||!finite(m.emissive)||!std::isfinite(m.metallic)
               ||glm::any(glm::greaterThan(glm::abs(m.base_color),glm::vec4(1e6f)))
               ||glm::any(glm::greaterThan(glm::abs(m.emissive),glm::vec3(1e6f))))
                throw std::invalid_argument("GpuVolumeGi: non-finite or out-of-range material");
            glm::vec3 color(0);
            for(const auto& vertex:t.vertices) {
                if(!finite(vertex.position)||!finite(vertex.color)
                   ||glm::any(glm::greaterThan(glm::abs(vertex.color),glm::vec4(1e6f)))
                   ||glm::any(glm::greaterThan(glm::abs(vertex.position),glm::vec3(1e5f))))
                    throw std::invalid_argument("GpuVolumeGi: world geometry outside finite +/-100000 range");
                lo=glm::min(lo,vertex.position); hi=glm::max(hi,vertex.position); color+=glm::vec3(vertex.color)/3.0f;
            }
            const auto diffuse=glm::clamp(glm::vec3(m.base_color)*color,glm::vec3(0),glm::vec3(1))*(1-std::clamp(m.metallic,0.0f,1.0f));
            upload.push_back({glm::vec4(t.vertices[0].position,m.double_sided?1.0f:0.0f),
                glm::vec4(t.vertices[1].position,0),glm::vec4(t.vertices[2].position,0),
                glm::vec4(diffuse,0),glm::vec4(glm::max(m.emissive,glm::vec3(0)),0)});
        }
        if(!needsGeometry) {
            // RSM 仅需完整场景边界用于灯光投影，不创建体素三角形或 BVH。
            const auto worlds=resolve_world_transforms(scene);
            const auto include=[&](const Mesh& mesh,const glm::mat4& world) {
                for(const auto& vertex:mesh.vertices) {
                    const auto p=glm::vec3(world*glm::vec4(vertex.position,1));
                    if(!finite(p)||glm::any(glm::greaterThan(glm::abs(p),glm::vec3(1e5f))))
                        throw std::invalid_argument("GpuVolumeGi: world geometry outside finite +/-100000 range");
                    lo=glm::min(lo,p);hi=glm::max(hi,p);
                }
            };
            if(scene.nodes.empty())for(const auto& mesh:scene.meshes)include(mesh,glm::mat4(1));
            else for(std::size_t i=0;i<scene.nodes.size();++i)if(scene.nodes[i].mesh>=0)
                include(scene.meshes[std::size_t(scene.nodes[i].mesh)],worlds[i]);
        }
        if(!finite(lo)||!finite(hi)) { lo=glm::vec3(-1); hi=glm::vec3(1); }
        const auto nodes=geometryBvh(upload);
        if(nodes.size()>properties.limits.maxStorageBufferRange/sizeof(GeometryNode))
            throw std::length_error("GpuVolumeGi: complete geometry BVH exceeds this GPU's storage-buffer range; no truncation");
        auto candidate=std::make_unique<Resources>(); candidate->device=device; candidate->scene=&scene;
        candidate->revision=scene.revision; candidate->extent=extent; candidate->resolution=resolution;
        candidate->triangleCount=std::uint32_t(upload.size());
        candidate->bvhCount=std::uint32_t(nodes.size());candidate->geometryKey=key;candidate->hasGeometry=needsGeometry;
        candidate->instancedTriangles=expected;
        const auto delta=hi-lo; candidate->side=std::max({delta.x,delta.y,delta.z,.01f})*1.25f;
        candidate->minimum=(lo+hi)*.5f-glm::vec3(candidate->side*.5f);
        candidate->maximum=candidate->minimum+glm::vec3(candidate->side);
        candidate->cell=candidate->side/float(resolution);
        candidate->mips=resolution==16?5:6;
        candidate->sparseCapacity=sparse_node_capacity(resolution);
        createBuffer(candidate->triangles,upload.size()*sizeof(WorldTriangle),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,false);
        createBuffer(candidate->bvh,nodes.size()*sizeof(GeometryNode),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,false);
        createBuffer(candidate->triangleUpload,upload.size()*sizeof(WorldTriangle),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,true);
        createBuffer(candidate->bvhUpload,nodes.size()*sizeof(GeometryNode),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,true);
        if(!upload.empty()) std::memcpy(candidate->triangleUpload.mapped,upload.data(),upload.size()*sizeof(WorldTriangle));
        if(!nodes.empty())std::memcpy(candidate->bvhUpload.mapped,nodes.data(),nodes.size()*sizeof(GeometryNode));
        checked(vmaFlushAllocation(allocator,candidate->triangleUpload.allocation,0,VK_WHOLE_SIZE),"flush world triangle staging");
        checked(vmaFlushAllocation(allocator,candidate->bvhUpload.allocation,0,VK_WHOLE_SIZE),"flush geometry BVH staging");
        const std::uint32_t sets=frame_slots*candidate->mips;
        std::array<VkDescriptorPoolSize,4> sizes={VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,sets},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,7*sets},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,7*sets},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3*sets}};
        VkDescriptorPoolCreateInfo dc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dc.maxSets=sets; dc.poolSizeCount=std::uint32_t(sizes.size()); dc.pPoolSizes=sizes.data();
        checked(vkCreateDescriptorPool(device,&dc,nullptr,&candidate->pool),"create descriptor pool");
        for(auto& frame:candidate->frames) {
            createBuffer(frame.uniform,sizeof(VolumeParams),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,true);
            createBuffer(frame.packets,max_rsm_samples*packetStride,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,false);
            createBuffer(frame.sh,VkDeviceSize(resolution)*resolution*resolution*shStride*3,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,false);
            constexpr auto sparseUsage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            createBuffer(frame.sparsePages,VkDeviceSize(candidate->sparseCapacity)*sizeof(std::uint32_t),sparseUsage,false);
            createBuffer(frame.sparseNodes,VkDeviceSize(candidate->sparseCapacity)*sizeof(glm::vec4),sparseUsage,false);
            createBuffer(frame.sparseCounters,sizeof(SparseCounters),sparseUsage,false);
            createImage(frame.volume,{resolution,resolution,resolution},true,candidate->mips,VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            createImage(frame.output,{extent.width,extent.height,1},false,1);
            frame.sets.resize(candidate->mips);
            std::vector<VkDescriptorSetLayout> layouts(candidate->mips,descriptors);
            VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            da.descriptorPool=candidate->pool; da.descriptorSetCount=candidate->mips; da.pSetLayouts=layouts.data();
            checked(vkAllocateDescriptorSets(device,&da,frame.sets.data()),"allocate descriptor sets");
        }
        // 新资源构建失败不影响旧资源。仅最终释放旧资源前等待；普通帧不会进入此处。
        checked(vkDeviceWaitIdle(device),"wait before scene/extent reconfigure");
        resources.swap(candidate);
    }
    void descriptorsFor(Frame& frame,const Inputs& in) const {
        const std::array<const SampledImage*,6> inputs={&in.position,&in.normal,&in.albedo,&in.rsmPosition,&in.rsmNormal,&in.rsmAlbedo};
        for(std::size_t level=0;level<frame.sets.size();++level) {
            std::array<VkDescriptorBufferInfo,8> buffers={VkDescriptorBufferInfo{frame.uniform.handle,0,frame.uniform.size},
                {resources->triangles.handle,0,resources->triangles.size},{frame.packets.handle,0,frame.packets.size},
                {frame.sh.handle,0,frame.sh.size},{frame.sparsePages.handle,0,frame.sparsePages.size},
                {frame.sparseNodes.handle,0,frame.sparseNodes.size},{frame.sparseCounters.handle,0,frame.sparseCounters.size},
                {resources->bvh.handle,0,resources->bvh.size}};
            std::array<VkDescriptorImageInfo,10> images{};
            for(std::size_t i=0;i<inputs.size();++i) images[i]={sampler,inputs[i]->view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            images[6]={VK_NULL_HANDLE,frame.volume.levels[level],VK_IMAGE_LAYOUT_GENERAL};
            images[7]={sampler,frame.volume.view,VK_IMAGE_LAYOUT_GENERAL};
            images[8]={VK_NULL_HANDLE,frame.output.view,VK_IMAGE_LAYOUT_GENERAL};
            images[9]={VK_NULL_HANDLE,frame.volume.levels[std::min(level+1,frame.sets.size()-1)],VK_IMAGE_LAYOUT_GENERAL};
            std::array<VkWriteDescriptorSet,18> writes{};
            for(std::uint32_t binding=0;binding<writes.size();++binding) {
                auto& w=writes[binding]; w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet=frame.sets[level]; w.dstBinding=binding; w.descriptorCount=1;
                if(binding==0) { w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo=&buffers[0]; }
                else if(binding<=6) { w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo=&images[binding-1]; }
                else if(binding<=9) { w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo=&buffers[binding-6]; }
                else if(binding>=14) { w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo=&buffers[binding-10]; }
                else { w.descriptorType=binding==11?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w.pImageInfo=&images[binding-4]; }
            }
            vkUpdateDescriptorSets(device,std::uint32_t(writes.size()),writes.data(),0,nullptr);
        }
    }
    Output record(VkCommandBuffer cmd,const Inputs& in) {
        if(!cmd||!resources||resources->scene!=&in.scene||resources->revision!=in.scene.revision)
            throw std::invalid_argument("GpuVolumeGi: call configure with the current scene before recording");
        if(in.frameSlot>=frame_slots) throw std::invalid_argument("GpuVolumeGi: invalid frame slot");
        if(in.settings.voxel_resolution!=int(resources->resolution))
            throw std::invalid_argument("GpuVolumeGi: configure resolution differs from settings.voxel_resolution");
        if(in.settings.propagation_steps<0||in.settings.propagation_steps>32)
            throw std::invalid_argument("GpuVolumeGi: propagation_steps must be in [0,32]");
        if((in.settings.gi==GiMode::lpv||in.settings.gi==GiMode::voxel)&&!resources->hasGeometry)
            throw std::invalid_argument("GpuVolumeGi: configure the geometry-backed mode before recording LPV/VCT");
        if((in.lightFaceCount!=1&&in.lightFaceCount!=6)||(in.lightFaceCount==6&&!in.rsmAtlas))
            throw std::invalid_argument("GpuVolumeGi: RSM must be a single view or six-face atlas");
        const std::array<const SampledImage*,6> inputs={&in.position,&in.normal,&in.albedo,&in.rsmPosition,&in.rsmNormal,&in.rsmAlbedo};
        for(std::size_t i=0;i<inputs.size();++i) {
            const auto& x=*inputs[i]; const auto extent=i<3?resources->extent:in.rsmPosition.extent;
            if(!x.image||!x.view||x.extent.width!=extent.width||x.extent.height!=extent.height
               ||x.extent.width==0||x.extent.height==0||x.extent.width>4096||x.extent.height>4096
               ||(x.layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&x.layout!=VK_IMAGE_LAYOUT_GENERAL
                  &&x.layout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL&&x.layout!=VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL))
                throw std::invalid_argument("GpuVolumeGi: invalid sampled input/layout/extent");
        }
        if(!finite(in.lightViewProjection)||!finite(in.camera.position)||!std::isfinite(in.settings.shadow_bias))
            throw std::invalid_argument("GpuVolumeGi: non-finite camera/light matrix/bias");
        const float det=glm::determinant(in.lightViewProjection);
        if(!std::isfinite(det)||std::abs(det)<1e-20f) throw std::invalid_argument("GpuVolumeGi: singular light VP");
        const auto inverse=glm::inverse(in.lightViewProjection);
        if(!finite(inverse)) throw std::invalid_argument("GpuVolumeGi: light VP inverse overflow");
        Light light; light.intensity=0;
        if(!in.scene.lights.empty()&&in.lightIndex!=std::numeric_limits<std::uint32_t>::max()) {
            if(in.lightIndex>=in.scene.lights.size()) throw std::invalid_argument("GpuVolumeGi: lightIndex outside scene");
            light=in.scene.lights[in.lightIndex];
            if(light.kind!=LightKind::directional&&light.kind!=LightKind::point&&light.kind!=LightKind::rectangle)
                throw std::invalid_argument("GpuVolumeGi: unsupported light kind");
        }
        if(!finite(light.position)||!finite(light.direction)||!finite(light.color)||!std::isfinite(light.intensity)
           ||!std::isfinite(light.range)||light.intensity<0||light.intensity>1e6f||light.range<=0
           ||glm::any(glm::lessThan(light.color,glm::vec3(0)))||glm::any(glm::greaterThan(light.color,glm::vec3(1e6f)))
           ||!finite(light.size)||glm::any(glm::greaterThan(glm::abs(light.position),glm::vec3(1e5f)))
           ||glm::any(glm::greaterThan(glm::abs(light.direction),glm::vec3(1e6f)))
           ||glm::any(glm::greaterThan(glm::abs(light.size),glm::vec2(1e6f)))
           ||(light.kind==LightKind::rectangle&&(light.size.x<=0||light.size.y<=0))
           ||(light.kind!=LightKind::point&&glm::dot(light.direction,light.direction)<1e-20f))
            throw std::invalid_argument("GpuVolumeGi: invalid/out-of-range light");
        VolumeParams p;
        p.lightVP=in.lightViewProjection; p.inverseLightVP=inverse;
        for(std::uint32_t face=0;face<6;++face) {
            const auto matrix=face==0||in.lightFaceCount==1?in.lightViewProjection:in.lightFaceViewProjections[face];
            const auto determinant=glm::determinant(matrix);
            if(!finite(matrix)||!std::isfinite(determinant)||std::abs(determinant)<1e-20f)
                throw std::invalid_argument("GpuVolumeGi: invalid RSM face projection");
            p.lightFaces[face]=matrix;p.inverseLightFaces[face]=glm::inverse(matrix);
            if(!finite(p.inverseLightFaces[face]))throw std::invalid_argument("GpuVolumeGi: RSM face inverse overflow");
        }
        p.boundsMinCell={resources->minimum,resources->cell}; p.boundsMaxSide={resources->maximum,resources->side};
        p.cameraPosition={in.camera.position,1}; p.lightPositionRange={light.position,light.range};
        p.lightDirectionKind={safe_normalize(light.direction),float(int(light.kind))}; p.lightColorIntensity={light.color,light.intensity};
        p.dimensions={int(resources->extent.width),int(resources->extent.height),int(resources->resolution),int(resources->triangleCount)};
        const auto rw=in.rsmPosition.extent.width,rh=in.rsmPosition.extent.height;
        if(in.rsmAtlas&&(rw%6!=0||rw<6))throw std::invalid_argument("GpuVolumeGi: invalid six-tile RSM atlas dimensions");
        const auto tileWidth=in.rsmAtlas?rw/6:rw;
        // LPV 的体素场跨帧缓存，不是每个屏幕像素逐帧重新采样。固定使用已有
        // 4096 包容量：32³ 网格投影到一个表面约为 32² 个格，六面分摊后
        // 需要接近这一密度，不能复用逐像素射线的低 samples 留下成片未注入格。
        const auto budget=in.settings.gi==GiMode::lpv?GpuVolumeGi::max_rsm_samples:
            std::uint32_t(std::clamp(in.settings.samples,2,32))*32u;
        const auto strata=std::uint32_t(std::sqrt(float(budget/in.lightFaceCount)));
        p.rsmDimensions={int(rw),int(rh),int(std::min(strata,tileWidth)),int(std::min(strata,rh))};
        p.geometry={int(resources->bvhCount),int(in.lightFaceCount),int(tileWidth),int(rh)};
        p.lightSize={glm::abs(light.size),0,0};
        p.modes={int(in.settings.gi),in.settings.propagation_steps,int(resources->mips),in.lightViewportYDown?1:-1};
        p.transport={.8f,std::max(.001f,resources->side*.001f),std::max(1e-5f,in.settings.shadow_bias),0};
        p.random={in.settings.seed,in.settings.sparse_voxels?1u:0u,resources->sparseCapacity,sparse_node_capacity(resources->resolution)};
        auto& frame=resources->frames[in.frameSlot];
        std::memcpy(frame.uniform.mapped,&p,sizeof(p));
        checked(vmaFlushAllocation(allocator,frame.uniform.allocation,0,sizeof(p)),"flush uniforms");
        descriptorsFor(frame,in); // 本 slot fence 必须已完成；不更新正在执行的 descriptor/UBO。
        // 同一图像不允许在六个输入中别名；这样 producer layout 只有一个可靠状态。
        for(std::size_t i=0;i<inputs.size();++i) for(std::size_t j=0;j<i;++j)
            if(inputs[i]->image==inputs[j]->image) throw std::invalid_argument("GpuVolumeGi: G-buffer input image alias");
        for(const auto* x:inputs) imageBarrier(cmd,x->image,1,x->layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            x->stage,x->access,compute,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        const bool first=!frame.initialized;
        if(first) imageBarrier(cmd,frame.volume.handle,resources->mips,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_NONE,0,compute,shaderRead|shaderWrite);
        else memoryBarrier(cmd,compute,shaderRead|shaderWrite,compute,shaderRead|shaderWrite);
        imageBarrier(cmd,frame.output.handle,1,first?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,first?VK_PIPELINE_STAGE_2_NONE:VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            first?0:VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,compute,shaderWrite);
        memoryBarrier(cmd,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_WRITE_BIT,compute,
            VK_ACCESS_2_UNIFORM_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        if(!resources->geometryUploaded) {
            // 完整几何只上传一次；正常 GPU 查询读本地显存，不在每帧反复跨 PCIe 读 CPU 数组。
            const auto uploadOnce=[&](const Buffer& source,const Buffer& destination,VkDeviceSize bytes) {
                if(!bytes)return;
                bufferBarrier(cmd,source,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
                VkBufferCopy copy{0,0,bytes};vkCmdCopyBuffer(cmd,source.handle,destination.handle,1,&copy);
                bufferBarrier(cmd,destination,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    compute,VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            };
            uploadOnce(resources->triangleUpload,resources->triangles,VkDeviceSize(resources->triangleCount)*sizeof(WorldTriangle));
            uploadOnce(resources->bvhUpload,resources->bvh,VkDeviceSize(resources->bvhCount)*sizeof(GeometryNode));
            resources->geometryUploaded=true;
        }
        const auto dispatch=[&](std::size_t pipeline,std::size_t set,glm::ivec4 push,
                                std::uint32_t x,std::uint32_t y,std::uint32_t z) {
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[pipeline]);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&frame.sets[set],0,nullptr);
            vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,16,&push);
            vkCmdDispatch(cmd,x,y,z);
            memoryBarrier(cmd,compute,shaderRead|shaderWrite,compute,shaderRead|shaderWrite);
        };
        const auto grid=(resources->resolution+3)/4;
        VolumeParams transportParams=p;transportParams.cameraPosition={};
        std::uint64_t transportKey=1469598103934665603ull;
        const auto* transportBytes=reinterpret_cast<const unsigned char*>(&transportParams);
        for(std::size_t i=0;i<sizeof(transportParams);++i){transportKey^=transportBytes[i];transportKey*=1099511628211ull;}
        const bool rebuild=!in.cacheStaticTransport||!frame.transportBuilt||frame.transportKey!=transportKey;
        if(rebuild&&(in.settings.gi==GiMode::rsm||in.settings.gi==GiMode::lpv))
            dispatch(0,0,{},(std::uint32_t(p.rsmDimensions.z*p.rsmDimensions.w*p.geometry.y)+63)/64,1,1);
        if(rebuild&&(in.settings.gi==GiMode::lpv||in.settings.gi==GiMode::voxel)) {
            dispatch(1,0,{},grid,grid,grid);
            if(in.settings.gi==GiMode::lpv) {
                dispatch(2,0,{},grid,grid,grid);
                const int cells=int(resources->resolution*resources->resolution*resources->resolution);
                for(int step=0;step<in.settings.propagation_steps;++step)
                    dispatch(3,0,{(step%2)*cells,((step+1)%2)*cells,0,0},grid,grid,grid);
            } else {
                for(std::uint32_t level=1;level<resources->mips;++level) {
                    const auto size=resources->resolution>>level,groups=(size+3)/4;
                    dispatch(4,level-1,{0,0,int(size),0},groups,groups,groups);
                }
                if(in.settings.sparse_voxels) {
                    // 页表和 allocator 都在 GPU 清零；空场景、改灯和 slot 重用不会留下旧页。
                    // counters 的容量来自 UBO，计数器只记录真实 GPU attempt/stored/overflow。
                    for(const auto* b:std::array<const Buffer*,2>{&frame.sparsePages,&frame.sparseCounters}) {
                        bufferBarrier(cmd,*b,first?VK_PIPELINE_STAGE_2_NONE:compute,first?0:shaderRead|shaderWrite,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
                        vkCmdFillBuffer(cmd,b->handle,0,b->size,0);
                        bufferBarrier(cmd,*b,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            compute,shaderRead|shaderWrite);
                    }
                    bufferBarrier(cmd,frame.sparseNodes,first?VK_PIPELINE_STAGE_2_NONE:compute,first?0:shaderRead|shaderWrite,
                        compute,shaderWrite);
                    for(std::uint32_t level=0;level<resources->mips;++level) {
                        const auto size=resources->resolution>>level,groups=(size+3)/4;
                        dispatch(6,level,{0,0,int(size),int(level)},groups,groups,groups);
                        // 下一 mip 的 atomic allocator 和最后的 cone 查询必须看到本 mip 的发布。
                        for(const auto* b:std::array<const Buffer*,3>{&frame.sparsePages,&frame.sparseNodes,&frame.sparseCounters})
                            bufferBarrier(cmd,*b,compute,shaderRead|shaderWrite,compute,shaderRead|shaderWrite);
                    }
                }
            }
        }
        frame.transportBuilt=true;frame.transportKey=transportKey;
        dispatch(5,0,{},(resources->extent.width+7)/8,(resources->extent.height+7)/8,1);
        imageBarrier(cmd,frame.output.handle,1,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            compute,shaderWrite,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|compute,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        frame.initialized=true;
        return {frame.output.handle,frame.output.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,resources->extent};
    }
};

GpuVolumeGi::GpuVolumeGi(VkPhysicalDevice p,VkDevice d,VmaAllocator a,std::filesystem::path shaders,VkPipelineCache cache)
    :impl_(std::make_unique<Impl>(p,d,a,cache)) {
    impl_->initialize();
    auto initial=impl_->prepare(shaders); impl_->directory.swap(initial->directory);
    impl_->pipelines.swap(initial->pipelines); impl_->reflections.swap(initial->reflections);
}
GpuVolumeGi::~GpuVolumeGi()=default;
std::span<const char* const> GpuVolumeGi::shader_files() noexcept { return volumeShaders; }
void GpuVolumeGi::configure(const Scene& scene,VkExtent2D size,std::uint32_t resolution,GiMode mode) { impl_->configure(scene,size,resolution,mode); }
GpuVolumeGi::RsmProjection GpuVolumeGi::rsm_projection(const Scene& scene,glm::vec3 minimum,glm::vec3 maximum) {
    RsmProjection result;result.viewProjection.fill(glm::mat4(1));
    result.lightIndex=volume_gi_primary_light(scene);
    if(result.lightIndex<0)return result;
    const auto& light=scene.lights[std::size_t(result.lightIndex)];
    const auto center=(minimum+maximum)*.5f;
    const float radius=std::max(glm::length(maximum-minimum)*.5f,.1f);
    if(light.kind==LightKind::directional) {
        const auto direction=safe_normalize(light.direction);
        const auto up=std::abs(direction.y)>.98f?glm::vec3(0,0,1):glm::vec3(0,1,0);
        glm::mat4 p(1);p[0][0]=1/radius;p[1][1]=-1/radius;p[2][2]=-1/(radius*4);p[3][2]=0;
        result.viewProjection[0]=p*glm::lookAtRH(center-direction*(radius*2),center,up);
    } else {
        // 点光必须覆盖六个方向，而不是把朝向某一点的单个视锥误称全向覆盖。
        // 面积光也用六面覆盖；用一个接近 180° 的视锥会把房间压到中央少数 RSM 像素。
        // 六面只改善覆盖，辐射仍按面积和发光余弦估计，不能宣称完整面积光积分。
        result.faceCount=6;
        const float farPlane=std::max(.01f,glm::length(light.position-center)+radius);
        const float nearPlane=std::max(.0001f,farPlane*1e-5f);
        glm::mat4 p(0);p[0][0]=1;p[1][1]=-1;p[2][2]=farPlane/(nearPlane-farPlane);
        p[2][3]=-1;p[3][2]=p[2][2]*nearPlane;
        const std::array<glm::vec3,6> directions={glm::vec3(1,0,0),{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        for(std::uint32_t face=0;face<result.faceCount;++face) {
            const auto direction=directions[face];
            const auto up=std::abs(direction.y)>.98f?glm::vec3(0,0,1):glm::vec3(0,1,0);
            result.viewProjection[face]=p*glm::lookAtRH(light.position,light.position+direction,up);
        }
    }
    return result;
}
GpuVolumeGi::Output GpuVolumeGi::record(VkCommandBuffer cmd,const Inputs& in) { return impl_->record(cmd,in); }
GpuVolumeGi::AccelerationStats GpuVolumeGi::acceleration_stats() const noexcept {
    if(!impl_->resources)return {};
    const auto& r=*impl_->resources;
    return {r.instancedTriangles,r.triangleCount,r.bvhCount,r.triangles.size+r.bvh.size,
        r.triangleUpload.size+r.bvhUpload.size,r.hasGeometry};
}
GpuVolumeGi::Output GpuVolumeGi::output(std::uint32_t slot) const {
    if(!impl_->resources||slot>=frame_slots) throw std::invalid_argument("GpuVolumeGi: output unavailable/invalid slot");
    const auto& frame=impl_->resources->frames[slot];
    return {frame.output.handle,frame.output.view,frame.initialized?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,impl_->resources->extent};
}
glm::vec3 GpuVolumeGi::bounds_minimum() const { return impl_->resources?impl_->resources->minimum:glm::vec3(0); }
glm::vec3 GpuVolumeGi::bounds_maximum() const { return impl_->resources?impl_->resources->maximum:glm::vec3(0); }
std::unique_ptr<GpuVolumeGi::PreparedPipelines> GpuVolumeGi::prepare_pipelines(const std::filesystem::path& path,std::string* error) const {
    try {
        auto state=impl_->prepare(path);
        if(error) error->clear();
        return std::unique_ptr<PreparedPipelines>(new PreparedPipelines(std::move(state)));
    } catch(const std::exception& e) { if(error) *error=e.what(); return {}; }
}
bool GpuVolumeGi::commit_pipelines(std::unique_ptr<PreparedPipelines> prepared) noexcept {
    if(!prepared||!prepared->state_||prepared->state_->owner!=impl_.get()) return false;
    impl_->pipelines.swap(prepared->state_->pipelines); impl_->reflections.swap(prepared->state_->reflections);
    impl_->directory.swap(prepared->state_->directory);
    if(impl_->resources)for(auto& frame:impl_->resources->frames)frame.transportBuilt=false;
    return true;
}
bool GpuVolumeGi::reload_pipelines(const std::filesystem::path& path,std::string* error) {
    auto prepared=prepare_pipelines(path,error); if(!prepared) return false;
    const auto result=vkDeviceWaitIdle(impl_->device);
    if(result!=VK_SUCCESS) { if(error) *error="GpuVolumeGi: wait_idle failed during reload ("+std::to_string(result)+")"; return false; }
    return commit_pipelines(std::move(prepared));
}

TestResults test_gpu_volume(VkPhysicalDevice physical,VkDevice device,VmaAllocator allocator,VkQueue queue,
                           std::uint32_t family,const std::filesystem::path& shaders,VkPipelineCache cache) {
    TestResults results;
    // 只在显式 helper 中提交/等待/回读。生产 record 完全不走这些代码。
    struct Commands {
        VkDevice device{}; VkQueue queue{}; VkCommandPool pool{}; VkCommandBuffer cmd{}; VkFence fence{};
        ~Commands() {
            if(device) vkDeviceWaitIdle(device);
            if(fence) vkDestroyFence(device,fence,nullptr);
            if(pool) vkDestroyCommandPool(device,pool,nullptr);
        }
        void begin() {
            checked(vkResetCommandPool(device,pool,0),"test reset command pool");
            VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            checked(vkBeginCommandBuffer(cmd,&b),"test begin command buffer");
        }
        void submit() {
            checked(vkEndCommandBuffer(cmd),"test end command buffer");
            checked(vkResetFences(device,1,&fence),"test reset fence");
            VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount=1; s.pCommandBuffers=&cmd;
            checked(vkQueueSubmit(queue,1,&s,fence),"test submit");
            checked(vkWaitForFences(device,1,&fence,VK_TRUE,30'000'000'000ull),"test wait fence");
        }
    } commands;
    commands.device=device; commands.queue=queue;
    try {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex=family;
        pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        checked(vkCreateCommandPool(device,&pool,nullptr,&commands.pool),"test create pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool=commands.pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
        checked(vkAllocateCommandBuffers(device,&ca,&commands.cmd),"test allocate command");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; checked(vkCreateFence(device,&fc,nullptr,&commands.fence),"test create fence");
        GpuVolumeGi volume(physical,device,allocator,shaders,cache);
        Scene scene; scene.materials.emplace_back(); scene.materials[0].base_color=glm::vec4(.5f,.5f,.5f,1);
        Mesh floor; for(const auto p:std::array<glm::vec3,4>{{{-1,0,-1},{-1,0,1},{1,0,-1},{1,0,1}}}) {
            Vertex vertex; vertex.position=p; vertex.normal={0,1,0}; floor.vertices.push_back(vertex);
        }
        floor.indices={0,1,2,2,1,3}; scene.meshes.push_back(std::move(floor));
        Light light; light.direction={0,-1,0}; light.position={0,3,0}; light.intensity=2; scene.lights.push_back(light);
        constexpr VkExtent2D extent{8,8}; Settings settings; settings.voxel_resolution=16; settings.propagation_steps=6;
        Camera camera; volume.configure(scene,extent,16);
        std::array<VolumeImage,9> fixtures;
        for(std::size_t i=0;i<fixtures.size();++i) {
            const auto size=i<3?8u:i<6?32u:64u;
            volume.impl_->createImage(fixtures[i],{size,size,1},false,1,VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        }
        Buffer readback;
        VkBufferCreateInfo rb{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; rb.size=32*32*32*shStride*3;
        rb.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT; rb.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ac{}; ac.usage=VMA_MEMORY_USAGE_AUTO;
        ac.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo ai{}; readback.allocator=allocator; readback.size=rb.size;
        checked(vmaCreateBuffer(allocator,&rb,&ac,&readback.handle,&readback.allocation,&ai),"test create readback");
        readback.mapped=ai.pMappedData;
        struct IdleBeforeRelease {
            VkDevice device;
            ~IdleBeforeRelease() { vkDeviceWaitIdle(device); }
        } idleBeforeRelease{device}; // 即使显式诊断中抛错，也先停 GPU 再销毁 fixture/module。
        std::array<bool,9> initialized{};
        const auto clear=[&](std::size_t i,glm::vec4 color) {
            imageBarrier(commands.cmd,fixtures[i].handle,1,initialized[i]?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,initialized[i]?VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT:VK_PIPELINE_STAGE_2_NONE,
                initialized[i]?VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT:0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkClearColorValue value{}; std::memcpy(value.float32,&color,sizeof(color));
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(commands.cmd,fixtures[i].handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);
            imageBarrier(commands.cmd,fixtures[i].handle,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,compute,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            initialized[i]=true;
        };
        commands.begin(); clear(0,{0,1,0,1}); clear(1,{0,-1,0,.5f}); clear(2,{.4f,.4f,.4f,0});
        for(std::size_t i:std::array<std::size_t,2>{3,6}) {
            clear(i,{0,0,0,1}); clear(i+1,{0,1,0,.5f}); clear(i+2,{.5f,.5f,.5f,0});
        }
        commands.submit();
        const auto input=[&](std::size_t i) {
            const auto size=i<3?8u:i<6?32u:64u;
            return GpuVolumeGi::SampledImage{fixtures[i].handle,fixtures[i].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,{size,size},
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_MEMORY_READ_BIT};
        };
        glm::mat4 orthographic(1); orthographic[2][2]=-.25f; orthographic[3][2]=-.25f;
        const auto lightView=glm::lookAtRH(glm::vec3(0,3,0),glm::vec3(0),glm::vec3(0,0,-1));
        glm::mat4 lightVP=orthographic*lightView;
        std::uint32_t slot=0;
        std::array<glm::vec4,64> lastPixels{};
        const auto readOutput=[&](const GpuVolumeGi::Output& output) {
            bufferBarrier(commands.cmd,readback,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT,
                VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_READ_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            imageBarrier(commands.cmd,output.image,1,output.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_MEMORY_READ_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={8,8,1};
            vkCmdCopyImageToBuffer(commands.cmd,output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle,1,&copy);
            imageBarrier(commands.cmd,output.image,1,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.layout,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,compute|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            memoryBarrier(commands.cmd,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
            commands.submit(); checked(vmaInvalidateAllocation(allocator,readback.allocation,0,VK_WHOLE_SIZE),"test invalidate readback");
            glm::vec3 mean(0); const auto* pixels=static_cast<const glm::vec4*>(readback.mapped);
            std::memcpy(lastPixels.data(),pixels,sizeof(lastPixels));
            for(std::size_t i=0;i<64;++i) {
                if(!finite(pixels[i])||glm::any(glm::lessThan(glm::vec3(pixels[i]),glm::vec3(-1e-7f))))
                    throw std::runtime_error("GPU output non-finite/negative");
                mean+=glm::vec3(pixels[i])/64.0f;
            }
            return mean;
        };
        const auto evaluate=[&](GiMode mode,std::size_t rsmBase=3) {
            settings.gi=mode;
            GpuVolumeGi::Inputs inputs{camera,scene,settings,input(0),input(1),input(2),input(rsmBase),input(rsmBase+1),input(rsmBase+2),lightVP,0,slot,true};
            commands.begin(); const auto output=volume.record(commands.cmd,inputs);
            const auto mean=readOutput(output);
            slot=(slot+1)%GpuVolumeGi::frame_slots; return mean;
        };
        const auto check=[&](const char* name,bool passed,const std::string& detail) { results.push_back({name,passed,detail}); };
        const auto close=[](float a,float b) { return std::abs(a-b)<=std::max(1e-6f,std::abs(b)*.002f); };
        const auto samePixels=[&](const auto& a,const auto& b) {
            for(std::size_t i=0;i<a.size();++i) for(int channel=0;channel<4;++channel)
                if(std::abs(a[i][channel]-b[i][channel])>std::max(1e-7f,std::abs(b[i][channel])*2e-6f)) return false;
            return true;
        };
        struct SparseSnapshot {
            SparseCounters counters;
            std::vector<std::uint32_t> pages;
            std::vector<glm::vec4> nodes,dense;
        };
        const auto lastSlot=[&]() { return (slot+GpuVolumeGi::frame_slots-1)%GpuVolumeGi::frame_slots; };
        const auto snapshot=[&](std::uint32_t frameSlot) {
            auto& frame=volume.impl_->resources->frames[frameSlot];
            const auto capacity=GpuVolumeGi::sparse_node_capacity(volume.impl_->resources->resolution);
            SparseSnapshot result; result.pages.resize(capacity); result.nodes.resize(capacity); result.dense.resize(capacity);
            const VkDeviceSize pagesOffset=sizeof(SparseCounters);
            const VkDeviceSize nodesOffset=(pagesOffset+frame.sparsePages.size+15)&~VkDeviceSize(15);
            const VkDeviceSize denseOffset=nodesOffset+frame.sparseNodes.size;
            if(denseOffset+VkDeviceSize(capacity)*sizeof(glm::vec4)>readback.size)
                throw std::runtime_error("GpuVolumeGi: diagnostic sparse readback capacity exceeded");
            commands.begin();
            bufferBarrier(commands.cmd,readback,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT,
                VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_READ_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const std::array<const Buffer*,3> buffers={&frame.sparseCounters,&frame.sparsePages,&frame.sparseNodes};
            const std::array<VkDeviceSize,3> offsets={0,pagesOffset,nodesOffset};
            for(std::size_t i=0;i<buffers.size();++i) {
                bufferBarrier(commands.cmd,*buffers[i],compute,shaderRead|shaderWrite,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
                const VkBufferCopy copy{0,offsets[i],buffers[i]->size};
                vkCmdCopyBuffer(commands.cmd,buffers[i]->handle,readback.handle,1,&copy);
                bufferBarrier(commands.cmd,*buffers[i],VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,
                    compute,shaderRead|shaderWrite);
            }
            imageBarrier(commands.cmd,frame.volume.handle,frame.volume.mips,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                compute,shaderRead|shaderWrite,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
            std::vector<VkBufferImageCopy> copies(frame.volume.mips);
            VkDeviceSize offset=denseOffset;
            for(std::uint32_t level=0;level<frame.volume.mips;++level) {
                const auto size=volume.impl_->resources->resolution>>level;
                copies[level].bufferOffset=offset; copies[level].imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,1};
                copies[level].imageExtent={size,size,size}; offset+=VkDeviceSize(size)*size*size*sizeof(glm::vec4);
            }
            vkCmdCopyImageToBuffer(commands.cmd,frame.volume.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle,
                std::uint32_t(copies.size()),copies.data());
            imageBarrier(commands.cmd,frame.volume.handle,frame.volume.mips,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,compute,shaderRead|shaderWrite);
            bufferBarrier(commands.cmd,readback,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
            commands.submit(); checked(vmaInvalidateAllocation(allocator,readback.allocation,0,VK_WHOLE_SIZE),"test invalidate sparse structure");
            const auto* bytes=static_cast<const std::byte*>(readback.mapped);
            std::memcpy(&result.counters,bytes,sizeof(SparseCounters));
            std::memcpy(result.pages.data(),bytes+pagesOffset,result.pages.size()*sizeof(std::uint32_t));
            std::memcpy(result.nodes.data(),bytes+nodesOffset,result.nodes.size()*sizeof(glm::vec4));
            std::memcpy(result.dense.data(),bytes+denseOffset,result.dense.size()*sizeof(glm::vec4));
            return result;
        };
        const auto auditSparse=[&](const SparseSnapshot& data,std::uint32_t resolution) {
            std::vector<bool> used(data.nodes.size()); std::uint32_t expected=0,offset=0;
            bool passed=data.pages.size()==GpuVolumeGi::sparse_node_capacity(resolution);
            for(std::uint32_t level=0,size=resolution;size;size>>=1,++level) {
                std::uint32_t activeLevel=0;
                for(std::uint32_t i=0;i<size*size*size;++i) {
                    const auto& value=data.dense[offset+i]; const auto entry=data.pages[offset+i];
                    const bool active=value.a!=0||glm::any(glm::notEqual(glm::vec3(value),glm::vec3(0)));
                    if(!active) { passed&=entry==0; continue; }
                    ++expected; ++activeLevel;
                    if(entry==0||entry>data.nodes.size()) { passed=false; continue; }
                    passed&=!used[entry-1]; used[entry-1]=true;
                    passed&=std::memcmp(&value,&data.nodes[entry-1],sizeof(glm::vec4))==0;
                }
                passed&=data.counters.levels[level/4][level%4]==activeLevel;
                offset+=size*size*size;
            }
            passed&=data.counters.counts.x==expected&&data.counters.counts.z==expected&&data.counters.counts.y==0;
            passed&=data.counters.counts.w==0&&data.counters.levels[1].z==0&&data.counters.levels[1].w==0;
            return passed;
        };
        const auto traceCurrent=[&](bool clearNodes) {
            const auto frameSlot=lastSlot(); auto& frame=volume.impl_->resources->frames[frameSlot];
            commands.begin();
            if(clearNodes) {
                // 只在显式诊断清空节点：稠密 mip 保持有光，重跑 trace 必须变黑才证明读取 SSBO。
                bufferBarrier(commands.cmd,frame.sparseNodes,compute,shaderRead|shaderWrite,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
                vkCmdFillBuffer(commands.cmd,frame.sparseNodes.handle,0,frame.sparseNodes.size,0);
                bufferBarrier(commands.cmd,frame.sparseNodes,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    compute,VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            }
            imageBarrier(commands.cmd,frame.output.handle,1,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,compute,shaderWrite);
            vkCmdBindPipeline(commands.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,volume.impl_->pipelines[5]);
            vkCmdBindDescriptorSets(commands.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,volume.impl_->layout,0,1,&frame.sets[0],0,nullptr);
            const glm::ivec4 push(0); vkCmdPushConstants(commands.cmd,volume.impl_->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,16,&push);
            vkCmdDispatch(commands.cmd,(extent.width+7)/8,(extent.height+7)/8,1);
            imageBarrier(commands.cmd,frame.output.handle,1,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                compute,shaderWrite,compute|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            return readOutput(volume.output(frameSlot));
        };
        const auto compactFixture=[&](glm::vec4 value) {
            // GPU clear 全部 mip 的解析 fixture，直接检验 compact 上界和空场景 reset；无 CPU 节点构建。
            auto& frame=volume.impl_->resources->frames[lastSlot()];
            commands.begin();
            imageBarrier(commands.cmd,frame.volume.handle,frame.volume.mips,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
                compute,shaderRead|shaderWrite,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkClearColorValue clearValue{}; std::memcpy(clearValue.float32,&value,sizeof(value));
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,frame.volume.mips,0,1};
            vkCmdClearColorImage(commands.cmd,frame.volume.handle,VK_IMAGE_LAYOUT_GENERAL,&clearValue,1,&range);
            imageBarrier(commands.cmd,frame.volume.handle,frame.volume.mips,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,compute,VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            for(const auto* b:std::array<const Buffer*,2>{&frame.sparsePages,&frame.sparseCounters}) {
                bufferBarrier(commands.cmd,*b,compute,shaderRead|shaderWrite,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
                vkCmdFillBuffer(commands.cmd,b->handle,0,b->size,0);
                bufferBarrier(commands.cmd,*b,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,compute,shaderRead|shaderWrite);
            }
            bufferBarrier(commands.cmd,frame.sparseNodes,compute,shaderRead|shaderWrite,compute,shaderWrite);
            for(std::uint32_t level=0;level<frame.volume.mips;++level) {
                const auto size=volume.impl_->resources->resolution>>level,groups=(size+3)/4;
                vkCmdBindPipeline(commands.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,volume.impl_->pipelines[6]);
                vkCmdBindDescriptorSets(commands.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,volume.impl_->layout,0,1,&frame.sets[level],0,nullptr);
                const glm::ivec4 push(0,0,int(size),int(level));
                vkCmdPushConstants(commands.cmd,volume.impl_->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,16,&push);
                vkCmdDispatch(commands.cmd,groups,groups,groups);
                for(const auto* b:std::array<const Buffer*,3>{&frame.sparsePages,&frame.sparseNodes,&frame.sparseCounters})
                    bufferBarrier(commands.cmd,*b,compute,shaderRead|shaderWrite,compute,shaderRead|shaderWrite);
            }
            commands.submit();
        };
        const auto shEnergy=[&](int iterations) {
            auto& frame=volume.impl_->resources->frames[(slot+GpuVolumeGi::frame_slots-1)%GpuVolumeGi::frame_slots];
            commands.begin();
            bufferBarrier(commands.cmd,readback,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT,
                VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_READ_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            memoryBarrier(commands.cmd,compute,shaderRead|shaderWrite,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferCopy copy{0,0,frame.sh.size}; vkCmdCopyBuffer(commands.cmd,frame.sh.handle,readback.handle,1,&copy);
            memoryBarrier(commands.cmd,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,compute,shaderRead|shaderWrite);
            memoryBarrier(commands.cmd,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
            commands.submit(); checked(vmaInvalidateAllocation(allocator,readback.allocation,0,VK_WHOLE_SIZE),"test invalidate SH");
            const int count=int(volume.impl_->resources->resolution*volume.impl_->resources->resolution*volume.impl_->resources->resolution);
            const auto* data=static_cast<const float*>(readback.mapped);
            double frontier=0,accumulated=0;
            for(int i=0;i<count;++i) {
                frontier+=data[(iterations%2*count+i)*12]/.28209479177387814;
                accumulated+=data[(2*count+i)*12]/.28209479177387814;
            }
            return glm::dvec2(frontier,accumulated);
        };
        const auto rsm=evaluate(GiMode::rsm);
        const float expected=4*.4f/(pi*pi); // E=2, source rho=.5, A=4, receiver rho=.4, r=1.
        check("gpu_volume_rsm_flux_area",close(rsm.r,expected),"Lo="+std::to_string(rsm.r)+", expected="+std::to_string(expected));
        const auto rsm64=evaluate(GiMode::rsm,6);
        check("gpu_volume_rsm_resolution_normalization",close(rsm.r,rsm64.r),"32^2="+std::to_string(rsm.r)+", 64^2="+std::to_string(rsm64.r));
        commands.begin(); clear(5,{.25f,.25f,.25f,0}); clear(2,{.2f,.2f,.2f,0}); commands.submit();
        const auto quarter=evaluate(GiMode::rsm);
        check("gpu_volume_rsm_albedo_once",close(quarter.r,rsm.r*.25f),"both rho /2 -> Lo /4: "+std::to_string(quarter.r));
        commands.begin(); clear(5,{.5f,.5f,.5f,0}); clear(2,{.4f,.4f,.4f,0}); commands.submit();
        const auto oldLight=scene.lights[0]; scene.lights[0].kind=LightKind::point;
        Camera pointProjection; pointProjection.fov=glm::degrees(2*std::atan(1.0f/3)); pointProjection.near_plane=1; pointProjection.far_plane=5;
        lightVP=pointProjection.projection(1,false)*lightView;
        const auto point=evaluate(GiMode::rsm);
        check("gpu_volume_point_rsm_inverse_square",close(point.r,expected/9),"point Lo="+std::to_string(point.r));
        scene.lights[0]=oldLight; lightVP=orthographic*lightView;
        settings.propagation_steps=0; const auto injection=evaluate(GiMode::lpv); const auto injectedEnergy=shEnergy(0);
        settings.propagation_steps=6; const auto propagated=evaluate(GiMode::lpv); const auto propagatedEnergy=shEnergy(6);
        check("gpu_volume_lpv_neighbor_transport",propagated.r>1e-7f&&propagated.r>injection.r,
            "0 steps="+std::to_string(injection.r)+", 6 steps="+std::to_string(propagated.r));
        check("gpu_volume_lpv_sh_energy_bound",std::abs(injectedEnergy.x-4)<.01
            &&propagatedEnergy.x<=injectedEnergy.x*std::pow(.8,6)+1e-4
            &&propagatedEnergy.y<=injectedEnergy.x*(1-std::pow(.8,7))/.2+1e-4,
            "injected="+std::to_string(injectedEnergy.x)+", frontier="+std::to_string(propagatedEnergy.x)+", accumulated="+std::to_string(propagatedEnergy.y));
        {
            auto ceiling=scene.meshes[0]; for(auto& vertex:ceiling.vertices) vertex.position.y=.5f;
            scene.meshes.push_back(std::move(ceiling)); ++scene.revision; volume.configure(scene,extent,16);
            const auto wallBlocked=evaluate(GiMode::lpv);
            check("gpu_volume_lpv_geometry_absorption",wallBlocked.r<1e-6f,"ceiling occupancy blocks six-step transport: "+std::to_string(wallBlocked.r));
            scene.meshes.pop_back(); ++scene.revision; volume.configure(scene,extent,16);
        }
        const auto voxel=evaluate(GiMode::voxel);
        check("gpu_volume_conservative_voxel_mips_cones",voxel.r>1e-5f&&voxel.r<=2,
            "triangle surface -> GPU occupancy/radiance mips -> cone Lo="+std::to_string(voxel.r));
        const auto sparse16Pixels=lastPixels;
        settings.sparse_voxels=false; const auto dense16=evaluate(GiMode::voxel);
        check("gpu_volume_sparse_dense_hdr16",samePixels(sparse16Pixels,lastPixels)&&close(voxel.r,dense16.r),
            "all 64 HDR pixels/channels: sparse="+std::to_string(voxel.r)+", dense="+std::to_string(dense16.r));
        settings.sparse_voxels=true; evaluate(GiMode::voxel);
        const auto sparse16=snapshot(lastSlot());
        check("gpu_volume_sparse_pages_active_empty_mips16",auditSparse(sparse16,16)
            &&sparse16.counters.counts.z>0&&sparse16.counters.counts.z<sparse16.pages.size(),
            "GPU active="+std::to_string(sparse16.counters.counts.z)+", capacity=4681; every mip page/payload and empty page checked");
        const auto poisoned=traceCurrent(true);
        check("gpu_volume_sparse_trace_reads_ssbo",poisoned.r<1e-7f&&poisoned.g<1e-7f&&poisoned.b<1e-7f&&voxel.r>1e-5f,
            "clear only compact node SSBO -> zero Lo; dense radiance mips retain light");
        evaluate(GiMode::voxel);
        const auto isolatedSlot=lastSlot(); const auto beforeOtherSlot=snapshot(isolatedSlot);
        scene.lights[0].intensity=0; evaluate(GiMode::voxel);
        const auto afterOtherSlot=snapshot(isolatedSlot);
        bool distinctBuffers=true;
        for(std::uint32_t i=0;i<GpuVolumeGi::frame_slots;++i) for(std::uint32_t j=0;j<i;++j) {
            const auto& a=volume.impl_->resources->frames[i]; const auto& b=volume.impl_->resources->frames[j];
            distinctBuffers&=a.sparsePages.handle!=b.sparsePages.handle&&a.sparseNodes.handle!=b.sparseNodes.handle
                &&a.sparseCounters.handle!=b.sparseCounters.handle;
        }
        check("gpu_volume_sparse_frame_slot_isolation",distinctBuffers&&lastSlot()!=isolatedSlot
            &&beforeOtherSlot.pages==afterOtherSlot.pages
            &&std::memcmp(&beforeOtherSlot.counters,&afterOtherSlot.counters,sizeof(SparseCounters))==0
            &&std::memcmp(beforeOtherSlot.nodes.data(),afterOtherSlot.nodes.data(),beforeOtherSlot.nodes.size()*sizeof(glm::vec4))==0,
            "three independent buffer sets; other-slot zero-light rebuild preserves submitted slot pages/nodes/counters");
        scene.lights[0].intensity=0;
        const auto darkRsm=evaluate(GiMode::rsm),darkLpv=evaluate(GiMode::lpv),darkVoxel=evaluate(GiMode::voxel);
        check("gpu_volume_no_stale_light",darkRsm.r<1e-7f&&darkLpv.r<1e-7f&&darkVoxel.r<1e-7f,"zero light clears all three modes on reused slots");
        scene.lights[0]=oldLight;
        std::string error; const auto before=volume.output();
        auto rejected=volume.prepare_pipelines(shaders/"volume_missing_candidate",&error);
        const auto after=evaluate(GiMode::rsm);
        check("gpu_volume_failed_reload_preserves_live",!rejected&&!error.empty()&&volume.output().image==before.image&&close(after.r,rsm.r),error);
        auto candidate=volume.prepare_pipelines(shaders,&error);
        candidate.reset(); const auto canceled=evaluate(GiMode::rsm);
        check("gpu_volume_cancel_candidate",error.empty()&&close(canceled.r,rsm.r),"candidate destruction preserved live pipelines");
        candidate=volume.prepare_pipelines(shaders,&error);
        checked(vkDeviceWaitIdle(device),"test wait before transactional commit");
        const bool committed=volume.commit_pipelines(std::move(candidate)); const auto committedResult=evaluate(GiMode::rsm);
        check("gpu_volume_prepare_commit",committed&&close(committedResult.r,rsm.r),"all candidate pipelines swapped after idle");
        {
            // 临时候选只属于显式 GPU 诊断；修改合法 SPIR-V 的节点 stride，必须在创建该 pipeline 前拒绝。
            struct AbiFixture {
                std::filesystem::path directory;
                ~AbiFixture() {
                    if(directory.empty()) return;
                    std::error_code ignored; std::filesystem::remove_all(directory,ignored);
                }
            } abi;
            const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
            const auto volumeTempRoot=std::filesystem::canonical(std::filesystem::temp_directory_path());
            const auto fixturePath=volumeTempRoot/("EmberFrame-Volume-ABI-"+std::to_string(stamp));
            if(!std::filesystem::create_directory(fixturePath)) throw std::runtime_error("GpuVolumeGi: diagnostic ABI fixture collision");
            const auto resolvedFixture=std::filesystem::canonical(fixturePath);
            if(resolvedFixture.parent_path()!=volumeTempRoot||resolvedFixture.filename()!=fixturePath.filename())
                throw std::runtime_error("GpuVolumeGi: diagnostic ABI fixture escaped temporary directory");
            abi.directory=resolvedFixture; // 仅删除已核对绝对路径、由本诊断创建的 temp 子目录。
            for(const auto* file:GpuVolumeGi::shader_files()) {
                const auto name=std::string(file)+".spv";
                std::filesystem::copy_file(shaders/name,abi.directory/name);
            }
            auto module=read_spirv(abi.directory/"volume_sparse.comp.spv",ShaderStage::compute);
            const auto visit=[&](auto fn) {
                for(std::size_t p=5;p<module.words.size();p+=module.words[p]>>16)
                    fn(p,module.words[p]&0xffff,module.words[p]>>16);
            };
            std::uint32_t variable=0,pointer=0,block=0,array=0;
            visit([&](std::size_t p,auto op,auto size) {
                if(op==71&&size==4&&module.words[p+2]==33&&module.words[p+3]==15) variable=module.words[p+1];
            });
            visit([&](std::size_t p,auto op,auto size) {
                if(op==59&&size>=4&&module.words[p+2]==variable) pointer=module.words[p+1];
            });
            visit([&](std::size_t p,auto op,auto size) {
                if(op==32&&size==4&&module.words[p+1]==pointer) block=module.words[p+3];
            });
            visit([&](std::size_t p,auto op,auto size) {
                if(op==30&&size==3&&module.words[p+1]==block) array=module.words[p+2];
            });
            bool changed=false;
            visit([&](std::size_t p,auto op,auto size) {
                if(op==71&&size==4&&module.words[p+1]==array&&module.words[p+2]==6&&module.words[p+3]==16) {
                    module.words[p+3]=32; changed=true;
                }
            });
            if(!variable||!pointer||!block||!array||!changed) throw std::runtime_error("GpuVolumeGi: sparse ABI diagnostic could not find node stride");
            std::ofstream stream(abi.directory/"volume_sparse.comp.spv",std::ios::binary|std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(module.words.data()),std::streamsize(module.words.size()*sizeof(std::uint32_t)));
            stream.close(); if(!stream) throw std::runtime_error("GpuVolumeGi: sparse ABI diagnostic write failed");
            const auto baseline=evaluate(GiMode::voxel); const auto baselinePixels=lastPixels;
            auto incompatible=volume.prepare_pipelines(abi.directory,&error);
            const auto preserved=evaluate(GiMode::voxel);
            check("gpu_volume_sparse_reload_stride_rejected",!incompatible&&error.find("ABI")!=std::string::npos
                &&close(baseline.r,preserved.r)&&samePixels(baselinePixels,lastPixels),
                "candidate node stride 16 -> 32 rejected; seven live pipelines and sparse HDR preserved: "+error);
        }
        bool refusedResolution=false,refusedGeometry=false;
        try { volume.configure(scene,extent,24); } catch(const std::invalid_argument&) { refusedResolution=true; }
        Scene invalid;invalid.meshes.emplace_back();invalid.meshes[0].vertices.resize(3);invalid.meshes[0].indices={0,1,9};
        try { volume.configure(invalid,extent,16); } catch(const std::invalid_argument&) { refusedGeometry=true; }
        check("gpu_volume_bounds_fail_closed",refusedResolution&&refusedGeometry&&volume.output().image==before.image,
            "24 resolution and invalid triangle index rejected without resource replacement; valid >4096 geometry is no longer rejected");
        settings.voxel_resolution=32; volume.configure(scene,extent,32);
        const auto voxel32=evaluate(GiMode::voxel);
        check("gpu_volume_resolution32",voxel32.r>1e-5f,"32^3 conservative GPU volume Lo="+std::to_string(voxel32.r));
        const auto sparse32Pixels=lastPixels;
        settings.sparse_voxels=false; const auto dense32=evaluate(GiMode::voxel);
        check("gpu_volume_sparse_dense_hdr32",samePixels(sparse32Pixels,lastPixels)&&close(voxel32.r,dense32.r),
            "all 64 HDR pixels/channels: sparse="+std::to_string(voxel32.r)+", dense="+std::to_string(dense32.r));
        settings.sparse_voxels=true; evaluate(GiMode::voxel);
        const auto sparse32=snapshot(lastSlot());
        check("gpu_volume_sparse_pages_active_empty_mips32",auditSparse(sparse32,32)
            &&sparse32.counters.counts.z>0&&sparse32.counters.counts.z<sparse32.pages.size(),
            "GPU active="+std::to_string(sparse32.counters.counts.z)+", capacity=37449; includes guarded 2^3/1^3 mip dispatch edges");
        {
            auto& image=volume.impl_->resources->frames[(slot+GpuVolumeGi::frame_slots-1)%GpuVolumeGi::frame_slots].volume;
            commands.begin();
            bufferBarrier(commands.cmd,readback,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT,
                VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_READ_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            imageBarrier(commands.cmd,image.handle,image.mips,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                compute,shaderRead|shaderWrite,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
            std::array<VkBufferImageCopy,2> copies{};
            copies[0].imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copies[0].imageExtent={32,32,32};
            copies[1].bufferOffset=32*32*32*sizeof(glm::vec4);
            copies[1].imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,1,0,1}; copies[1].imageExtent={16,16,16};
            vkCmdCopyImageToBuffer(commands.cmd,image.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle,2,copies.data());
            imageBarrier(commands.cmd,image.handle,image.mips,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,compute,shaderRead|shaderWrite);
            memoryBarrier(commands.cmd,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
            commands.submit(); checked(vmaInvalidateAllocation(allocator,readback.allocation,0,VK_WHOLE_SIZE),"test invalidate radiance mips");
            const auto* data=static_cast<const glm::vec4*>(readback.mapped);
            const auto base=data[16+32*(16+32*16)],mip=data[32*32*32+8+16*(7+16*8)];
            check("gpu_volume_sat_interior_and_optical_mip",close(base.a,1)&&close(mip.a,.75f)&&base.r>0&&mip.r>0,
                "triangle interior alpha="+std::to_string(base.a)+", parent alpha="+std::to_string(mip.a));
        }
        {
            const auto basePage=sparse32.pages[16+32*(16+32*16)];
            const auto mipPage=sparse32.pages[32*32*32+8+16*(7+16*8)];
            const bool mapped=basePage>0&&basePage<=sparse32.nodes.size()&&mipPage>0&&mipPage<=sparse32.nodes.size();
            check("gpu_volume_sparse_optical_opacity",mapped&&close(sparse32.nodes[basePage-1].a,1)
                &&close(sparse32.nodes[mipPage-1].a,.75f)&&sparse32.nodes[mipPage-1].r>0,
                "compact SSBO preserves base alpha=1, optical mip alpha=.75 and premultiplied radiance");
        }
        const auto fullCapacity=volume.impl_->resources->sparseCapacity;
        volume.impl_->resources->sparseCapacity=1; evaluate(GiMode::voxel);
        const auto overflow=snapshot(lastSlot());
        const auto storedPages=std::count_if(overflow.pages.begin(),overflow.pages.end(),[](auto entry) { return entry!=0; });
        check("gpu_volume_sparse_capacity_guard",overflow.counters.counts.x==sparse32.counters.counts.x
            &&overflow.counters.counts.z==1&&overflow.counters.counts.y==overflow.counters.counts.x-1&&storedPages==1
            &&std::all_of(overflow.pages.begin(),overflow.pages.end(),[](auto entry) { return entry<=1; }),
            "capacity=1: attempted="+std::to_string(overflow.counters.counts.x)+", stored=1, overflow="
                +std::to_string(overflow.counters.counts.y)+"; no out-of-range page published");
        volume.impl_->resources->sparseCapacity=fullCapacity; const auto recovered=evaluate(GiMode::voxel);
        const auto recoveredSparse=snapshot(lastSlot());
        check("gpu_volume_sparse_counter_reset_after_overflow",auditSparse(recoveredSparse,32)&&close(recovered.r,voxel32.r),
            "restore normal capacity -> all counters reset and original HDR restored");
        compactFixture({.25f,.5f,.75f,1});
        const auto fullSparse=snapshot(lastSlot());
        check("gpu_volume_sparse_full_capacity_partial_edges",auditSparse(fullSparse,32)
            &&fullSparse.counters.counts.z==fullCapacity&&fullSparse.counters.counts.x==37449
            &&fullSparse.counters.levels[1].x==8&&fullSparse.counters.levels[1].y==1,
            "GPU all-occupied fixture stores exactly 37449 nodes; guarded 2^3 and 1^3 mips store exactly 8 and 1");
        compactFixture(glm::vec4(0));
        const auto emptySparse=snapshot(lastSlot()); const auto emptyTrace=traceCurrent(false);
        check("gpu_volume_sparse_empty_reset",auditSparse(emptySparse,32)&&emptySparse.counters.counts.x==0
            &&emptySparse.counters.counts.z==0&&glm::all(glm::lessThan(glm::abs(emptyTrace),glm::vec3(1e-7f))),
            "GPU empty fixture resets full page table/counters; stale compact payload is unreachable and cone output is zero");
        scene.meshes.clear(); ++scene.revision; volume.configure(scene,extent,32);
        const auto emptyScene=evaluate(GiMode::voxel); const auto emptySceneSparse=snapshot(lastSlot());
        check("gpu_volume_sparse_empty_scene",auditSparse(emptySceneSparse,32)&&emptySceneSparse.counters.counts.z==0
            &&glm::all(glm::lessThan(glm::abs(emptyScene),glm::vec3(1e-7f))),
            "normal voxelize -> mip -> compact -> trace with zero world triangles yields zero active nodes and zero HDR");
        {
            // 旧上限的两倍以上：完整顶点/索引进入 BVH，叶子范围无重叠、无遗漏。
            Scene large;large.materials.emplace_back();large.meshes.emplace_back();
            auto& mesh=large.meshes[0];mesh.vertices.resize(3);
            mesh.vertices[0].position={-1,0,-1};mesh.vertices[1].position={1,0,-1};mesh.vertices[2].position={0,0,1};
            for(int i=0;i<10000;++i)mesh.indices.insert(mesh.indices.end(),{0,2,1});
            volume.configure(large,extent,16,GiMode::rsm);
            check("gpu_volume_large_rsm_without_voxel_geometry",volume.impl_->resources->triangleCount==0
                &&volume.impl_->resources->bvhCount==0,"10000 instanced triangles; RSM needs raster only, no triangle/BVH SSBO upload");
            volume.configure(large,extent,16,GiMode::voxel);
            const auto& resources=*volume.impl_->resources;
            const auto* nodes=static_cast<const GeometryNode*>(resources.bvhUpload.mapped);
            std::vector<unsigned> coverage(resources.triangleCount,0);bool validTree=resources.bvhCount>1;
            for(std::uint32_t i=0;i<resources.bvhCount;++i) {
                const auto& node=nodes[i];validTree&=node.range.z>i&&node.range.z<=resources.bvhCount;
                if(node.range.x>coverage.size()||node.range.y>coverage.size()-node.range.x)validTree=false;
                else for(std::uint32_t t=0;t<node.range.y;++t)++coverage[node.range.x+t];
            }
            check("gpu_volume_complete_large_geometry_bvh",validTree&&resources.triangleCount==10000
                &&std::all_of(coverage.begin(),coverage.end(),[](auto visits){return visits==1;}),
                "10000 triangles retained exactly once; stackless escape links stay inside uploaded BVH");
            Light point;point.kind=LightKind::point;point.position={0,2,0};large.lights.push_back(point);
            const auto projection=GpuVolumeGi::rsm_projection(large,{-1,-1,-1},{1,1,1});
            const std::array<glm::vec3,6> directions={glm::vec3(1,0,0),{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
            bool covered=projection.faceCount==6&&projection.lightIndex==0;
            for(std::size_t face=0;face<6;++face) {
                auto clip=projection.viewProjection[face]*glm::vec4(point.position+directions[face],1);
                auto ndc=glm::vec3(clip)/clip.w;
                covered&=finite(ndc)&&clip.w>0&&std::abs(ndc.x)<1e-5f&&std::abs(ndc.y)<1e-5f&&ndc.z>=0&&ndc.z<=1;
            }
            check("gpu_volume_point_six_direction_projection",covered,"all six point-light axes have a matching Vulkan-Z RSM view");
            large.lights[0].kind=LightKind::rectangle;large.lights[0].direction={0,-1,0};
            const auto area=GpuVolumeGi::rsm_projection(large,{-1,-1,-1},{1,1,1});
            auto center=area.viewProjection[3]*glm::vec4(0,0,0,1);
            check("gpu_volume_rectangle_center_projection",area.faceCount==6&&area.lightIndex==0
                &&center.w>0&&std::abs(center.x/center.w)<1e-5f&&std::abs(center.y/center.w)<1e-5f,
                "area source enabled with six-face coverage and explicit one-sided lamp-center radiance approximation");
            large.lights.clear();
            check("gpu_volume_no_invented_primary_source",GpuVolumeGi::rsm_projection(large,{-1,-1,-1},{1,1,1}).lightIndex<0,
                "empty light table selects no GI source, not a synthesized default directional light");
            scene=std::move(large);scene.lights.push_back(light);++scene.revision;
            settings.voxel_resolution=16;volume.configure(scene,extent,16);
            const auto largeVoxel=evaluate(GiMode::voxel);const auto largeSparse=snapshot(lastSlot());
            check("gpu_volume_complete_large_geometry_gpu",volume.acceleration_stats().uploadedTriangles==10000
                &&largeSparse.counters.counts.z>0&&largeVoxel.r>1e-5f,
                "10000 triangles upload to device-local buffers, execute BVH voxel queries, compact and produce nonzero GPU indirect");
        }
        scene.lights.clear();++scene.revision;settings.voxel_resolution=32;volume.configure(scene,extent,32);
        const auto noSourceRsm=evaluate(GiMode::rsm),noSourceLpv=evaluate(GiMode::lpv),noSourceVoxel=evaluate(GiMode::voxel);
        check("gpu_volume_no_light_zero_indirect",glm::length(noSourceRsm)<1e-7f&&glm::length(noSourceLpv)<1e-7f
            &&glm::length(noSourceVoxel)<1e-7f,"no lights and no emissive geometry yields zero RSM/LPV/VCT indirect on GPU");
        checked(vkDeviceWaitIdle(device),"test finish idle");
    } catch(const std::exception& e) { results.push_back({"gpu_volume_setup_execution",false,e.what()}); }
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_GPU_VOLUME_SELF_TEST
#include <atomic>
#include <iostream>
namespace {
std::atomic<unsigned> volumeValidationErrors=0;
VKAPI_ATTR VkBool32 VKAPI_CALL volumeDebug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* message,void*) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) { ++volumeValidationErrors; std::cerr<<message->pMessage<<'\n'; }
    return VK_FALSE;
}
}
int main(int argc,char** argv) {
    VkInstance instance{}; VkDevice device{}; VmaAllocator allocator{}; VkDebugUtilsMessengerEXT messenger{};
    int status=1;
    try {
        if(argc!=2) throw std::invalid_argument("Pass the compiled volume shader directory");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName="GpuVolumeGi diagnostics"; app.apiVersion=VK_API_VERSION_1_3;
        const char* layer="VK_LAYER_KHRONOS_validation"; const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkValidationFeatureEnableEXT sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validation.enabledValidationFeatureCount=1; validation.pEnabledValidationFeatures=&sync;
        VkDebugUtilsMessengerCreateInfoEXT dc{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dc.pNext=&validation; dc.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dc.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dc.pfnUserCallback=volumeDebug;
        VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ic.pApplicationInfo=&app;
        ic.enabledLayerCount=1; ic.ppEnabledLayerNames=&layer; ic.enabledExtensionCount=1; ic.ppEnabledExtensionNames=&extension; ic.pNext=&dc;
        emberframe::lab::checked(vkCreateInstance(&ic,nullptr,&instance),"diagnostic instance");
        dc.pNext=nullptr;
        auto createDebug=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
        emberframe::lab::checked(createDebug(instance,&dc,nullptr,&messenger),"diagnostic messenger");
        std::uint32_t count=0; emberframe::lab::checked(vkEnumeratePhysicalDevices(instance,&count,nullptr),"enumerate devices");
        std::vector<VkPhysicalDevice> devices(count); emberframe::lab::checked(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"enumerate devices");
        VkPhysicalDevice physical{}; std::uint32_t family=0;
        for(auto candidate:devices) {
            VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(candidate,&properties);
            if(properties.apiVersion<VK_API_VERSION_1_3) continue;
            std::uint32_t n=0; vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,nullptr);
            std::vector<VkQueueFamilyProperties> families(n); vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,families.data());
            for(std::uint32_t i=0;i<n;++i) if((families[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) {
                physical=candidate; family=i; break;
            }
            if(physical) { std::cout<<"Device: "<<properties.deviceName<<'\n'; break; }
        }
        if(!physical) throw std::runtime_error("No Vulkan 1.3 graphics+compute device");
        const float priority=1;
        VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qc.queueFamilyIndex=family; qc.queueCount=1; qc.pQueuePriorities=&priority;
        VkPhysicalDeviceVulkan13Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features.dynamicRendering=VK_TRUE; features.synchronization2=VK_TRUE;
        VkDeviceCreateInfo dev{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dev.pNext=&features; dev.queueCreateInfoCount=1; dev.pQueueCreateInfos=&qc;
        emberframe::lab::checked(vkCreateDevice(physical,&dev,nullptr,&device),"diagnostic device");
        VkQueue queue{}; vkGetDeviceQueue(device,family,0,&queue);
        VmaAllocatorCreateInfo ac{}; ac.physicalDevice=physical; ac.device=device; ac.instance=instance; ac.vulkanApiVersion=VK_API_VERSION_1_3;
        emberframe::lab::checked(vmaCreateAllocator(&ac,&allocator),"diagnostic allocator");
        const auto results=emberframe::lab::test_gpu_volume(physical,device,allocator,queue,family,std::filesystem::path(argv[1]));
        status=0;
        for(const auto& result:results) { std::cout<<(result.passed?"PASS ":"FAIL ")<<result.name<<": "<<result.detail<<'\n'; if(!result.passed) status=1; }
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; status=1; }
    if(device) vkDeviceWaitIdle(device);
    if(allocator) vmaDestroyAllocator(allocator);
    if(device) vkDestroyDevice(device,nullptr);
    if(messenger) {
        auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));
        destroy(instance,messenger,nullptr);
    }
    if(instance) vkDestroyInstance(instance,nullptr);
    std::cout<<"Validation errors: "<<volumeValidationErrors.load()<<'\n';
    return volumeValidationErrors?1:status;
}
#endif
