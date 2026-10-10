#ifdef EMBERFRAME_GPU_EFFECTS_SELF_TEST
#define VMA_IMPLEMENTATION // 仅独立测试程序；主工程继续复用 renderer 的唯一 VMA 实现。
#endif
#include "gpu_effects.h"
#include "effects_motion.h"
#include "effects_motion_tests.h"
#include "systems.h"
#include "shader_assets.h"
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <utility>

namespace emberframe::lab {
namespace {
constexpr VkFormat effects_format=VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr std::uint32_t sample_bindings=16,sets_per_slot=64;
constexpr std::array<const char*,10> shader_names={
    "effects_occlusion.comp","effects_trace.comp","effects_compose.comp","effects_temporal.comp",
    "effects_filter.comp","effects_snapshot.comp","effects_bloom.comp","effects_npr.comp",
    "effects_debug.comp","effects_copy.comp"};
enum EffectPipeline:std::size_t { ao_pipeline,trace_pipeline,compose_pipeline,temporal_pipeline,
    filter_pipeline,snapshot_pipeline,bloom_pipeline,npr_pipeline,debug_pipeline,copy_pipeline };
void effects_check(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS)throw std::runtime_error(std::string("GpuEffects ")+operation+": "+std::to_string(result));
}
float effects_parameter(float value,float fallback,float low,float high) {
    return std::isfinite(value) ? std::clamp(value,low,high):fallback;
}
float effects_radical(std::uint64_t n,std::uint64_t base) {
    double factor=1,result=0;while(n){factor/=double(base);result+=double(n%base)*factor;n/=base;}return float(result);
}
struct alignas(16) EffectsUniform {
    glm::mat4 vp{1},previous_vp{1},previous_inverse_vp{1},view{1},inverse_view{1};
    glm::vec4 camera_near{},sky_top_far{},sky_bottom_radius{};
    glm::ivec4 dimensions{},modes{},flags{};
    glm::vec4 temporal{},jitter{};
    glm::ivec4 extensions{};
};
struct EffectsPush { glm::ivec4 control{};glm::vec4 parameters{}; };
static_assert(sizeof(EffectsUniform)==464&&offsetof(EffectsUniform,extensions)==448);
static_assert(sizeof(EffectsPush)==32);
bool effects_finite(const glm::mat4& m) {
    for(int c=0;c<4;c++)for(int r=0;r<4;r++)if(!std::isfinite(m[c][r]))return false;return true;
}
// 显式列字段，不能 hash Settings 的 padding；只要会改变历史信号就清历史。
std::uint64_t effects_key(const EffectsInputs& i) {
    std::uint64_t h=14695981039346656037ull;
    auto add=[&](std::uint32_t v){h^=v;h*=1099511628211ull;};auto f=[&](float v){add(std::bit_cast<std::uint32_t>(v));};
    const auto& s=i.settings;
    for(auto v:{int(s.path),int(s.shadows),int(s.gi),int(s.ao),int(s.shading),int(s.culling),int(s.filter),int(s.debug),
        int(s.reversed_z),int(s.energy_compensation),int(s.bloom),int(s.taa),int(s.denoise),int(s.svgf),
        int(s.outline),int(s.hatching),s.samples,s.max_bounces,s.shadow_resolution,s.voxel_resolution,s.propagation_steps})add(std::uint32_t(v));
    for(float v:{s.bloom_strength,s.bloom_threshold,s.ao_radius,s.ao_strength,s.shadow_bias,s.light_size,s.temporal_weight,
        i.camera.fov,i.camera.near_plane,i.camera.far_plane,i.sky_top.x,i.sky_top.y,i.sky_top.z,
        i.sky_bottom.x,i.sky_bottom.y,i.sky_bottom.z})f(v);
    add(s.seed);add(bool(i.optional_tangent));add(bool(i.optional_meta));add(bool(i.indirect_baseline));return h;
}
struct EffectsOwnedImage {
    VkDevice device{};VmaAllocator allocator{};VkImage image{};VkImageView view{};VmaAllocation allocation{};
    VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage=VK_PIPELINE_STAGE_2_NONE;VkAccessFlags2 access=VK_ACCESS_2_NONE;
    std::uint32_t width=0,height=0;VkDeviceSize bytes=0;
    EffectsOwnedImage()=default;
    EffectsOwnedImage(const EffectsOwnedImage&)=delete;EffectsOwnedImage& operator=(const EffectsOwnedImage&)=delete;
    EffectsOwnedImage(EffectsOwnedImage&& other) noexcept {swap(other);}
    EffectsOwnedImage& operator=(EffectsOwnedImage&& other) noexcept {if(this!=&other){reset();swap(other);}return *this;}
    ~EffectsOwnedImage(){reset();}
    void swap(EffectsOwnedImage& o) noexcept {
        std::swap(device,o.device);std::swap(allocator,o.allocator);std::swap(image,o.image);std::swap(view,o.view);
        std::swap(allocation,o.allocation);std::swap(layout,o.layout);std::swap(stage,o.stage);std::swap(access,o.access);
        std::swap(width,o.width);std::swap(height,o.height);std::swap(bytes,o.bytes);
    }
    void reset() noexcept {
        if(view)vkDestroyImageView(device,view,nullptr);if(image)vmaDestroyImage(allocator,image,allocation);
        view={};image={};allocation={};bytes=0;layout=VK_IMAGE_LAYOUT_UNDEFINED;stage=VK_PIPELINE_STAGE_2_NONE;access=0;
    }
    EffectsOutput output() const noexcept {return {image,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,effects_format,width,height};}
};
struct EffectsBuffer {
    VmaAllocator allocator{};VkBuffer buffer{};VmaAllocation allocation{};void* mapped{};
    VkDeviceSize capacity=0;
    void reset() noexcept {if(buffer)vmaDestroyBuffer(allocator,buffer,allocation);buffer={};allocation={};mapped=nullptr;capacity=0;}
    ~EffectsBuffer(){if(buffer)vmaDestroyBuffer(allocator,buffer,allocation);}
};
struct EffectsPipelineBundle {
    VkDevice device{};std::array<VkPipeline,shader_names.size()> pipelines{};
    ~EffectsPipelineBundle(){for(auto p:pipelines)if(p)vkDestroyPipeline(device,p,nullptr);}
};
std::vector<std::uint32_t> effects_spirv(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)throw std::runtime_error("Cannot open effects SPIR-V: "+path.string());auto length=file.tellg();
    if(length<20||length>8*1024*1024||std::uint64_t(length)%4)throw std::runtime_error("Invalid effects SPIR-V size: "+path.string());
    std::vector<std::uint32_t> code(std::size_t(length)/4);file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(code.data()),std::streamsize(length)))throw std::runtime_error("Truncated effects SPIR-V");
    if(code[0]!=0x07230203||code[1]>0x00010300||code[3]>1000000||code[4]!=0)throw std::runtime_error("Effects require SPIR-V <= 1.3, valid header");
    bool compute=false,local=false;
    // 先拒绝不兼容 stage/capability/set；下面再通过结构反射核对 host ABI。
    for(std::size_t p=5;p<code.size();) {
        auto count=code[p]>>16,op=code[p]&65535;
        if(!count||p+count>code.size())throw std::runtime_error("Malformed effects SPIR-V instruction");
        // Shader + ImageQuery（textureSize/imageSize）均为 Vulkan 核心，不需可选 feature。
        if(op==17&&(count!=2||(code[p+1]!=1&&code[p+1]!=50)))throw std::runtime_error("Effects SPIR-V requests an optional capability");
        if(op==15&&count>=4){if(code[p+1]!=5)throw std::runtime_error("Effects entry point is not compute");compute=true;}
        if(op==16&&count>=6&&code[p+2]==17) {local=code[p+3]==8&&code[p+4]==8&&code[p+5]==1;}
        if(op==71&&count>=4&&code[p+2]==34&&code[p+3]!=0)throw std::runtime_error("Effects must use their independent set=0");
        if(op==71&&count>=4&&code[p+2]==33&&code[p+3]>19)throw std::runtime_error("Unknown effects descriptor binding");
        p+=count;
    }
    if(!compute||!local)throw std::runtime_error("Effects require literal LocalSize 8,8,1 (no maintenance4/LocalSizeId)");
    // 使用主工程已有的结构反射；新增的是一个普通 SSBO 的 runtime 数据数组。
    // 对其元素 stride/成员偏移/矩阵 stride 作精确 ABI 检查，拒绝“编译成功但读错内存”。
    const auto reflection=reflect_spirv(code,ShaderStage::compute);
    if(!reflection.complete)throw std::runtime_error("Incomplete effects shader ABI reflection");
    static const auto motion_hash=shader_content_hash(
        "struct{0:runtime:80<struct{0:mat4:16:0<vec4<22:32>>;64:vec4<21:32:0>;}>;}");
    bool has_motion=false;
    for(const auto& d:reflection.descriptors) {
        const auto expected=d.binding<16 ? ShaderDescriptorKind::combined_image_sampler:
            (d.binding<18 ? ShaderDescriptorKind::storage_image:(d.binding==18 ? ShaderDescriptorKind::uniform_buffer:ShaderDescriptorKind::storage_buffer));
        if(d.set!=0||d.binding>19||d.kind!=expected||d.count!=1||d.runtime_array)
            throw std::runtime_error("Effects descriptor ABI does not match host layout");
        if(d.binding==18&&d.minimum_buffer_bytes!=sizeof(EffectsUniform))throw std::runtime_error("Effects UBO ABI size mismatch");
        if(d.binding==19) {
            has_motion=true;
            if(d.type_layout_hash!=motion_hash)throw std::runtime_error("Effects motion SSBO ABI must be std430 stride 80 (mat4 + uvec4)");
        }
    }
    if((path.filename()=="effects_temporal.comp.spv"||path.filename()=="effects_snapshot.comp.spv")&&!has_motion)
        throw std::runtime_error("Effects rigid-motion shaders need binding 19; rebuild the complete shader set");
    for(const auto& p:reflection.push_constants)if(p.offset!=0||p.size!=sizeof(EffectsPush))throw std::runtime_error("Effects push constant ABI mismatch");
    return code;
}
}

struct GpuEffects::Impl {
    VkPhysicalDevice physical{};VkDevice device{};VmaAllocator allocator{};VkPipelineCache cache{};
    VkPhysicalDeviceProperties properties{};VkDescriptorSetLayout set_layout{};VkPipelineLayout pipeline_layout{};
    VkDescriptorPool pool{};VkSampler sampler{};std::unique_ptr<EffectsPipelineBundle> bundle;
    std::array<std::array<VkDescriptorSet,sets_per_slot>,2> sets{};std::array<EffectsBuffer,2> uniforms;
    std::array<EffectsBuffer,2> motions;
    std::array<std::vector<EffectsObjectMotion>,2> motion_records;
    bool graph_dispatch=false;
    std::uint32_t graph_sample_mask=0xffff;
    GraphPlan post_plan;
    std::vector<EffectsGraphImage> graph_images;
    std::string graph_output;
    VkDeviceSize uniform_stride=0;std::uint32_t width=0,height=0,slot=0,set_cursor=0,uniform_phase=0;
    std::uint64_t serial=0,last_revision=0,last_key=0,current_key=0;bool history_ready=false,frame_open=false;
    const char* pending_reset="initialization";EffectsInputs frame_inputs;Camera last_camera;
    EffectsDiagnostics diagnostics;
    EffectsOwnedImage ao_raw,ao,trace,composed,indirect_image,motion_image,sink,dummy_image;
    std::array<EffectsOwnedImage,2> scratch,scratch_variance;
    std::array<EffectsOwnedImage,2> history_position,history_normal,history_meta;
    std::array<EffectsOwnedImage,2> denoise_color,denoise_moments,taa_color,taa_moments;
    std::vector<EffectsOwnedImage> bloom_down,bloom_up;
    EffectsOwnedImage* variance_result=nullptr;
    Impl(VkPhysicalDevice p,VkDevice d,VmaAllocator a,VkPipelineCache c):physical(p),device(d),allocator(a),cache(c) {}
    ~Impl() {
        bundle.reset();if(pool)vkDestroyDescriptorPool(device,pool,nullptr);
        if(pipeline_layout)vkDestroyPipelineLayout(device,pipeline_layout,nullptr);
        if(set_layout)vkDestroyDescriptorSetLayout(device,set_layout,nullptr);if(sampler)vkDestroySampler(device,sampler,nullptr);
    }
    void setup();
    std::unique_ptr<EffectsPipelineBundle> pipelines(const std::filesystem::path&) const;
    EffectsOwnedImage make_image(std::uint32_t,std::uint32_t) const;
    std::vector<EffectsOwnedImage*> images();
    void barrier(VkCommandBuffer,VkImage,VkImageLayout,VkImageLayout,VkPipelineStageFlags2,VkAccessFlags2,
                 VkPipelineStageFlags2,VkAccessFlags2);
    void transition(VkCommandBuffer,EffectsOwnedImage&,VkImageLayout,VkPipelineStageFlags2,VkAccessFlags2);
    EffectsImageInput read(VkCommandBuffer,EffectsOwnedImage&);
    void read_external(VkCommandBuffer,const EffectsImageInput&);
    void initialize(VkCommandBuffer);
    void validate(const EffectsInputs&) const;
    void uniform(const EffectsInputs&);
    void upload_motion(const EffectsInputs&);
    std::vector<std::pair<std::string,EffectsOwnedImage*>> named_images();
    void dispatch(VkCommandBuffer,std::size_t,const char*,EffectsOwnedImage&,EffectsOwnedImage*,
                  EffectsImageInput source={},EffectsImageInput aux={},EffectsImageInput history={},
                  EffectsImageInput moments={},EffectsPush push={});
    EffectsOwnedImage& work(EffectsImageInput source);
};

struct GpuEffects::PreparedPipelines::State {
    const Impl* owner{};std::unique_ptr<EffectsPipelineBundle> bundle;
};
GpuEffects::PreparedPipelines::PreparedPipelines(std::unique_ptr<State> s):state_(std::move(s)) {}
GpuEffects::PreparedPipelines::~PreparedPipelines()=default;

void GpuEffects::Impl::setup() {
    if(!physical||!device||!allocator)throw std::invalid_argument("GpuEffects requires physical/device/VMA handles");
    vkGetPhysicalDeviceProperties(physical,&properties);VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(physical,effects_format,&format);
    const auto needed=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if((format.optimalTilingFeatures&needed)!=needed)throw std::runtime_error("GpuEffects needs sampled/storage/transfer RGBA32F images");
    if(properties.apiVersion<VK_API_VERSION_1_3||properties.limits.maxPerStageDescriptorSamplers<sample_bindings||
       properties.limits.maxPerStageDescriptorSampledImages<sample_bindings||properties.limits.maxPerStageDescriptorStorageImages<2||
       properties.limits.maxPerStageDescriptorStorageBuffers<1||properties.limits.maxDescriptorSetStorageBuffers<1||
       properties.limits.maxStorageBufferRange<sizeof(EffectsObjectMotion)||
       properties.limits.maxComputeWorkGroupInvocations<64||properties.limits.maxComputeWorkGroupSize[0]<8||properties.limits.maxComputeWorkGroupSize[1]<8)
        throw std::runtime_error("GpuEffects device limits are insufficient");
    std::array<VkDescriptorSetLayoutBinding,20> bindings{};
    for(std::uint32_t i=0;i<bindings.size();i++) {
        bindings[i].binding=i;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].descriptorType=i<16 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:(i<18 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:(i==18 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
    }
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};set.bindingCount=std::uint32_t(bindings.size());set.pBindings=bindings.data();
    effects_check(vkCreateDescriptorSetLayout(device,&set,nullptr,&set_layout),"descriptor layout");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(EffectsPush)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=&set_layout;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&range;
    effects_check(vkCreatePipelineLayout(device,&layout,nullptr,&pipeline_layout),"pipeline layout");
    std::array<VkDescriptorPoolSize,4> sizes={{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2*sets_per_slot*sample_bindings},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2*sets_per_slot*2},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2*sets_per_slot},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2*sets_per_slot}}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool_info.maxSets=2*sets_per_slot;pool_info.poolSizeCount=4;pool_info.pPoolSizes=sizes.data();
    effects_check(vkCreateDescriptorPool(device,&pool_info,nullptr,&pool),"descriptor pool");
    std::array<VkDescriptorSetLayout,sets_per_slot> layouts;layouts.fill(set_layout);
    for(auto& descriptors:sets) {VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};info.descriptorPool=pool;info.descriptorSetCount=sets_per_slot;info.pSetLayouts=layouts.data();effects_check(vkAllocateDescriptorSets(device,&info,descriptors.data()),"descriptor sets");}
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler_info.magFilter=VK_FILTER_NEAREST;sampler_info.minFilter=VK_FILTER_NEAREST;
    sampler_info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler_info.addressModeU=sampler_info.addressModeV=sampler_info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sampler_info.maxLod=0;
    effects_check(vkCreateSampler(device,&sampler_info,nullptr,&sampler),"nearest sampler");
    auto alignment=std::max(VkDeviceSize(16),properties.limits.minUniformBufferOffsetAlignment);
    uniform_stride=(sizeof(EffectsUniform)+alignment-1)/alignment*alignment;
    for(auto& b:uniforms) {
        b.allocator=allocator;VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=uniform_stride*2;bi.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo ai{};ai.usage=VMA_MEMORY_USAGE_AUTO;ai.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;VmaAllocationInfo result{};
        effects_check(vmaCreateBuffer(allocator,&bi,&ai,&b.buffer,&b.allocation,&result),"uniform buffer");b.mapped=result.pMappedData;
    }
}
std::unique_ptr<EffectsPipelineBundle> GpuEffects::Impl::pipelines(const std::filesystem::path& directory) const {
    auto result=std::make_unique<EffectsPipelineBundle>();result->device=device;
    for(std::size_t i=0;i<shader_names.size();i++) {
        auto code=effects_spirv(directory/(std::string(shader_names[i])+".spv"));VkShaderModule module{};
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=code.size()*4;mi.pCode=code.data();
        effects_check(vkCreateShaderModule(device,&mi,nullptr,&module),"shader module");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=pipeline_layout;
        ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";
        auto created=vkCreateComputePipelines(device,cache,1,&ci,nullptr,&result->pipelines[i]);vkDestroyShaderModule(device,module,nullptr);
        effects_check(created,shader_names[i]);
    }
    return result;
}
EffectsOwnedImage GpuEffects::Impl::make_image(std::uint32_t w,std::uint32_t h) const {
    EffectsOwnedImage out;out.device=device;out.allocator=allocator;out.width=w;out.height=h;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=effects_format;ci.extent={w,h,1};
    ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
    ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ai{};ai.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;VmaAllocationInfo allocation{};
    effects_check(vmaCreateImage(allocator,&ci,&ai,&out.image,&out.allocation,&allocation),"effect image");out.bytes=allocation.size;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=out.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=effects_format;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};effects_check(vkCreateImageView(device,&vi,nullptr,&out.view),"effect view");return out;
}
std::vector<EffectsOwnedImage*> GpuEffects::Impl::images() {
    std::vector<EffectsOwnedImage*> result={&ao_raw,&ao,&trace,&composed,&indirect_image,&motion_image,&sink,&dummy_image};
    for(auto* collection:{&scratch,&scratch_variance,&history_position,&history_normal,&history_meta,&denoise_color,&denoise_moments,&taa_color,&taa_moments})
        for(auto& image:*collection)result.push_back(&image);
    for(auto* collection:{&bloom_down,&bloom_up})for(auto& image:*collection)result.push_back(&image);return result;
}
std::vector<std::pair<std::string,EffectsOwnedImage*>> GpuEffects::Impl::named_images() {
    std::vector<std::pair<std::string,EffectsOwnedImage*>> result={
        {"effects/ao-raw",&ao_raw},{"effects/ao",&ao},{"effects/trace",&trace},
        {"effects/composed",&composed},{"effects/indirect",&indirect_image},
        {"effects/motion",&motion_image},{"effects/sink",&sink},{"effects/dummy",&dummy_image}};
    auto pair=[&](const char* name,auto& collection){for(std::size_t i=0;i<collection.size();++i)
        result.emplace_back(std::string("effects/")+name+"/"+std::to_string(i),&collection[i]);};
    pair("scratch",scratch);pair("scratch-variance",scratch_variance);
    pair("history-position",history_position);pair("history-normal",history_normal);pair("history-meta",history_meta);
    pair("denoise-color",denoise_color);pair("denoise-moments",denoise_moments);
    pair("taa-color",taa_color);pair("taa-moments",taa_moments);
    pair("bloom-down",bloom_down);pair("bloom-up",bloom_up);return result;
}
void GpuEffects::Impl::barrier(VkCommandBuffer cmd,VkImage image,VkImageLayout old,VkImageLayout next,
    VkPipelineStageFlags2 source_stage,VkAccessFlags2 source_access,VkPipelineStageFlags2 target_stage,VkAccessFlags2 target_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};b.srcStageMask=source_stage;b.srcAccessMask=source_access;
    b.dstStageMask=target_stage;b.dstAccessMask=target_access;b.oldLayout=old;b.newLayout=next;b.image=image;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};d.imageMemoryBarrierCount=1;d.pImageMemoryBarriers=&b;vkCmdPipelineBarrier2(cmd,&d);++diagnostics.barriers;
}
void GpuEffects::Impl::transition(VkCommandBuffer cmd,EffectsOwnedImage& image,VkImageLayout layout,VkPipelineStageFlags2 stage,VkAccessFlags2 access) {
    // 即使 layout 相同也发 barrier：跨帧读->写/WAW 必须有实际 dependency。
    barrier(cmd,image.image,image.layout,layout,image.stage,image.access,stage,access);image.layout=layout;image.stage=stage;image.access=access;
}
EffectsImageInput GpuEffects::Impl::read(VkCommandBuffer cmd,EffectsOwnedImage& image) {
    // 采用 ALL_COMMANDS 记录后续 caller fragment/debug/readback 读，跨帧重写也覆盖。
    transition(cmd,image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    return {image.image,image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,image.stage,image.access};
}
void GpuEffects::Impl::read_external(VkCommandBuffer cmd,const EffectsImageInput& image) {
    if(!image)return;barrier(cmd,image.image,image.layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,image.stage,image.access,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}
void GpuEffects::Impl::initialize(VkCommandBuffer cmd) {
    bool any=false;VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkClearColorValue zero{};
    for(auto* image:images())if(image->layout==VK_IMAGE_LAYOUT_UNDEFINED) {
        transition(cmd,*image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(cmd,image->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&range);read(cmd,*image);any=true;
    }
    if(any)diagnostics.pass_names.emplace_back("effects-history-initialize (GPU clear)");
}
void GpuEffects::Impl::validate(const EffectsInputs& i) const {
    if(!width||!height||i.width!=width||i.height!=height||i.frame_slot>1)throw std::invalid_argument("GpuEffects extent/frame_slot mismatch; resize while idle first");
    if(!i.position||!i.normal_roughness||!i.albedo_metallic||!i.emission_ao)throw std::invalid_argument("GpuEffects requires first four sampled G-buffer images");
    if(!effects_finite(i.current_vp)||!effects_finite(i.previous_vp)||!effects_finite(i.camera.view())||
       glm::length(i.camera.target-i.camera.position)<1e-5f||i.camera.near_plane<=0||i.camera.far_plane<=i.camera.near_plane)
        throw std::invalid_argument("GpuEffects invalid camera/VP");
    if(!i.object_motion.empty()&&!i.optional_meta)throw std::invalid_argument("Effects object motion requires exact object metadata");
    effects_motion_upload_bytes(i.object_motion.size(),properties.limits.maxStorageBufferRange);
    for(const auto& motion:i.object_motion) {
        if(motion.history_valid>1||motion.previous_object_id>effects_exact_object_id_limit||motion.reserved0||motion.reserved1||
           (motion.history_valid&&!effects_motion_affine(motion.current_to_previous_world)))
            throw std::invalid_argument("Effects motion ABI/affine/history flags invalid");
    }
}
void GpuEffects::Impl::upload_motion(const EffectsInputs& i) {
    const auto bytes=effects_motion_upload_bytes(i.object_motion.size(),properties.limits.maxStorageBufferRange);
    // caller 已等待本 slot fence：仅可替换本槽旧 buffer，不能毁掉另一帧正在读的 SSBO。
    auto& b=motions[slot];auto& copy=motion_records[slot];copy.assign(i.object_motion.begin(),i.object_motion.end());
    if(b.capacity<bytes) {
        const auto previous_capacity=b.capacity;
        EffectsBuffer candidate;candidate.allocator=allocator;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=bytes;ci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo ai{};ai.usage=VMA_MEMORY_USAGE_AUTO;
        ai.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;VmaAllocationInfo info{};
        effects_check(vmaCreateBuffer(allocator,&ci,&ai,&candidate.buffer,&candidate.allocation,&info),"per-slot motion SSBO");
        candidate.mapped=info.pMappedData;candidate.capacity=bytes;
        if(!candidate.mapped)throw std::runtime_error("Effects motion SSBO is not host mapped");
        b.reset();std::swap(b.allocator,candidate.allocator);std::swap(b.buffer,candidate.buffer);
        std::swap(b.allocation,candidate.allocation);std::swap(b.mapped,candidate.mapped);std::swap(b.capacity,candidate.capacity);
        diagnostics.allocated_bytes+=std::size_t(b.capacity-previous_capacity);
    }
    const EffectsObjectMotion dummy;
    std::memcpy(b.mapped,copy.empty() ? static_cast<const void*>(&dummy):static_cast<const void*>(copy.data()),bytes);
    effects_check(vmaFlushAllocation(allocator,b.allocation,0,bytes),"motion SSBO flush");
    frame_inputs.object_motion=copy; // 不保留调用者可能已经失效的 span。
    diagnostics.motion_upload_bytes=bytes;diagnostics.motion_objects=copy.size();
}
void GpuEffects::Impl::uniform(const EffectsInputs& i) {
    EffectsUniform u;u.vp=i.current_vp;u.previous_vp=i.previous_vp;u.previous_inverse_vp=glm::inverse(i.previous_vp);
    u.view=i.camera.view();u.inverse_view=glm::inverse(u.view);
    u.camera_near={i.camera.position,i.camera.near_plane};u.sky_top_far={i.sky_top,i.camera.far_plane};
    u.sky_bottom_radius={i.sky_bottom,effects_parameter(i.settings.ao_radius,.5f,0,100)};
    u.dimensions={width,height,int(serial%2147483647),std::bit_cast<std::int32_t>(i.settings.seed)};
    u.modes={int(i.settings.ao),int(i.settings.gi),std::clamp(i.settings.samples,1,64),int(i.settings.debug)};
    u.flags={diagnostics.history_valid ? 1:0,bool(i.indirect_baseline),i.settings.outline,i.settings.hatching};
    u.temporal={effects_parameter(i.settings.temporal_weight,.1f,0,1),effects_parameter(i.settings.ao_strength,1,0,8),
        effects_parameter(i.settings.bloom_threshold,1,0,60000),effects_parameter(i.settings.bloom_strength,.08f,0,16)};
    u.jitter={i.current_jitter.x,i.current_jitter.y,i.previous_jitter.x,i.previous_jitter.y};
    u.extensions={bool(i.optional_tangent),bool(i.optional_meta),bool(i.volume_indirect),int(motion_records[slot].size())};
    auto& b=uniforms[slot];auto offset=uniform_stride*uniform_phase;
    std::memcpy(static_cast<std::byte*>(b.mapped)+offset,&u,sizeof(u));effects_check(vmaFlushAllocation(allocator,b.allocation,offset,sizeof(u)),"uniform flush");
}
void GpuEffects::Impl::dispatch(VkCommandBuffer cmd,std::size_t pipeline,const char* name,EffectsOwnedImage& output,EffectsOwnedImage* output_aux,
    EffectsImageInput source,EffectsImageInput aux,EffectsImageInput history,EffectsImageInput moments,EffectsPush push) {
    if(set_cursor>=sets_per_slot)throw std::length_error("GpuEffects per-slot dispatch capacity exceeded");
    // 独立 wrapper 可细分计时；加入外层图后由唯一 observer 包围 Barrier+dispatch。
    const auto callbacks=graph_dispatch ? EffectsPassCallbacks{}:frame_inputs.pass_callbacks;
    if(callbacks.begin)callbacks.begin(callbacks.user,cmd,name);
    struct ScopeEnd {EffectsPassCallbacks c;VkCommandBuffer cmd;const char* name;~ScopeEnd(){if(c.end)c.end(c.user,cmd,name);}} scope{callbacks,cmd,name};
    auto dummy=EffectsImageInput{dummy_image.image,dummy_image.view};auto choose=[&](EffectsImageInput im){return im ? im:dummy;};
    const auto& f=frame_inputs;std::size_t previous=std::size_t((serial+1)%2);
    std::array<EffectsImageInput,sample_bindings> inputs={f.position,f.normal_roughness,f.albedo_metallic,f.emission_ao,
        choose(source),choose(aux),choose(history),{history_position[previous].image,history_position[previous].view},
        {history_normal[previous].image,history_normal[previous].view},choose(moments),choose(f.indirect_baseline),
        choose(f.optional_tangent),choose(f.optional_meta),{history_meta[previous].image,history_meta[previous].view},
        choose(f.volume_indirect),{ao.image,ao.view}};
    // inactive descriptor bindings 也必须合法；禁止输出与 sampled 输入指向同一图。
    for(std::size_t k=0;k<inputs.size();++k) {
        if(graph_dispatch&&!(graph_sample_mask&(1u<<k)))inputs[k]=dummy;
        if(inputs[k].image==output.image||(output_aux&&inputs[k].image==output_aux->image)) {
            if(graph_dispatch&&(graph_sample_mask&(1u<<k)))throw std::logic_error("Effects graph sampled/storage image alias");
            inputs[k]=dummy;
        }
    }
    if(!graph_dispatch)transition(cmd,output,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    else if(output.layout!=VK_IMAGE_LAYOUT_GENERAL)throw std::logic_error("Effects graph output barrier was not applied");
    // 无 aux 的 shader 不写它；绑定专用 1x1 sink，此时必须使用 GENERAL（不能假报只读）。
    auto& second=output_aux ? *output_aux:sink;
    if(!graph_dispatch)transition(cmd,second,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    else if(second.layout!=VK_IMAGE_LAYOUT_GENERAL)throw std::logic_error("Effects graph auxiliary barrier was not applied");
    std::array<VkDescriptorImageInfo,18> image_info{};std::array<VkWriteDescriptorSet,20> writes{};
    VkDescriptorSet set=sets[slot][set_cursor++];
    for(std::size_t k=0;k<18;k++) {
        if(k<16) {
            image_info[k]={sampler,inputs[k].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        } else image_info[k]={VK_NULL_HANDLE,k==16 ? output.view:second.view,VK_IMAGE_LAYOUT_GENERAL};
        writes[k]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[k].dstSet=set;writes[k].dstBinding=std::uint32_t(k);writes[k].descriptorCount=1;
        writes[k].descriptorType=k<16 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[k].pImageInfo=&image_info[k];
    }
    VkDescriptorBufferInfo buffer{uniforms[slot].buffer,uniform_stride*uniform_phase,sizeof(EffectsUniform)};
    writes[18]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[18].dstSet=set;writes[18].dstBinding=18;writes[18].descriptorCount=1;writes[18].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;writes[18].pBufferInfo=&buffer;
    VkDescriptorBufferInfo motion_buffer{motions[slot].buffer,0,effects_motion_upload_bytes(motion_records[slot].size(),properties.limits.maxStorageBufferRange)};
    writes[19]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[19].dstSet=set;writes[19].dstBinding=19;writes[19].descriptorCount=1;writes[19].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[19].pBufferInfo=&motion_buffer;
    vkUpdateDescriptorSets(device,std::uint32_t(writes.size()),writes.data(),0,nullptr);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,bundle->pipelines[pipeline]);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);
    vkCmdPushConstants(cmd,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
    vkCmdDispatch(cmd,(output.width+7)/8,(output.height+7)/8,1);++diagnostics.dispatches;diagnostics.pass_names.emplace_back(name);
    if(!graph_dispatch){read(cmd,output);if(output_aux)read(cmd,*output_aux);}
}
EffectsOwnedImage& GpuEffects::Impl::work(EffectsImageInput source) {return source.image==scratch[0].image ? scratch[1]:scratch[0];}

GpuEffects::GpuEffects(VkPhysicalDevice physical,VkDevice device,VmaAllocator allocator,const std::filesystem::path& directory,VkPipelineCache cache)
    :impl_(std::make_unique<Impl>(physical,device,allocator,cache)) {impl_->setup();impl_->bundle=impl_->pipelines(directory);}
GpuEffects::~GpuEffects()=default;
std::span<const char* const> GpuEffects::shader_files() noexcept {return shader_names;}
std::unique_ptr<GpuEffects::PreparedPipelines> GpuEffects::prepare_pipelines(const std::filesystem::path& directory,std::string* error) const {
    try {auto state=std::make_unique<PreparedPipelines::State>();state->owner=impl_.get();state->bundle=impl_->pipelines(directory);
        if(error)error->clear();return std::unique_ptr<PreparedPipelines>(new PreparedPipelines(std::move(state)));
    } catch(const std::exception& e){if(error)*error=e.what();return {};}
}
bool GpuEffects::commit_pipelines(std::unique_ptr<PreparedPipelines> candidate) noexcept {
    if(!candidate||candidate->state_->owner!=impl_.get()||impl_->frame_open)return false;
    impl_->bundle.swap(candidate->state_->bundle);impl_->history_ready=false;impl_->pending_reset="pipeline reload";return true;
}
bool GpuEffects::reload_pipelines(const std::filesystem::path& directory,std::string* error) {
    auto candidate=prepare_pipelines(directory,error);if(!candidate)return false;
    try {if(impl_->frame_open)throw std::logic_error("Cannot reload effects during an open frame");effects_check(vkDeviceWaitIdle(impl_->device),"reload wait idle");
        if(!commit_pipelines(std::move(candidate)))throw std::runtime_error("Cannot commit effects pipelines");if(error)error->clear();return true;
    } catch(const std::exception& e){if(error)*error=e.what();return false;}
}
void GpuEffects::resize(std::uint32_t w,std::uint32_t h) {
    auto& e=*impl_;if(!w||!h||w>e.properties.limits.maxImageDimension2D||h>e.properties.limits.maxImageDimension2D||
        (w+7)/8>e.properties.limits.maxComputeWorkGroupCount[0]||(h+7)/8>e.properties.limits.maxComputeWorkGroupCount[1])throw std::invalid_argument("GpuEffects resize invalid extent");
    // 先完整分配候选；OOM 保留旧图，调用方此前已 wait_idle。
    std::vector<EffectsOwnedImage> full;full.reserve(24);for(int k=0;k<24;k++)full.push_back(e.make_image(w,h));
    auto new_sink=e.make_image(1,1),new_dummy=e.make_image(1,1);std::vector<EffectsOwnedImage> down,up;auto bw=std::max(1u,(w+1)/2),bh=std::max(1u,(h+1)/2);
    for(int level=0;level<6;level++){down.push_back(e.make_image(bw,bh));up.push_back(e.make_image(bw,bh));if(bw==1&&bh==1)break;bw=std::max(1u,(bw+1)/2);bh=std::max(1u,(bh+1)/2);}
    std::size_t next=0;
    for(auto* image:{&e.ao_raw,&e.ao,&e.trace,&e.composed,&e.indirect_image,&e.motion_image})*image=std::move(full[next++]);
    for(auto* collection:{&e.scratch,&e.scratch_variance,&e.history_position,&e.history_normal,&e.history_meta,&e.denoise_color,&e.denoise_moments,&e.taa_color,&e.taa_moments})
        for(auto& image:*collection)image=std::move(full[next++]);
    e.sink=std::move(new_sink);e.dummy_image=std::move(new_dummy);e.bloom_down=std::move(down);e.bloom_up=std::move(up);e.width=w;e.height=h;e.serial=0;e.frame_open=false;
    e.history_ready=false;e.pending_reset="resize/initialization";e.variance_result=&e.dummy_image;e.diagnostics={};
    for(auto* image:e.images())e.diagnostics.allocated_bytes+=std::size_t(image->bytes);
    for(const auto& b:e.motions)e.diagnostics.allocated_bytes+=std::size_t(b.capacity);
    e.diagnostics.allocated_bytes+=std::size_t(e.uniform_stride*4);
    e.post_plan={};e.graph_images.clear();e.graph_output.clear();
}
glm::vec2 GpuEffects::jitter() const noexcept {
    const auto& e=*impl_;if(!e.width||!e.height)return {};auto n=e.serial%16+1;
    return {(effects_radical(n,2)-.5f)*2/float(e.width),(effects_radical(n,3)-.5f)*2/float(e.height)};
}
glm::mat4 GpuEffects::jittered_projection(glm::mat4 p,glm::vec2 jitter) noexcept {
    // clip.xy += jitter*clip.w，支持 Y 已翻转/正反 Z，避免假定 perspective 的列符号。
    for(int c=0;c<4;c++){p[c][0]+=jitter.x*p[c][3];p[c][1]+=jitter.y*p[c][3];}return p;
}
GpuEffects::Output GpuEffects::record_occlusion(VkCommandBuffer cmd,const Inputs& i) {
    auto& e=*impl_;e.validate(i);if(!cmd||e.frame_open)throw std::logic_error("GpuEffects occlusion must begin a new paired frame");
    e.slot=i.frame_slot;e.set_cursor=0;e.uniform_phase=0;e.frame_inputs=i;e.current_key=effects_key(i);
    auto bytes=e.diagnostics.allocated_bytes;e.diagnostics={};e.diagnostics.allocated_bytes=bytes;e.diagnostics.jitter=i.current_jitter;
    auto reset=[&](const char* reason){e.diagnostics.history_reset_reason=reason;e.diagnostics.history_valid=false;};
    e.diagnostics.history_valid=e.history_ready;
    if(!e.history_ready)reset(e.pending_reset);
    else if(i.invalidate_history)reset("caller invalidation (resource/light/camera cut)");
    // 普通 pose revision 不代表整幅历史无效。资源/灯光变化由 caller 显式 invalidate。
    else if(e.current_key!=e.last_key)reset("effect/camera configuration");
    else {
        auto oldDirection=safe_normalize(e.last_camera.target-e.last_camera.position),direction=safe_normalize(i.camera.target-i.camera.position);
        if(glm::length(i.camera.position-e.last_camera.position)>std::max(1.f,i.settings.ao_radius*4)||glm::dot(oldDirection,direction)<.75f)reset("camera cut");
    }
    e.initialize(cmd);
    for(auto input:{i.position,i.normal_roughness,i.albedo_metallic,i.emission_ao,i.optional_tangent,i.optional_meta})e.read_external(cmd,input);
    // 已转换布局，post 阶段不会再把这些 GB 当 attachment；caller 同步其 graph state。
    for(auto* input:{&e.frame_inputs.position,&e.frame_inputs.normal_roughness,&e.frame_inputs.albedo_metallic,&e.frame_inputs.emission_ao,&e.frame_inputs.optional_tangent,&e.frame_inputs.optional_meta})
        if(*input){input->layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;input->stage=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;input->access=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;}
    e.upload_motion(i);e.uniform(i);e.dispatch(cmd,ao_pipeline,i.settings.ao==AoMode::gtao ? "gtao-horizon-integral":(i.settings.ao==AoMode::ssao ? "ssao-hemisphere":"occlusion-identity"),e.ao_raw,nullptr);
    EffectsPush push;push.control={2,2,0,0};e.dispatch(cmd,filter_pipeline,"ao-joint-bilateral",e.ao,nullptr,e.read(cmd,e.ao_raw),{}, {},{},push);
    e.frame_open=true;return e.ao.output();
}
void GpuEffects::add_post_passes(RenderGraph& graph,VkCommandBuffer cmd,const Inputs& description,
    std::function<Inputs()> factory,std::span<const ResourceUse> external_uses,Output& output) {
    auto& e=*impl_;using A=ResourceAccess;using S=ResourceState;
    if(!cmd||!factory||(external_uses.size()!=9&&external_uses.size()!=10)||!e.width||
       description.width!=e.width||description.height!=e.height||description.frame_slot>1)
        throw std::invalid_argument("Effects graph requires description, lazy factory and 9/10 semantic resource slots");
    for(std::size_t k:{0u,1u,2u,3u,4u})if(external_uses[k].resource.empty())
        throw std::invalid_argument("Effects graph required external resource is absent");
    struct Job {
        Inputs planned,inputs;
        std::function<Inputs()> factory;
        std::vector<std::string> external,passes;
        std::uint64_t serial=0;bool started=false;
    };
    auto job=std::make_shared<Job>();job->planned=description;job->planned.object_motion={};
    job->factory=std::move(factory);job->serial=e.serial;
    for(const auto& u:external_uses) {
        if(!u.resource.empty()&&(u.access!=A::read||u.state!=S::shader_read))
            throw std::invalid_argument("Effects external slots describe sampled-read resources");
        job->external.push_back(u.resource);
    }
    const bool volume=description.settings.gi==GiMode::rsm||description.settings.gi==GiMode::lpv||description.settings.gi==GiMode::voxel;
    const bool effects_debug=description.settings.debug==DebugView::ao||description.settings.debug==DebugView::indirect||
        description.settings.debug==DebugView::motion||description.settings.debug==DebugView::variance;
    const bool core_debug=description.settings.debug!=DebugView::final_color&&!effects_debug;
    if(volume&&!core_debug&&job->external[8].empty())throw std::invalid_argument("Volume GI graph resource is required");
    const auto current=std::size_t(e.serial%2),previous=std::size_t((e.serial+1)%2);

    // 名称对应真实 VkImage；历史与 scratch 使用实际 ping-pong 索引，不创建“看起来像 Pass”的标签。
    e.graph_images.clear();
    for(auto [name,image]:e.named_images()) {
        const bool alias_ao=image==&e.ao&&job->external.size()==10&&!job->external[9].empty();
        if(alias_ao)name=job->external[9];
        else {
            // occlusion 的 initialize 会清空未初始化图像，首个 post Pass 前它们已有合法内容。
            auto initial=image==&e.sink||image->layout==VK_IMAGE_LAYOUT_GENERAL ? S::storage:S::shader_read;
            graph.add_resource(name,true,initial);
        }
        e.graph_images.push_back({std::move(name),image->image,image->view,image->width,image->height});
    }
    struct Ref {EffectsOwnedImage* owned=nullptr;int external=-1;};
    auto own=[](EffectsOwnedImage& image){return Ref{&image,-1};};
    auto ext=[](int index){return Ref{nullptr,index};};
    const Ref dummy=own(e.dummy_image);
    auto resource=[&](Ref r)->std::string {
        if(r.external>=0)return job->external.at(std::size_t(r.external));
        for(const auto& image:e.graph_images)if(r.owned&&image.image==r.owned->image)return image.resource;
        throw std::logic_error("Effects graph image not registered");
    };
    auto resolve=[&e](Ref r)->EffectsImageInput {
        if(r.owned)return {r.owned->image,r.owned->view};
        const auto& f=e.frame_inputs;
        const std::array<EffectsImageInput,9> external={f.position,f.normal_roughness,f.albedo_metallic,
            f.emission_ao,f.hdr,f.indirect_baseline,f.optional_tangent,f.optional_meta,f.volume_indirect};
        return r.external>=0 ? external.at(std::size_t(r.external)):EffectsImageInput{};
    };
    auto prepare=[&e,job,core_debug,volume] {
        if(job->started)return;
        auto i=job->factory();e.validate(i);
        if(!e.frame_open||e.serial!=job->serial||i.frame_slot!=e.slot||
           i.scene_revision!=e.frame_inputs.scene_revision||effects_key(i)!=e.current_key||
           i.position.image!=e.frame_inputs.position.image||i.current_vp!=e.frame_inputs.current_vp||
           i.frame_slot!=job->planned.frame_slot||
           i.settings.debug!=job->planned.settings.debug||i.settings.gi!=job->planned.settings.gi||
           i.settings.denoise!=job->planned.settings.denoise||i.settings.svgf!=job->planned.settings.svgf||
           i.settings.taa!=job->planned.settings.taa||i.settings.bloom!=job->planned.settings.bloom||
           i.settings.outline!=job->planned.settings.outline||i.settings.hatching!=job->planned.settings.hatching||
           (i.settings.bloom_strength>0)!=(job->planned.settings.bloom_strength>0))
            throw std::logic_error("Effects graph must execute its matching occlusion frame");
        if(!i.hdr||(volume&&!core_debug&&!i.volume_indirect))
            throw std::invalid_argument("Effects lazy factory did not supply the produced HDR/volume output");
        const std::array<bool,9> present={bool(i.position),bool(i.normal_roughness),bool(i.albedo_metallic),
            bool(i.emission_ao),bool(i.hdr),bool(i.indirect_baseline),bool(i.optional_tangent),bool(i.optional_meta),bool(i.volume_indirect)};
        for(std::size_t k=0;k<9;++k)if(present[k]!=!job->external[k].empty()&&!(core_debug&&k==8))
            throw std::invalid_argument("Effects lazy input/resource declaration mismatch");
        // motion span 已在 occlusion 复制进当前 slot，此处不能改写在途表。
        job->inputs=i;job->inputs.object_motion=e.motion_records[e.slot];
        e.frame_inputs.hdr=i.hdr;e.frame_inputs.indirect_baseline=i.indirect_baseline;e.frame_inputs.volume_indirect=i.volume_indirect;
        e.uniform_phase=1;e.uniform(i);e.diagnostics.baseline_replaced=!core_debug&&bool(i.indirect_baseline);job->started=true;
    };
    auto pass=[&](std::size_t pipeline,std::string name,EffectsOwnedImage& target,EffectsOwnedImage* target_aux,
                  Ref source,Ref aux,Ref history,Ref moments,EffectsPush push={},bool keep=false) {
        std::uint32_t mask=0;
        auto bits=[&](std::initializer_list<unsigned> ids){for(auto id:ids)mask|=1u<<id;};
        switch(pipeline) {
        case trace_pipeline:bits({0,1,2,4,11,12});break;
        case compose_pipeline:bits({0,1,2,3,4,5,10,15});break;
        case temporal_pipeline:bits({0,1,4,6,7,8,9,12,13});break;
        case filter_pipeline:bits({0,1,4,5,12});break;
        case snapshot_pipeline:if(push.control.x==0)bits({0,1});else bits({0,1,12});break;
        case bloom_pipeline:bits({4});if(push.control.x!=0)bits({5});break;
        case npr_pipeline:bits({0,1,4,12});break;
        case debug_pipeline:bits({4,15});break;
        case copy_pipeline:bits({4});break;
        default:throw std::logic_error("Unknown effects graph pipeline");
        }
        std::array<Ref,16> refs={ext(0),ext(1),ext(2),ext(3),source,aux,history,
            own(e.history_position[previous]),own(e.history_normal[previous]),moments,
            ext(5),ext(6),ext(7),own(e.history_meta[previous]),ext(8),own(e.ao)};
        std::vector<ResourceUse> uses;
        auto add=[&](std::string name,A access,S state){
            if(name.empty())name=resource(dummy);
            for(auto& u:uses)if(u.resource==name) {
                if(u.state!=state||u.access!=access)throw std::logic_error("Effects sampled/storage alias");
                return;
            }
            uses.push_back({std::move(name),access,state});
        };
        for(unsigned k=0;k<16;++k)if(mask&(1u<<k))add(resource(refs[k]),A::read,S::shader_read);
        add(resource(dummy),A::read,S::shader_read); // inactive sampled descriptors 也绑定真实只读 dummy。
        add(resource(own(target)),A::write,S::storage);
        if(target_aux)add(resource(own(*target_aux)),A::write,S::storage);
        job->passes.push_back(name);
        graph.add_pass(name,std::move(uses),
            [&e,cmd,job,prepare,resolve,pipeline,name,target=&target,target_aux,source,aux,history,moments,push,mask] {
                prepare();
                struct ManagedScope {
                    Impl& e;bool previous;std::uint32_t mask;
                    ~ManagedScope(){e.graph_dispatch=previous;e.graph_sample_mask=mask;}
                } scope{e,e.graph_dispatch,e.graph_sample_mask};
                e.graph_dispatch=true;e.graph_sample_mask=mask;
                e.dispatch(cmd,pipeline,name.c_str(),*target,target_aux,resolve(source),resolve(aux),resolve(history),resolve(moments),push);
            },keep);
    };
    auto no=Ref{&e.dummy_image,-1};Ref raw=ext(4),gi=no;
    auto scratch=[&](Ref source)->EffectsOwnedImage& {return source.owned==&e.scratch[0] ? e.scratch[1]:e.scratch[0];};
    e.variance_result=&e.dummy_image;
    if(core_debug) {
        pass(copy_pipeline,"effects-core-debug-format-copy",e.composed,nullptr,raw,no,no,no);
        raw=own(e.composed);
    } else {
        if(description.settings.gi==GiMode::ssr||description.settings.gi==GiMode::ssgi) {
            pass(trace_pipeline,description.settings.gi==GiMode::ssr ? "ssr-world-ray-march":"ssgi-cosine-ray-march",
                 e.trace,nullptr,raw,no,no,no);gi=own(e.trace);
        } else if(volume)gi=ext(8);
        pass(compose_pipeline,"indirect-baseline-replace (AO indirect only)",e.composed,&e.indirect_image,raw,gi,no,no);
        raw=own(e.composed);
        if(description.settings.denoise||description.settings.svgf) {
            EffectsPush control;control.control.x=description.settings.svgf ? 2:1;
            if(!description.settings.svgf) {
                auto& filtered=scratch(raw);EffectsPush bilateral;bilateral.control={0,2,0,0};
                pass(filter_pipeline,"hw5-joint-bilateral",filtered,nullptr,raw,no,no,no,bilateral);raw=own(filtered);
            }
            pass(temporal_pipeline,description.settings.svgf ? "svgf-temporal-moments-variance":"hw5-temporal-reprojection",
                e.denoise_color[current],&e.denoise_moments[current],raw,no,own(e.denoise_color[previous]),own(e.denoise_moments[previous]),control);
            raw=own(e.denoise_color[current]);e.variance_result=&e.denoise_moments[current];
            if(description.settings.svgf)for(int level=0;level<5;++level) {
                auto& filtered=scratch(raw);auto& variance=e.scratch_variance[std::size_t(level%2)];control.control={1,1<<level,0,0};
                pass(filter_pipeline,"svgf-atrous-step-"+std::to_string(1<<level),filtered,&variance,raw,own(*e.variance_result),no,no,control);
                raw=own(filtered);e.variance_result=&variance;
            }
        }
        if(description.settings.taa) {
            pass(temporal_pipeline,"taa-jitter-reproject-reject-ycocg-clamp",e.taa_color[current],&e.taa_moments[current],
                 raw,no,own(e.taa_color[previous]),own(e.taa_moments[previous]));
            raw=own(e.taa_color[current]);
            if(!description.settings.denoise&&!description.settings.svgf)e.variance_result=&e.taa_moments[current];
        }
    }
    // 几何快照是跨帧副作用；始终保留，不把 Bloom/NPR 污染进 temporal history。
    pass(snapshot_pipeline,"history-world-position-normal",e.history_position[current],&e.history_normal[current],no,no,no,no,{},true);
    EffectsPush snapshot;snapshot.control.x=1;
    pass(snapshot_pipeline,"history-material-object-motion",e.history_meta[current],&e.motion_image,no,no,no,no,snapshot,true);
    if(!core_debug&&description.settings.debug==DebugView::final_color) {
        if(description.settings.bloom&&description.settings.bloom_strength>0) {
            Ref bright=raw;EffectsPush bloom;
            for(std::size_t level=0;level<e.bloom_down.size();++level) {
                bloom.control={0,level==0 ? 1:0,0,0};
                pass(bloom_pipeline,"bloom-down-"+std::to_string(level),e.bloom_down[level],nullptr,bright,no,no,no,bloom);
                bright=own(e.bloom_down[level]);
            }
            for(std::size_t level=e.bloom_down.size()-1;level>0;--level) {
                bloom.control={1,0,0,0};
                pass(bloom_pipeline,"bloom-up-"+std::to_string(level-1),e.bloom_up[level-1],nullptr,bright,own(e.bloom_down[level-1]),no,no,bloom);
                bright=own(e.bloom_up[level-1]);
            }
            auto& combined=scratch(raw);bloom.control={2,0,0,0};
            pass(bloom_pipeline,"bloom-linear-composite",combined,nullptr,raw,bright,no,no,bloom);raw=own(combined);
        }
        if(description.settings.outline||description.settings.hatching) {
            auto& styled=scratch(raw);pass(npr_pipeline,"npr-depth-normal-outline-crosshatch",styled,nullptr,raw,no,no,no);raw=own(styled);
        }
    } else if(!core_debug&&effects_debug) {
        auto& debug=scratch(raw);EffectsPush control;control.control.x=int(description.settings.debug);
        Ref source=own(e.indirect_image);
        if(description.settings.debug==DebugView::motion)source=own(e.motion_image);
        if(description.settings.debug==DebugView::variance)source=own(*e.variance_result);
        pass(debug_pipeline,"effects-debug-view",debug,nullptr,source,no,no,no,control);raw=own(debug);
    }
    e.graph_output=resource(raw);output=raw.owned->output();
    std::vector<ResourceUse> publish;
    auto publish_image=[&](EffectsOwnedImage& image) {
        const auto name=resource(own(image));
        for(const auto& u:publish)if(u.resource==name)return;
        publish.push_back({name,A::read,S::shader_read});
    };
    for(auto* im:{raw.owned,&e.indirect_image,&e.motion_image,e.variance_result,&e.history_position[current],
                 &e.history_normal[current],&e.history_meta[current],&e.sink})publish_image(*im);
    if(!core_debug&&(description.settings.denoise||description.settings.svgf)) {
        publish_image(e.denoise_color[current]);publish_image(e.denoise_moments[current]);
    }
    if(!core_debug&&description.settings.taa){publish_image(e.taa_color[current]);publish_image(e.taa_moments[current]);}
    // 真实的跨帧发布边界：执行读布局屏障、提交 CPU 历史序号。不冒充 compute dispatch。
    // 依赖所有已声明的 dispatch，保证后续帧不会在当前帧尚未写完快照时切换槽位。
    graph.add_pass("effects-history-publish",std::move(publish),[&e,job,prepare]{
        prepare();e.frame_open=false;e.history_ready=true;e.last_revision=job->inputs.scene_revision;
        e.last_key=e.current_key;e.last_camera=job->inputs.camera;++e.serial;
    },true,job->passes);
    graph.export_resource(e.graph_output);
}
bool GpuEffects::record_graph_barrier(VkCommandBuffer cmd,const ResourceBarrier& b) {
    auto& e=*impl_;
    for(const auto& binding:e.graph_images)if(binding.resource==b.resource) {
        for(auto [name,image]:e.named_images())if(image->image==binding.image) {
            VkImageLayout layout;VkPipelineStageFlags2 stage;VkAccessFlags2 access;
            switch(b.after) {
            case ResourceState::shader_read:layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;stage=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;access=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;break;
            case ResourceState::storage:layout=VK_IMAGE_LAYOUT_GENERAL;stage=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                access=(b.next_access==ResourceAccess::read ? 0:VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)|
                       (b.next_access==ResourceAccess::write ? 0:VK_ACCESS_2_SHADER_STORAGE_READ_BIT);break;
            case ResourceState::transfer_src:layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;stage=VK_PIPELINE_STAGE_2_COPY_BIT;access=VK_ACCESS_2_TRANSFER_READ_BIT;break;
            case ResourceState::transfer_dst:layout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;stage=VK_PIPELINE_STAGE_2_COPY_BIT;access=VK_ACCESS_2_TRANSFER_WRITE_BIT;break;
            default:throw std::invalid_argument("Unsupported effects graph image state");
            }
            e.transition(cmd,*image,layout,stage,access);return true;
        }
        throw std::logic_error("Effects graph references an obsolete resize generation");
    }
    return false;
}
GpuEffects::Output GpuEffects::record_post(VkCommandBuffer cmd,const Inputs& i) {
    // 测试/旧调用者 wrapper 也执行同一份真实图，没有第二套手写 Pass 顺序。
    auto& e=*impl_;e.validate(i);RenderGraph graph;Output output;
    if(!cmd||!e.frame_open||i.frame_slot!=e.slot||i.scene_revision!=e.frame_inputs.scene_revision||
       effects_key(i)!=e.current_key||i.position.image!=e.frame_inputs.position.image||i.current_vp!=e.frame_inputs.current_vp)
        throw std::logic_error("Effects wrapper post must match its occlusion inputs");
    std::array<EffectsImageInput,9> external={i.position,i.normal_roughness,i.albedo_metallic,i.emission_ao,
        i.hdr,i.indirect_baseline,i.optional_tangent,i.optional_meta,i.volume_indirect};
    std::array<ResourceUse,9> uses;
    auto initial=[](VkImageLayout layout){
        if(layout==VK_IMAGE_LAYOUT_GENERAL)return ResourceState::storage;
        if(layout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)return ResourceState::color_attachment;
        if(layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)return ResourceState::transfer_dst;
        if(layout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)return ResourceState::transfer_src;
        return ResourceState::shader_read;
    };
    for(std::size_t k=0;k<external.size();++k)if(external[k]) {
        auto name="effects-input/"+std::to_string(k);graph.add_resource(name,true,initial(external[k].layout));
        uses[k]={std::move(name),ResourceAccess::read,ResourceState::shader_read};
    }
    add_post_passes(graph,cmd,i,[i]{return i;},uses,output);e.post_plan=graph.compile();
    graph.execute(e.post_plan,[&](const ResourceBarrier& b){
        if(record_graph_barrier(cmd,b))return;
        for(std::size_t k=0;k<uses.size();++k)if(uses[k].resource==b.resource) {
            auto& image=external[k];e.barrier(cmd,image.image,image.layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                image.stage,image.access,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            image.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;image.stage=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            image.access=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;return;
        }
        throw std::logic_error("Unmapped effects wrapper graph resource");
    },[&](std::string_view name,bool begin) {
        const auto c=i.pass_callbacks;
        if(begin){if(c.begin)c.begin(c.user,cmd,name);}
        else if(c.end)c.end(c.user,cmd,name);
    });
    return output;
}
GpuEffects::Output GpuEffects::occlusion() const noexcept {return impl_->ao.output();}
GpuEffects::Output GpuEffects::indirect() const noexcept {return impl_->indirect_image.output();}
GpuEffects::Output GpuEffects::variance() const noexcept {return impl_->variance_result ? impl_->variance_result->output():Output{};}
GpuEffects::Output GpuEffects::motion() const noexcept {return impl_->motion_image.output();}
const EffectsDiagnostics& GpuEffects::diagnostics() const noexcept {return impl_->diagnostics;}
const GraphPlan& GpuEffects::post_graph() const noexcept {return impl_->post_plan;}
std::span<const EffectsGraphImage> GpuEffects::post_graph_images() const noexcept {return impl_->graph_images;}
std::string_view GpuEffects::post_output_resource() const noexcept {return impl_->graph_output;}
} // namespace emberframe::lab

namespace emberframe::lab {
namespace {
// 仅显式测试使用 CPU fixture/回读；生产 record 无任何 host 图像计算。
struct EffectsGpuFixture {
    VkPhysicalDevice physical{};VkDevice device{};VmaAllocator allocator{};VkQueue queue{};
    VkCommandPool pool{};VkCommandBuffer command{};
    static constexpr std::uint32_t w=64,h=48;
    std::array<EffectsOwnedImage,9> inputs;
    EffectsOwnedImage lazy_volume;
    EffectsBuffer upload,download;
    std::unique_ptr<GpuEffects> effects;
    std::uint64_t sequence=0;glm::mat4 previous_vp{1};
    EffectsInputs last_inputs;bool corner=false,occluder=false,ao_radius_probe=false,steep_plane=false;
    float radiance=1,material_ao=1,plane_depth=5;int object=1000003;
    bool rigid=false,rigid_previous_known=false,rigid_new=false,new_object_half=false;
    glm::mat4 rigid_world{1},previous_rigid_world{1};
    std::uint32_t previous_object=0;std::uint64_t pose_revision=7;
    std::vector<EffectsObjectMotion> object_motion;
    GraphPlan external_plan;int factory_calls=0,legacy_callback_calls=0;
    bool factory_after_volume=false,observer_balanced=false;
    glm::vec3 camera_offset{0};
    ~EffectsGpuFixture() {
        if(device)vkDeviceWaitIdle(device);effects.reset();if(pool)vkDestroyCommandPool(device,pool,nullptr);
    }
    void create_buffer(EffectsBuffer& b,VkDeviceSize size,VkBufferUsageFlags usage) {
        b.allocator=allocator;VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=size;ci.usage=usage;
        VmaAllocationCreateInfo ai{};ai.usage=VMA_MEMORY_USAGE_AUTO;ai.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;VmaAllocationInfo info{};
        effects_check(vmaCreateBuffer(allocator,&ci,&ai,&b.buffer,&b.allocation,&info),"test staging buffer");b.mapped=info.pMappedData;
    }
    void setup(VkPhysicalDevice p,VkDevice d,VmaAllocator a,VkQueue q,std::uint32_t family,const std::filesystem::path& path) {
        physical=p;device=d;allocator=a;queue=q;
        VkCommandPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool_ci.queueFamilyIndex=family;pool_ci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        effects_check(vkCreateCommandPool(device,&pool_ci,nullptr,&pool),"test command pool");
        VkCommandBufferAllocateInfo command_ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};command_ci.commandPool=pool;command_ci.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command_ci.commandBufferCount=1;
        effects_check(vkAllocateCommandBuffers(device,&command_ci,&command),"test command buffer");
        std::vector<EffectsOwnedImage*> targets;for(auto& im:inputs)targets.push_back(&im);targets.push_back(&lazy_volume);
        for(auto* target:targets) {
            auto& im=*target;
            im.device=device;im.allocator=allocator;im.width=w;im.height=h;
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=effects_format;ci.extent={w,h,1};ci.mipLevels=ci.arrayLayers=1;
            ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo ai{};ai.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            effects_check(vmaCreateImage(allocator,&ci,&ai,&im.image,&im.allocation,nullptr),"test input image");
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=im.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=effects_format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            effects_check(vkCreateImageView(device,&vi,nullptr,&im.view),"test input view");
        }
        create_buffer(upload,VkDeviceSize(w)*h*16*inputs.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        create_buffer(download,VkDeviceSize(w)*h*16,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        effects=std::make_unique<GpuEffects>(physical,device,allocator,path);effects->resize(w,h);
    }
    void begin() {
        effects_check(vkResetCommandPool(device,pool,0),"test reset pool");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;effects_check(vkBeginCommandBuffer(command,&bi),"test begin");
    }
    void submit() {
        effects_check(vkEndCommandBuffer(command),"test end");VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&command;
        effects_check(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE),"test submit");effects_check(vkQueueWaitIdle(queue),"test wait idle");
    }
    void barrier(VkImage image,VkImageLayout old,VkImageLayout next,VkPipelineStageFlags2 src,VkAccessFlags2 src_access,VkPipelineStageFlags2 dst,VkAccessFlags2 dst_access) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};b.image=image;b.oldLayout=old;b.newLayout=next;
        b.srcStageMask=src;b.srcAccessMask=src_access;b.dstStageMask=dst;b.dstAccessMask=dst_access;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};di.imageMemoryBarrierCount=1;di.pImageMemoryBarriers=&b;vkCmdPipelineBarrier2(command,&di);
    }
    EffectsInputs build_inputs(Settings settings) {
        EffectsInputs i;i.width=w;i.height=h;i.frame_slot=std::uint32_t(sequence%2);i.settings=settings;i.scene_revision=pose_revision;
        i.camera.position=camera_offset;i.camera.target=camera_offset+glm::vec3(0,0,-1);i.camera.fov=static_cast<float>(ao_radius_probe ? 90:50);i.camera.far_plane=30;
        auto projection=i.camera.projection(float(w)/h,settings.reversed_z);projection[1][1]*=-1;
        i.current_jitter=settings.taa ? effects->jitter():glm::vec2(0);
        i.current_vp=GpuEffects::jittered_projection(projection,i.current_jitter)*i.camera.view();i.previous_vp=previous_vp;
        std::array<EffectsImageInput*,9> bindings={&i.position,&i.normal_roughness,&i.albedo_metallic,&i.emission_ao,&i.hdr,&i.indirect_baseline,&i.optional_tangent,&i.optional_meta,&i.volume_indirect};
        for(std::size_t k=0;k<inputs.size();k++)*bindings[k]={inputs[k].image,inputs[k].view};
        if(rigid) {
            if(object<0||object>1024)throw std::invalid_argument("Rigid GPU fixture ID outside its bounded table");
            object_motion.assign(std::size_t(object)+(new_object_half ? 2:1),EffectsObjectMotion{});
            object_motion[std::size_t(object)]=effects_make_object_motion(rigid_world,previous_rigid_world,previous_object,rigid_previous_known&&!rigid_new);
            i.object_motion=object_motion;
        }
        return i;
    }
    void upload_images(const EffectsInputs& i) {
        auto* target=static_cast<glm::vec4*>(upload.mapped);auto inverse=glm::inverse(i.current_vp);
        for(std::uint32_t y=0;y<h;y++)for(std::uint32_t x=0;x<w;x++) {
            auto pixel=std::size_t(y)*w+x;glm::vec2 ndc=glm::vec2((float(x)+.5f)/w,(float(y)+.5f)/h)*2.f-1.f;
            auto point=inverse*glm::vec4(ndc,.5f,1);glm::vec3 ray=glm::vec3(point)/point.w-i.camera.position;ray/=(-ray.z);
            float depth=plane_depth+i.camera.position.z;glm::vec3 normal{0,0,1};
            if(rigid) {
                // 从当前相机射线与变换后的局部平面求交，模拟真实刚体几何的 G-buffer。
                const auto inverse_model=glm::inverse(rigid_world);
                const auto origin=glm::vec3(inverse_model*glm::vec4(i.camera.position,1));
                const auto local_ray=glm::mat3(inverse_model)*ray;
                depth=(-plane_depth-origin.z)/local_ray.z;
                normal=glm::normalize(glm::transpose(glm::inverse(glm::mat3(rigid_world)))*normal);
            }
            if(occluder&&x>w/2&&x<3*w/4&&y>h/4&&y<3*h/4)depth=3.5f;
            if(corner&&ray.y<-.2f){depth=(-1.f-i.camera.position.y)/ray.y;normal={0,1,0};}
            if(ao_radius_probe&&x==51&&y==24)depth=4.4f;
            if(steep_plane) {
                normal=glm::normalize(glm::vec3(1,0,.1f));
                depth=-glm::dot(normal,i.camera.position+glm::vec3(0,0,plane_depth))/glm::dot(normal,ray);
            }
            bool valid=depth>i.camera.near_plane&&depth<i.camera.far_plane;
            glm::vec3 world=i.camera.position+ray*depth;float signal=radiance*(.2f+float((x+y)%5)*.2f);
            if(corner)signal=normal.y>.5f ? .4f:4.f;
            float material=ao_radius_probe&&x==55&&y==24 ? 2.f:1.f;
            const auto pixel_object=object+(new_object_half&&x>=w/2 ? 1:0);
            std::array<glm::vec4,9> values={valid ? glm::vec4(world,1):glm::vec4(0),glm::vec4(normal,.12f),glm::vec4(.6f,.4f,.2f,.15f),
                glm::vec4(0,0,0,material_ao),glm::vec4(glm::vec3(signal),1),glm::vec4(glm::vec3(.1f),1),
                glm::vec4(1,0,0,1),glm::vec4(material,float(pixel_object),0,0),glm::vec4(.35f,.25f,.15f,1)};
            for(std::size_t k=0;k<values.size();k++)target[k*w*h+pixel]=values[k];
        }
        effects_check(vmaFlushAllocation(allocator,upload.allocation,0,VK_WHOLE_SIZE),"test upload flush");begin();
        VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};bb.buffer=upload.buffer;bb.size=VK_WHOLE_SIZE;
        bb.srcStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;bb.srcAccessMask=VK_ACCESS_2_HOST_WRITE_BIT;bb.dstStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;bb.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
        bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;VkDependencyInfo bd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};bd.bufferMemoryBarrierCount=1;bd.pBufferMemoryBarriers=&bb;vkCmdPipelineBarrier2(command,&bd);
        for(std::size_t k=0;k<inputs.size();k++) {
            auto& im=inputs[k];barrier(im.image,im.layout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                im.layout==VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE:VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                im.layout==VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE:VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy{};copy.bufferOffset=VkDeviceSize(k)*w*h*16;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={w,h,1};
            vkCmdCopyBufferToImage(command,upload.buffer,im.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            barrier(im.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);im.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        submit();
    }
    EffectsOutput render(Settings settings,bool invalidate=false,bool baseline=true,bool meta=true) {
        auto i=build_inputs(settings);i.invalidate_history=invalidate;upload_images(i);if(!baseline)i.indirect_baseline={};if(!meta)i.optional_meta={};
        // sky fallback 合约的测试 HDR 不含间接，不需要从输入的固定信号额外扣除。
        begin();effects->record_occlusion(command,i);auto result=effects->record_post(command,i);submit();
        previous_vp=i.current_vp;last_inputs=i;last_inputs.object_motion={};++sequence;
        if(rigid){previous_rigid_world=rigid_world;previous_object=std::uint32_t(object);rigid_previous_known=true;}
        return result;
    }
    EffectsOutput render_external_graph(Settings settings,bool delayed_volume) {
        auto i=build_inputs(settings);upload_images(i);if(delayed_volume)i.volume_indirect={};
        begin();effects->record_occlusion(command,i);
        using A=ResourceAccess;using S=ResourceState;RenderGraph graph;EffectsOutput output;
        std::array<EffectsImageInput,9> images={i.position,i.normal_roughness,i.albedo_metallic,i.emission_ao,
            i.hdr,i.indirect_baseline,i.optional_tangent,i.optional_meta,i.volume_indirect};
        struct External {std::string name;EffectsImageInput image;};std::vector<External> states;
        std::array<ResourceUse,9> uses;
        for(std::size_t k=0;k<9;++k) {
            const auto name="fixture/input/"+std::to_string(k);
            if(k==8&&delayed_volume) {
                graph.add_resource(name,false,S::undefined);
                states.push_back({name,{lazy_volume.image,lazy_volume.view,lazy_volume.layout,lazy_volume.stage,lazy_volume.access}});
            } else {graph.add_resource(name,true,S::shader_read);states.push_back({name,images[k]});}
            uses[k]={name,A::read,S::shader_read};
        }
        bool volume_ready=!delayed_volume;factory_calls=legacy_callback_calls=0;factory_after_volume=false;
        if(delayed_volume) {
            graph.add_resource("fixture/volume-source",true,S::shader_read);
            states.push_back({"fixture/volume-source",{inputs[8].image,inputs[8].view,inputs[8].layout}});
            graph.add_pass("fixture-volume-copy",{{"fixture/volume-source",A::read,S::transfer_src},{uses[8].resource,A::write,S::transfer_dst}},[&]{
                VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.extent={w,h,1};
                vkCmdCopyImage(command,inputs[8].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,lazy_volume.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
                volume_ready=true;
            });
        }
        i.pass_callbacks.user=&legacy_callback_calls;
        i.pass_callbacks.begin=[](void* p,VkCommandBuffer,std::string_view) noexcept {++*static_cast<int*>(p);};
        i.pass_callbacks.end=i.pass_callbacks.begin;
        effects->add_post_passes(graph,command,i,[&]{
            ++factory_calls;factory_after_volume=volume_ready;
            if(!volume_ready)throw std::logic_error("Effects factory ran before its volume producer");
            auto live=i;if(delayed_volume)live.volume_indirect={lazy_volume.image,lazy_volume.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};return live;
        },uses,output);
        external_plan=graph.compile();std::vector<std::string> observed;std::size_t events=0;observer_balanced=true;
        graph.execute(external_plan,[&](const ResourceBarrier& b){
            if(effects->record_graph_barrier(command,b))return;
            for(auto& state:states)if(state.name==b.resource) {
                auto& image=state.image;VkImageLayout target;VkPipelineStageFlags2 stage;VkAccessFlags2 access;
                if(b.after==S::shader_read){target=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;stage=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;access=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;}
                else if(b.after==S::transfer_src){target=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;stage=VK_PIPELINE_STAGE_2_COPY_BIT;access=VK_ACCESS_2_TRANSFER_READ_BIT;}
                else if(b.after==S::transfer_dst){target=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;stage=VK_PIPELINE_STAGE_2_COPY_BIT;access=VK_ACCESS_2_TRANSFER_WRITE_BIT;}
                else throw std::logic_error("Unexpected fixture graph state");
                barrier(image.image,image.layout,target,image.stage,image.access,stage,access);image.layout=target;image.stage=stage;image.access=access;return;
            }
            throw std::logic_error("Unmapped fixture graph resource");
        },[&](std::string_view name,bool begin){
            if(begin){observed.emplace_back(name);++events;}
            else {if(observed.empty()||observed.back()!=name)observer_balanced=false;else observed.pop_back();}
        });
        observer_balanced=observer_balanced&&observed.empty()&&events==external_plan.order.size();submit();
        for(const auto& state:states) {
            for(auto& input:inputs)if(input.image==state.image.image)input.layout=state.image.layout;
            if(lazy_volume.image==state.image.image){lazy_volume.layout=state.image.layout;lazy_volume.stage=state.image.stage;lazy_volume.access=state.image.access;}
        }
        previous_vp=i.current_vp;last_inputs=i;last_inputs.object_motion={};++sequence;return output;
    }
    std::vector<glm::vec4> readback(EffectsOutput output) {
        if(!output.image||!output.width||!output.height)throw std::runtime_error("Empty effects output");begin();
        barrier(output.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};bb.buffer=download.buffer;bb.size=VK_WHOLE_SIZE;bb.srcStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;bb.srcAccessMask=VK_ACCESS_2_HOST_READ_BIT;
        bb.dstStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;bb.dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        VkDependencyInfo bd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};bd.bufferMemoryBarrierCount=1;bd.pBufferMemoryBarriers=&bb;vkCmdPipelineBarrier2(command,&bd);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={output.width,output.height,1};
        vkCmdCopyImageToBuffer(command,output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,download.buffer,1,&copy);
        barrier(output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        bb.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;bb.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;bb.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;bb.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;vkCmdPipelineBarrier2(command,&bd);
        submit();effects_check(vmaInvalidateAllocation(allocator,download.allocation,0,VK_WHOLE_SIZE),"test readback invalidate");
        std::vector<glm::vec4> data(std::size_t(output.width)*output.height);std::memcpy(data.data(),download.mapped,data.size()*16);return data;
    }
};
bool effects_pixels_finite(const std::vector<glm::vec4>& data) {
    for(auto p:data)for(int k=0;k<4;k++)if(!std::isfinite(p[k])||p[k]<-.0001f)return false;return true;
}
float effects_difference(const std::vector<glm::vec4>& a,const std::vector<glm::vec4>& b) {
    if(a.size()!=b.size())throw std::runtime_error("Effects comparison extent mismatch");double sum=0;
    for(std::size_t k=0;k<a.size();k++)sum+=glm::length(glm::vec3(a[k]-b[k]));return float(sum/double(a.size()));
}
}
TestResults test_gpu_effects(VkPhysicalDevice physical,VkDevice device,VmaAllocator allocator,VkQueue queue,
    std::uint32_t queue_family,const std::filesystem::path& shaders) {
    TestResults results=test_effects_motion_cpu();EffectsGpuFixture f;
    try {f.setup(physical,device,allocator,queue,queue_family,shaders);}
    catch(const std::exception& e){results.push_back({"GpuEffects real-device setup",false,e.what()});return results;}
    auto test=[&](const char* name,auto run) {
        try {bool pass=run();results.push_back({name,pass,pass ? "Real compute dispatch + fence-equivalent queue wait + RGBA32F readback":"Actual GPU output assertion failed"});}
        catch(const std::exception& e){results.push_back({name,false,e.what()});}
    };
    Settings s;s.path=RenderPath::deferred;s.bloom=false;s.ao=AoMode::none;s.gi=GiMode::environment;s.samples=12;s.ao_radius=2;
    struct RigidCase {
        EffectsGpuFixture& f;
        explicit RigidCase(EffectsGpuFixture& fixture):f(fixture) {
            f.rigid=true;f.rigid_previous_known=false;f.rigid_new=false;f.new_object_half=false;
            f.rigid_world=f.previous_rigid_world=glm::mat4(1);f.object=0;f.previous_object=0;
            f.corner=f.occluder=f.steep_plane=f.ao_radius_probe=false;f.camera_offset={};f.plane_depth=5;
        }
        ~RigidCase(){f.rigid=false;f.rigid_previous_known=false;f.new_object_half=false;f.rigid_new=false;
            f.object=1000003;f.camera_offset={};f.plane_depth=5;f.corner=f.occluder=f.steep_plane=f.ao_radius_probe=false;}
    };
    test("GpuEffects resize publishes valid AO view before recording",[&]{return bool(f.effects->occlusion().view)&&f.effects->occlusion().width==f.w;});
    test("GpuEffects identity keeps HDR and yields white AO",[&]{
        auto data=f.readback(f.render(s));auto ao=f.readback(f.effects->occlusion());
        for(std::size_t p=0;p<data.size();p++){float expected=.2f+float((p%f.w+p/f.w)%5)*.2f;if(std::abs(data[p].x-expected)>.0001f||std::abs(ao[p].x-1)>.0001f)return false;}
        return effects_pixels_finite(data)&&!f.effects->diagnostics().history_valid;
    });
    test("GpuEffects GI none removes baseline without scaling direct light",[&]{
        auto cfg=s;cfg.gi=GiMode::none;auto data=f.readback(f.render(cfg));
        for(std::size_t p=0;p<data.size();p++){float expected=.1f+float((p%f.w+p/f.w)%5)*.2f;if(std::abs(data[p].x-expected)>.0001f)return false;}return true;
    });
    std::vector<glm::vec4> ssao,gtao;
    test("GpuEffects SSAO samples actual nearby occluder",[&]{f.occluder=true;auto cfg=s;cfg.ao=AoMode::ssao;f.render(cfg);ssao=f.readback(f.effects->occlusion());
        float minimum=1;for(auto p:ssao)minimum=std::min(minimum,p.x);return effects_pixels_finite(ssao)&&minimum<.97f;
    });
    test("GpuEffects GTAO horizon integration differs from SSAO",[&]{auto cfg=s;cfg.ao=AoMode::gtao;f.render(cfg);gtao=f.readback(f.effects->occlusion());
        float minimum=1;for(auto p:gtao)minimum=std::min(minimum,p.x);return effects_pixels_finite(gtao)&&minimum<.97f&&!ssao.empty()&&effects_difference(ssao,gtao)>.0001f;
    });
    test("GpuEffects GTAO keeps projected radius when its endpoint leaves the viewport",[&]{
        f.occluder=false;f.ao_radius_probe=true;auto cfg=s;cfg.ao=AoMode::gtao;cfg.samples=12;cfg.ao_radius=2;
        f.render(cfg);auto ao=f.readback(f.effects->occlusion());f.ao_radius_probe=false;
        // FOV=90, z=5: radius=2 projects to 9.6 pixels even at x=55 (endpoint x=65.1).
        // A single foreground texel at (51,24) lies within the world radius. A distinct
        // material ID at the receiver isolates its AO from the following bilateral pass.
        // The old 19.2-pixel fallback misses this blocker with seed=42 / 12 slices.
        return effects_pixels_finite(ao)&&ao[24*f.w+55].x<.995f;
    });
    test("GpuEffects SSR hits reflected wall and replaces baseline",[&]{
        f.occluder=false;f.corner=true;auto env=f.readback(f.render(s));auto cfg=s;cfg.gi=GiMode::ssr;auto reflected=f.readback(f.render(cfg));
        return effects_pixels_finite(reflected)&&effects_difference(env,reflected)>.00001f&&f.effects->diagnostics().baseline_replaced;
    });
    test("GpuEffects SSR refines coarse crossings without wall hit bands",[&]{
        f.corner=true;auto cfg=s;cfg.gi=GiMode::ssr;
        for(int samples:{4,8,16}) {
            cfg.samples=samples;auto reflected=f.readback(f.render(cfg));int hits=0,total=0;
            // These floor pixels all reflect into the interior of the visible z=-5 wall.
            // Its view-space thickness is .075; a coarse ray step can be much larger.
            for(std::uint32_t y=35;y<=45;y++)for(std::uint32_t x=24;x<=39;x++) {
                ++total;if(reflected[std::size_t(y)*f.w+x].x>.45f)++hits;
            }
            if(!effects_pixels_finite(reflected)||hits*10<total*9)return false;
        }
        return true;
    });
    test("GpuEffects SSGI hemisphere hits visible wall",[&]{
        auto env=f.readback(f.render(s));auto cfg=s;cfg.gi=GiMode::ssgi;auto gi=f.readback(f.render(cfg));return effects_pixels_finite(gi)&&effects_difference(env,gi)>.00001f;
    });
    test("GpuEffects volume GI only replaces indirect and applies material AO",[&]{
        f.corner=false;f.material_ao=.5f;auto cfg=s;cfg.gi=GiMode::lpv;auto data=f.readback(f.render(cfg));f.material_ao=1;
        for(std::size_t p=0;p<data.size();p++){float input=.2f+float((p%f.w+p/f.w)%5)*.2f;
            if(glm::length(glm::vec3(data[p])-glm::vec3(input-.1f+.175f,input-.1f+.125f,input-.1f+.075f))>.0001f)return false;}return true;
    });
    test("GpuEffects HW5 bilateral and temporal accumulation execute",[&]{
        auto reference=f.readback(f.render(s));auto cfg=s;cfg.denoise=true;auto filtered=f.readback(f.render(cfg));f.render(cfg);
        auto moments=f.readback(f.effects->variance());auto center=std::size_t(f.h/2)*f.w+f.w/4;
        return effects_pixels_finite(filtered)&&effects_difference(reference,filtered)>.001f&&moments[center].w>1.5f&&f.effects->diagnostics().history_valid;
    });
    test("GpuEffects SVGF computes moments variance and five atrous levels",[&]{
        auto cfg=s;cfg.svgf=true;f.render(cfg);f.radiance=1.1f;auto data=f.readback(f.render(cfg));f.radiance=1;
        auto variance=f.readback(f.effects->variance());std::size_t levels=0;for(auto& name:f.effects->diagnostics().pass_names)if(name.find("svgf-atrous-step-")==0)++levels;
        float total=0;for(auto p:variance)total+=p.x;return levels==5&&effects_pixels_finite(data)&&effects_pixels_finite(variance)&&total>0;
    });
    test("GpuEffects TAA applies Halton projection and reprojects history",[&]{
        auto cfg=s;cfg.taa=true;auto j=f.effects->jitter();auto projection=f.last_inputs.camera.projection(float(f.w)/f.h,true);auto shifted=GpuEffects::jittered_projection(projection,j);
        glm::vec4 point(1,.5f,-5,1),a=projection*point,b=shifted*point;auto delta=glm::vec2(b)/b.w-glm::vec2(a)/a.w;
        f.render(cfg);f.render(cfg);auto data=f.readback(f.effects->variance());auto center=std::size_t(f.h/2)*f.w+f.w/4;
        return glm::length(delta-j)<1e-6f&&glm::length(j)>0&&data[center].w>1.5f&&f.effects->diagnostics().history_valid;
    });
    test("GpuEffects TAA accepts sloped-plane taps and still rejects depth disocclusion",[&]{
        // Reset Halton to its known first two samples: a quarter-pixel horizontal shift.
        f.effects->resize(f.w,f.h);f.corner=false;f.steep_plane=true;auto cfg=s;cfg.taa=true;
        f.render(cfg);f.render(cfg);auto accepted=f.readback(f.effects->variance());
        // At x=32, plane slope dz/dx=-10 makes both valid history texel centers farther
        // from the current world position than the old 2*z/height distance threshold.
        f.plane_depth=5.5f;f.render(cfg);auto rejected=f.readback(f.effects->variance());
        f.steep_plane=false;f.plane_depth=5;
        auto center=std::size_t(f.h/2)*f.w+f.w/2;
        return effects_pixels_finite(accepted)&&effects_pixels_finite(rejected)&&
            accepted[center].w>1.99f&&std::abs(rejected[center].w-1)<.001f;
    });
    test("GpuEffects exact object IDs reject stale temporal history",[&]{
        auto cfg=s;cfg.svgf=true;f.render(cfg);f.render(cfg);++f.object;f.render(cfg);auto data=f.readback(f.effects->variance());
        auto center=std::size_t(f.h/2)*f.w+f.w/4;return f.effects->diagnostics().history_valid&&std::abs(data[center].w-1)<.001f;
    });
    test("GpuEffects camera translation reprojects static world and reports motion",[&]{
        auto cfg=s;cfg.svgf=true;f.render(cfg);f.render(cfg);f.camera_offset.x=.03f;f.render(cfg);
        auto motion=f.readback(f.effects->motion()),moments=f.readback(f.effects->variance());f.camera_offset={0,0,0};
        auto center=std::size_t(f.h/2)*f.w+f.w/4;return std::abs(motion[center].x)>.001f&&moments[center].w>2.5f&&f.effects->diagnostics().history_valid;
    });
    test("GpuEffects caller invalidate resets moving-node history",[&]{auto cfg=s;cfg.taa=true;f.render(cfg);f.render(cfg);f.render(cfg,true);
        auto data=f.readback(f.effects->variance());auto center=std::size_t(f.h/2)*f.w+f.w/4;return !f.effects->diagnostics().history_valid&&std::abs(data[center].w-1)<.001f;
    });
    test("GpuEffects rigid translation and camera motion retain history across ordinary pose revision",[&]{
        RigidCase scope(f);auto cfg=s;cfg.svgf=true;f.render(cfg);f.render(cfg);
        f.rigid_world=glm::translate(glm::mat4(1),glm::vec3(.09f,0,-.04f));f.camera_offset.x=.03f;++f.pose_revision;
        f.render(cfg);const auto mv=f.readback(f.effects->motion()),moments=f.readback(f.effects->variance());
        const auto positions=f.readback(f.inputs[0].output());const auto p=std::size_t(f.h/2)*f.w+f.w/4;
        auto old=f.last_inputs.previous_vp*f.object_motion[0].current_to_previous_world*glm::vec4(glm::vec3(positions[p]),1);
        auto expected=glm::vec2((float(f.w/4)+.5f)/f.w,(float(f.h/2)+.5f)/f.h)-(glm::vec2(old)/old.w*.5f+.5f);
        return f.effects->diagnostics().history_valid&&moments[p].w>2.9f&&mv[p].w==1&&
            glm::length(glm::vec2(mv[p])-expected)<1e-5f&&glm::length(glm::vec2(mv[p]))>.001f;
    });
    test("GpuEffects rigid normal rotation and previous-plane depth preserve valid taps",[&]{
        RigidCase scope(f);auto cfg=s;cfg.taa=true;f.render(cfg);f.render(cfg);
        // 旋转足够大：直接拿 current normal 对比 history normal 会误拒绝。
        f.rigid_world=glm::rotate(glm::mat4(1),.48f,glm::vec3(0,1,0));++f.pose_revision;
        f.render(cfg);auto moments=f.readback(f.effects->variance());auto p=std::size_t(f.h/2)*f.w+f.w/2;
        return f.effects->diagnostics().history_valid&&moments[p].w>2.8f;
    });
    test("GpuEffects persistent identity survives current indirect-index reordering",[&]{
        RigidCase scope(f);auto cfg=s;cfg.taa=true;f.render(cfg);f.render(cfg);
        f.object=7;++f.pose_revision;f.render(cfg);auto moments=f.readback(f.effects->variance());auto p=std::size_t(f.h/2)*f.w+f.w/4;
        return f.object_motion[7].previous_object_id==0&&f.effects->diagnostics().history_valid&&moments[p].w>2.9f;
    });
    test("GpuEffects newly inserted object rejects locally without resetting stable objects",[&]{
        RigidCase scope(f);auto cfg=s;cfg.taa=true;f.render(cfg);f.render(cfg);
        f.new_object_half=true;++f.pose_revision;f.render(cfg);auto moments=f.readback(f.effects->variance()),motion=f.readback(f.effects->motion());
        auto left=std::size_t(f.h/2)*f.w+f.w/4,right=std::size_t(f.h/2)*f.w+3*f.w/4;
        return f.effects->diagnostics().history_valid&&moments[left].w>2.9f&&
            std::abs(moments[right].w-1)<.001f&&motion[right].w==0&&glm::length(glm::vec2(motion[right]))==0;
    });
    test("GpuEffects same-object newly exposed surface rejects stale depth history",[&]{
        // 控制 G-buffer 的局部前后面可见性；两面故意共用 object/material ID。
        // 因此只能靠 previous depth/位置拒绝旧前景，不能依赖 ID 换号或全图 reset。
        RigidCase scope(f);auto cfg=s;cfg.taa=true;f.occluder=true;f.render(cfg);f.render(cfg);
        f.occluder=false;++f.pose_revision;f.render(cfg);auto moments=f.readback(f.effects->variance());
        auto stable=std::size_t(f.h/2)*f.w+f.w/4,revealed=std::size_t(f.h/2)*f.w+5*f.w/8;
        return f.effects->diagnostics().history_valid&&moments[stable].w>2.9f&&std::abs(moments[revealed].w-1)<.001f;
    });
    test("GpuEffects multi-level bloom and NPR change GPU output",[&]{
        f.radiance=4;auto original=f.readback(f.render(s));auto cfg=s;cfg.bloom=true;cfg.bloom_strength=.2f;auto bloomed=f.readback(f.render(cfg));
        f.corner=true;auto corner=f.readback(f.render(cfg));cfg.outline=true;cfg.hatching=true;auto styled=f.readback(f.render(cfg));f.radiance=1;f.corner=false;
        return effects_difference(original,bloomed)>.01f&&effects_difference(corner,styled)>.0001f&&effects_pixels_finite(styled);
    });
    test("GpuEffects post wrapper executes individual dispatches with real ping-pong resource hazards",[&]{
        auto cfg=s;cfg.svgf=true;cfg.taa=true;cfg.bloom=true;cfg.outline=true;f.render(cfg);
        const auto& plan=f.effects->post_graph();const auto& diagnostic=f.effects->diagnostics();
        bool history=false,scratch_hazard=false,output=false;
        for(const auto& life:plan.lifetimes)if(life.resource.find("effects/history-position/")==0&&life.imported)history=true;
        for(const auto& b:plan.barriers)if(b.resource.find("effects/scratch/")==0&&b.memory_dependency&&
            b.previous_access==ResourceAccess::read&&b.next_access==ResourceAccess::write)scratch_hazard=true;
        for(const auto& image:f.effects->post_graph_images())if(image.resource==f.effects->post_output_resource()&&image.image&&image.view)output=true;
        // AO 的两次 dispatch 在 Lighting 前，post 图额外包含一个真正的 history publish 边界。
        if(plan.order.size()!=diagnostic.dispatches-2+1||plan.dependencies.empty())return false;
        for(const auto& name:plan.pass_names)if(name!="effects-history-publish"&&
            std::find(diagnostic.pass_names.begin(),diagnostic.pass_names.end(),name)==diagnostic.pass_names.end())return false;
        return history&&scratch_hazard&&output&&effects_pixels_finite(f.readback(f.effects->variance()));
    });
    test("GpuEffects same-frame graph lazily consumes produced volume and uses one observer only",[&]{
        auto cfg=s;cfg.gi=GiMode::lpv;cfg.svgf=true;cfg.taa=true;cfg.bloom=true;
        auto data=f.readback(f.render_external_graph(cfg,true));
        bool producer=false,compose=false,dependency=false;
        std::size_t producer_index=0,compose_index=0;
        for(std::size_t p=0;p<f.external_plan.pass_names.size();++p) {
            if(f.external_plan.pass_names[p]=="fixture-volume-copy"){producer=true;producer_index=p;}
            if(f.external_plan.pass_names[p]=="indirect-baseline-replace (AO indirect only)"){compose=true;compose_index=p;}
        }
        for(auto edge:f.external_plan.dependencies)if(edge.first==producer_index&&edge.second==compose_index)dependency=true;
        return producer&&compose&&dependency&&producer_index<compose_index&&f.factory_calls==1&&f.factory_after_volume&&
            f.observer_balanced&&f.legacy_callback_calls==0&&effects_pixels_finite(data);
    });
    test("GpuEffects core debug bypasses composition TAA bloom NPR",[&]{
        auto cfg=s;cfg.debug=DebugView::normal;cfg.gi=GiMode::none;cfg.svgf=true;cfg.taa=true;cfg.bloom=true;cfg.outline=true;
        auto data=f.readback(f.render(cfg));for(std::size_t p=0;p<data.size();p++){float expected=.2f+float((p%f.w+p/f.w)%5)*.2f;if(std::abs(data[p].x-expected)>.0001f)return false;}
        for(auto& name:f.effects->diagnostics().pass_names)if(name.find("compose")!=std::string::npos||name.find("bloom")!=std::string::npos||name.find("svgf")!=std::string::npos)return false;return true;
    });
    test("GpuEffects failed pipeline prepare leaves live pipelines usable",[&]{
        std::string error;auto candidate=f.effects->prepare_pipelines(shaders/"missing-effects-candidate",&error);auto data=f.readback(f.render(s));
        return !candidate&&!error.empty()&&effects_pixels_finite(data);
    });
    test("GpuEffects successful transactional reload resets history",[&]{
        std::string error;auto candidate=f.effects->prepare_pipelines(shaders,&error);if(!candidate||!error.empty())return false;
        effects_check(vkQueueWaitIdle(queue),"test reload idle");if(!f.effects->commit_pipelines(std::move(candidate)))return false;
        f.render(s);return !f.effects->diagnostics().history_valid&&f.effects->diagnostics().history_reset_reason=="pipeline reload";
    });
    test("GpuEffects resize clears GPU history and all outputs remain sampled",[&]{
        effects_check(vkQueueWaitIdle(queue),"test resize idle");f.effects->resize(f.w,f.h);auto out=f.render(s);auto data=f.readback(out);
        return effects_pixels_finite(data)&&!f.effects->diagnostics().history_valid&&out.layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    });
    test("GpuEffects two FrameSlots submit without intervening GPU wait",[&]{
        RigidCase scope(f);auto cfg=s;cfg.svgf=true;f.render(cfg);auto input=f.build_inputs(cfg);input.previous_vp=input.current_vp;
        std::array<EffectsObjectMotion,1> staged;
        struct Pending {
            VkDevice device{};std::array<VkCommandPool,2> pools{};std::array<VkFence,2> fences{};
            ~Pending(){vkDeviceWaitIdle(device);for(auto fence:fences)if(fence)vkDestroyFence(device,fence,nullptr);for(auto pool:pools)if(pool)vkDestroyCommandPool(device,pool,nullptr);}
        } pending;pending.device=device;
        for(std::uint32_t slot=0;slot<2;slot++) {
            VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool_info.queueFamilyIndex=queue_family;
            effects_check(vkCreateCommandPool(device,&pool_info,nullptr,&pending.pools[slot]),"two-slot command pool");VkCommandBuffer cmd{};
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pending.pools[slot];ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
            effects_check(vkAllocateCommandBuffers(device,&ai,&cmd),"two-slot command buffer");
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;effects_check(vkBeginCommandBuffer(cmd,&bi),"two-slot begin");
            // 两个在途槽读取不同 flags；record 后覆盖 CPU 源表，验证模块确实复制并保活。
            staged[0]=effects_make_object_motion(glm::mat4(1),glm::mat4(1),0,slot==1);input.object_motion=staged;
            input.frame_slot=slot;f.effects->record_occlusion(cmd,input);f.effects->record_post(cmd,input);effects_check(vkEndCommandBuffer(cmd),"two-slot end");
            staged[0].history_valid=0;staged[0].previous_object_id=99;
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};effects_check(vkCreateFence(device,&fi,nullptr,&pending.fences[slot]),"two-slot fence");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
            effects_check(vkQueueSubmit(queue,1,&submit,pending.fences[slot]),"two-slot submit");
        }
        effects_check(vkWaitForFences(device,2,pending.fences.data(),VK_TRUE,std::numeric_limits<std::uint64_t>::max()),"two-slot completion");f.sequence+=2;
        auto moments=f.readback(f.effects->variance());auto center=std::size_t(f.h/2)*f.w+f.w/4;
        return moments[center].w>1.99f&&moments[center].w<2.01f&&f.effects->diagnostics().motion_upload_bytes==80;
    });
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_GPU_EFFECTS_SELF_TEST
namespace {
struct EffectsTestDevice {
    VkInstance instance{};VkDevice device{};VmaAllocator allocator{};VkDebugUtilsMessengerEXT debug{};
    static inline unsigned validation_errors=0;
    static VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*) {
        if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT){++validation_errors;std::cerr<<"[validation] "<<data->pMessage<<'\n';}return VK_FALSE;
    }
    ~EffectsTestDevice() {
        if(device)vkDeviceWaitIdle(device);if(allocator)vmaDestroyAllocator(allocator);if(device)vkDestroyDevice(device,nullptr);
        if(debug){auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));destroy(instance,debug,nullptr);}
        if(instance)vkDestroyInstance(instance,nullptr);
    }
};
}
int main(int argc,char** argv) {
    using namespace emberframe::lab;if(argc!=2){std::cerr<<"Usage: effects-test <SPIR-V directory>\n";return 2;}EffectsTestDevice d;
    try {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="EmberFrame GPU effects checks";app.apiVersion=VK_API_VERSION_1_3;
        std::uint32_t count=0;vkEnumerateInstanceLayerProperties(&count,nullptr);std::vector<VkLayerProperties> layers(count);vkEnumerateInstanceLayerProperties(&count,layers.data());
        bool validation=false;for(auto& layer:layers)if(std::strcmp(layer.layerName,"VK_LAYER_KHRONOS_validation")==0)validation=true;
        const char* layer="VK_LAYER_KHRONOS_validation";const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkDebugUtilsMessengerCreateInfoEXT debug_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};debug_info.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug_info.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;debug_info.pfnUserCallback=&EffectsTestDevice::message;
        // 显式同步验证；device 只开启主工程已授权的两个 Vulkan 1.3 feature。
        VkValidationFeatureEnableEXT synchronization=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validation_info{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};validation_info.enabledValidationFeatureCount=1;validation_info.pEnabledValidationFeatures=&synchronization;validation_info.pNext=&debug_info;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};instance_info.pApplicationInfo=&app;
        if(validation){instance_info.enabledLayerCount=1;instance_info.ppEnabledLayerNames=&layer;instance_info.enabledExtensionCount=1;instance_info.ppEnabledExtensionNames=&extension;instance_info.pNext=&validation_info;}
        effects_check(vkCreateInstance(&instance_info,nullptr,&d.instance),"self-test instance");
        if(validation){auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(d.instance,"vkCreateDebugUtilsMessengerEXT"));effects_check(create(d.instance,&debug_info,nullptr,&d.debug),"self-test validation callback");}
        vkEnumeratePhysicalDevices(d.instance,&count,nullptr);std::vector<VkPhysicalDevice> physicals(count);vkEnumeratePhysicalDevices(d.instance,&count,physicals.data());
        VkPhysicalDevice chosen{};std::uint32_t family=0;
        for(auto physical:physicals) {
            VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);if(props.apiVersion<VK_API_VERSION_1_3)continue;
            std::uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,nullptr);std::vector<VkQueueFamilyProperties> queues(families);vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,queues.data());
            for(std::uint32_t k=0;k<families;k++)if((queues[k].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){chosen=physical;family=k;break;}
            if(chosen)break;
        }
        if(!chosen)throw std::runtime_error("No Vulkan 1.3 graphics+compute queue");float priority=1;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue_info.queueFamilyIndex=family;queue_info.queueCount=1;queue_info.pQueuePriorities=&priority;
        VkPhysicalDeviceVulkan13Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};features.dynamicRendering=VK_TRUE;features.synchronization2=VK_TRUE;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device_info.pNext=&features;device_info.queueCreateInfoCount=1;device_info.pQueueCreateInfos=&queue_info;
        effects_check(vkCreateDevice(chosen,&device_info,nullptr,&d.device),"self-test device (no optional features)");VkQueue queue{};vkGetDeviceQueue(d.device,family,0,&queue);
        VmaAllocatorCreateInfo allocator_info{};allocator_info.instance=d.instance;allocator_info.physicalDevice=chosen;allocator_info.device=d.device;allocator_info.vulkanApiVersion=VK_API_VERSION_1_3;
        effects_check(vmaCreateAllocator(&allocator_info,&d.allocator),"self-test VMA");VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(chosen,&properties);
        std::cout<<"Device: "<<properties.deviceName<<"; synchronization validation="<<validation<<'\n';unsigned failed=0;
        for(auto& result:test_gpu_effects(chosen,d.device,d.allocator,queue,family,std::filesystem::path(argv[1]))) {
            std::cout<<(result.passed ? "PASS ":"FAIL ")<<result.name<<": "<<result.detail<<'\n';if(!result.passed)++failed;
        }
        std::cout<<"Validation errors: "<<EffectsTestDevice::validation_errors<<'\n';return failed||EffectsTestDevice::validation_errors ? 1:0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
#endif
