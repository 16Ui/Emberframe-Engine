#include "gpu_scene_resources.h"
#include "scene_resources.h"
#include <cstring>
#include <utility>

namespace emberframe::lab {
namespace {
void scene_check(VkResult r,const char* operation) {
    if(r!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+": VkResult "+std::to_string(r));
}
struct SceneBuffer {
    VmaAllocator allocator{};VkBuffer handle{};VmaAllocation allocation{};void* mapped{};
    ~SceneBuffer(){if(handle)vmaDestroyBuffer(allocator,handle,allocation);}
};
void allocate_scene_buffer(SceneBuffer& b,VmaAllocator allocator,VkDeviceSize bytes,VkBufferUsageFlags usage,bool host) {
    b.allocator=allocator;VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=bytes;ci.usage=usage;
    VmaAllocationCreateInfo ai{};ai.usage=host?VMA_MEMORY_USAGE_AUTO:VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if(host)ai.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
    else ai.requiredFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VmaAllocationInfo info{};scene_check(vmaCreateBuffer(allocator,&ci,&ai,&b.handle,&b.allocation,&info),"scene buffer");b.mapped=info.pMappedData;
}
// 每项显式 16 字节对齐；不把 glm::vec3 放进 std140 数组。
struct alignas(16) SceneUniform {
    std::array<glm::vec4,9> radiance_sh{};
    glm::vec4 sdf_min{},sdf_max{};
    glm::ivec4 grid{}; // xyz 格点数，w 是否可查询。
    glm::vec4 trace{}; // Lipschitz、格点覆盖半径、softness、world bias。
    glm::ivec4 modes{}; // environment diffuse、PRT ready、SDF requested、保留。
};
static_assert(sizeof(SceneUniform)==224&&offsetof(SceneUniform,modes)==208);
}
struct GpuSceneResources::SdfUpload::Impl {
    SceneBuffer buffer;
    std::shared_ptr<const SceneSdfGrid> grid;
};
GpuSceneResources::SdfUpload::SdfUpload(std::unique_ptr<Impl> p):impl_(std::move(p)){}
GpuSceneResources::SdfUpload::~SdfUpload()=default;
VkBuffer GpuSceneResources::SdfUpload::buffer() const noexcept{return impl_->buffer.handle;}
std::span<const std::byte> GpuSceneResources::SdfUpload::bytes() const noexcept {
    return {reinterpret_cast<const std::byte*>(impl_->grid->values.data()),impl_->grid->values.size()*sizeof(float)};
}
struct GpuSceneResources::Impl {
    VkDevice device{};VmaAllocator allocator{};VkDeviceSize max_range{};
    VkDescriptorSetLayout layout{};VkDescriptorPool pool{};
    SceneBuffer dummy;
    struct Frame {SceneBuffer uniform;VkDescriptorSet set{};std::shared_ptr<SdfUpload> owner;};
    std::array<Frame,frame_count> frames;
    bool prt_ready=false,sdf_ready=false;
    ~Impl(){if(pool)vkDestroyDescriptorPool(device,pool,nullptr);if(layout)vkDestroyDescriptorSetLayout(device,layout,nullptr);}
};
GpuSceneResources::GpuSceneResources(VkPhysicalDevice physical,VkDevice device,VmaAllocator allocator):impl_(std::make_unique<Impl>()) {
    auto& p=*impl_;p.device=device;p.allocator=allocator;
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);p.max_range=properties.limits.maxStorageBufferRange;
    if(properties.limits.maxBoundDescriptorSets<5)throw std::runtime_error("SH/PRT/SDF requires five descriptor sets");
    std::array<VkDescriptorSetLayoutBinding,2> bindings{{{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}}};
    VkDescriptorSetLayoutCreateInfo lc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};lc.bindingCount=2;lc.pBindings=bindings.data();scene_check(vkCreateDescriptorSetLayout(device,&lc,nullptr,&p.layout),"scene layout");
    std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,frame_count},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,frame_count}}};
    VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.maxSets=frame_count;pc.poolSizeCount=2;pc.pPoolSizes=sizes.data();scene_check(vkCreateDescriptorPool(device,&pc,nullptr,&p.pool),"scene descriptor pool");
    // 即使没有 SDF 也绑定合法 buffer；shader 的 grid.w 禁止访问占位格点。
    allocate_scene_buffer(p.dummy,allocator,sizeof(float),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true);
    *static_cast<float*>(p.dummy.mapped)=0;scene_check(vmaFlushAllocation(allocator,p.dummy.allocation,0,VK_WHOLE_SIZE),"scene dummy flush");
    for(auto& frame:p.frames){
        allocate_scene_buffer(frame.uniform,allocator,sizeof(SceneUniform),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,true);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=p.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&p.layout;scene_check(vkAllocateDescriptorSets(device,&ai,&frame.set),"scene descriptor");
    }
}
GpuSceneResources::~GpuSceneResources()=default;
VkDescriptorSetLayout GpuSceneResources::descriptor_layout() const noexcept{return impl_->layout;}
VkDescriptorSet GpuSceneResources::descriptor_set(std::uint32_t index) const{return impl_->frames.at(index).set;}
std::shared_ptr<GpuSceneResources::SdfUpload> GpuSceneResources::prepare_sdf(std::shared_ptr<const SceneSdfGrid> grid) {
    if(!grid)return {};
    const auto n=grid->resolution;
    if(n<4||n>64||grid->values.size()!=std::size_t(n)*n*n||grid->values.size()>impl_->max_range/sizeof(float))throw std::invalid_argument("GPU scene SDF grid size/range invalid");
    for(int axis=0;axis<3;++axis)if(!std::isfinite(grid->min[axis])||!std::isfinite(grid->max[axis])||grid->max[axis]<=grid->min[axis])throw std::invalid_argument("GPU scene SDF bounds invalid");
    auto payload=std::make_unique<SdfUpload::Impl>();payload->grid=std::move(grid);
    allocate_scene_buffer(payload->buffer,impl_->allocator,payload->grid->values.size()*sizeof(float),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,false);
    return std::shared_ptr<SdfUpload>(new SdfUpload(std::move(payload)));
}
void GpuSceneResources::configure(const Scene& scene,const Settings& settings,std::uint32_t index,std::shared_ptr<SdfUpload> sdf,bool prt_ready,bool sdf_ready) {
    auto& p=*impl_;auto& frame=p.frames.at(index);SceneUniform u;
    // 解析线性天空仅有 l=0 与 world-Y l=1；这是 radiance 系数，卷积在片元完成。
    const glm::vec3 mean=(scene.sky_top+scene.sky_bottom)*.5f,gradient=(scene.sky_top-scene.sky_bottom)*.5f;
    u.radiance_sh[0]=glm::vec4(mean*std::sqrt(4*pi),0);
    u.radiance_sh[1]=glm::vec4(gradient*std::sqrt(4*pi/3),0);
    // 任意非负合法天空与 CPU 烘焙 SH 一致；缓存提供完整 SH9 时保留全部系数。
    if(prt_ready&&scene.baked_resources)for(std::size_t i=0;i<9;++i)u.radiance_sh[i]=glm::vec4(scene.baked_resources->environment_sh9[i],0);
    p.prt_ready=prt_ready;p.sdf_ready=sdf_ready;
    const bool has_sdf=sdf_ready&&bool(sdf); // 空场景无需距离格点，仍是已完成且全可见。
    u.modes={int(settings.environment_diffuse),prt_ready?1:0,settings.sdf_shadows?1:0,0};
    if(has_sdf){
        const auto& grid=*sdf->impl_->grid;const auto cell=(grid.max-grid.min)/float(grid.resolution-1);
        u.sdf_min=glm::vec4(grid.min,0);u.sdf_max=glm::vec4(grid.max,0);u.grid={grid.resolution,grid.resolution,grid.resolution,1};
        // 原世界空间 unsigned 距离为 1-Lipschitz；覆盖修正给出其距离下界。
        u.trace={1.f,glm::length(cell)*.5f,std::max(.001f,settings.sdf_softness),std::max(0.f,settings.shadow_bias)};
    }
    std::memcpy(frame.uniform.mapped,&u,sizeof(u));scene_check(vmaFlushAllocation(p.allocator,frame.uniform.allocation,0,VK_WHOLE_SIZE),"scene uniform flush");
    std::array<VkDescriptorBufferInfo,2> buffers{{{frame.uniform.handle,0,sizeof(u)},{has_sdf?sdf->buffer():p.dummy.handle,0,has_sdf?sdf->bytes().size():sizeof(float)}}};
    std::array<VkWriteDescriptorSet,2> writes{};
    for(std::uint32_t i=0;i<2;++i){auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=frame.set;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;w.pBufferInfo=&buffers[i];}
    vkUpdateDescriptorSets(p.device,2,writes.data(),0,nullptr);frame.owner=has_sdf?std::move(sdf):nullptr;
}
bool GpuSceneResources::ready(const Settings& settings) const noexcept {
    return (settings.environment_diffuse!=EnvironmentDiffuse::prt||impl_->prt_ready)&&(!settings.sdf_shadows||impl_->sdf_ready);
}
} // namespace emberframe::lab
