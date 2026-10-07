#include "gpu_shadows.h"
#include "shadow_gi.h"
#include "systems.h"
#include <cstddef>
#include <cstring>
#include <fstream>
#include <utility>

namespace emberframe::lab {
namespace {
void require(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": VkResult="+std::to_string(result));
}
bool finite(glm::vec3 p) {return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
bool finite(const glm::mat4& m) {
    for(int c=0;c<4;++c)for(int r=0;r<4;++r)if(!std::isfinite(m[c][r]))return false;
    return true;
}
struct Bounds {glm::vec3 lo{1e30f},hi{-1e30f};bool any=false;};
Bounds bounds(const Scene& scene,std::span<const glm::mat4> worlds) {
    Bounds b;
    auto add=[&](const Mesh& mesh,const glm::mat4& model) {
        if(!finite(model)||std::abs(model[0][3])>1e-6f||std::abs(model[1][3])>1e-6f||
            std::abs(model[2][3])>1e-6f||std::abs(model[3][3]-1)>1e-6f)
            throw std::invalid_argument("Shadow worlds must be finite affine transforms");
        for(const auto& v:mesh.vertices) {
            const auto p=glm::vec3(model*glm::vec4(v.position,1));
            if(!finite(p))throw std::invalid_argument("Non-finite shadow geometry");
            b.lo=glm::min(b.lo,p);b.hi=glm::max(b.hi,p);b.any=true;
        }
    };
    if(scene.nodes.empty())for(const auto& mesh:scene.meshes)add(mesh,glm::mat4(1));
    else {
        if(worlds.size()!=scene.nodes.size())throw std::invalid_argument("Shadow worlds must match scene.nodes");
        for(std::size_t i=0;i<scene.nodes.size();++i) {
            const int mesh=scene.nodes[i].mesh;
            if(mesh<-1||mesh>=int(scene.meshes.size()))throw std::invalid_argument("Invalid shadow mesh index");
            if(mesh>=0)add(scene.meshes[std::size_t(mesh)],worlds[i]);
        }
    }
    if(!b.any){b.lo=glm::vec3(-1);b.hi=glm::vec3(1);}
    const auto extent=b.hi-b.lo;
    float pad=std::max(.02f,std::max({extent.x,extent.y,extent.z})*.025f);
    b.lo-=glm::vec3(pad);b.hi+=glm::vec3(pad);return b;
}
struct LightBasis {
    glm::vec3 x,y,z;
    explicit LightBasis(glm::vec3 direction):z(safe_normalize(direction,{0,-1,0})) {
        // 与 lookAtRH 的 right/up 一致，保证现有 shadow.frag 的 gl_FrontFacing
        // 单面材质判定成立；不能把 CPU depth-ray 基底的手性直接套到 raster。
        x=safe_normalize(glm::cross(z,std::abs(z.y)<.95f?glm::vec3(0,1,0):glm::vec3(1,0,0)));
        y=glm::cross(x,z);
    }
    glm::vec3 project(glm::vec3 p)const {return {glm::dot(x,p),glm::dot(y,p),glm::dot(z,p)};}
};
glm::mat4 light_matrix(const LightBasis& b,glm::vec2 center,float radius,float nearZ,float farZ) {
    glm::mat4 m(0);
    for(int c=0;c<3;++c){m[c][0]=b.x[c]/radius;m[c][1]=-b.y[c]/radius;m[c][2]=b.z[c]/(farZ-nearZ);}
    m[3][0]=-center.x/radius;m[3][1]=center.y/radius;m[3][2]=-nearZ/(farZ-nearZ);m[3][3]=1;
    return m;
}
std::uint32_t bounded_resolution(std::uint32_t value){return std::clamp(value,32u,2048u);}

template<class T,auto destroy> struct Owned {
    VkDevice device{};T handle{};
    explicit Owned(VkDevice d={}):device(d){}
    ~Owned(){if(handle)destroy(device,handle,nullptr);}
    Owned(const Owned&)=delete;Owned& operator=(const Owned&)=delete;
};
struct Buffer {
    VmaAllocator allocator{};VkBuffer handle{};VmaAllocation allocation{};void* mapped{};
    Buffer()=default;~Buffer(){reset();}
    Buffer(const Buffer&)=delete;Buffer& operator=(const Buffer&)=delete;
    Buffer(Buffer&& other) noexcept{swap(other);}
    Buffer& operator=(Buffer&& other)noexcept{if(this!=&other){reset();swap(other);}return *this;}
    void swap(Buffer& o)noexcept{std::swap(allocator,o.allocator);std::swap(handle,o.handle);std::swap(allocation,o.allocation);std::swap(mapped,o.mapped);}
    void reset()noexcept{if(handle)vmaDestroyBuffer(allocator,handle,allocation);handle={};allocation={};mapped=nullptr;}
};
Buffer buffer(VmaAllocator allocator,VkDeviceSize size,VkBufferUsageFlags usage) {
    Buffer b;b.allocator=allocator;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=size;ci.usage=usage;
    VmaAllocationCreateInfo ai{};ai.usage=VMA_MEMORY_USAGE_AUTO;ai.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};require(vmaCreateBuffer(allocator,&ci,&ai,&b.handle,&b.allocation,&info),"create shadow buffer");
    b.mapped=info.pMappedData;if(!b.mapped)throw std::runtime_error("Shadow buffer is not mapped");return b;
}
struct ImageResource {
    VmaAllocator allocator{};VkDevice device{};VkImage image{};VkImageView view{};VmaAllocation allocation{};
    VkFormat format=VK_FORMAT_UNDEFINED;std::uint32_t width=0,height=0;VkImageAspectFlags aspect{};
    VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
    ImageResource()=default;~ImageResource(){reset();}
    ImageResource(const ImageResource&)=delete;ImageResource& operator=(const ImageResource&)=delete;
    ImageResource(ImageResource&& o)noexcept{swap(o);}
    ImageResource& operator=(ImageResource&& o)noexcept{if(this!=&o){reset();swap(o);}return *this;}
    void swap(ImageResource& o)noexcept {
        std::swap(allocator,o.allocator);std::swap(device,o.device);std::swap(image,o.image);std::swap(view,o.view);
        std::swap(allocation,o.allocation);std::swap(format,o.format);std::swap(width,o.width);std::swap(height,o.height);std::swap(aspect,o.aspect);std::swap(layout,o.layout);
    }
    void reset()noexcept{if(view)vkDestroyImageView(device,view,nullptr);if(image)vmaDestroyImage(allocator,image,allocation);view={};image={};allocation={};}
    GpuShadowTarget target()const{return {image,view,format,width,height,layout};}
};
ImageResource image(VmaAllocator allocator,VkDevice device,std::uint32_t size,VkFormat format,VkImageUsageFlags usage,VkImageAspectFlags aspect,std::uint32_t width_multiplier=1) {
    ImageResource r;r.allocator=allocator;r.device=device;r.format=format;r.width=size*width_multiplier;r.height=size;r.aspect=aspect;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={r.width,r.height,1};
    ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;
    VmaAllocationCreateInfo ai{};ai.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    require(vmaCreateImage(allocator,&ci,&ai,&r.image,&r.allocation,nullptr),"create shadow image");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=r.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={aspect,0,1,0,1};
    require(vkCreateImageView(device,&vi,nullptr,&r.view),"create shadow image view");return r;
}
void transition(VkCommandBuffer cmd,ImageResource& image,VkImageLayout layout,VkPipelineStageFlags2 stage,VkAccessFlags2 access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask=image.layout==VK_IMAGE_LAYOUT_UNDEFINED?VK_PIPELINE_STAGE_2_NONE:VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask=image.layout==VK_IMAGE_LAYOUT_UNDEFINED?VK_ACCESS_2_NONE:VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask=stage;b.dstAccessMask=access;b.oldLayout=image.layout;b.newLayout=layout;b.image=image.image;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={image.aspect,0,1,0,1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.imageMemoryBarrierCount=1;dep.pImageMemoryBarriers=&b;
    vkCmdPipelineBarrier2(cmd,&dep);image.layout=layout;
}
std::vector<std::uint32_t> shader_code(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Missing shadow shader: "+path.string());
    const auto bytes=file.tellg();if(bytes<20||bytes%4!=0)throw std::runtime_error("Invalid shadow SPIR-V size");
    std::vector<std::uint32_t> code(std::size_t(bytes)/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),bytes);
    if(!file||code[0]!=0x07230203u)throw std::runtime_error("Invalid shadow SPIR-V");return code;
}
void compute_pipeline(VkDevice device,const std::filesystem::path& path,VkPipelineLayout layout,VkPipelineCache cache,VkPipeline& result) {
    auto code=shader_code(path);Owned<VkShaderModule,vkDestroyShaderModule> module(device);
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=code.size()*4;mi.pCode=code.data();
    require(vkCreateShaderModule(device,&mi,nullptr,&module.handle),"create shadow compute shader");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=layout;
    ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module.handle;ci.stage.pName="main";
    require(vkCreateComputePipelines(device,cache,1,&ci,nullptr,&result),"create shadow compute pipeline");
}
} // namespace

GpuShadowUniform make_gpu_shadow_uniform(const Scene& scene,const Camera& camera,const Settings& settings,
    std::span<const glm::mat4> worlds,std::uint32_t resolution) {
    resolution=bounded_resolution(resolution);
    if(!finite(camera.position)||!finite(camera.target)||!std::isfinite(camera.fov)||camera.fov<=0||camera.fov>=175||
        !std::isfinite(camera.near_plane)||!std::isfinite(camera.far_plane)||camera.near_plane<=0||camera.far_plane<=camera.near_plane||
        !std::isfinite(settings.shadow_bias)||!std::isfinite(settings.light_size)||
        int(settings.shadows)<0||int(settings.shadows)>6)throw std::invalid_argument("Invalid GPU shadow camera/settings");
    GpuShadowUniform u;u.controls={int(settings.shadows),settings.shadows==ShadowMode::csm?3:1,-1,int(resolution)};
    for(std::size_t i=0;i<std::min<std::size_t>(64,scene.lights.size());++i)if(scene.lights[i].kind==LightKind::directional) {
        if(!finite(scene.lights[i].direction))throw std::invalid_argument("Invalid shadow light direction");
        u.controls.z=int(i);break;
    }
    if(u.controls.z<0&&settings.shadows!=ShadowMode::csm)
        for(std::size_t i=0;i<std::min<std::size_t>(64,scene.lights.size());++i)
            if(scene.lights[i].kind==LightKind::rectangle){u.controls.z=int(i);break;}
    const glm::vec3 forward=safe_normalize(camera.target-camera.position,{0,0,-1});
    const glm::vec3 right=safe_normalize(glm::cross(forward,std::abs(forward.y)<.999f?glm::vec3(0,1,0):glm::vec3(0,0,1)),{1,0,0});
    const glm::vec3 up=glm::cross(right,forward);
    u.camera_view=glm::lookAtRH(camera.position,camera.position+forward,up);
    const auto b=bounds(scene,worlds);const LightBasis light(u.controls.z>=0?scene.lights[std::size_t(u.controls.z)].direction:glm::vec3(0,-1,0));
    if(u.controls.z>=0&&scene.lights[std::size_t(u.controls.z)].kind==LightKind::rectangle) {
        const auto& source=scene.lights[std::size_t(u.controls.z)];
        if(!finite(source.position)||!finite(source.direction)||!std::isfinite(source.size.x)||!std::isfinite(source.size.y))
            throw std::invalid_argument("Invalid rectangle shadow source");
        // 从发光面中心向发光方向观察；不能把有位置的面积光当作方向光。
        const auto view=glm::lookAtRH(source.position,source.position+light.z,light.y);
        const float nearZ=.02f;
        const float maxTanHalf=std::tan(glm::radians(85.f));
        float farZ=nearZ+1,tanHalf=1;
        // Z 是线性方向投影，AABB 角点仍足以计算 far；错误只在透视 x/z 拟合。
        for(int mask=0;mask<8;++mask) {
            const glm::vec3 corner{mask&1?b.hi.x:b.lo.x,mask&2?b.hi.y:b.lo.y,mask&4?b.hi.z:b.lo.z};
            farZ=std::max(farZ,light.project(corner-source.position).z+.1f);
        }
        // 光源可能位于场景 AABB 内部。只投影 AABB 八个角会漏掉中间的
        // 接收面（例如房间地板）：更远的 AABB 底角反而具有更小的 x/z。
        // 用实际几何顶点拟合；三角形跨 near 时还必须包含裁剪产生的交点。
        auto include=[&](glm::vec3 p) {
            farZ=std::max(farZ,p.z+.1f);
            if(p.z>=nearZ)tanHalf=std::max(tanHalf,std::max(std::abs(p.x),std::abs(p.y))/p.z);
        };
        auto fit=[&](const Mesh& mesh,const glm::mat4& model) {
            if(tanHalf>=maxTanHalf)return;
            std::vector<glm::vec3> points;points.reserve(mesh.vertices.size());
            for(const auto& vertex:mesh.vertices) {
                auto p=light.project(glm::vec3(model*glm::vec4(vertex.position,1))-source.position);
                points.push_back(p);include(p);
                if(tanHalf>=maxTanHalf)return;
            }
            auto edge=[&](std::uint32_t a,std::uint32_t c) {
                if(a>=points.size()||c>=points.size())throw std::invalid_argument("Invalid area shadow mesh index");
                const auto p=points[a],q=points[c];
                if((p.z<nearZ)!=(q.z<nearZ)) {
                    auto intersection=glm::mix(p,q,(nearZ-p.z)/(q.z-p.z));intersection.z=nearZ;include(intersection);
                }
            };
            if(mesh.indices.empty()) {
                for(std::uint32_t i=0;i+2<points.size()&&tanHalf<maxTanHalf;i+=3){edge(i,i+1);edge(i+1,i+2);edge(i+2,i);}
            } else for(std::size_t i=0;i+2<mesh.indices.size()&&tanHalf<maxTanHalf;i+=3) {
                edge(mesh.indices[i],mesh.indices[i+1]);edge(mesh.indices[i+1],mesh.indices[i+2]);edge(mesh.indices[i+2],mesh.indices[i]);
            }
        };
        if(scene.nodes.empty())for(const auto& mesh:scene.meshes)fit(mesh,glm::mat4(1));
        else for(std::size_t i=0;i<scene.nodes.size();++i)if(scene.nodes[i].mesh>=0)
            fit(scene.meshes[std::size_t(scene.nodes[i].mesh)],worlds[i]);
        // 不覆盖光源背后；接近平行于灯面的极端接收点最多覆盖 170°。
        tanHalf=std::clamp(tanHalf*1.03f,1.f,maxTanHalf);
        auto projection=glm::perspectiveRH_ZO(2*std::atan(tanHalf),1.f,nearZ,farZ);projection[1][1]*=-1;
        u.light_vp[0]=projection*view;u.plane_transform=glm::transpose(glm::inverse(u.light_vp[0]));
        const float sourceRadius=.5f*std::max(std::abs(source.size.x),std::abs(source.size.y));
        u.area_projection={nearZ,farZ,tanHalf,sourceRadius*std::clamp(settings.light_size/.15f,0.f,5.f)};
        u.cascade[0]={farZ-nearZ,2*tanHalf/float(resolution),0,0};
        u.tuning.x=std::max(settings.shadow_bias,0.f);u.split_far=glm::vec4(camera.far_plane);u.split_far.w=camera.near_plane;
        for(int c=1;c<4;++c){u.light_vp[c]=u.light_vp[0];u.cascade[c]=u.cascade[0];}
        if(!finite(u.light_vp[0])||!finite(u.plane_transform))throw std::invalid_argument("Area shadow projection overflow");
        return u;
    }
    glm::vec3 lo(1e30f),hi(-1e30f);
    for(int mask=0;mask<8;++mask) {
        const auto p=light.project({mask&1?b.hi.x:b.lo.x,mask&2?b.hi.y:b.lo.y,mask&4?b.hi.z:b.lo.z});
        lo=glm::min(lo,p);hi=glm::max(hi,p);
    }
    const float span=std::max(hi.z-lo.z,.01f),tangent=std::tan(glm::radians(camera.fov)*.5f);
    const float aspect=float(std::max(settings.render_width,1))/float(std::max(settings.render_height,1));
    u.tuning.x=std::max(settings.shadow_bias,0.f);u.split_far.w=camera.near_plane;
    float previous=camera.near_plane;
    for(int c=0;c<u.controls.y;++c) {
        float split=camera.far_plane,radius=std::max((hi.x-lo.x)*.5f,(hi.y-lo.y)*.5f);
        glm::vec2 center=(glm::vec2(lo)+glm::vec2(hi))*.5f;
        if(u.controls.y==3) {
            float t=float(c+1)/3;
            split=.65f*camera.near_plane*std::pow(camera.far_plane/camera.near_plane,t)
                +.35f*(camera.near_plane+(camera.far_plane-camera.near_plane)*t);
            float start=previous;
            if(c>0) {float prior=c==1?camera.near_plane:u.split_far[c-2];start=previous-.1f*(previous-prior);}
            auto worldCenter=camera.position+forward*((start+split)*.5f);radius=0;
            for(float depth:{start,split})for(int mask=0;mask<4;++mask) {
                auto p=camera.position+forward*depth+right*((mask&1?1.f:-1.f)*depth*tangent*aspect)
                    +up*((mask&2?1.f:-1.f)*depth*tangent);
                radius=std::max(radius,glm::length(p-worldCenter));
            }
            center=glm::vec2(light.project(worldCenter));
        }
        // 球包围稳定级联尺寸；增加 3 纹素边界容纳 PCF、中心取整和级联交叠。
        // 只裁 XY，不裁场景 Z，防止视锥外遮挡物在级联中消失。
        radius=std::ceil(std::max(radius,.01f)*float(resolution)/float(resolution-6)*16)/16;
        const float texel=2*radius/float(resolution);center=glm::round(center/texel)*texel;
        u.light_vp[std::size_t(c)]=light_matrix(light,center,radius,lo.z,lo.z+span);
        const float scale=std::min(std::tan(std::clamp(settings.light_size,0.f,.75f))*span/texel,1e7f);
        u.cascade[std::size_t(c)]={span,texel,scale,std::clamp(std::ceil(scale),1.f,float(resolution))};
        u.split_far[c]=split;previous=split;
    }
    for(int c=u.controls.y;c<4;++c){u.light_vp[std::size_t(c)]=u.light_vp[0];u.cascade[std::size_t(c)]=u.cascade[0];}
    if(u.controls.y==1)u.split_far.x=u.split_far.y=u.split_far.z=camera.far_plane;
    for(const auto& m:u.light_vp)if(!finite(m))throw std::invalid_argument("Shadow projection overflow");
    return u;
}

struct GpuShadowSystem::Impl {
    VkPhysicalDevice physical{};VkDevice device{};VmaAllocator allocator{};VkQueue queue{};std::uint32_t family{};
    VkFormat depth_format=VK_FORMAT_UNDEFINED;std::uint32_t size=0,active_frame=0,count=1;int light=-1;
    bool configured=false,moments_ready=false;VkCommandBuffer prepared{};
    std::array<Buffer,2> uniforms;std::array<GpuShadowUniform,2> data;
    std::array<VkDescriptorSet,2> graphics_sets{};
    std::array<VkDescriptorSet,3> compute_sets{}; // moments / rows / columns
    Owned<VkDescriptorSetLayout,vkDestroyDescriptorSetLayout> graphics_layout,compute_layout;
    Owned<VkDescriptorPool,vkDestroyDescriptorPool> pool;
    Owned<VkSampler,vkDestroySampler> sampler;
    Owned<VkPipelineLayout,vkDestroyPipelineLayout> pipeline_layout;
    Owned<VkPipeline,vkDestroyPipeline> moments_pipeline,sat_pipeline;
    VkPipelineCache cache{};
    // 深度三层常驻，让切换 CSM 不改正在使用的 frame 描述符。矩/SAT 仅一层。
    std::array<ImageResource,3> depth;ImageResource moments,scratch,sat;
    explicit Impl(VkPhysicalDevice p,VkDevice d,VmaAllocator a,VkQueue q,std::uint32_t f)
        :physical(p),device(d),allocator(a),queue(q),family(f),graphics_layout(d),compute_layout(d),pool(d),sampler(d),
        pipeline_layout(d),moments_pipeline(d),sat_pipeline(d){}
    void initialize(const std::filesystem::path& shaders,VkPipelineCache pipelineCache) {
        cache=pipelineCache;
        VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
        if(properties.apiVersion<VK_API_VERSION_1_3)throw std::runtime_error("GPU shadows require Vulkan 1.3");
        if(properties.limits.maxPerStageDescriptorSamplers<12||properties.limits.maxPerStageDescriptorSampledImages<12||
            properties.limits.maxBoundDescriptorSets<3||properties.limits.maxUniformBufferRange<sizeof(GpuShadowUniform)||
            properties.limits.maxComputeWorkGroupInvocations<128||properties.limits.maxComputeWorkGroupSize[0]<128||
            properties.limits.maxComputeSharedMemorySize<128*16+16)throw std::runtime_error("Insufficient GPU shadow limits");
        std::uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,nullptr);
        std::vector<VkQueueFamilyProperties> qp(families);vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,qp.data());
        if(family>=families||(qp[family].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))!=(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))
            throw std::runtime_error("GPU shadows require one graphics+compute queue family");
        for(auto format:{VK_FORMAT_D32_SFLOAT,VK_FORMAT_D16_UNORM}) {
            VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(physical,format,&fp);
            if((fp.optimalTilingFeatures&(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))==
                (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)){depth_format=format;break;}
        }
        VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R32G32B32A32_SFLOAT,&fp);
        if(depth_format==VK_FORMAT_UNDEFINED||(fp.optimalTilingFeatures&(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))!=
            (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))throw std::runtime_error("Required depth/RGBA32F shadow formats unavailable");
        std::array<VkDescriptorSetLayoutBinding,4> bindings{{
            {0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4,VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4,VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4,VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=4;li.pBindings=bindings.data();
        require(vkCreateDescriptorSetLayout(device,&li,nullptr,&graphics_layout.handle),"create shadow sampling layout");
        bindings={{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {3,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        require(vkCreateDescriptorSetLayout(device,&li,nullptr,&compute_layout.handle),"create shadow compute layout");
        std::array<VkDescriptorPoolSize,3> ps{{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,27},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,9}}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};poolInfo.maxSets=5;poolInfo.poolSizeCount=3;poolInfo.pPoolSizes=ps.data();
        require(vkCreateDescriptorPool(device,&poolInfo,nullptr,&pool.handle),"create shadow descriptor pool");
        std::array<VkDescriptorSetLayout,5> layouts{graphics_layout.handle,graphics_layout.handle,compute_layout.handle,compute_layout.handle,compute_layout.handle};
        std::array<VkDescriptorSet,5> sets{};VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool.handle;ai.descriptorSetCount=5;ai.pSetLayouts=layouts.data();
        require(vkAllocateDescriptorSets(device,&ai,sets.data()),"allocate shadow sets");
        graphics_sets={sets[0],sets[1]};compute_sets={sets[2],sets[3],sets[4]};
        for(auto& u:uniforms)u=buffer(allocator,sizeof(GpuShadowUniform),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        require(vkCreateSampler(device,&si,nullptr,&sampler.handle),"create shadow sampler");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,16};VkPipelineLayoutCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pci.setLayoutCount=1;pci.pSetLayouts=&compute_layout.handle;pci.pushConstantRangeCount=1;pci.pPushConstantRanges=&push;
        require(vkCreatePipelineLayout(device,&pci,nullptr,&pipeline_layout.handle),"create shadow compute pipeline layout");
        compute_pipeline(device,shaders/"gpu_shadow_moments.comp.spv",pipeline_layout.handle,cache,moments_pipeline.handle);
        compute_pipeline(device,shaders/"gpu_shadow_sat.comp.spv",pipeline_layout.handle,cache,sat_pipeline.handle);
    }
    void descriptors() {
        std::array<VkDescriptorImageInfo,4> depths{},ms{},sats{};
        for(std::size_t i=0;i<4;++i){depths[i]={sampler.handle,depth[i<3?i:0].view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
            ms[i]={sampler.handle,moments.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};sats[i]={sampler.handle,sat.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};}
        for(std::size_t f=0;f<2;++f) {
            VkDescriptorBufferInfo bi{uniforms[f].handle,0,sizeof(GpuShadowUniform)};std::array<VkWriteDescriptorSet,4> w{};
            for(std::uint32_t k=0;k<4;++k){w[k]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w[k].dstSet=graphics_sets[f];w[k].dstBinding=k;w[k].descriptorCount=k?4:1;w[k].descriptorType=k?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;}
            w[0].pBufferInfo=&bi;w[1].pImageInfo=depths.data();w[2].pImageInfo=ms.data();w[3].pImageInfo=sats.data();vkUpdateDescriptorSets(device,4,w.data(),0,nullptr);
        }
        for(std::size_t pass=0;pass<3;++pass) {
            std::array<VkDescriptorImageInfo,4> info{{{sampler.handle,depth[0].view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                {VK_NULL_HANDLE,moments.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,pass==2?scratch.view:moments.view,VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE,pass==2?sat.view:scratch.view,VK_IMAGE_LAYOUT_GENERAL}}};
            std::array<VkWriteDescriptorSet,4> writes{};
            for(std::uint32_t k=0;k<4;++k){auto& w=writes[k];w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=compute_sets[pass];w.dstBinding=k;w.descriptorCount=1;w.descriptorType=k?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&info[k];}
            vkUpdateDescriptorSets(device,4,writes.data(),0,nullptr);
        }
    }
};

GpuShadowSystem::GpuShadowSystem(VkPhysicalDevice p,VkDevice d,VmaAllocator a,VkQueue q,std::uint32_t f,const std::filesystem::path& shaders,VkPipelineCache cache) {
    if(!p||!d||!a||!q)throw std::invalid_argument("GPU shadows require borrowed physical/device/allocator/queue");
    // 所有句柄先交给 RAII Impl，initialize 或 resize 失败同样完整清理。
    impl_=std::make_unique<Impl>(p,d,a,q,f);impl_->initialize(shaders,cache);resize(256);
}
GpuShadowSystem::~GpuShadowSystem()=default;
VkDescriptorSetLayout GpuShadowSystem::descriptor_layout()const noexcept{return impl_->graphics_layout.handle;}
VkDescriptorSet GpuShadowSystem::descriptor_set(std::uint32_t f)const {if(f>=2)throw std::out_of_range("Shadow frame index");return impl_->graphics_sets[f];}
std::uint32_t GpuShadowSystem::resolution()const noexcept{return impl_->size;}
std::uint32_t GpuShadowSystem::cascade_count()const noexcept{return impl_->count;}
int GpuShadowSystem::light_index()const noexcept{return impl_->light;}
const GpuShadowUniform& GpuShadowSystem::uniform(std::uint32_t f)const {if(f>=2)throw std::out_of_range("Shadow frame index");return impl_->data[f];}
void GpuShadowSystem::resize(std::uint32_t resolution) {
    auto& s=*impl_;if(s.prepared)throw std::logic_error("Cannot resize during shadow recording");resolution=bounded_resolution(resolution);if(s.size==resolution)return;
    VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(s.physical,&p);
    if(resolution*2>p.limits.maxImageDimension2D||resolution>p.limits.maxComputeWorkGroupCount[0])throw std::length_error("Shadow resolution exceeds device limits");
    std::array<ImageResource,3> depth;
    for(auto& d:depth)d=image(s.allocator,s.device,resolution,s.depth_format,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    constexpr auto usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    auto moments=image(s.allocator,s.device,resolution,VK_FORMAT_R32G32B32A32_SFLOAT,usage,VK_IMAGE_ASPECT_COLOR_BIT);
    // SAT 每项交错保存 high/low 两个 float，避免 1024/2048 图像中前缀相减丢精度。
    auto scratch=image(s.allocator,s.device,resolution,VK_FORMAT_R32G32B32A32_SFLOAT,usage,VK_IMAGE_ASPECT_COLOR_BIT,2);
    auto sat=image(s.allocator,s.device,resolution,VK_FORMAT_R32G32B32A32_SFLOAT,usage,VK_IMAGE_ASPECT_COLOR_BIT,2);
    // 完整成功后才替换；分配失败保留旧资源及描述符。
    s.depth=std::move(depth);s.moments=std::move(moments);s.scratch=std::move(scratch);s.sat=std::move(sat);
    s.size=resolution;s.configured=false;s.moments_ready=false;s.descriptors();
}
void GpuShadowSystem::configure(const Scene& scene,const Camera& camera,const Settings& settings,std::span<const glm::mat4> worlds,std::uint32_t f) {
    auto& s=*impl_;if(f>=2)throw std::out_of_range("Shadow frame index");if(s.prepared)throw std::logic_error("Cannot configure during shadow recording");
    if(bounded_resolution(std::uint32_t(std::max(settings.shadow_resolution,0)))!=s.size)throw std::logic_error("Resize GPU shadows after wait_idle before configure");
    auto data=make_gpu_shadow_uniform(scene,camera,settings,worlds,s.size);
    std::memcpy(s.uniforms[f].mapped,&data,sizeof(data));require(vmaFlushAllocation(s.allocator,s.uniforms[f].allocation,0,sizeof(data)),"flush shadow uniform");
    s.data[f]=data;s.active_frame=f;s.count=std::uint32_t(data.controls.y);s.light=data.controls.z;s.configured=true;
}
GpuShadowTarget GpuShadowSystem::depth_target(std::uint32_t c)const {if(c>=impl_->count)throw std::out_of_range("Shadow cascade index");return impl_->depth[c].target();}
void GpuShadowSystem::prepare(VkCommandBuffer cmd) {
    auto& s=*impl_;if(!cmd||!s.configured||s.prepared)throw std::logic_error("Invalid shadow prepare order");
    for(auto& d:s.depth)transition(cmd,d,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    s.prepared=cmd;
}
void GpuShadowSystem::prepare_before_depth(std::uint32_t c,VkCommandBuffer cmd) {
    if(c>=impl_->count)throw std::out_of_range("Shadow cascade index");
    if(!impl_->prepared)prepare(cmd);
    if(impl_->prepared!=cmd)throw std::logic_error("Shadow cascades must use the same command buffer");
}
void GpuShadowSystem::finish(VkCommandBuffer cmd,std::uint32_t f) {
    auto& s=*impl_;if(!cmd||cmd!=s.prepared||f!=s.active_frame)throw std::logic_error("Invalid shadow finish order/frame");
    for(auto& d:s.depth)transition(cmd,d,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const auto mode=ShadowMode(s.data[f].controls.x);
    if(!s.moments_ready||mode==ShadowMode::vsm||mode==ShadowMode::vssm||mode==ShadowMode::msm) {
        transition(cmd,s.moments,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.moments_pipeline.handle);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.pipeline_layout.handle,0,1,&s.compute_sets[0],0,nullptr);
        const auto projection=s.data[f].area_projection;
        vkCmdPushConstants(cmd,s.pipeline_layout.handle,VK_SHADER_STAGE_COMPUTE_BIT,0,16,&projection);
        vkCmdDispatch(cmd,(s.size+7)/8,(s.size+7)/8,1);
        transition(cmd,s.moments,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        transition(cmd,s.scratch,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.sat_pipeline.handle);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.pipeline_layout.handle,0,1,&s.compute_sets[1],0,nullptr);
        int axis=0;vkCmdPushConstants(cmd,s.pipeline_layout.handle,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&axis);vkCmdDispatch(cmd,s.size,1,1);
        transition(cmd,s.scratch,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        transition(cmd,s.sat,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.pipeline_layout.handle,0,1,&s.compute_sets[2],0,nullptr);
        axis=1;vkCmdPushConstants(cmd,s.pipeline_layout.handle,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&axis);vkCmdDispatch(cmd,s.size,1,1);
        s.moments_ready=true;
    }
    transition(cmd,s.moments,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    transition(cmd,s.sat,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    s.prepared={};
    // 后续 draw_items 必须重新绑定自己的 graphics pipeline，finish 改的是 compute bind point。
}
struct GpuShadowSystem::PreparedPipelines::State {
    const GpuShadowSystem* owner{};
    Owned<VkPipeline,vkDestroyPipeline> moments,sat;
    explicit State(VkDevice device):moments(device),sat(device){}
};
GpuShadowSystem::PreparedPipelines::PreparedPipelines(std::unique_ptr<State> state):state_(std::move(state)){}
GpuShadowSystem::PreparedPipelines::~PreparedPipelines()=default;
std::unique_ptr<GpuShadowSystem::PreparedPipelines> GpuShadowSystem::prepare_pipelines(const std::filesystem::path& directory,std::string* error)const {
    auto& s=*impl_;
    try {
        if(s.prepared)throw std::logic_error("Cannot reload during shadow recording");
        auto candidate=std::make_unique<PreparedPipelines::State>(s.device);candidate->owner=this;
        compute_pipeline(s.device,directory/"gpu_shadow_moments.comp.spv",s.pipeline_layout.handle,s.cache,candidate->moments.handle);
        compute_pipeline(s.device,directory/"gpu_shadow_sat.comp.spv",s.pipeline_layout.handle,s.cache,candidate->sat.handle);
        auto token=std::unique_ptr<PreparedPipelines>(new PreparedPipelines(std::move(candidate)));
        if(error)error->clear();return token;
    } catch(const std::exception& e) {if(error)*error=e.what();return {};}
}
bool GpuShadowSystem::commit_pipelines(std::unique_ptr<PreparedPipelines> token)noexcept {
    auto& s=*impl_;if(!token||!token->state_||token->state_->owner!=this||s.prepared)return false;
    std::swap(s.moments_pipeline.handle,token->state_->moments.handle);std::swap(s.sat_pipeline.handle,token->state_->sat.handle);return true;
}
bool GpuShadowSystem::reload_pipelines(const std::filesystem::path& directory,std::string* error) {
    auto candidate=prepare_pipelines(directory,error);if(!candidate)return false;
    auto result=vkDeviceWaitIdle(impl_->device);
    if(result!=VK_SUCCESS){if(error)*error="wait_idle for shadow reload: VkResult="+std::to_string(result);return false;}
    return commit_pipelines(std::move(candidate));
}
std::span<const char* const> GpuShadowSystem::shader_files()noexcept {
    static constexpr const char* files[]{"gpu_shadow.vert","gpu_shadow_moments.comp","gpu_shadow_sat.comp","gpu_shadow_check.comp"};return files;
}

TestResults test_gpu_shadow_formulas() {
    TestResults out;
    auto add=[&](const char* name,bool okay,const char* detail){out.push_back({name,okay,detail});};
    Scene scene;Mesh mesh;mesh.vertices.resize(2);mesh.vertices[0].position={-2,-1,-3};mesh.vertices[1].position={2,4,3};scene.meshes.push_back(mesh);scene.lights.emplace_back();
    Camera camera;Settings settings;settings.shadows=ShadowMode::hard;auto u=make_gpu_shadow_uniform(scene,camera,settings,{},256);
    bool contained=true;for(int mask=0;mask<8;++mask){auto p=u.light_vp[0]*glm::vec4(mask&1?2:-2,mask&2?4:-1,mask&4?3:-3,1);contained&=std::abs(p.x)<=1&&std::abs(p.y)<=1&&p.z>=0&&p.z<=1;}
    add("GPU shadow scene projection",contained,"All scene corners fit ordinary light Z");
    const LightBasis rasterLight(scene.lights[0].direction);auto view=glm::lookAtRH(glm::vec3(0),rasterLight.z,rasterLight.y);
    bool handed=true;for(int c=0;c<3;++c)handed&=std::abs(view[c][0]-rasterLight.x[c])<1e-6f&&std::abs(view[c][1]-rasterLight.y[c])<1e-6f;
    add("GPU shadow raster handedness",handed,"Light view right/up preserve the existing fragment front-face material contract");
    settings.shadows=ShadowMode::csm;u=make_gpu_shadow_uniform(scene,camera,settings,{},256);
    bool csm=u.controls.y==3&&u.split_far.x<u.split_far.y&&u.split_far.y<u.split_far.z;
    auto f=safe_normalize(camera.target-camera.position);auto r=safe_normalize(glm::cross(f,{0,1,0}));auto up=glm::cross(r,f);float near=camera.near_plane;
    for(int c=0;c<3;++c){for(float distance:{near,u.split_far[c]})for(int mask=0;mask<4;++mask){float h=distance*std::tan(glm::radians(camera.fov)*.5f);auto p=camera.position+f*distance+r*((mask&1?1.f:-1.f)*h*float(settings.render_width)/settings.render_height)+up*((mask&2?1.f:-1.f)*h);auto q=u.light_vp[c]*glm::vec4(p,1);csm&=std::abs(q.x)<=1.0001f&&std::abs(q.y)<=1.0001f;}near=u.split_far[c];}
    add("GPU shadow CSM frustum fit",csm,"Three monotonic splits; all receiver frustum XY corners fit");
    bool snapped=true;for(int c=0;c<3;++c)for(int axis=0;axis<2;++axis){float q=u.light_vp[c][3][axis]*128;snapped&=std::abs(q-std::round(q))<.0001f;}
    add("GPU shadow CSM texel snapping",snapped,"Projection translation is an integral texel");
    auto shifted=camera;const LightBasis light(scene.lights[0].direction);shifted.position+=light.x*u.cascade[0].y*.00001f;shifted.target+=light.x*u.cascade[0].y*.00001f;
    auto v=make_gpu_shadow_uniform(scene,shifted,settings,{},256);bool stable=true;for(int c=0;c<4;++c)for(int r0=0;r0<4;++r0)stable&=std::abs(u.light_vp[0][c][r0]-v.light_vp[0][c][r0])<1e-6f;
    add("GPU shadow CSM subtexel stability",stable,"Tiny camera pan leaves snapped light projection unchanged");
    Image<float> d(16,16,.3f);MomentShadowMap map(d);
    add("GPU shadow CPU contact reference",map.pcss({.5,.5},.6f,4,10)==0&&map.vssm({.5,.5},.6f,4,10)<1e-4f,"Uniform blocker remains dark for PCSS/VSSM");
    auto m=map.moments({.5,.5},2);
    add("GPU shadow CPU MSM reference",msm_visibility(m,.6)<1e-4f&&msm_visibility(m,.2)>.999f,"Four raw moments classify constant depth");
    bool rejected=false;try{make_gpu_shadow_uniform(scene,Camera{.fov=0},settings,{},256);}catch(const std::invalid_argument&){rejected=true;}
    add("GPU shadow invalid camera",rejected,"Invalid FOV rejected before GPU state changes");
    scene.lights.clear();u=make_gpu_shadow_uniform(scene,camera,settings,{},256);
    add("GPU shadow no-light UBO",u.controls.z==-1,"No supported shadow source is explicitly unshadowed");
    Light area;area.kind=LightKind::rectangle;area.position={0,5,0};area.direction={0,-1,0};area.size={1.2f,1.2f};scene.lights.push_back(area);
    settings.shadows=ShadowMode::pcss;u=make_gpu_shadow_uniform(scene,camera,settings,{},512);
    auto nearPoint=u.light_vp[0]*glm::vec4(area.position+safe_normalize(area.direction)*u.area_projection.x,1);
    auto farPoint=u.light_vp[0]*glm::vec4(area.position+safe_normalize(area.direction)*u.area_projection.y,1);
    add("GPU rectangle perspective projection",u.controls.z==0&&u.controls.y==1&&u.area_projection.x>0&&
        std::abs(nearPoint.z/nearPoint.w)<1e-4f&&std::abs(farPoint.z/farPoint.w-1)<1e-4f,
        "Area light center defines perspective near/far; no fake directional projection");
    bool linear=true;for(float distance:{.03f,.5f,2.f,5.f}) {
        auto q=u.light_vp[0]*glm::vec4(area.position+safe_normalize(area.direction)*distance,1);
        const float n=u.area_projection.x,f=u.area_projection.y,z=q.z/q.w;
        const float restored=n*f/(f-z*(f-n));linear&=std::abs(restored-distance)<.001f;
    }
    add("GPU rectangle linear depth contract",linear,"Perspective depth is inverted before moment and blocker-distance calculations");
    // 真实房间：灯在场景 AABB 内部，地板不在 AABB 的最下表面。
    // 旧八角拟合只覆盖地板到 x/z=4.248，外侧约 .25 单位会错误漏光。
    auto room=make_demo_scene(1);auto roomWorlds=scene_world_transforms(room);
    auto roomUniform=make_gpu_shadow_uniform(room,camera,settings,roomWorlds,1024);
    bool floorCovered=true;
    for(float x:{-4.49f,0.f,4.49f})for(float z:{-4.49f,0.f,4.49f}) {
        const auto q=roomUniform.light_vp[0]*glm::vec4(x,0,z,1);
        floorCovered&=q.w>0&&std::abs(q.x/q.w)<1&&std::abs(q.y/q.w)<1&&q.z/q.w>=0&&q.z/q.w<=1;
    }
    add("GPU area light inside scene covers complete floor",floorCovered,
        "Actual receiver geometry and near-plane edge intersections, not just scene AABB corners");
    settings.shadows=ShadowMode::csm;u=make_gpu_shadow_uniform(scene,camera,settings,{},256);
    add("GPU rectangle does not impersonate CSM",u.controls.z==-1&&u.area_projection.x==0,"Only directional light may create cascades; workbench rejects this incompatible request");
    return out;
}

TestResults test_gpu_shadows(VkPhysicalDevice physical,VkDevice device,VmaAllocator allocator,VkQueue queue,
    std::uint32_t family,const std::filesystem::path& shaders,VkPipelineCache cache) {
    TestResults out;
    auto add=[&](std::string name,bool okay,std::string detail){out.push_back({std::move(name),okay,std::move(detail)});};
    try {
        require(vkQueueWaitIdle(queue),"shadow diagnostic initial queue wait");
        // 任意异常都先结束已提交的诊断工作，随后才销毁资源。
        struct QueueGuard {VkQueue queue;~QueueGuard(){vkQueueWaitIdle(queue);}};
        GpuShadowSystem system(physical,device,allocator,queue,family,shaders,cache);
        Owned<VkCommandPool,vkDestroyCommandPool> commandPool(device);
        Owned<VkFence,vkDestroyFence> fence(device);
        Owned<VkDescriptorSetLayout,vkDestroyDescriptorSetLayout> emptyLayout(device),checkLayout(device);
        Owned<VkDescriptorPool,vkDestroyDescriptorPool> checkPool(device);
        Owned<VkPipelineLayout,vkDestroyPipelineLayout> graphicsLayout(device),samplingLayout(device);
        Owned<VkPipeline,vkDestroyPipeline> graphics(device),sampling(device);
        Owned<VkShaderModule,vkDestroyShaderModule> vertexShader(device);
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;
        require(vkCreateCommandPool(device,&pc,nullptr,&commandPool.handle),"create shadow test command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=commandPool.handle;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
        VkCommandBuffer cmd{};require(vkAllocateCommandBuffers(device,&ca,&cmd),"allocate shadow test command buffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};require(vkCreateFence(device,&fi,nullptr,&fence.handle),"create shadow test fence");
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        require(vkCreateDescriptorSetLayout(device,&li,nullptr,&emptyLayout.handle),"create shadow test empty layout");
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};li.bindingCount=1;li.pBindings=&binding;
        require(vkCreateDescriptorSetLayout(device,&li,nullptr,&checkLayout.handle),"create shadow test check layout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};di.maxSets=1;di.poolSizeCount=1;di.pPoolSizes=&ps;
        require(vkCreateDescriptorPool(device,&di,nullptr,&checkPool.handle),"create shadow test check pool");
        VkDescriptorSet checkSet{};VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sa.descriptorPool=checkPool.handle;sa.descriptorSetCount=1;sa.pSetLayouts=&checkLayout.handle;
        require(vkAllocateDescriptorSets(device,&sa,&checkSet),"allocate shadow test check set");
        std::array<VkDescriptorSetLayout,3> layouts{emptyLayout.handle,emptyLayout.handle,system.descriptor_layout()};
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,80};VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=3;pli.pSetLayouts=layouts.data();pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&push;
        require(vkCreatePipelineLayout(device,&pli,nullptr,&graphicsLayout.handle),"create shadow test graphics layout");
        layouts[0]=checkLayout.handle;pli.pushConstantRangeCount=0;
        require(vkCreatePipelineLayout(device,&pli,nullptr,&samplingLayout.handle),"create shadow test sampling layout");
        compute_pipeline(device,shaders/"gpu_shadow_check.comp.spv",samplingLayout.handle,cache,sampling.handle);
        auto code=shader_code(shaders/"gpu_shadow.vert.spv");VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=code.size()*4;mi.pCode=code.data();
        require(vkCreateShaderModule(device,&mi,nullptr,&vertexShader.handle),"create shadow test vertex shader");
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stage.stage=VK_SHADER_STAGE_VERTEX_BIT;stage.module=vertexShader.handle;stage.pName="main";
        VkVertexInputBindingDescription vb{0,sizeof(Vertex),VK_VERTEX_INPUT_RATE_VERTEX};
        std::array<VkVertexInputAttributeDescription,3> attrs{{{0,0,VK_FORMAT_R32G32B32_SFLOAT,std::uint32_t(offsetof(Vertex,position))},
            {2,0,VK_FORMAT_R32G32_SFLOAT,std::uint32_t(offsetof(Vertex,uv))},{4,0,VK_FORMAT_R32G32B32A32_SFLOAT,std::uint32_t(offsetof(Vertex,color))}}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&vb;vi.vertexAttributeDescriptionCount=3;vi.pVertexAttributeDescriptions=attrs.data();
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vs.viewportCount=vs.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthTestEnable=ds.depthWriteEnable=VK_TRUE;ds.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        std::array<VkDynamicState,2> dynamic{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dyn.dynamicStateCount=2;dyn.pDynamicStates=dynamic.data();
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.depthAttachmentFormat=system.depth_target(0).format;
        VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gi.pNext=&rendering;gi.stageCount=1;gi.pStages=&stage;gi.pVertexInputState=&vi;gi.pInputAssemblyState=&ia;gi.pViewportState=&vs;gi.pRasterizationState=&rs;gi.pMultisampleState=&ms;gi.pDepthStencilState=&ds;gi.pColorBlendState=&blend;gi.pDynamicState=&dyn;gi.layout=graphicsLayout.handle;
        require(vkCreateGraphicsPipelines(device,cache,1,&gi,nullptr,&graphics.handle),"create shadow test graphics pipeline");
        Scene scene;scene.lights.emplace_back();scene.lights[0].direction={0,0,1};Mesh mesh;
        // 两片真正绘制的平面，左 z=.35、右 z=.65；不是上传/伪造深度图。
        for(int side=0;side<2;++side) {
            // 接缝避开偶数/奇数分辨率像素中心，CPU 解析夹具不依赖 top-left tie。
            float x0=side==0?-1.f:.0133f,x1=side==0?.0133f:1.f,z=side==0?.35f:.65f;
            for(auto p:std::array<glm::vec3,6>{{{x0,-1,z},{x1,-1,z},{x1,1,z},{x0,-1,z},{x1,1,z},{x0,1,z}}}){Vertex v;v.position=p;mesh.vertices.push_back(v);}
        }
        scene.meshes.push_back(mesh);auto vertices=buffer(allocator,mesh.vertices.size()*sizeof(Vertex),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        std::memcpy(vertices.mapped,mesh.vertices.data(),mesh.vertices.size()*sizeof(Vertex));require(vmaFlushAllocation(allocator,vertices.allocation,0,VK_WHOLE_SIZE),"flush shadow test vertices");
        struct CheckData {std::array<glm::vec4,8> query,result,moments,normal;};auto checks=buffer(allocator,sizeof(CheckData),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        CheckData host{};host.query={glm::vec4(-.5,0,.2,0),glm::vec4(-.5,0,.68,0),glm::vec4(.5,0,.2,0),glm::vec4(.5,0,.68,0),
            glm::vec4(-.5,0,.5,0),glm::vec4(.5,0,.5,0),glm::vec4(0,0,.5,0),glm::vec4(0,0,.5,0)};
        for(auto& normal:host.normal)normal={0,0,-1,0};
        VkDescriptorBufferInfo bi{checks.handle,0,sizeof(CheckData)};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=checkSet;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&bi;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        Camera camera;camera.position={0,0,-2};camera.target={0,0,1};camera.fov=30;camera.near_plane=.1f;camera.far_plane=4;
        Settings settings;settings.render_width=settings.render_height=256;settings.shadow_bias=0;settings.light_size=.08f;
        unsigned iteration=0;
        QueueGuard guard{queue}; // 比所有 GPU 资源更早析构。
        auto run=[&](ShadowMode mode,std::uint32_t size) {
            require(vkResetCommandPool(device,commandPool.handle,0),"reset shadow test command pool");
            system.resize(size);settings.shadow_resolution=int(size);settings.shadows=mode;
            auto frame=iteration++%2;system.configure(scene,camera,settings,{},frame);
            std::memcpy(checks.mapped,&host,sizeof(host));require(vmaFlushAllocation(allocator,checks.allocation,0,VK_WHOLE_SIZE),"flush shadow test queries");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            require(vkBeginCommandBuffer(cmd,&begin),"begin shadow test command");
            // flush + queue submit 保证 HOST 写可见；图像读写完全由模块 barrier 负责。
            for(std::uint32_t c=0;c<system.cascade_count();++c) {
                system.prepare_before_depth(c,cmd);auto target=system.depth_target(c);
                VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};attachment.imageView=target.view;attachment.imageLayout=target.layout;
                attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.clearValue.depthStencil={1,0};
                VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea.extent={size,size};ri.layerCount=1;ri.pDepthAttachment=&attachment;
                vkCmdBeginRendering(cmd,&ri);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,graphics.handle);
                auto set=system.descriptor_set(frame);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,graphicsLayout.handle,2,1,&set,0,nullptr);
                VkViewport viewport{0,0,float(size),float(size),0,1};VkRect2D scissor{{0,0},{size,size}};
                vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);VkDeviceSize offset=0;vkCmdBindVertexBuffers(cmd,0,1,&vertices.handle,&offset);
                struct Push {glm::mat4 model{1};glm::ivec4 ids{0};} constants;constants.ids.w=int(c);
                vkCmdPushConstants(cmd,graphicsLayout.handle,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(constants),&constants);
                vkCmdDraw(cmd,std::uint32_t(mesh.vertices.size()),1,0,0);vkCmdEndRendering(cmd);
            }
            system.finish(cmd,frame);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,sampling.handle);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,samplingLayout.handle,0,1,&checkSet,0,nullptr);
            auto set=system.descriptor_set(frame);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,samplingLayout.handle,2,1,&set,0,nullptr);
            vkCmdDispatch(cmd,8,1,1);
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};barrier.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;barrier.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;barrier.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;barrier.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.memoryBarrierCount=1;dep.pMemoryBarriers=&barrier;vkCmdPipelineBarrier2(cmd,&dep);
            require(vkEndCommandBuffer(cmd),"end shadow test command");require(vkResetFences(device,1,&fence.handle),"reset shadow test fence");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;require(vkQueueSubmit(queue,1,&submit,fence.handle),"submit shadow test");
            require(vkWaitForFences(device,1,&fence.handle,VK_TRUE,60'000'000'000ull),"wait shadow test fence");
            require(vmaInvalidateAllocation(allocator,checks.allocation,0,VK_WHOLE_SIZE),"invalidate shadow test readback");
            CheckData result;std::memcpy(&result,checks.mapped,sizeof(result));return result;
        };
        auto reference=[&](const GpuShadowUniform& uniform,std::uint32_t size) {
            Image<float> depth(int(size),int(size),1);auto inverse=glm::inverse(uniform.light_vp[0]);
            const float left=(uniform.light_vp[0]*glm::vec4(0,0,.35f,1)).z,right=(uniform.light_vp[0]*glm::vec4(0,0,.65f,1)).z;
            for(std::uint32_t y=0;y<size;++y)for(std::uint32_t x=0;x<size;++x) {
                auto p=inverse*glm::vec4((float(x)+.5f)/size*2-1,(float(y)+.5f)/size*2-1,.5f,1);
                if(p.x>=-1&&p.x<1&&p.y>=-1&&p.y<1)depth.at(int(x),int(y))=p.x<.0133f?left:right;
            }
            return MomentShadowMap(std::move(depth));
        };
        bool depthsOkay=true,meanOkay=true;float maxDifference=0;
        for(auto mode:{ShadowMode::hard,ShadowMode::pcf,ShadowMode::pcss,ShadowMode::vsm,ShadowMode::vssm,ShadowMode::msm}) {
            auto result=run(mode,64);auto& u=system.uniform((iteration-1)%2);auto map=reference(u,64);
            bool okay=true;std::string mismatch;
            for(int i=0;i<8;++i) {
                auto q=u.light_vp[0]*glm::vec4(glm::vec3(host.query[std::size_t(i)]),1);glm::vec2 uv=glm::vec2(q)*.5f+.5f;float expected=1;
                const int search=int(u.cascade[0].w);const float scale=u.cascade[0].z;
                switch(mode) {
                case ShadowMode::hard:expected=map.hard(uv,q.z);break;
                case ShadowMode::pcf:expected=map.pcf(uv,q.z,1);break;
                case ShadowMode::pcss:expected=map.pcss(uv,q.z,search,scale);break;
                case ShadowMode::vsm:{auto m=map.filtered_moments(uv,2);expected=cantelli_visibility({m.x,m.y},q.z);break;}
                case ShadowMode::vssm:expected=map.vssm(uv,q.z,search,scale);break;
                case ShadowMode::msm:expected=msm_visibility(map.filtered_moments(uv,2),q.z);break;
                default:break;
                }
                auto r=result.result[std::size_t(i)];float difference=std::abs(r.x-expected);maxDifference=std::max(maxDifference,difference);
                if(difference>=(mode==ShadowMode::msm?.035f:.002f))mismatch+=" query="+std::to_string(i)+" gpu="+std::to_string(r.x)+" cpu="+std::to_string(expected);
                okay&=finite(glm::vec3(r))&&r.x>=0&&r.x<=1&&difference<(mode==ShadowMode::msm?.035f:.002f);
                auto raw=map.moments(uv,i==7?8:0);
                depthsOkay&=std::abs(r.y-map.depth().at(int(uv.x*64),int(uv.y*64)))<.0001f;
                meanOkay&=std::abs(r.z-float(raw.x))<.0002f&&std::abs(r.w-float(raw.y))<.0002f;
                for(int k=0;k<4;++k)meanOkay&=std::abs(result.moments[std::size_t(i)][k]-float(raw[k]))<.0002f;
            }
            add("GPU shadow visibility mode "+std::to_string(int(mode)),okay,"Actual depth draw -> sampling readback; compare shadow_gi CPU formula"+mismatch);
        }
        add("GPU shadow real depth/moments",depthsOkay&&meanOkay,"Two rendered depth planes; all four raw moments and wide SAT mean agree with CPU");
        add("GPU shadow maximum formula error",maxDifference<.035f,"Max observed visibility error="+std::to_string(maxDifference));
        for(auto size:{256u,257u}) {
            auto result=run(ShadowMode::vssm,size);auto& u=system.uniform((iteration-1)%2);auto map=reference(u,size);
            auto q=u.light_vp[0]*glm::vec4(glm::vec3(host.query[7]),1);auto m=map.moments(glm::vec2(q)*.5f+.5f,8);auto r=result.result[7];
            add("GPU shadow segmented SAT "+std::to_string(size),std::abs(r.z-float(m.x))<.0004f&&std::abs(r.w-float(m.y))<.0004f,
                "Inclusive row/column scans across 128-thread chunks and partial tail; GPU/CPU mean="+std::to_string(r.z)+"/"+std::to_string(m.x));
        }
        for(auto size:{1024u,1025u}) {
            const auto result=run(ShadowMode::vssm,size);const auto& u=system.uniform((iteration-1)%2);const auto map=reference(u,size);
            const auto q=u.light_vp[0]*glm::vec4(glm::vec3(host.query[7]),1);const auto expected=map.moments(glm::vec2(q)*.5f+.5f,8);
            float maximum=0;for(int k=0;k<4;++k)maximum=std::max(maximum,std::abs(result.moments[7][k]-float(expected[k])));
            add("GPU shadow compensated wide-prefix narrow-query SAT "+std::to_string(size),maximum<2e-7f,
                "High/low float prefix subtraction; all four narrow-kernel moments vs double CPU, max="+std::to_string(maximum));
        }
        auto ordinaryQueries=host.query;
        for(auto& query:host.query)query.x=std::abs(query.x)>.01f ? std::copysign(.2f,query.x):query.x;
        for(int c=0;c<3;++c) {
            camera.position.z=float(-c);auto csm=run(ShadowMode::csm,64);bool okay=system.cascade_count()==3;
            for(int i:{0,2,5})okay&=csm.result[std::size_t(i)].x>.999f;
            for(int i:{1,3,4})okay&=csm.result[std::size_t(i)].x<.001f;
            auto& u=system.uniform((iteration-1)%2);float cameraDepth=-(u.camera_view*glm::vec4(glm::vec3(host.query[1]),1)).z;
            int selected=cameraDepth<=u.split_far.x?0:(cameraDepth<=u.split_far.y?1:2);okay&=selected==c;
            add("GPU shadow rendered CSM cascade "+std::to_string(c),okay,"All three views drawn with DrawPush.ids.w; this cascade's actual depth sampled for lit/occluded receivers");
        }
        host.query=ordinaryQueries;
        auto directional=scene.lights[0];scene.lights.clear();auto unshadowed=run(ShadowMode::hard,64);bool noLight=true;
        for(auto& r:unshadowed.result)noLight&=r.x==1;scene.lights.push_back(directional);
        add("GPU shadow no directional light",noLight,"GPU visibility returns one when set2 UBO light index=-1");
        std::string error;auto failed=system.prepare_pipelines(shaders/"missing-shadow-candidate",&error);
        add("GPU shadow failed reload rollback",!failed&&!error.empty(),"Missing candidate rejected without replacing live pipelines");
        auto candidate=system.prepare_pipelines(shaders,&error);bool prepared=bool(candidate);candidate.reset();
        auto after=run(ShadowMode::vsm,64);
        add("GPU shadow discarded reload rollback",prepared&&after.result[1].x<.001f&&after.result[0].x>.999f,"Prepared token destruction keeps live pipelines working");
        candidate=system.prepare_pipelines(shaders,&error);require(vkQueueWaitIdle(queue),"wait before shadow diagnostic commit");
        bool committed=system.commit_pipelines(std::move(candidate));after=run(ShadowMode::msm,64);
        add("GPU shadow successful reload commit",committed&&after.result[1].x<.001f&&after.result[0].x>.999f,"Candidate compute pipelines committed after queue retirement");
        bool failClean=false;try{GpuShadowSystem missing(physical,device,allocator,queue,family,shaders/"missing-shadow-constructor");}catch(const std::exception&){failClean=true;}
        add("GPU shadow construction failure",failClean,"Partial construction rejects missing shader and cleans borrowed-device resources");
        settings.shadows=ShadowMode::hard;system.configure(scene,camera,settings,{},0);auto first=system.uniform(0);
        settings.shadows=ShadowMode::pcss;system.configure(scene,camera,settings,{},1);
        add("GPU shadow independent frame UBOs",system.uniform(0).controls.x==first.controls.x&&system.uniform(1).controls.x==2,"Frame slot 1 configuration preserves slot 0 data");

        // 回归本次可见条纹：原来的常深度夹具无法覆盖倾斜表面自遮挡。
        // 真实绘制 z=.35+.22*x+.31*y 的平面，查询点故意不位于纹素中心。
        // 必须同时证明平面无 acne、距平面仅 .015 world units 的遮挡物仍投影，
        // 防止用过大 Bias / 关闭阴影骗过“画面变干净”的检查。
        const auto slopeNormal=safe_normalize(glm::vec3(.22f,.31f,-1));
        auto planeDepth=[](float x,float y){return .35f+.22f*x+.31f*y;};
        auto slopeMesh=[&](bool blocker) {
            mesh.vertices.clear();
            auto quad=[&](float extent,float offset) {
                for(auto xy:std::array<glm::vec2,6>{{{-extent,-extent},{extent,-extent},{extent,extent},
                                                    {-extent,-extent},{extent,extent},{-extent,extent}}}) {
                    Vertex v;v.position={xy.x,xy.y,planeDepth(xy.x,xy.y)+offset};v.normal=slopeNormal;mesh.vertices.push_back(v);
                }
            };
            quad(1,0);if(blocker)quad(.35f,-.015f);
            scene.meshes[0]=mesh;
            std::memcpy(vertices.mapped,mesh.vertices.data(),mesh.vertices.size()*sizeof(Vertex));
            require(vmaFlushAllocation(allocator,vertices.allocation,0,VK_WHOLE_SIZE),"flush sloped shadow fixture");
        };
        camera.position={0,0,-3};camera.target={0,0,0};camera.fov=45;camera.far_plane=6;
        settings.shadow_bias=.002f; // 保持工作台默认值；没有人为增大。
        const std::array<glm::vec2,8> slopeQueries{{{-.73f,-.61f},{-.43f,.24f},{-.11f,.67f},{.19f,-.52f},
                                                 {.37f,.42f},{.65f,-.18f},{.81f,.13f},{.05f,.06f}}};
        for(auto& normal:host.normal)normal=glm::vec4(slopeNormal,0);
        for(auto size:{64u,256u})for(auto mode:{ShadowMode::hard,ShadowMode::pcf,ShadowMode::pcss,ShadowMode::vsm,ShadowMode::vssm,ShadowMode::msm,ShadowMode::csm}) {
            slopeMesh(false);
            for(std::size_t i=0;i<host.query.size();++i) {
                auto xy=slopeQueries[i];host.query[i]={xy.x,xy.y,planeDepth(xy.x,xy.y),0};
            }
            auto lit=run(mode,size);float minimum=1;
            for(const auto& value:lit.result)minimum=std::min(minimum,value.x);
            add("GPU sloped receiver no acne mode "+std::to_string(int(mode))+" size "+std::to_string(size),minimum>.995f,
                "Default .002 world bias; all off-center samples must be lit; minimum="+std::to_string(minimum));
            slopeMesh(true);
            host.query[0]={0,0,planeDepth(0,0),0};host.query[1]={.04f,.04f,planeDepth(.04f,.04f),0};
            auto blocked=run(mode,size);
            const float maximum=std::max(blocked.result[0].x,blocked.result[1].x);
            const bool retained=maximum<.01f&&blocked.result[2].x>.995f&&blocked.result[5].x>.995f;
            add("GPU sloped receiver retains thin blocker mode "+std::to_string(int(mode))+" size "+std::to_string(size),retained,
                ".015 world-unit blocker remains dark, exterior remains lit; blocker max="+std::to_string(maximum));
        }
        // 同一倾斜平面改由有位置的面积光照射；逆转置需要四维平面常数项。
        scene.lights[0].kind=LightKind::rectangle;scene.lights[0].position={0,0,-2};scene.lights[0].direction={0,0,1};scene.lights[0].size={.12f,.12f};
        for(auto mode:{ShadowMode::hard,ShadowMode::pcf,ShadowMode::pcss,ShadowMode::vsm,ShadowMode::vssm,ShadowMode::msm}) {
            slopeMesh(false);
            for(std::size_t i=0;i<host.query.size();++i){auto xy=slopeQueries[i];host.query[i]={xy.x,xy.y,planeDepth(xy.x,xy.y),0};}
            auto lit=run(mode,256);float minimum=1;for(const auto& value:lit.result)minimum=std::min(minimum,value.x);
            slopeMesh(true);host.query[0]={0,0,planeDepth(0,0),0};host.query[1]={.04f,.04f,planeDepth(.04f,.04f),0};
            auto blocked=run(mode,256);float maximum=std::max(blocked.result[0].x,blocked.result[1].x);
            // MSM 返回带 moment-bias 正则化的可见性上界，不应把新测试写成硬阴影的精确零。
            // 仍要求至少 98% 遮挡；其它模式至少 99%，且保留同样的接触距离和 Bias。
            const float visibility_limit=mode==ShadowMode::msm?.02f:.01f;
            add("GPU area perspective slope and contact mode "+std::to_string(int(mode)),minimum>.995f&&maximum<visibility_limit&&blocked.result[5].x>.995f,
                "Center perspective map: plane stays lit, .015-unit blocker stays dark; lit minimum="+std::to_string(minimum)+", blocker max="+std::to_string(maximum));
        }
        slopeMesh(false);
        for(auto& normal:host.normal)normal=glm::vec4(-slopeNormal,0);
        for(auto mode:{ShadowMode::hard,ShadowMode::pcss}) {
            const auto back=run(mode,256);bool occluded=true;
            for(auto value:back.result)occluded&=value.x==0;
            add("GPU back-facing receiver avoids grazing filter stripes mode "+std::to_string(int(mode)),occluded,
                "A geometric face turned away from the shadow-source center is self-occluded, not extrapolated across a large PCSS kernel");
        }
        for(std::size_t i=0;i<host.query.size();++i)
            host.query[i]={.03f*float(i),0,-2.f+(i%2==0?1e-6f:-1e-6f),0};
        for(auto mode:{ShadowMode::hard,ShadowMode::pcss}) {
            const auto emitter=run(mode,256);bool stable=true;
            for(auto value:emitter.result)stable&=value.x==1;
            add("GPU source-plane visibility is stable mode "+std::to_string(int(mode)),stable,
                "Points on both sides of the emitter plane are outside the center shadow map near plane, independent of rounding");
        }
    }catch(const std::exception& e){add("GPU shadow diagnostic execution",false,e.what());}
    return out;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_GPU_SHADOWS_SELF_TEST
#include <iostream>
namespace {
unsigned shadowValidationErrors=0;
VKAPI_ATTR VkBool32 VKAPI_CALL shadow_validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,void*) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)++shadowValidationErrors;
    std::cerr<<"[validation] "<<data->pMessage<<'\n';return VK_FALSE;
}
}
int main(int argc,char** argv) {
    using namespace emberframe::lab;
    VkInstance instance{};VkDevice device{};VmaAllocator allocator{};VkDebugUtilsMessengerEXT debug{};
    auto cleanup=[&](){if(device)vkDeviceWaitIdle(device);if(allocator)vmaDestroyAllocator(allocator);if(device)vkDestroyDevice(device,nullptr);
        if(debug){auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));if(destroy)destroy(instance,debug,nullptr);}if(instance)vkDestroyInstance(instance,nullptr);};
    try {
        if(argc!=2)throw std::runtime_error("Usage: gpu_shadows_test <compiled shader directory>");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="EmberFrame shadow diagnostic";app.apiVersion=VK_API_VERSION_1_3;
        std::uint32_t layerCount=0;vkEnumerateInstanceLayerProperties(&layerCount,nullptr);std::vector<VkLayerProperties> layers(layerCount);vkEnumerateInstanceLayerProperties(&layerCount,layers.data());
        bool validation=false;for(auto& layer:layers)if(std::strcmp(layer.layerName,"VK_LAYER_KHRONOS_validation")==0)validation=true;
        const char* layer="VK_LAYER_KHRONOS_validation";const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkValidationFeatureEnableEXT enabled=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};validationFeatures.enabledValidationFeatureCount=1;validationFeatures.pEnabledValidationFeatures=&enabled;
        VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};debugInfo.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        debugInfo.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;debugInfo.pfnUserCallback=shadow_validation;debugInfo.pNext=&validationFeatures;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;
        if(validation){ci.enabledLayerCount=1;ci.ppEnabledLayerNames=&layer;ci.enabledExtensionCount=1;ci.ppEnabledExtensionNames=&extension;ci.pNext=&debugInfo;}
        require(vkCreateInstance(&ci,nullptr,&instance),"create diagnostic instance");
        if(validation){auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));debugInfo.pNext=nullptr;if(create)require(create(instance,&debugInfo,nullptr,&debug),"create validation messenger");}
        std::uint32_t count=0;require(vkEnumeratePhysicalDevices(instance,&count,nullptr),"enumerate shadow test device");std::vector<VkPhysicalDevice> devices(count);require(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"enumerate shadow test devices");
        VkPhysicalDevice physical{};std::uint32_t family=0;
        for(auto p:devices){std::uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(p,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(p,&n,families.data());
            for(std::uint32_t f=0;f<n;++f)if((families[f].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){physical=p;family=f;break;}if(physical)break;}
        if(!physical)throw std::runtime_error("No graphics+compute device");VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
        std::cout<<"Device: "<<properties.deviceName<<"; validation="<<validation<<"; sampler limit="<<properties.limits.maxPerStageDescriptorSamplers<<'\n';
        float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
        VkPhysicalDeviceVulkan13Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};features.dynamicRendering=features.synchronization2=VK_TRUE;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.pNext=&features;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qi;require(vkCreateDevice(physical,&dc,nullptr,&device),"create diagnostic device");
        VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);VmaAllocatorCreateInfo ac{};ac.instance=instance;ac.physicalDevice=physical;ac.device=device;ac.vulkanApiVersion=VK_API_VERSION_1_3;
        require(vmaCreateAllocator(&ac,&allocator),"create diagnostic allocator");
        auto tests=emberframe::lab::test_gpu_shadow_formulas();auto gpu=emberframe::lab::test_gpu_shadows(physical,device,allocator,queue,family,std::filesystem::path(argv[1]));tests.insert(tests.end(),gpu.begin(),gpu.end());
        unsigned failures=0;for(auto& t:tests){std::cout<<(t.passed?"PASS ":"FAIL ")<<t.name<<": "<<t.detail<<'\n';failures+=!t.passed;}
        cleanup();std::cout<<"Tests="<<tests.size()<<", failed="<<failures<<", validation errors="<<shadowValidationErrors<<'\n';return failures||shadowValidationErrors?1:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';cleanup();return 1;}
}
#endif
