#ifdef EMBERFRAME_GPU_LIGHTING_TEST_MAIN
#define VMA_IMPLEMENTATION
#endif
#include "gpu_lighting.h"
#include "shading.h"
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <utility>
#ifdef EMBERFRAME_GPU_LIGHTING_TEST_MAIN
#include <atomic>
#include <iostream>
#include <thread>
#endif

namespace emberframe::lab {
namespace {
void lighting_check(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": VkResult "+std::to_string(result));
}
bool finite(glm::vec3 x) { return std::isfinite(x.x)&&std::isfinite(x.y)&&std::isfinite(x.z); }
struct SkyKey {
    glm::vec3 top{},bottom{};
    bool operator==(const SkyKey& other) const { return top==other.top&&bottom==other.bottom; }
};
SkyKey sky_key(const Scene& scene) {
    if(!finite(scene.sky_top)||!finite(scene.sky_bottom)) throw std::invalid_argument("Non-finite lighting sky");
    // environment() 在插值之后才截断负辐亮度，不提前改变 sky 的两个端点。
    return {scene.sky_top,scene.sky_bottom};
}
IblData bake_sky(SkyKey key) {
    Scene sky;sky.sky_top=key.top;sky.sky_bottom=key.bottom;
    return precompute_ibl([sky=std::move(sky)](glm::vec3 d){return environment(sky,d);});
}
const LtcLut& lighting_ltc() {
    // 共享算法自行拟合的 8x8 粗表，每进程只拟合一次；保留 error，不能宣称论文精度。
    static const LtcLut lut=precompute_ltc();return lut;
}
// 与 CPU reference rect_axes / ShadowGi::Basis 完全相同的轴与 .95 阈值。
// size 是整个宽高，cross(half_u,half_v) 与 Light::direction 一致。
RectangleLight rectangle_frame(const Light& light) {
    const auto n=safe_normalize(light.direction,{0,-1,0});
    const auto u=safe_normalize(glm::cross(std::abs(n.y)<.95f?glm::vec3(0,1,0):glm::vec3(1,0,0),n));
    const auto v=glm::cross(n,u);
    return {light.position,u*(light.size.x*.5f),v*(light.size.y*.5f),light.color*light.intensity,false};
}
struct alignas(16) LightingRectangle {
    glm::vec4 center_kind{},half_u{},half_v{},radiance_two_sided{};
};
struct alignas(16) LightingUniform {
    glm::vec4 sky_top{},sky_bottom{};
    glm::ivec4 metadata{};
    std::array<LightingRectangle,GpuLighting::light_limit> rectangles{};
};
static_assert(sizeof(glm::vec4)==16&&sizeof(LightingRectangle)==64);
static_assert(offsetof(LightingUniform,rectangles)==48&&sizeof(LightingUniform)==4144);

struct LightingBuffer {
    VmaAllocator allocator{};VkBuffer buffer{};VmaAllocation allocation{};void* mapped{};
    ~LightingBuffer(){if(buffer)vmaDestroyBuffer(allocator,buffer,allocation);}
};
struct LightingTexture {
    VkDevice device{};VmaAllocator allocator{};VkImage image{};VkImageView view{};
    VmaAllocation allocation{};std::uint32_t levels=0;
    ~LightingTexture(){if(view)vkDestroyImageView(device,view,nullptr);if(image)vmaDestroyImage(allocator,image,allocation);}
};
struct LightingEnvironment {
    SkyKey sky;std::shared_ptr<LightingTexture> diffuse,specular;
};
struct TextureUpload {
    std::shared_ptr<LightingTexture> texture;
    std::vector<Image<glm::vec4>> levels;
};
Image<glm::vec4> rgba(const Image<glm::vec3>& input) {
    Image<glm::vec4> out(input.width,input.height);
    for(std::size_t i=0;i<input.pixels.size();++i) out.pixels[i]=glm::vec4(input.pixels[i],1);
    return out;
}
Image<glm::vec4> brdf_rgba(const IblData& ibl) {
    Image<glm::vec4> out(ibl.brdf.width,ibl.brdf.height);
    for(int y=0;y<out.height;++y) {
        // 对同一 BRDF LUT 端点插值精确求 Eavg=2*integral(E(mu)*mu)，
        // 避免另外一套采样精度导致白炉 one-scatter 与 missing-energy 不闭合。
        double average=0;
        for(int x=0;x<out.width-1;++x) {
            const double lo=double(x)/(out.width-1),hi=double(x+1)/(out.width-1);
            const auto a=ibl.brdf.at(x,y),b=ibl.brdf.at(x+1,y);
            const double ea=a.x+a.y,eb=b.x+b.y,slope=(eb-ea)/(hi-lo);
            average+=2*((ea-slope*lo)*(hi*hi-lo*lo)/2+slope*(hi*hi*hi-lo*lo*lo)/3);
        }
        for(int x=0;x<out.width;++x) out.at(x,y)=glm::vec4(ibl.brdf.at(x,y),std::clamp(float(average),0.f,1.f),0);
    }
    return out;
}
std::uint32_t round_shift(std::uint32_t x,unsigned shift) {
    const auto base=x>>shift,mask=(1u<<shift)-1,half=1u<<(shift-1),rem=x&mask;
    return base+std::uint32_t(rem>half||(rem==half&&(base&1u)));
}
std::uint16_t lighting_half(float value) {
    if(!std::isfinite(value)) throw std::runtime_error("Non-finite lighting texture data");
    value=std::clamp(value,-65504.f,65504.f);
    const auto bits=std::bit_cast<std::uint32_t>(value),sign=(bits>>16)&0x8000u,mantissa=bits&0x7fffffu;
    const int exponent=int((bits>>23)&255u)-127;
    if(exponent<-25) return std::uint16_t(sign);
    if(exponent<-14) return std::uint16_t(sign|round_shift(mantissa|0x800000u,unsigned(-exponent-1)));
    return std::uint16_t((sign|(std::uint32_t(exponent+15)<<10))+round_shift(mantissa,13));
}
} // namespace

struct GpuLighting::Impl {
    VkPhysicalDevice physical{};VkDevice device{};VmaAllocator allocator{};VkQueue queue{};
    VkFormat format=VK_FORMAT_UNDEFINED;
    VkDescriptorSetLayout layout{};VkDescriptorPool descriptors{};VkSampler sampler{};VkCommandPool commands{};
    std::array<VkDescriptorSet,frame_count> sets{};
    std::array<std::unique_ptr<LightingBuffer>,frame_count> uniforms;
    std::shared_ptr<LightingTexture> brdf,inverse,amplitude;
    std::shared_ptr<LightingEnvironment> active;
    std::array<std::shared_ptr<LightingEnvironment>,frame_count> slot_environment;
    std::filesystem::path shader_directory;
    SkyKey wanted;
    struct SkyBake {SkyKey sky;IblData data;};
    std::future<SkyBake> bake;
    std::uint64_t bake_count=0;
    struct Upload {
        VkCommandBuffer command{};VkFence fence{};bool submitted=false;
        std::unique_ptr<LightingBuffer> staging;
        std::vector<TextureUpload> textures;
        std::shared_ptr<LightingEnvironment> environment;
    };
    std::unique_ptr<Upload> upload;

    ~Impl() {
        // 这里只等本模块自己的 fence；调用方负责已提交 core 帧的资源生命周期。
        if(upload&&upload->submitted) vkWaitForFences(device,1,&upload->fence,VK_TRUE,UINT64_MAX);
        release_upload();
        if(commands)vkDestroyCommandPool(device,commands,nullptr);
        if(descriptors)vkDestroyDescriptorPool(device,descriptors,nullptr);
        if(layout)vkDestroyDescriptorSetLayout(device,layout,nullptr);
        if(sampler)vkDestroySampler(device,sampler,nullptr);
    }
    void release_upload() noexcept {
        if(!upload)return;
        if(upload->fence)vkDestroyFence(device,upload->fence,nullptr);
        if(upload->command)vkFreeCommandBuffers(device,commands,1,&upload->command);
        upload.reset();
    }
    std::unique_ptr<LightingBuffer> buffer(VkDeviceSize bytes,VkBufferUsageFlags usage,bool readback=false) {
        auto out=std::make_unique<LightingBuffer>();out->allocator=allocator;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=bytes;info.usage=usage;
        VmaAllocationCreateInfo memory{};memory.usage=VMA_MEMORY_USAGE_AUTO;
        memory.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|(readback?VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT:VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        VmaAllocationInfo allocation_info{};
        lighting_check(vmaCreateBuffer(allocator,&info,&memory,&out->buffer,&out->allocation,&allocation_info),"lighting buffer");
        out->mapped=allocation_info.pMappedData;
        if(!out->mapped)throw std::runtime_error("Lighting host buffer was not mapped");
        return out;
    }
    std::shared_ptr<LightingTexture> texture(const std::vector<Image<glm::vec4>>& levels) {
        if(levels.empty())throw std::invalid_argument("Empty lighting texture");
        auto out=std::make_shared<LightingTexture>();out->allocator=allocator;out->device=device;out->levels=std::uint32_t(levels.size());
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;info.format=format;
        info.extent={std::uint32_t(levels[0].width),std::uint32_t(levels[0].height),1};info.mipLevels=out->levels;info.arrayLayers=1;
        info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        VkImageFormatProperties properties{};
        lighting_check(vkGetPhysicalDeviceImageFormatProperties(physical,format,info.imageType,info.tiling,info.usage,0,&properties),"lighting image format");
        if(info.mipLevels>properties.maxMipLevels||info.extent.width>properties.maxExtent.width||info.extent.height>properties.maxExtent.height)
            throw std::runtime_error("Lighting image exceeds device limits");
        VmaAllocationCreateInfo memory{};memory.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        lighting_check(vmaCreateImage(allocator,&info,&memory,&out->image,&out->allocation,nullptr),"lighting image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=out->image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;
        view.format=format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,info.mipLevels,0,1};
        lighting_check(vkCreateImageView(device,&view,nullptr,&out->view),"lighting image view");return out;
    }
    TextureUpload to_upload(std::vector<Image<glm::vec4>> levels) {return {texture(levels),std::move(levels)};}
    std::pair<std::shared_ptr<LightingEnvironment>,std::vector<TextureUpload>> environment_upload(SkyKey key,const IblData& ibl) {
        auto environment=std::make_shared<LightingEnvironment>();environment->sky=key;
        std::vector<TextureUpload> textures;textures.push_back(to_upload({rgba(ibl.diffuse)}));
        std::vector<Image<glm::vec4>> levels;levels.reserve(ibl.specular.size());
        for(const auto& level:ibl.specular) levels.push_back(rgba(level));
        textures.push_back(to_upload(std::move(levels)));
        environment->diffuse=textures[0].texture;environment->specular=textures[1].texture;
        return {std::move(environment),std::move(textures)};
    }
    void submit_upload(std::vector<TextureUpload> textures,std::shared_ptr<LightingEnvironment> environment={}) {
        if(upload)throw std::logic_error("Lighting upload already pending");
        upload=std::make_unique<Upload>();upload->textures=std::move(textures);upload->environment=std::move(environment);
        const std::size_t stride=format==VK_FORMAT_R32G32B32A32_SFLOAT?16:8;
        std::size_t bytes=0;for(const auto& t:upload->textures)for(const auto& level:t.levels)bytes+=level.pixels.size()*stride;
        try {
            upload->staging=buffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            auto* destination=static_cast<std::byte*>(upload->staging->mapped);std::size_t offset=0;
            for(const auto& t:upload->textures)for(const auto& level:t.levels) {
                if(stride==16) {
                    for(const auto& pixel:level.pixels)for(int c=0;c<4;++c)
                        if(!std::isfinite(pixel[c]))throw std::runtime_error("Non-finite lighting texture data");
                    std::memcpy(destination+offset,level.pixels.data(),level.pixels.size()*stride);
                } else {
                    auto* half=reinterpret_cast<std::uint16_t*>(destination+offset);
                    for(const auto& pixel:level.pixels)for(int c=0;c<4;++c)*half++=lighting_half(pixel[c]);
                }
                offset+=level.pixels.size()*stride;
            }
            lighting_check(vmaFlushAllocation(allocator,upload->staging->allocation,0,VK_WHOLE_SIZE),"lighting staging flush");
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocate.commandPool=commands;
            allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
            lighting_check(vkAllocateCommandBuffers(device,&allocate,&upload->command),"lighting upload command");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            lighting_check(vkBeginCommandBuffer(upload->command,&begin),"lighting upload begin");
            offset=0;
            for(const auto& t:upload->textures) {
                VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};barrier.image=t.texture->image;
                barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
                barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.dstStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;barrier.dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,t.texture->levels,0,1};
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.imageMemoryBarrierCount=1;dependency.pImageMemoryBarriers=&barrier;
                vkCmdPipelineBarrier2(upload->command,&dependency);
                for(std::size_t level=0;level<t.levels.size();++level) {
                    VkBufferImageCopy copy{};copy.bufferOffset=offset;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,std::uint32_t(level),0,1};
                    copy.imageExtent={std::uint32_t(t.levels[level].width),std::uint32_t(t.levels[level].height),1};
                    vkCmdCopyBufferToImage(upload->command,upload->staging->buffer,t.texture->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
                    offset+=t.levels[level].pixels.size()*stride;
                }
                barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;barrier.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;barrier.dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                vkCmdPipelineBarrier2(upload->command,&dependency);
            }
            lighting_check(vkEndCommandBuffer(upload->command),"lighting upload end");
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};lighting_check(vkCreateFence(device,&fence,nullptr,&upload->fence),"lighting upload fence");
            VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};command.commandBuffer=upload->command;
            VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};submit.commandBufferInfoCount=1;submit.pCommandBufferInfos=&command;
            lighting_check(vkQueueSubmit2(queue,1,&submit,upload->fence),"lighting upload submit");upload->submitted=true;
        } catch(...) {release_upload();throw;}
    }
    void write_set(std::uint32_t frame) {
        VkDescriptorBufferInfo uniform{uniforms[frame]->buffer,0,sizeof(LightingUniform)};
        std::array<VkDescriptorImageInfo,5> images{};
        const std::array<std::shared_ptr<LightingTexture>,5> textures{active->diffuse,active->specular,brdf,inverse,amplitude};
        std::array<VkWriteDescriptorSet,6> writes{};
        for(std::size_t i=0;i<writes.size();++i) {
            auto& write=writes[i];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=sets[frame];
            write.dstBinding=std::uint32_t(i);write.descriptorCount=1;
            if(i==0){write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&uniform;}
            else {images[i-1]={sampler,textures[i-1]->view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&images[i-1];}
        }
        vkUpdateDescriptorSets(device,std::uint32_t(writes.size()),writes.data(),0,nullptr);
        slot_environment[frame]=active;
    }
    void initialize(std::uint32_t family) {
        const std::array<VkFormat,2> candidates{
#ifdef EMBERFRAME_GPU_LIGHTING_TEST_HALF
            VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT
#else
            VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT
#endif
        };
        for(const auto candidate:candidates) {
            VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(physical,candidate,&p);
            constexpr auto needed=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if((p.optimalTilingFeatures&needed)==needed){format=candidate;break;}
        }
        if(format==VK_FORMAT_UNDEFINED)throw std::runtime_error("No sampled RGBA32F/RGBA16F transfer-destination format");
        VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
        if(properties.limits.maxUniformBufferRange<sizeof(LightingUniform))throw std::runtime_error("Lighting UBO exceeds device range");
        std::array<VkDescriptorSetLayoutBinding,6> bindings{};
        for(std::size_t i=0;i<bindings.size();++i) {
            bindings[i].binding=std::uint32_t(i);bindings[i].descriptorCount=1;
            bindings[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout_info.bindingCount=std::uint32_t(bindings.size());layout_info.pBindings=bindings.data();
        lighting_check(vkCreateDescriptorSetLayout(device,&layout_info,nullptr,&layout),"lighting descriptor layout");
        const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,frame_count},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,5*frame_count}}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=frame_count;
        pool.poolSizeCount=std::uint32_t(sizes.size());pool.pPoolSizes=sizes.data();
        lighting_check(vkCreateDescriptorPool(device,&pool,nullptr,&descriptors),"lighting descriptor pool");
        std::array<VkDescriptorSetLayout,frame_count> layouts{};layouts.fill(layout);
        VkDescriptorSetAllocateInfo sets_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sets_info.descriptorPool=descriptors;
        sets_info.descriptorSetCount=frame_count;sets_info.pSetLayouts=layouts.data();
        lighting_check(vkAllocateDescriptorSets(device,&sets_info,sets.data()),"lighting descriptor sets");
        VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler_info.magFilter=sampler_info.minFilter=VK_FILTER_NEAREST;
        sampler_info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler_info.addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeV=sampler_info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sampler_info.maxLod=VK_LOD_CLAMP_NONE;
        lighting_check(vkCreateSampler(device,&sampler_info,nullptr,&sampler),"lighting sampler");
        VkCommandPoolCreateInfo commands_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};commands_info.queueFamilyIndex=family;
        commands_info.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        lighting_check(vkCreateCommandPool(device,&commands_info,nullptr,&commands),"lighting command pool");
        for(auto& u:uniforms)u=buffer(sizeof(LightingUniform),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        wanted=sky_key(Scene{});const auto ibl=bake_sky(wanted);++bake_count;
        auto [environment,textures]=environment_upload(wanted,ibl);
        textures.push_back(to_upload({brdf_rgba(ibl)}));brdf=textures.back().texture;
        const auto& ltc=lighting_ltc();Image<glm::vec4> matrix(ltc.entries.width,ltc.entries.height),weights(matrix.width,matrix.height);
        for(std::size_t i=0;i<ltc.entries.pixels.size();++i) {
            const auto& e=ltc.entries.pixels[i];
            matrix.pixels[i]={e.inverse[0][0],e.inverse[2][0],e.inverse[0][2],e.inverse[2][2]};
            weights.pixels[i]={e.inverse[1][1],e.fresnel_integral.x,e.fresnel_integral.y,e.fit_error};
        }
        textures.push_back(to_upload({std::move(matrix)}));inverse=textures.back().texture;
        textures.push_back(to_upload({std::move(weights)}));amplitude=textures.back().texture;
        submit_upload(std::move(textures),environment);
        // 首次初始化需要完整可用资源；以后 configure 只 poll fence，不等待队列。
        lighting_check(vkWaitForFences(device,1,&upload->fence,VK_TRUE,UINT64_MAX),"lighting initial upload wait");
        active=std::move(environment);release_upload();
        for(std::uint32_t frame=0;frame<frame_count;++frame) {
            LightingUniform data{};data.sky_top=glm::vec4(active->sky.top,0);data.sky_bottom=glm::vec4(active->sky.bottom,0);
            data.metadata={0,int(active->specular->levels),int(ShadingMode::pbr),0};
            std::memcpy(uniforms[frame]->mapped,&data,sizeof(data));
            lighting_check(vmaFlushAllocation(allocator,uniforms[frame]->allocation,0,VK_WHOLE_SIZE),"lighting initial UBO flush");write_set(frame);
        }
    }
    void advance_environment() {
        if(upload) {
            const auto status=vkGetFenceStatus(device,upload->fence);
            if(status!=VK_NOT_READY) {
                lighting_check(status,"lighting upload poll");
                if(upload->environment&&upload->environment->sky==wanted)active=upload->environment;
                release_upload();
            }
        }
        if(bake.valid()&&bake.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            auto ready=bake.get();
            if(ready.sky==wanted&&!(active->sky==wanted)) {
                auto [environment,textures]=environment_upload(ready.sky,ready.data);
                submit_upload(std::move(textures),std::move(environment));
            }
        }
        if(!bake.valid()&&!upload&&!(active->sky==wanted)) {
            const auto key=wanted;++bake_count;
            // 不捕获 Scene& 或 this；编辑场景不会与后台读取产生竞争。
            bake=std::async(std::launch::async,[key]{return SkyBake{key,bake_sky(key)};});
        }
    }
};

GpuLighting::GpuLighting(VkPhysicalDevice physical,VkDevice device,VmaAllocator allocator,VkQueue queue,
                         std::uint32_t family,const std::filesystem::path& shaders,VkPipelineCache cache)
    :impl_(std::make_unique<Impl>()) {
    if(!physical||!device||!allocator||!queue)throw std::invalid_argument("Invalid lighting Vulkan handles");
    impl_->physical=physical;impl_->device=device;impl_->allocator=allocator;impl_->queue=queue;
    impl_->shader_directory=shaders;(void)cache; // 没有自有 pipeline，不借用或销毁主线的 cache。
    impl_->initialize(family);
}
GpuLighting::~GpuLighting()=default;
VkDescriptorSetLayout GpuLighting::descriptor_layout() const noexcept {return impl_->layout;}
VkDescriptorSet GpuLighting::descriptor_set(std::uint32_t frame) const {
    if(frame>=frame_count)throw std::out_of_range("Lighting frame index must be 0 or 1");return impl_->sets[frame];
}
void GpuLighting::configure(const Scene& scene,const Settings& settings,std::uint32_t frame) {
    if(frame>=frame_count)throw std::out_of_range("Lighting frame index must be 0 or 1");
    LightingUniform data{};data.metadata={int(std::min(scene.lights.size(),std::size_t(light_limit))),int(impl_->active->specular->levels),int(settings.shading),settings.energy_compensation?1:0};
    for(int i=0;i<data.metadata.x;++i) {
        const auto& light=scene.lights[std::size_t(i)];auto& rectangle=data.rectangles[std::size_t(i)];
        rectangle.center_kind=glm::vec4(light.position,float(light.kind));
        if(light.kind!=LightKind::rectangle)continue;
        if(!finite(light.position)||!finite(light.direction)||!finite(light.color)||glm::dot(light.direction,light.direction)<1e-16f||
           !std::isfinite(light.size.x)||!std::isfinite(light.size.y)||light.size.x<=0||light.size.y<=0||
           !std::isfinite(light.intensity)||light.intensity<0||glm::any(glm::lessThan(light.color,glm::vec3(0))))
            throw std::invalid_argument("Invalid GPU rectangle light");
        const auto source=rectangle_frame(light);
        if(!finite(source.radiance))throw std::invalid_argument("GPU rectangle radiance overflow");
        rectangle.half_u=glm::vec4(source.half_u,0);rectangle.half_v=glm::vec4(source.half_v,0);
        rectangle.radiance_two_sided=glm::vec4(source.radiance,source.two_sided?1:0);
    }
    impl_->wanted=sky_key(scene);impl_->advance_environment();
    data.sky_top=glm::vec4(impl_->active->sky.top,0);data.sky_bottom=glm::vec4(impl_->active->sky.bottom,0);
    data.metadata.y=int(impl_->active->specular->levels);
    std::memcpy(impl_->uniforms[frame]->mapped,&data,sizeof(data));
    lighting_check(vmaFlushAllocation(impl_->allocator,impl_->uniforms[frame]->allocation,0,VK_WHOLE_SIZE),"lighting UBO flush");
    if(impl_->slot_environment[frame]!=impl_->active)impl_->write_set(frame);
}
bool GpuLighting::reload_pipelines(const std::filesystem::path& directory,std::string* error) {
    try {
        auto candidate=directory;std::error_code ec;
        if(!std::filesystem::is_directory(candidate,ec)||ec)throw std::runtime_error("Lighting shader directory is not readable: "+candidate.string());
        // include-only 模块的可执行 shader 候选属于主 ShaderLibrary；此接口不创建隐藏 pipeline。
        lighting_check(vkDeviceWaitIdle(impl_->device),"lighting reload wait idle");
        impl_->shader_directory.swap(candidate);
        if(error)error->clear();return true;
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
bool GpuLighting::environment_ready() const noexcept {return impl_->active&&impl_->active->sky==impl_->wanted;}
std::uint64_t GpuLighting::environment_bake_count() const noexcept {return impl_->bake_count;}
VkFormat GpuLighting::texture_format() const noexcept {return impl_->format;}
} // namespace emberframe::lab

#ifdef EMBERFRAME_GPU_LIGHTING_TEST_MAIN
// 独立诊断入口，不进入生产构建；使用真实 Vulkan compute + SSBO 回读，
// VkDevice 仅启用 dynamicRendering/synchronization2，不启用 sampled-image indexing。
namespace {
using namespace emberframe::lab;
void require_lighting(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
float vec_error(glm::vec3 a,glm::vec3 b) {return glm::max(glm::max(glm::abs(a-b).x,glm::abs(a-b).y),glm::abs(a-b).z);}
struct LightingTestDevice {
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};std::uint32_t family=0;
    VmaAllocator allocator{};VkDebugUtilsMessengerEXT messenger{};std::atomic<int> errors{0};
    static VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
        if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            ++static_cast<LightingTestDevice*>(user)->errors;std::cerr<<"VALIDATION "<<data->pMessage<<'\n';
        }
        return VK_FALSE;
    }
    void initialize() {
        std::uint32_t count=0;lighting_check(vkEnumerateInstanceLayerProperties(&count,nullptr),"test layers");
        std::vector<VkLayerProperties> layers(count);lighting_check(vkEnumerateInstanceLayerProperties(&count,layers.data()),"test layers");
        const bool validation=std::any_of(layers.begin(),layers.end(),[](const auto& p){return std::strcmp(p.layerName,"VK_LAYER_KHRONOS_validation")==0;});
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};application.pApplicationName="GpuLighting numerical diagnostic";application.apiVersion=VK_API_VERSION_1_3;
        const char* layer="VK_LAYER_KHRONOS_validation";const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        debug.pfnUserCallback=message;debug.pUserData=this;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&application;
        if(validation){info.enabledLayerCount=1;info.ppEnabledLayerNames=&layer;info.enabledExtensionCount=1;info.ppEnabledExtensionNames=&extension;info.pNext=&debug;}
        lighting_check(vkCreateInstance(&info,nullptr,&instance),"test instance");
        if(validation) {
            const auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
            require_lighting(create!=nullptr,"No validation debug messenger");lighting_check(create(instance,&debug,nullptr,&messenger),"test messenger");
        }
        lighting_check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"test physical devices");
        std::vector<VkPhysicalDevice> devices(count);lighting_check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"test physical devices");
        int best=-1;
        for(const auto candidate:devices) {
            VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(candidate,&p);if(p.apiVersion<VK_API_VERSION_1_3)continue;
            VkPhysicalDeviceVulkan13Features f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&f;vkGetPhysicalDeviceFeatures2(candidate,&features);
            if(!f.dynamicRendering||!f.synchronization2)continue;
            std::uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,families.data());
            for(std::uint32_t i=0;i<n;++i)if((families[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) {
                const int score=p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?2:1;
                if(score>best){best=score;physical=candidate;family=i;}break;
            }
        }
        require_lighting(physical!=VK_NULL_HANDLE,"No Vulkan 1.3 graphics/compute device");
        float priority=1;VkDeviceQueueCreateInfo queues{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queues.queueFamilyIndex=family;queues.queueCount=1;queues.pQueuePriorities=&priority;
        VkPhysicalDeviceVulkan13Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};enabled.dynamicRendering=enabled.synchronization2=VK_TRUE;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device_info.pNext=&enabled;device_info.queueCreateInfoCount=1;device_info.pQueueCreateInfos=&queues;
        lighting_check(vkCreateDevice(physical,&device_info,nullptr,&device),"test device");vkGetDeviceQueue(device,family,0,&queue);
        VmaAllocatorCreateInfo memory{};memory.instance=instance;memory.physicalDevice=physical;memory.device=device;memory.vulkanApiVersion=VK_API_VERSION_1_3;
        lighting_check(vmaCreateAllocator(&memory,&allocator),"test allocator");
        VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(physical,&p);
        std::cout<<"DEVICE "<<p.deviceName<<"; validation="<<validation<<"; only dynamicRendering+sync2 enabled\n";
    }
    ~LightingTestDevice() {
        if(device)vkDeviceWaitIdle(device);
        if(allocator)vmaDestroyAllocator(allocator);if(device)vkDestroyDevice(device,nullptr);
        if(messenger)reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,messenger,nullptr);
        if(instance)vkDestroyInstance(instance,nullptr);
    }
};
struct LightingDiagnostic {
    static constexpr std::uint32_t value_count=180;
    LightingTestDevice& context;
    VkDescriptorSetLayout output_layout{},empty_layout{};VkDescriptorPool descriptors{};VkDescriptorSet output_set{};
    VkShaderModule shader{};VkPipelineLayout layout{};VkPipeline pipeline{};VkCommandPool commands{};VkCommandBuffer command{};VkFence fence{};
    std::unique_ptr<LightingBuffer> output;
    explicit LightingDiagnostic(LightingTestDevice& d):context(d){}
    void initialize(GpuLighting& lighting,const std::filesystem::path& spv) {
        const auto device=context.device;
        output=std::make_unique<LightingBuffer>();output->allocator=context.allocator;
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=value_count*sizeof(glm::vec4);buffer.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo memory{};memory.usage=VMA_MEMORY_USAGE_AUTO;memory.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo allocation{};
        lighting_check(vmaCreateBuffer(context.allocator,&buffer,&memory,&output->buffer,&output->allocation,&allocation),"diagnostic output");output->mapped=allocation.pMappedData;
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};set_info.bindingCount=1;set_info.pBindings=&binding;
        lighting_check(vkCreateDescriptorSetLayout(device,&set_info,nullptr,&output_layout),"diagnostic layout");
        set_info.bindingCount=0;set_info.pBindings=nullptr;lighting_check(vkCreateDescriptorSetLayout(device,&set_info,nullptr,&empty_layout),"diagnostic empty layout");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        lighting_check(vkCreateDescriptorPool(device,&pool,nullptr,&descriptors),"diagnostic pool");
        VkDescriptorSetAllocateInfo set_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};set_allocate.descriptorPool=descriptors;set_allocate.descriptorSetCount=1;set_allocate.pSetLayouts=&output_layout;
        lighting_check(vkAllocateDescriptorSets(device,&set_allocate,&output_set),"diagnostic set");
        VkDescriptorBufferInfo output_info{output->buffer,0,buffer.size};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=output_set;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&output_info;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        std::ifstream stream(spv,std::ios::binary|std::ios::ate);require_lighting(bool(stream),"Missing lighting diagnostic SPIR-V");
        const auto bytes=stream.tellg();require_lighting(bytes>0&&bytes%4==0,"Invalid lighting diagnostic SPIR-V size");
        std::vector<std::uint32_t> words(std::size_t(bytes)/4);stream.seekg(0);stream.read(reinterpret_cast<char*>(words.data()),std::streamsize(bytes));
        require_lighting(bool(stream),"Truncated lighting diagnostic SPIR-V");
        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader_info.codeSize=words.size()*4;shader_info.pCode=words.data();
        lighting_check(vkCreateShaderModule(device,&shader_info,nullptr,&shader),"diagnostic shader");
        const std::array<VkDescriptorSetLayout,4> layouts{output_layout,empty_layout,empty_layout,lighting.descriptor_layout()};
        VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pipeline_layout.setLayoutCount=4;pipeline_layout.pSetLayouts=layouts.data();
        lighting_check(vkCreatePipelineLayout(device,&pipeline_layout,nullptr,&layout),"diagnostic pipeline layout");
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};compute.layout=layout;
        compute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};compute.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;compute.stage.module=shader;compute.stage.pName="main";
        lighting_check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&compute,nullptr,&pipeline),"diagnostic compute pipeline");
        VkCommandPoolCreateInfo command_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};command_pool.queueFamilyIndex=context.family;command_pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        lighting_check(vkCreateCommandPool(device,&command_pool,nullptr,&commands),"diagnostic command pool");
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocate.commandPool=commands;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
        lighting_check(vkAllocateCommandBuffers(device,&allocate,&command),"diagnostic command");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};lighting_check(vkCreateFence(device,&fence_info,nullptr,&fence),"diagnostic fence");
    }
    std::array<glm::vec4,value_count> run(GpuLighting& lighting,std::uint32_t frame) {
        lighting_check(vkResetFences(context.device,1,&fence),"diagnostic fence reset");lighting_check(vkResetCommandBuffer(command,0),"diagnostic command reset");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        lighting_check(vkBeginCommandBuffer(command,&begin),"diagnostic begin");vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        const auto lighting_set=lighting.descriptor_set(frame);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&output_set,0,nullptr);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,3,1,&lighting_set,0,nullptr);
        vkCmdDispatch(command,value_count,1,1);
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};barrier.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask=VK_ACCESS_2_SHADER_WRITE_BIT;barrier.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;barrier.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=output->buffer;barrier.size=VK_WHOLE_SIZE;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.bufferMemoryBarrierCount=1;dependency.pBufferMemoryBarriers=&barrier;
        vkCmdPipelineBarrier2(command,&dependency);lighting_check(vkEndCommandBuffer(command),"diagnostic end");
        VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};command_info.commandBuffer=command;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};submit.commandBufferInfoCount=1;submit.pCommandBufferInfos=&command_info;
        lighting_check(vkQueueSubmit2(context.queue,1,&submit,fence),"diagnostic submit");lighting_check(vkWaitForFences(context.device,1,&fence,VK_TRUE,UINT64_MAX),"diagnostic wait");
        lighting_check(vmaInvalidateAllocation(context.allocator,output->allocation,0,VK_WHOLE_SIZE),"diagnostic invalidate");
        std::array<glm::vec4,value_count> values{};std::memcpy(values.data(),output->mapped,sizeof(values));
        for(const auto value:values)require_lighting(finite(glm::vec3(value))&&std::isfinite(value.w),"Non-finite GPU diagnostic result");return values;
    }
    ~LightingDiagnostic() {
        const auto device=context.device;
        if(fence)vkDestroyFence(device,fence,nullptr);if(commands)vkDestroyCommandPool(device,commands,nullptr);
        if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
        if(shader)vkDestroyShaderModule(device,shader,nullptr);if(descriptors)vkDestroyDescriptorPool(device,descriptors,nullptr);
        if(output_layout)vkDestroyDescriptorSetLayout(device,output_layout,nullptr);if(empty_layout)vkDestroyDescriptorSetLayout(device,empty_layout,nullptr);
    }
};
Scene lighting_test_scene() {
    Scene scene;
    for(int j=0;j<16;++j) {
        Light light;light.kind=LightKind::rectangle;light.position={0,0,2};light.direction={0,0,-1};
        light.size={2,2};light.color={1,.7f,.4f};light.intensity=1+.1f*j;
        if(j==1)light.size={8,8};if(j==2)light.position.z=.05f;if(j==3)light.direction.z=1;
        if(j==4||j==5){light.position={1,0,j==4?.15f:-2.f};light.direction={-1,0,0};light.size={1.2f,1.4f};}
        if(j>=6) {
            const float y=j%2==0?-.949f:-.951f;
            light.direction={std::sqrt(1-y*y),y,0};light.position=-light.direction*2.f+glm::vec3(0,0,.3f);
            light.size={.5f+.1f*j,1.7f};
        }
        if(j==15)light.kind=LightKind::point;scene.lights.push_back(light);
    }
    return scene;
}
void await_sky(GpuLighting& lighting,const Scene& scene,const Settings& settings) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    do {
        lighting.configure(scene,settings,0);lighting.configure(scene,settings,1);
        if(lighting.environment_ready())return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while(std::chrono::steady_clock::now()<deadline);
    throw std::runtime_error("Lighting async sky bake timed out");
}
int run_lighting_diagnostic(const std::filesystem::path& spv) {
    LightingTestDevice device;device.initialize();
    {
        GpuLighting lighting(device.physical,device.device,device.allocator,device.queue,device.family,spv.parent_path());
        LightingDiagnostic diagnostic(device);diagnostic.initialize(lighting,spv);
        auto scene=lighting_test_scene();Settings settings;settings.shading=ShadingMode::disney;settings.energy_compensation=false;
        lighting.configure(scene,settings,0);lighting.configure(scene,settings,1);const auto values=diagnostic.run(lighting,0);
        const auto ibl=bake_sky(sky_key(scene));const auto& ltc=lighting_ltc();
        float brdf_error=0,ibl_error=0,area_error=0,polygon_error=0,lut_error=0;
        for(std::uint32_t i=0;i<64;++i) {
            Material material;material.base_color={.7f,.2f,.08f,1};material.roughness=.02f+.98f*float(i%9)/8;material.metallic=float(i%3)/2;
            material.clearcoat=float(i%5)/4;material.clearcoat_roughness=.001f+.998f*float(i%4)/3;
            material.anisotropy=-.95f+1.9f*float(i%6)/5;material.sheen=float(i%4)/3;
            auto v=safe_normalize(glm::vec3(.45f,.25f,.1f+.9f*float(i%7)/6),{0,0,1});
            auto l=safe_normalize(glm::vec3(-.7f+.2f*float(i%8),.35f,.2f+.8f*float(i%5)/4),{0,0,1});
            if(i==62)l.z=-l.z;if(i==63)v.z=-v.z;
            const auto expected=evaluate_brdf(material,{0,0,1},v,l,{1,.2f,.1f,i%2==0?1.f:-1.f},settings);
            const float error=vec_error(glm::vec3(values[i]),expected)/(1+glm::length(expected));brdf_error=std::max(brdf_error,error);
        }
        require_lighting(brdf_error<4e-4f,"GPU Disney differs from shared CPU formula");
        for(std::uint32_t j=0;j<16;++j) {
            const float angle=float(j)*2*pi/16;auto n=safe_normalize(glm::vec3(std::cos(angle),-.95f+1.9f*float(j%5)/4,std::sin(angle)));
            if(j==0)n={0,1,0};if(j==1)n={0,-1,0};if(j==2)n=safe_normalize(glm::vec3(1,0,-.00001f));
            const auto v=safe_normalize(n+glm::vec3(.2f,.1f,.15f),n);const float r=float(j%7)/6,metal=float(j%3)/2,nv=glm::dot(n,v);
            const auto base=glm::vec3(.7f,.2f,.08f),f0=glm::mix(glm::vec3(.04f),base,metal);
            const auto expected=base*(1-metal)*(glm::vec3(1)-schlick_fresnel(f0,nv))*sample_ibl_diffuse(ibl,n)/pi+evaluate_ibl_specular(ibl,n,v,f0,r);
            ibl_error=std::max(ibl_error,vec_error(glm::vec3(values[64+j]),expected));
            const auto rectangle=rectangle_frame(scene.lights[j]);const auto view=safe_normalize(glm::vec3(.2f*float(j%3),.1f,1));
            const auto diffuse=base*(1-metal)*(glm::vec3(1)-schlick_fresnel(f0,view.z));
            const auto expected_area=j==15?glm::vec3(0):evaluate_ltc_rectangle(ltc,rectangle,{0,0,0},{0,0,1},view,diffuse,f0,.2f+.8f*float(j%5)/4);
            area_error=std::max(area_error,vec_error(glm::vec3(values[80+j]),expected_area));
            const float expected_polygon=j==15?0:integrate_ltc_rectangle(rectangle,{0,0,0},{0,0,1},{0,0,1},glm::mat3(1));
            polygon_error=std::max(polygon_error,std::abs(values[96+j].x-expected_polygon));
            const auto entry=sample_ltc(ltc,float(j%4)/3,float(j/4)/3);
            const auto expected_lut=glm::vec4(entry.inverse[0][0],entry.inverse[1][1],entry.fresnel_integral);
            for(int c=0;c<4;++c)lut_error=std::max(lut_error,std::abs(values[112+j][c]-expected_lut[c])/(1+std::abs(expected_lut[c])));
        }
        const float texture_tolerance=lighting.texture_format()==VK_FORMAT_R32G32B32A32_SFLOAT?4e-5f:3e-3f;
        require_lighting(ibl_error<texture_tolerance,"GPU IBL differs from CPU prefilter/LUT sampling");
        require_lighting(area_error<texture_tolerance,"GPU LTC table/polygon differs from CPU");
        require_lighting(polygon_error<2e-5f,"GPU physical/transformed horizon clipping mismatch");
        require_lighting(lut_error<texture_tolerance,"GPU LTC inverse/AB packing mismatch");
        float quadrature_error=0;
        Material white;white.base_color=glm::vec4(1);Settings toon;toon.shading=ShadingMode::toon;
        for(const int j:{0,1,4,5}) {
            auto rectangle=rectangle_frame(scene.lights[std::size_t(j)]);rectangle.radiance=glm::vec3(1);
            const auto reference=integrate_rectangle_reference(rectangle,{0,0,0},{0,0,1},{0,0,1},{1,0,0,1},white,toon,65536);
            quadrature_error=std::max(quadrature_error,std::abs(values[96+std::size_t(j)].x-reference.r));
        }
        require_lighting(quadrature_error<2e-4f,"GPU analytic Lambert polygon differs from independent area quadrature");
        std::cout<<"PASS Disney normalized max error="<<brdf_error<<"; IBL="<<ibl_error<<"; LTC="<<area_error
                 <<"; clipped polygon="<<polygon_error<<"; LUT="<<lut_error<<"; independent quadrature="<<quadrature_error<<'\n';
        float ggx_error=0,fit_error=0;Settings ggx;ggx.energy_compensation=false;
        for(const auto& entry:ltc.entries.pixels)fit_error=std::max(fit_error,entry.fit_error);
        for(int j=0;j<4;++j) {
            Material glossy;glossy.base_color=glm::vec4(1);glossy.metallic=1;glossy.roughness=float(4+j)/7;
            const auto reference=integrate_rectangle_reference(rectangle_frame(scene.lights[0]),{0,0,0},{0,0,1},{0,0,1},{1,0,0,1},glossy,ggx,65536);
            const float relative=glm::length(glm::vec3(values[144+std::size_t(j)])-reference)/std::max(glm::length(reference),1e-6f);
            ggx_error=std::max(ggx_error,relative);
        }
        require_lighting(ggx_error<.3f,"Coarse LTC GGX fit exceeds documented reference tolerance");
        std::cout<<"PASS coarse 8x8 LTC vs independent GGX area quadrature: relative="<<ggx_error<<"; max fit objective="<<fit_error<<'\n';
        // 面积改变必须改变积分，背面/半球外/非矩形必须为零，不能退回中心点近似。
        require_lighting(values[96].x>0&&values[97].x>values[96].x*3,"Rectangle size does not affect integral");
        require_lighting(values[99].x==0&&values[101].x==0&&glm::length(glm::vec3(values[95]))==0,"Backside/horizon/nonrectangle leak");
        // 独立 include 的面积光 KC 使用已上传 A/B/Eavg；CPU 显式用同一表，
        // 比较的是模型/几何/能量实现误差，不将两种 LUT 的采样误差混入门槛。
        const auto energy_pixels=brdf_rgba(ibl);EnergyLut area_energy;
        area_energy.directional.reset(ibl.brdf.width,ibl.brdf.height);area_energy.average.resize(ibl.brdf.height);
        for(int y=0;y<ibl.brdf.height;++y) {
            area_energy.average[std::size_t(y)]=energy_pixels.at(0,y).z;
            for(int x=0;x<ibl.brdf.width;++x) {const auto ab=ibl.brdf.at(x,y);area_energy.directional.at(x,y)=std::clamp(ab.x+ab.y,0.f,1.f);}
        }
        std::array<glm::vec3,4> model_responses{};float selected_error=0;const auto model_bakes=lighting.environment_bake_count();
        for(const auto mode:{ShadingMode::pbr,ShadingMode::blinn_phong,ShadingMode::disney,ShadingMode::toon}) {
            settings.shading=mode;settings.energy_compensation=false;lighting.configure(scene,settings,0);const auto off=diagnostic.run(lighting,0);
            settings.energy_compensation=true;lighting.configure(scene,settings,1);const auto on=diagnostic.run(lighting,1);
            model_responses[std::size_t(mode)]=glm::vec3(off[148]);
            for(int j=0;j<16;++j) {
                Material material;material.base_color={.7f,.2f,.08f,1};material.roughness=.45f+.55f*float(j%5)/4;material.metallic=float(j%3)/2;
                material.clearcoat=.7f;material.clearcoat_roughness=.25f;material.anisotropy=-.6f+.6f*float(j%3);material.sheen=.4f;
                const auto view=safe_normalize(glm::vec3(.2f*float(j%3),.1f,1));const glm::vec4 tangent(.8f,.6f,0,j%2==0?1.f:-1.f);
                for(const bool enabled:{false,true}) {
                    auto selected=settings;selected.energy_compensation=enabled;
                    const auto expected=j==15?glm::vec3(0):evaluate_rectangle_light(ltc,rectangle_frame(scene.lights[std::size_t(j)]),{0,0,0},{0,0,1},view,tangent,material,selected,area_energy);
                    const auto& result=enabled?on:off;const float error=vec_error(glm::vec3(result[148+std::size_t(j)]),expected)/(1+glm::length(expected));
                    selected_error=std::max(selected_error,error);require_lighting(error<texture_tolerance,"GPU selected rectangle BRDF/KC differs from shared CPU formula");
                    require_lighting(glm::length(glm::vec3(result[164+std::size_t(j)]))<1e-7f,"Legacy int/uint area overload is not equivalent to neutral tangent/lobes");
                }
                if(mode==ShadingMode::blinn_phong||mode==ShadingMode::toon||(mode==ShadingMode::disney&&std::abs(material.anisotropy)>=1e-5f))
                    require_lighting(vec_error(glm::vec3(off[148+std::size_t(j)]),glm::vec3(on[148+std::size_t(j)]))<1e-7f,"Isotropic KC leaked into a non-GGX/anisotropic rectangle model");
            }
            if(mode==ShadingMode::pbr)require_lighting(on[148].x>off[148].x+1e-7f,"PBR rectangle KC is inert");
            if(mode==ShadingMode::disney)require_lighting(on[149].x>off[149].x+1e-7f,"Isotropic Disney rectangle KC is inert");
        }
        require_lighting(vec_error(model_responses[0],model_responses[1])>1e-4f&&vec_error(model_responses[0],model_responses[2])>1e-4f,"GPU rectangle material-model selection is inert");
        require_lighting(lighting.environment_bake_count()==model_bakes,"Area model/KC diagnostic toggles rebake IBL");
        settings.shading=ShadingMode::disney;settings.energy_compensation=false;lighting.configure(scene,settings,0);lighting.configure(scene,settings,1);
        std::cout<<"PASS rectangle PBR/Blinn/Disney/Toon + KC scope + legacy overloads: normalized CPU error="<<selected_error<<'\n';
        const auto initial_bakes=lighting.environment_bake_count();
        for(std::uint32_t frame=0;frame<32;++frame){++scene.revision;scene.lights[0].intensity+=.01f;lighting.configure(scene,settings,frame%2);}
        require_lighting(lighting.environment_bake_count()==initial_bakes,"Ordinary revision/light changes rebake IBL");
        std::string error;const auto old_layout=lighting.descriptor_layout();const auto old_set=lighting.descriptor_set(0);
        require_lighting(!lighting.reload_pipelines(spv/"missing",&error)&&!error.empty(),"Bad reload unexpectedly accepted");
        require_lighting(lighting.descriptor_layout()==old_layout&&lighting.descriptor_set(0)==old_set,"Failed reload mutated old resources");
        require_lighting(lighting.reload_pipelines(spv.parent_path(),&error)&&error.empty(),"Valid include-only reload failed");
        // 两个旧 slot 的描述符在新天空上传时仍可读；只迁移调用 configure 的 slot。
        const auto old_slot=diagnostic.run(lighting,1);
        scene.sky_top=scene.sky_bottom=glm::vec3(1);lighting.configure(scene,settings,0);
        const auto retained=diagnostic.run(lighting,1);
        for(std::size_t i=64;i<80;++i)require_lighting(vec_error(glm::vec3(old_slot[i]),glm::vec3(retained[i]))<1e-7f,"In-flight slot lost old environment");
        await_sky(lighting,scene,settings);
        require_lighting(lighting.environment_bake_count()==initial_bakes+1,"Sky change did not coalesce to one bake");
        auto single=diagnostic.run(lighting,0);settings.energy_compensation=true;lighting.configure(scene,settings,1);
        auto multiple=diagnostic.run(lighting,1);float furnace_error=0;
        for(std::uint32_t j=0;j<16;++j)furnace_error=std::max(furnace_error,vec_error(glm::vec3(multiple[128+j]),glm::vec3(1)));
        require_lighting(furnace_error<texture_tolerance,"KC+IBL white furnace fails or single scatter counted twice");
        require_lighting(single[143].x<.4f&&multiple[143].x>.999f,"KC missing-energy path not effective at roughness=1/NV=1");
        std::cout<<"PASS KC+IBL white furnace: single="<<single[143].x<<" -> multiple="<<multiple[143].x<<"; worst="<<furnace_error<<'\n';
        require_lighting(lighting.environment_bake_count()==initial_bakes+1,"Settings/KC toggles rebake IBL");
        // 快速编辑两个天空后丢弃过时结果，只发布最新完整版本。
        scene.sky_top={.2f,.4f,.8f};lighting.configure(scene,settings,0);
        scene.sky_top={.8f,.3f,.1f};scene.sky_bottom={.1f,.07f,.04f};await_sky(lighting,scene,settings);
        const auto final_count=lighting.environment_bake_count();
        for(int i=0;i<8;++i)lighting.configure(scene,settings,std::uint32_t(i%2));
        require_lighting(lighting.environment_bake_count()==final_count,"Steady sky retriggers background bake");
        std::cout<<"PASS two-slot lifetime, async latest-sky coalescing, no per-frame bake, reload failure retention\n";
    }
    require_lighting(device.errors.load()==0,"Vulkan validation errors during lighting diagnostic");
    std::cout<<"PASS validation errors="<<device.errors.load()<<'\n';return 0;
}
} // namespace
int main(int argc,char** argv) {
    try {require_lighting(argc==2,"Usage: gpu_lighting_test <gpu_lighting.comp.spv>");return run_lighting_diagnostic(argv[1]);}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
#endif
