#include "vulkan_workbench.h"
#include "shading.h"
#include "systems.h"
#include "gpu_upload.h"
#include "gpu_pipeline_cache.h"
#include "gpu_shadows.h"
#include "gpu_effects.h"
#include "effects_motion.h"
#include "environment.h"
#include "gpu_lighting.h"
#include "gpu_volume.h"
#include "gpu_scene_resources.h"
#include "scene_resources.h"
#include "shader_assets.h"
#include "renderer/vulkan_context.h"
#include "renderer/vulkan_device.h"
#include "renderer/vulkan_swapchain.h"
#include "platform/sdl_window.h"
#include "platform/window_chrome.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <imgui.h>
#include "editor_ui.h"
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>
// 当前资产加载器使用 STB_IMAGE_STATIC；这里也保持独立的内部实现，避免引出重复全局符号。
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <vk_mem_alloc.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <cmath>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <numeric>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace emberframe::lab {
namespace {
// GI 的光源视图独立于直接光阴影分辨率；128px/面保持小几何可见，
// 六面横向 atlas 仅 768px，远低于 Vulkan 1.3 的最小 2D 图像尺寸能力。
constexpr std::uint32_t rsm_face_resolution=128;
constexpr VkFormat hdr_format=VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat depth_format=VK_FORMAT_D32_SFLOAT;
constexpr int light_limit=64,cluster_slices=16;
constexpr std::uint32_t profile_pass_limit=192,profile_queries_per_slot=2+profile_pass_limit*2;
using ProfileClock=std::chrono::steady_clock;
double elapsed_ms(ProfileClock::time_point start) {return std::chrono::duration<double,std::milli>(ProfileClock::now()-start).count();}
glm::vec4 primitive_sphere(const Mesh& mesh,const Primitive& primitive) {
    if(!primitive.index_count)return glm::vec4(0);
    if(primitive.first_index>mesh.indices.size()||primitive.index_count>mesh.indices.size()-primitive.first_index)
        throw std::invalid_argument("Primitive bounds range exceeds indices");
    glm::vec3 center(0);
    for(std::uint32_t i=0;i<primitive.index_count;++i)center+=mesh.vertices.at(mesh.indices[primitive.first_index+i]).position;
    center/=float(primitive.index_count);float radius=0;
    for(std::uint32_t i=0;i<primitive.index_count;++i)radius=std::max(radius,glm::length(mesh.vertices[mesh.indices[primitive.first_index+i]].position-center));
    return {center,radius};
}
void check(VkResult r,const char* what) {
    if(r!=VK_SUCCESS) throw std::runtime_error(std::string(what)+": VkResult "+std::to_string(r));
}
bool cpu_path(RenderPath p) { return p==RenderPath::cpu_raster||p==RenderPath::path_trace; }
bool srgb_format(VkFormat f) { return f==VK_FORMAT_B8G8R8A8_SRGB||f==VK_FORMAT_R8G8B8A8_SRGB; }
bool bmp_format(VkFormat f) { return srgb_format(f)||f==VK_FORMAT_B8G8R8A8_UNORM||f==VK_FORMAT_R8G8B8A8_UNORM; }
struct alignas(16) GpuLight { glm::vec4 position_range,direction_kind,color_intensity; };
struct alignas(16) Globals {
    glm::mat4 vp{1},view{1},projection{1},light_vp{1};
    glm::vec4 camera_near{},sky_top_far{},sky_bottom_exposure{};
    glm::ivec4 dimensions{},modes{},options{};
    glm::vec4 post{};
    glm::ivec4 misc{};
    std::array<GpuLight,light_limit> lights{};
};
// 材质资源保留 Disney 参数；固定 std430 布局与所有 Shader 的 MaterialData 对齐。
struct alignas(16) GpuMaterial { glm::vec4 base,emissive_normal,factors; glm::ivec4 flags; glm::vec4 lobes; };
struct Push { glm::mat4 model{1}; glm::ivec4 ids{}; };
struct GpuObject {glm::vec4 sphere;glm::uvec4 indices;};
static_assert(sizeof(GpuMaterial)==80&&sizeof(Push)==80&&sizeof(GpuLight)==48);
static_assert(offsetof(Globals,lights)==384);

// descriptor 反射之外补 Stage IO：location5 顶点与 attachment6 的类型也必须一致。
using ShaderIo=std::map<std::pair<std::uint32_t,std::uint32_t>,std::string>;
ShaderIo shader_io(std::span<const std::uint32_t> code) {
    std::map<std::uint32_t,std::vector<std::uint32_t>> types;
    std::map<std::uint32_t,std::uint32_t> locations;
    struct Variable {std::uint32_t type,id,storage;};std::vector<Variable> variables;
    for(std::size_t p=5;p<code.size();){
        auto count=code[p]>>16,op=code[p]&65535u;if(!count||count>code.size()-p)throw std::invalid_argument("Invalid core shader instruction");
        if(op==71&&count>=4&&code[p+2]==30)locations[code[p+1]]=code[p+3];
        if(op==59&&count>=4)variables.push_back({code[p+1],code[p+2],code[p+3]});
        if(op>=19&&op<=32&&count>=2){auto& type=types[code[p+1]];type.push_back(op);type.insert(type.end(),code.begin()+p+2,code.begin()+p+count);}
        p+=count;
    }
    std::function<std::string(std::uint32_t,unsigned)> type_name=[&](std::uint32_t id,unsigned depth){
        if(depth>16||!types.contains(id))throw std::invalid_argument("Unreflected core shader IO type");const auto& t=types.at(id);
        if(t[0]==32&&t.size()==3)return type_name(t[2],depth+1);
        if((t[0]==23||t[0]==24)&&t.size()==3)return std::to_string(t[0])+":"+std::to_string(t[2])+"<"+type_name(t[1],depth+1)+">";
        if(t[0]!=20&&t[0]!=21&&t[0]!=22)throw std::invalid_argument("Unsupported core shader located IO type");
        std::string name;for(auto value:t)name+=std::to_string(value)+":";return name;
    };
    ShaderIo result;for(auto v:variables)if((v.storage==1||v.storage==3)&&locations.contains(v.id))result.emplace(std::pair{v.storage,locations.at(v.id)},type_name(v.type,0));return result;
}

struct Buffer {
    VmaAllocator allocator{}; VkBuffer handle{}; VmaAllocation allocation{}; VkDeviceSize size{}; void* mapped{};
    Buffer()=default; Buffer(const Buffer&)=delete; Buffer& operator=(const Buffer&)=delete;
    Buffer(Buffer&& b) noexcept { swap(b); }
    Buffer& operator=(Buffer&& b) noexcept { if(this!=&b){reset();swap(b);} return *this; }
    void swap(Buffer& b) noexcept { std::swap(allocator,b.allocator);std::swap(handle,b.handle);std::swap(allocation,b.allocation);std::swap(size,b.size);std::swap(mapped,b.mapped); }
    void reset() noexcept { if(handle) vmaDestroyBuffer(allocator,handle,allocation);handle={};allocation={};mapped=nullptr;size=0; }
    ~Buffer(){reset();}
};
struct GpuImage {
    VmaAllocator allocator{}; VkDevice device{}; VkImage handle{}; VkImageView view{}; VmaAllocation allocation{};
    VkFormat format{}; VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED; std::uint32_t w=0,h=0,mips=1;
    GpuImage()=default; GpuImage(const GpuImage&)=delete; GpuImage& operator=(const GpuImage&)=delete;
    GpuImage(GpuImage&& b) noexcept {swap(b);}
    GpuImage& operator=(GpuImage&& b) noexcept {if(this!=&b){reset();swap(b);}return *this;}
    void swap(GpuImage& b) noexcept {std::swap(allocator,b.allocator);std::swap(device,b.device);std::swap(handle,b.handle);std::swap(view,b.view);std::swap(allocation,b.allocation);std::swap(format,b.format);std::swap(layout,b.layout);std::swap(w,b.w);std::swap(h,b.h);std::swap(mips,b.mips);}
    void reset() noexcept {if(view)vkDestroyImageView(device,view,nullptr);if(handle)vmaDestroyImage(allocator,handle,allocation);view={};handle={};allocation={};layout=VK_IMAGE_LAYOUT_UNDEFINED;}
    ~GpuImage(){reset();}
};
template<class H,auto Destroy> struct DeviceHandle {
    VkDevice device{};H handle{};
    ~DeviceHandle(){if(handle)Destroy(device,handle,nullptr);}
};
std::vector<glm::mat4> world_matrices(const Scene& scene) {
    std::vector<glm::mat4> result(scene.nodes.size(),glm::mat4(1));
    std::vector<unsigned char> state(scene.nodes.size());
    // 迭代拓扑追溯避免深层 glTF 节点导致递归栈溢出，同时明确拒绝环。
    for(std::size_t start=0;start<scene.nodes.size();++start) {
        if(state[start]==2)continue;std::vector<std::size_t> chain;int i=int(start);
        while(i>=0) {
            if(std::size_t(i)>=scene.nodes.size())throw std::runtime_error("Scene node parent out of range");
            if(state[i]==2)break;if(state[i]==1)throw std::runtime_error("Scene node cycle");
            state[i]=1;chain.push_back(std::size_t(i));i=scene.nodes[i].parent;
        }
        while(!chain.empty()) {auto n=chain.back();chain.pop_back();auto p=scene.nodes[n].parent;
            result[n]=(p<0?glm::mat4(1):result[p])*scene.nodes[n].local;state[n]=2;
        }
    }
    return result;
}
glm::mat4 light_projection(const Scene& scene,const std::vector<glm::mat4>& worlds,int light_index) {
    glm::vec3 lo(std::numeric_limits<float>::max()),hi(-std::numeric_limits<float>::max());bool any=false;
    auto include=[&](const Mesh& mesh,const glm::mat4& w){for(const auto& v:mesh.vertices){glm::vec3 p(w*glm::vec4(v.position,1));lo=glm::min(lo,p);hi=glm::max(hi,p);any=true;}};
    if(scene.nodes.empty())for(const auto& mesh:scene.meshes)include(mesh,glm::mat4(1));
    else for(std::size_t i=0;i<scene.nodes.size();++i)if(scene.nodes[i].mesh>=0&&std::size_t(scene.nodes[i].mesh)<scene.meshes.size())include(scene.meshes[scene.nodes[i].mesh],worlds[i]);
    glm::vec3 center=any?(lo+hi)*.5f:glm::vec3(0);float radius=any?std::max(glm::length(hi-lo)*.55f,.1f):10;
    glm::vec3 direction=light_index>=0?safe_normalize(scene.lights[light_index].direction):glm::vec3(0,-1,0);
    glm::vec3 up=std::abs(direction.y)>.98f?glm::vec3(0,0,1):glm::vec3(0,1,0);
    glm::mat4 view=glm::lookAtRH(center-direction*(radius*2),center,up),p(1);
    // Vulkan Z∈[0,1]，右手相机向 -Z 看；阴影始终使用 near=0、far=4r 的普通 Z。
    p[0][0]=1/radius;p[1][1]=-1/radius;p[2][2]=-1/(radius*4);p[3][2]=0;
    return p*view;
}
int depth_slice(float z,float near_z,float far_z) {
    return std::clamp(int(std::log(std::max(z,near_z)/near_z)/std::log(far_z/near_z)*cluster_slices),0,cluster_slices-1);
}
std::uint64_t timestamp_delta(std::uint64_t a,std::uint64_t b,std::uint32_t bits) {
    return bits>=64?b-a:(b-a)&((std::uint64_t(1)<<bits)-1);
}
std::uint8_t channel(float x) {return std::uint8_t(std::lround(std::clamp(std::isfinite(x)?x:0.f,0.f,1.f)*255));}
float from_half(std::uint16_t bits) {
    auto sign=std::uint32_t(bits&0x8000u)<<16;int exponent=(bits>>10)&31;auto mantissa=std::uint32_t(bits&1023);
    if(exponent==0){if(!mantissa)return std::bit_cast<float>(sign);int e=-14;while(!(mantissa&1024)){mantissa<<=1;--e;}return std::bit_cast<float>(sign|std::uint32_t(e+127)<<23|(mantissa&1023)<<13);}
    return std::bit_cast<float>(sign|std::uint32_t(exponent==31?255:exponent+112)<<23|mantissa<<13);
}
VkRect2D letterbox(std::uint32_t source_w,std::uint32_t source_h,VkExtent2D output,std::uint32_t inset_left=0,std::uint32_t inset_right=0,std::uint32_t inset_top=0,std::uint32_t inset_bottom=0) {
    // 逐项从剩余尺寸扣除，不把四边相加，防止极小窗口或恶意尺寸发生无符号溢出。
    const auto left=std::min(inset_left,output.width),right=std::min(inset_right,output.width-left);
    const auto top=std::min(inset_top,output.height),bottom=std::min(inset_bottom,output.height-top);
    const auto available_width=output.width-left-right,available_height=output.height-top-bottom;
    if(!available_width||!available_height||!source_w||!source_h)return {{int(left),int(top)},{0,0}};
    double scale=std::min(double(available_width)/source_w,double(available_height)/source_h);
    VkExtent2D size{std::max(1u,std::uint32_t(std::lround(source_w*scale))),std::max(1u,std::uint32_t(std::lround(source_h*scale)))};
    return {{int(left+(available_width-size.width)/2),int(top+(available_height-size.height)/2)},size};
}
VkSamplerCreateInfo texture_sampler_info(const Texture& tex,FilterMode mode) {
    auto wrap=[](Texture::Wrap value){switch(value){case Texture::Wrap::repeat:return VK_SAMPLER_ADDRESS_MODE_REPEAT;case Texture::Wrap::clamp_to_edge:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;case Texture::Wrap::mirrored_repeat:return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;}throw std::invalid_argument("Invalid texture wrap enum");};
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};s.addressModeU=wrap(tex.wrap_s);s.addressModeV=wrap(tex.wrap_t);s.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;s.maxLod=VK_LOD_CLAMP_NONE;
    s.magFilter=tex.mag_filter==Texture::Filter::nearest?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
    if(tex.mag_filter!=Texture::Filter::nearest&&tex.mag_filter!=Texture::Filter::linear)throw std::invalid_argument("Invalid glTF magnification filter");
    switch(tex.min_filter){
    case Texture::Filter::nearest:s.minFilter=VK_FILTER_NEAREST;s.maxLod=0;break;
    case Texture::Filter::linear:s.minFilter=VK_FILTER_LINEAR;s.maxLod=0;break;
    case Texture::Filter::nearest_mipmap_nearest:s.minFilter=VK_FILTER_NEAREST;s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;break;
    case Texture::Filter::linear_mipmap_nearest:s.minFilter=VK_FILTER_LINEAR;s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;break;
    case Texture::Filter::nearest_mipmap_linear:s.minFilter=VK_FILTER_NEAREST;s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;break;
    case Texture::Filter::linear_mipmap_linear:s.minFilter=VK_FILTER_LINEAR;s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;break;
    default:throw std::invalid_argument("Invalid glTF minification filter");}
    // Default trilinear mode honors glTF's complete sampler. Nearest/bilinear are
    // explicit laboratory filter overrides; imported addressing is always preserved.
    if(mode==FilterMode::nearest||mode==FilterMode::bilinear){s.minFilter=s.magFilter=mode==FilterMode::nearest?VK_FILTER_NEAREST:VK_FILTER_LINEAR;s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;s.maxLod=VK_LOD_CLAMP_NONE;}
    return s;
}
void write_bmp(const std::filesystem::path& path,int w,int h,const std::uint8_t* rgba,bool bgra) {
    const std::uint32_t stride=(std::uint32_t(w)*3+3)&~3u,size=54+stride*std::uint32_t(h);
    std::array<unsigned char,54> header{};
    auto u32=[&](int offset,std::uint32_t value){for(int i=0;i<4;++i)header[offset+i]=static_cast<unsigned char>(value>>(i*8));};
    header[0]='B';header[1]='M';u32(2,size);u32(10,54);u32(14,40);u32(18,w);u32(22,h);header[26]=1;header[28]=24;u32(34,stride*h);
    std::ofstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot open screenshot output");
    file.write(reinterpret_cast<const char*>(header.data()),header.size());std::vector<unsigned char> row(stride);
    for(int y=h-1;y>=0;--y) {for(int x=0;x<w;++x){auto p=rgba+(std::size_t(y)*w+x)*4;row[x*3]=p[bgra?0:2];row[x*3+1]=p[1];row[x*3+2]=p[bgra?2:0];}file.write(reinterpret_cast<const char*>(row.data()),stride);}
    if(!file)throw std::runtime_error("Screenshot write failed");
}
} // namespace

struct VulkanWorkbench::Impl {
    SDL_Window* window{};std::filesystem::path shaders;
    std::uint32_t viewport_inset_left=0,viewport_inset_right=0,viewport_inset_top=0,viewport_inset_bottom=0;
    std::unique_ptr<renderer::VulkanContext> context;
    std::unique_ptr<renderer::VulkanDevice> device;
    std::unique_ptr<renderer::VulkanSwapchain> swapchain;
    VkDevice vk{};VmaAllocator allocator{};VkPhysicalDeviceProperties properties{};
    std::unique_ptr<GpuPipelineCache> driver_cache;
    std::unique_ptr<GpuShadowSystem> advanced_shadows;
    std::unique_ptr<GpuEffects> effects;
    std::unique_ptr<GpuLighting> lighting;
    std::unique_ptr<GpuVolumeGi> volume_gi;
    std::unique_ptr<GpuSceneResources> scene_gpu;
    EffectsInputs effect_inputs;
    EffectsOutput processed;
    glm::mat4 previous_vp{1};glm::vec2 previous_jitter{0};
    std::vector<glm::mat4> previous_worlds;
    glm::vec3 previous_camera{0};std::uint64_t previous_revision=0,previous_light_key=0;
    bool previous_frame_valid=false;
    using ObjectKey=std::tuple<std::size_t,std::size_t,std::uint32_t,std::uint32_t>;
    struct ObjectHistory {glm::mat4 world;std::uint32_t id;};
    std::map<ObjectKey,ObjectHistory> previous_objects,pending_objects;
    std::vector<EffectsObjectMotion> object_motion;
    struct SceneResources;
    std::weak_ptr<SceneResources> previous_scene_owner;
    std::uint64_t temporal_epoch=1;
    bool has_scene_output=false; // 不支持实时配置时只显示最近完成的 GPU 结果，不转 CPU。
    DebugView last_scene_debug=DebugView::final_color;float last_scene_exposure=1;
    ImGuiContext* imgui{};bool sdl_initialized=false,imgui_initialized=false,dirty=true,failed=false,bindings_dirty=true,screenshot_supported=false;
    VkDescriptorPool descriptor_pool{},imgui_pool{};
    VkDescriptorSetLayout global_layout{},material_layout{};VkPipelineLayout pipeline_layout{};
    VkSampler linear_sampler{},nearest_sampler{},bilinear_sampler{},shadow_sampler{},presentation_sampler{};
    GpuImage application_icon;VkDescriptorSet icon_descriptor{};
    float font_scale=0;
    std::uint64_t render_pixel_limit=8294400;
    VkPipeline forward[2]{},transparent[2]{},gbuffer[2]{},shadow_pipeline{},sky_pipeline{},deferred_pipeline{},post_pipeline{},cull_pipeline{},object_pipeline{};
    struct CoreAbi {SpirvReflection resources;ShaderIo io;};std::map<std::string,CoreAbi> core_abi;
    VkRenderPass ui_pass{};std::vector<VkFramebuffer> ui_framebuffers;
    VkCommandPool upload_pool{};VkQueryPool query_pool{};std::uint32_t timestamp_bits{};
    struct GpuMesh {Buffer vertices,indices;std::map<std::pair<std::uint32_t,std::uint32_t>,glm::vec4> bounds;};
    bool bounds_cache_enabled=true;
    std::size_t uploaded_this_draw=0;
    struct ActiveProfileScope {std::size_t index;ProfileClock::time_point start;};
    std::vector<ActiveProfileScope> profile_scopes;
    struct SceneScratch {Buffer objects,indirect,visibility;};
    struct SceneResources {
        VkDevice device{};Scene snapshot;const Scene* identity{};std::uint64_t revision=0,asset_revision=0;FilterMode filter{};bool ready=false;
        Buffer material_buffer;std::vector<GpuMesh> meshes;std::vector<GpuImage> textures;
        std::shared_ptr<GpuSceneResources::SdfUpload> sdf_upload;
        std::shared_ptr<const SceneBakeResources> bake;
        VkDescriptorPool material_pool{};std::vector<VkDescriptorSet> material_sets;std::vector<VkSampler> samplers,owned_samplers;
        std::vector<Material> cached_materials;std::vector<GpuMaterial> packed_materials;std::size_t object_capacity=1;
        std::array<SceneScratch,2> prepared_scratch;
        ~SceneResources(){if(material_pool)vkDestroyDescriptorPool(device,material_pool,nullptr);for(auto sampler:owned_samplers)vkDestroySampler(device,sampler,nullptr);}
    };
    struct Frame {
        VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};VkSemaphore acquired{};
        VkDescriptorSet descriptors{},post_descriptors{};Buffer uniform,objects,indirect,visibility;
        // 每个 RSM 面独占 UBO slice/set，录制六次 draw 时不能反复改写同一份 CPU uniform。
        Buffer rsm_uniform;std::array<VkDescriptorSet,6> rsm_descriptors{};
        GpuVolumeGi::RsmProjection rsm_projection;
        // 参考图按帧槽保活：只在本槽 fence 完成后写暂存区或替换 Image，另一槽仍可显示。
        GpuImage reference;Buffer reference_staging;std::uint64_t reference_revision=0;bool reference_upload_pending=false;
        std::shared_ptr<SceneResources> scene_owner;std::size_t object_count=0,opaque_count=0;
        bool submitted=false;std::uint64_t serial=0,shadow_triangles=0;
        FrameProfile profile;
    };
    std::array<Frame,2> frames;std::size_t frame_index=0;
    // acquire 信号量按 CPU frame 复用；present 信号量按 swapchain image 复用。
    // 获得同一 image 意味着上次 present 的 semaphore wait 已完成。
    std::vector<VkSemaphore> presented;
    GpuImage hdr,indirect_baseline,depth,shadow;std::array<GpuImage,7> gb;
    std::array<GpuImage,7> rsm_gb;GpuImage rsm_depth;
    GpuImage energy_lut;Image<glm::vec3> pending_reference;std::uint64_t reference_revision=1;
    Buffer light_lists;std::shared_ptr<SceneResources> active_scene,placeholder;
    struct UploadTask {bool image=false;VkBuffer buffer{};const std::byte* source{};std::size_t bytes=0,offset=0,texture=0,level=0;};
    struct PendingScene {
        std::unique_ptr<SceneUploadSnapshot> snapshot;std::shared_ptr<SceneResources> resources;
        std::vector<UploadTask> tasks;std::vector<std::byte> scratch;std::map<std::array<int,4>,VkSampler> sampler_cache;
        int phase=0;std::size_t index=0,task=0,total=0,submitted=0,completed=0;std::uint64_t first_ticket=0,last_ticket=0;
        std::vector<unsigned char> node_state;std::vector<std::size_t> node_chain;
        std::size_t node_start=0,scratch_frame=0;int node_walk=-1;bool topology_started=false;
    };
    std::unique_ptr<PendingScene> pending_scene;std::unique_ptr<GpuUploadRing> upload_ring;
    const Scene* requested_identity{};std::uint64_t requested_revision=~std::uint64_t(0);FilterMode requested_filter{};
    std::shared_ptr<const SceneBakeResources> requested_bake;
    // 后台任务只持有已完成的独立快照；取消后的 future 轮询回收，析构不阻塞普通帧。
    struct BakeResult {std::shared_ptr<const Scene> source;Scene prepared;};
    struct BakeJob {std::shared_ptr<std::atomic<bool>> cancel;std::future<BakeResult> future;};
    std::optional<BakeJob> bake_job;std::vector<BakeJob> retired_bakes;
    std::unique_ptr<SceneUploadSnapshot> bake_snapshot;
    const Scene* bake_identity{};std::uint64_t bake_revision=0;
    std::vector<Node> bake_nodes;glm::vec3 bake_top{},bake_bottom{};
    Settings bake_settings;
    std::shared_ptr<const SceneBakeResources> bake_input,completed_bake;
    std::shared_ptr<const Scene> bake_source;
    std::shared_ptr<Scene> prepared_scene;
    std::uint64_t bake_generation=0,prepared_generation=~std::uint64_t(0);
    int width=0,height=0,shadow_size=0;std::filesystem::path pending_screenshot;
    std::optional<DebugView> pending_readback;std::optional<FrameReadback> completed_readback;std::uint64_t serial=0;
    WorkbenchStats counters;
    GraphPlan last_graph;

    ~Impl(){cleanup();}
    void cleanup() noexcept;
    void initialize(SDL_Window*,const std::filesystem::path&);
    Buffer buffer(VkDeviceSize,VkBufferUsageFlags,bool host=false);
    GpuImage image(std::uint32_t,std::uint32_t,VkFormat,VkImageUsageFlags,std::uint32_t mips=1);
    void immediate(const std::function<void(VkCommandBuffer)>&);
    void transition(VkCommandBuffer,GpuImage&,VkImageLayout);
    void barrier_image(VkCommandBuffer,VkImage,VkImageLayout,VkImageLayout,VkImageAspectFlags,std::uint32_t mips=1);
    void descriptors();void update_descriptors(Frame&,const SceneResources&);void create_pipelines();void destroy_pipelines() noexcept;
    VkPipeline pipeline(const char*,const char*,const std::vector<VkFormat>&,bool depth_test=false,bool reverse=false,bool blend=false);
    bool recreate_swapchain();void destroy_present_resources() noexcept;void create_present_resources();
    void update_font_scale();void upload_application_icon();
    void targets(const Settings&);void upload_scene(const Scene&,FilterMode);
    void prepare_reference(Frame&);void record_reference_upload(VkCommandBuffer,Frame&);void upload_energy_lut();
    void prepare_scene_upload(PendingScene&);void transfer_scene_upload(PendingScene&);
    const Scene& prepare_baked_scene(const Scene&,const Settings&);
    bool draw(const Scene&,const Camera&,const Settings&,const std::function<void()>&);
    void render_scene(VkCommandBuffer,const Scene&,const Camera&,const Settings&,const std::vector<glm::mat4>&,Buffer* readback,
                      const std::function<void(RenderGraph&)>& append_final,VkImage present_image);
    void render_begin(VkCommandBuffer,const std::vector<GpuImage*>&,GpuImage*,bool clear,bool reversed=false);
    void viewport(VkCommandBuffer,std::uint32_t,std::uint32_t);
    void fullscreen(VkCommandBuffer,VkPipeline);
    void profile_event(VkCommandBuffer,std::string_view,bool);
};

Buffer VulkanWorkbench::Impl::buffer(VkDeviceSize size,VkBufferUsageFlags usage,bool host) {
    Buffer b;b.allocator=allocator;b.size=std::max<VkDeviceSize>(size,4);
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=b.size;info.usage=usage;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo alloc{};alloc.usage=host?VMA_MEMORY_USAGE_AUTO:VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if(host)alloc.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
    else alloc.requiredFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VmaAllocationInfo result{};check(vmaCreateBuffer(allocator,&info,&alloc,&b.handle,&b.allocation,&result),"create buffer");b.mapped=result.pMappedData;return b;
}
GpuImage VulkanWorkbench::Impl::image(std::uint32_t w,std::uint32_t h,VkFormat format,VkImageUsageFlags usage,std::uint32_t mips) {
    GpuImage img;img.allocator=allocator;img.device=vk;img.w=w;img.h=h;img.format=format;img.mips=mips;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={w,h,1};ci.mipLevels=mips;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;
    VmaAllocationCreateInfo ai{};ai.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;ai.requiredFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    check(vmaCreateImage(allocator,&ci,&ai,&img.handle,&img.allocation,nullptr),"create image");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=img.handle;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={VkImageAspectFlags(format==depth_format?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,mips,0,1};
    check(vkCreateImageView(vk,&vi,nullptr,&img.view),"create image view");return img;
}
void VulkanWorkbench::Impl::barrier_image(VkCommandBuffer cmd,VkImage img,VkImageLayout old,VkImageLayout next,VkImageAspectFlags aspect,std::uint32_t mips) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask=old==VK_IMAGE_LAYOUT_UNDEFINED?VK_PIPELINE_STAGE_2_NONE:VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask=old==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;b.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout=old;b.newLayout=next;b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=img;b.subresourceRange={aspect,0,mips,0,1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};d.imageMemoryBarrierCount=1;d.pImageMemoryBarriers=&b;vkCmdPipelineBarrier2(cmd,&d);
}
void VulkanWorkbench::Impl::transition(VkCommandBuffer cmd,GpuImage& img,VkImageLayout next) {
    barrier_image(cmd,img.handle,img.layout,next,img.format==depth_format?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,img.mips);img.layout=next;
}
void VulkanWorkbench::Impl::immediate(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBuffer cmd{};VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=upload_pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
    check(vkAllocateCommandBuffers(vk,&ai,&cmd),"upload command");
    DeviceHandle<VkFence,vkDestroyFence> fence{vk};
    bool submitted=false;
    try {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(vk,&fi,nullptr,&fence.handle),"upload fence");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(cmd,&bi),"begin upload");record(cmd);check(vkEndCommandBuffer(cmd),"end upload");
        VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};si.commandBufferInfoCount=1;si.pCommandBufferInfos=&cb;
        check(vkQueueSubmit2(device->graphics_queue(),1,&si,fence.handle),"submit upload");submitted=true;
        check(vkWaitForFences(vk,1,&fence.handle,VK_TRUE,UINT64_MAX),"wait upload");
    } catch(...) {if(submitted)vkDeviceWaitIdle(vk);vkFreeCommandBuffers(vk,upload_pool,1,&cmd);throw;}
    vkFreeCommandBuffers(vk,upload_pool,1,&cmd);
}

void VulkanWorkbench::Impl::initialize(SDL_Window* win,const std::filesystem::path& directory) {
    if(!win)throw std::invalid_argument("VulkanWorkbench requires SDL_Window");window=win;shaders=directory;
    context=std::make_unique<renderer::VulkanContext>(win);
    device=std::make_unique<renderer::VulkanDevice>(context->instance(),context->surface());vk=device->device();
    vkGetPhysicalDeviceProperties(device->physical_device(),&properties);counters.gpu_name=properties.deviceName;
    if(properties.limits.maxColorAttachments<7)throw std::runtime_error("SH/PRT workbench requires seven color attachments");
    std::uint32_t queue_count=0;vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device(),&queue_count,nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device(),&queue_count,queues.data());
    auto& queue=queues.at(device->graphics_queue_family());if(!(queue.queueFlags&VK_QUEUE_COMPUTE_BIT))throw std::runtime_error("Workbench needs graphics+compute queue");
    timestamp_bits=queue.timestampValidBits;
    counters.timestamp_available=timestamp_bits!=0;
    for(auto format:{hdr_format,depth_format}) {
        VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(device->physical_device(),format,&fp);
        VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|(format==depth_format?VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        if((fp.optimalTilingFeatures&needed)!=needed)throw std::runtime_error("Workbench attachment format unsupported");
    }
    VmaAllocatorCreateInfo ac{};ac.instance=context->instance();ac.physicalDevice=device->physical_device();ac.device=vk;ac.vulkanApiVersion=VK_API_VERSION_1_3;
    check(vmaCreateAllocator(&ac,&allocator),"create VMA allocator");
    // 全套实验缓冲约需 320 字节/像素。最多使用最大本地堆的一半，保留模型、阴影及其它应用空间。
    VkPhysicalDeviceMemoryProperties heaps{};vkGetPhysicalDeviceMemoryProperties(device->physical_device(),&heaps);
    VkDeviceSize local_heap=0;
    for(std::uint32_t i=0;i<heaps.memoryHeapCount;++i)if(heaps.memoryHeaps[i].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)local_heap=std::max(local_heap,heaps.memoryHeaps[i].size);
    if(local_heap)render_pixel_limit=std::clamp<std::uint64_t>(local_heap/2/320,64,8294400);
    {
        std::unique_ptr<char,decltype(&SDL_free)> base(SDL_GetBasePath(),SDL_free);
        auto directory=shaders.parent_path();
        // SDL 返回 UTF-8；C++20 char8_t 路径构造保持中文路径语义，不经过系统代码页。
        if(base)directory=std::filesystem::path(std::u8string(base.get(),base.get()+std::strlen(base.get())));
        // 缓存是机器/驱动相关的用户数据，不写回只读演示包或源仓库。
        std::unique_ptr<char,decltype(&SDL_free)> pref(SDL_GetPrefPath("16Ui","EmberFrame"),SDL_free);
        if(pref)directory=std::filesystem::path(std::u8string(pref.get(),pref.get()+std::strlen(pref.get())));
        // 顶点 location5/set4/第七目标改变本 workbench ABI，独立 cache key 拒绝旧接口。
        auto abi="scene-abi-v3-area512-v"+std::to_string(sizeof(Vertex))+"-prt"+std::to_string(offsetof(Vertex,baked_irradiance));
        driver_cache=std::make_unique<GpuPipelineCache>(vk,properties,GpuPipelineCache::default_path(directory/abi,properties));
    }
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=device->graphics_queue_family();pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check(vkCreateCommandPool(vk,&pc,nullptr,&upload_pool),"create upload pool");
    upload_ring=std::make_unique<GpuUploadRing>(vk,allocator,device->graphics_queue(),device->graphics_queue_family());
    placeholder=std::make_shared<SceneResources>();placeholder->device=vk;placeholder->material_buffer=buffer(sizeof(GpuMaterial),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true);
    std::memset(placeholder->material_buffer.mapped,0,sizeof(GpuMaterial));check(vmaFlushAllocation(allocator,placeholder->material_buffer.allocation,0,VK_WHOLE_SIZE),"flush placeholder");
    for(auto& f:frames) {
        check(vkCreateCommandPool(vk,&pc,nullptr,&f.pool),"create frame pool");VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=f.pool;ca.commandBufferCount=1;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;check(vkAllocateCommandBuffers(vk,&ca,&f.command),"create frame command");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;check(vkCreateFence(vk,&fc,nullptr,&f.fence),"create frame fence");
        VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};check(vkCreateSemaphore(vk,&sc,nullptr,&f.acquired),"create acquire semaphore");f.uniform=buffer(sizeof(Globals),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,true);
        const auto alignment=properties.limits.minUniformBufferOffsetAlignment;
        const VkDeviceSize stride=(sizeof(Globals)+alignment-1)/alignment*alignment;
        f.rsm_uniform=buffer(stride*6,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,true);
        f.objects=buffer(sizeof(GpuObject),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true);f.indirect=buffer(2*sizeof(VkDrawIndexedIndirectCommand),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);f.visibility=buffer(f.indirect.size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
    }
    if(timestamp_bits) {VkQueryPoolCreateInfo qc{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qc.queryType=VK_QUERY_TYPE_TIMESTAMP;qc.queryCount=std::uint32_t(frames.size())*profile_queries_per_slot;check(vkCreateQueryPool(vk,&qc,nullptr,&query_pool),"create timestamp queries");}
    auto sampler=[&](VkFilter filter,VkSamplerMipmapMode mip,VkSamplerAddressMode address,float max_lod,VkSampler& out){
        VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};s.minFilter=s.magFilter=filter;s.mipmapMode=mip;s.addressModeU=s.addressModeV=s.addressModeW=address;s.maxLod=max_lod;s.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        // VulkanDevice 没有启用 samplerAnisotropy。保持 anisotropyEnable=false。
        check(vkCreateSampler(vk,&s,nullptr,&out),"create sampler");};
    sampler(VK_FILTER_LINEAR,VK_SAMPLER_MIPMAP_MODE_LINEAR,VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_LOD_CLAMP_NONE,linear_sampler);
    sampler(VK_FILTER_NEAREST,VK_SAMPLER_MIPMAP_MODE_NEAREST,VK_SAMPLER_ADDRESS_MODE_REPEAT,0,nearest_sampler);
    sampler(VK_FILTER_LINEAR,VK_SAMPLER_MIPMAP_MODE_NEAREST,VK_SAMPLER_ADDRESS_MODE_REPEAT,0,bilinear_sampler);
    sampler(VK_FILTER_NEAREST,VK_SAMPLER_MIPMAP_MODE_NEAREST,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,0,shadow_sampler);
    // 显式低比例渲染时做线性呈现；原生 1:1 下采样落在纹素中心，不会额外模糊。
    sampler(VK_FILTER_LINEAR,VK_SAMPLER_MIPMAP_MODE_NEAREST,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,0,presentation_sampler);
    upload_application_icon();
    upload_energy_lut();
    advanced_shadows=std::make_unique<GpuShadowSystem>(device->physical_device(),vk,allocator,device->graphics_queue(),device->graphics_queue_family(),shaders,driver_cache->handle());
    effects=std::make_unique<GpuEffects>(device->physical_device(),vk,allocator,shaders,driver_cache->handle());
    lighting=std::make_unique<GpuLighting>(device->physical_device(),vk,allocator,device->graphics_queue(),device->graphics_queue_family(),shaders,driver_cache->handle());
    volume_gi=std::make_unique<GpuVolumeGi>(device->physical_device(),vk,allocator,shaders,driver_cache->handle());
    scene_gpu=std::make_unique<GpuSceneResources>(device->physical_device(),vk,allocator);
    descriptors();create_pipelines();
    imgui=ImGui::CreateContext();ImGui::SetCurrentContext(imgui);ImGui::StyleColorsDark();ImGui::GetIO().IniFilename=nullptr;
    update_font_scale();
    if(!ImGui_ImplSDL2_InitForVulkan(window))throw std::runtime_error("ImGui SDL initialization failed");sdl_initialized=true;
    recreate_swapchain();
}
void VulkanWorkbench::Impl::update_font_scale() {
    int logical_w=0,logical_h=0,pixel_w=0,pixel_h=0;
    SDL_GetWindowSize(window,&logical_w,&logical_h);SDL_Vulkan_GetDrawableSize(window,&pixel_w,&pixel_h);
    if(logical_w<=0||logical_h<=0||pixel_w<=0||pixel_h<=0)return;
    const float scale=std::clamp(std::round(std::max(float(pixel_w)/logical_w,float(pixel_h)/logical_h)*100.f)/100.f,1.f,4.f);
    counters.display_scale=scale;
    if(std::abs(scale-font_scale)<.01f)return;
    // atlas 的 GPU 纹理仍可能被上一帧借用；先等完成，再销毁/重建，且必须在 NewFrame 前执行。
    if(imgui_initialized){check(vkDeviceWaitIdle(vk),"wait DPI font rebuild");ImGui_ImplVulkan_DestroyFontsTexture();}
    auto& io=ImGui::GetIO();io.Fonts->Clear();io.FontDefault=nullptr;
    io.Fonts->TexDesiredWidth=std::min(scale>2?8192:4096,int(properties.limits.maxImageDimension2D));
    ImFontConfig config;config.OversampleH=1;config.OversampleV=1;config.PixelSnapH=true;
    // 175% 时栅格化 28px，再以 16 个逻辑点布局；不是先生成 16px 再拉伸。
    auto font=shaders.parent_path()/"assets/fonts/DroidSansFallback.ttf";
    if(!std::filesystem::exists(font))font=std::filesystem::path("C:/Windows/Fonts/msyh.ttc");
    if(std::filesystem::exists(font)){const auto utf8=font.u8string();io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(utf8.c_str()),16*scale,&config,io.Fonts->GetGlyphRangesChineseFull());}
    if(io.Fonts->Fonts.empty()){config.SizePixels=16*scale;io.Fonts->AddFontDefault(&config);}
    io.FontGlobalScale=1/scale;font_scale=scale;counters.font_raster_scale=scale;
    if(imgui_initialized&&!ImGui_ImplVulkan_CreateFontsTexture())throw std::runtime_error("DPI font texture failed");
}
void VulkanWorkbench::Impl::upload_application_icon() {
    const auto path=shaders.parent_path()/"assets/branding/emberframe-icon.png";
    int w=0,h=0,channels=0;
    std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> pixels(stbi_load(path.string().c_str(),&w,&h,&channels,4),stbi_image_free);
    if(!pixels||w<=0||h<=0)return; // 单独使用 GPU 测试程序时，品牌资源可选。
    auto* surface=SDL_CreateRGBSurfaceWithFormatFrom(pixels.get(),w,h,32,w*4,SDL_PIXELFORMAT_RGBA32);
    if(surface){SDL_SetWindowIcon(window,surface);SDL_FreeSurface(surface);}
    auto staging=buffer(VkDeviceSize(w)*h*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,true);
    std::memcpy(staging.mapped,pixels.get(),std::size_t(w)*h*4);check(vmaFlushAllocation(allocator,staging.allocation,0,VK_WHOLE_SIZE),"flush application icon");
    application_icon=image(w,h,VK_FORMAT_R8G8B8A8_SRGB,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
    immediate([&](VkCommandBuffer cmd){transition(cmd,application_icon,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={std::uint32_t(w),std::uint32_t(h),1};vkCmdCopyBufferToImage(cmd,staging.handle,application_icon.handle,application_icon.layout,1,&copy);transition(cmd,application_icon,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);});
}
void VulkanWorkbench::Impl::descriptors() {
    std::array<VkDescriptorSetLayoutBinding,17> bindings{};
    for(std::uint32_t i=0;i<bindings.size();++i){bindings[i].binding=i;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_ALL_GRAPHICS|VK_SHADER_STAGE_COMPUTE_BIT;bindings[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:(i==2||i==3||i==11||i==12)?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;}
    VkDescriptorSetLayoutCreateInfo lc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};lc.bindingCount=std::uint32_t(bindings.size());lc.pBindings=bindings.data();check(vkCreateDescriptorSetLayout(vk,&lc,nullptr,&global_layout),"global layout");
    std::array<VkDescriptorSetLayoutBinding,5> mb{};for(std::uint32_t i=0;i<mb.size();++i)mb[i]={i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    lc.bindingCount=std::uint32_t(mb.size());lc.pBindings=mb.data();check(vkCreateDescriptorSetLayout(vk,&lc,nullptr,&material_layout),"material layout");
    std::array<VkDescriptorSetLayout,5> layouts{global_layout,material_layout,advanced_shadows->descriptor_layout(),lighting->descriptor_layout(),scene_gpu->descriptor_layout()};VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=5;pl.pSetLayouts=layouts.data();pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;check(vkCreatePipelineLayout(vk,&pl,nullptr,&pipeline_layout),"pipeline layout");
    std::array<VkDescriptorPoolSize,3> sizes{{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,16},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,64},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,192}}};
    VkDescriptorPoolCreateInfo dc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dc.maxSets=16;dc.poolSizeCount=std::uint32_t(sizes.size());dc.pPoolSizes=sizes.data();check(vkCreateDescriptorPool(vk,&dc,nullptr,&descriptor_pool),"global descriptor pool");
    for(auto& f:frames)for(auto* set:{&f.descriptors,&f.post_descriptors}){VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=descriptor_pool;ai.descriptorSetCount=1;ai.pSetLayouts=&global_layout;check(vkAllocateDescriptorSets(vk,&ai,set),"global descriptors");}
    for(auto& f:frames)for(auto& set:f.rsm_descriptors){VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=descriptor_pool;ai.descriptorSetCount=1;ai.pSetLayouts=&global_layout;check(vkAllocateDescriptorSets(vk,&ai,&set),"RSM face descriptors");}
    VkDescriptorPoolSize imgui_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,64};dc.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;dc.maxSets=64;dc.poolSizeCount=1;dc.pPoolSizes=&imgui_size;check(vkCreateDescriptorPool(vk,&dc,nullptr,&imgui_pool),"ImGui descriptor pool");
}
void VulkanWorkbench::Impl::update_descriptors(Frame& f,const SceneResources& resources) {
        std::array<VkDescriptorBufferInfo,5> buffers{{{f.uniform.handle,0,sizeof(Globals)},{resources.material_buffer.handle,0,resources.material_buffer.size},{light_lists.handle,0,light_lists.size},{f.objects.handle,0,f.objects.size},{f.indirect.handle,0,f.indirect.size}}};
        std::array<VkDescriptorImageInfo,12> images{{{shadow_sampler,shadow.view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {shadow_sampler,gb[0].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{shadow_sampler,gb[1].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {shadow_sampler,gb[2].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{shadow_sampler,gb[3].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {presentation_sampler,hdr.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{presentation_sampler,f.reference.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {shadow_sampler,energy_lut.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {shadow_sampler,effects->occlusion().view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {shadow_sampler,gb[4].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{shadow_sampler,gb[5].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{shadow_sampler,gb[6].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
        std::array<VkWriteDescriptorSet,17> writes{};std::size_t b=0,i=0;
        for(std::uint32_t k=0;k<writes.size();++k){auto& wr=writes[k];wr.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr.dstSet=f.descriptors;wr.dstBinding=k;wr.descriptorCount=1;
            if(k==0||k==2||k==3||k==11||k==12){wr.descriptorType=k==0?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;wr.pBufferInfo=&buffers[b++];}
            else{wr.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr.pImageInfo=&images[i++];}}
        vkUpdateDescriptorSets(vk,std::uint32_t(writes.size()),writes.data(),0,nullptr);
        for(auto& write:writes)write.dstSet=f.post_descriptors;
        vkUpdateDescriptorSets(vk,std::uint32_t(writes.size()),writes.data(),0,nullptr);
        const auto alignment=properties.limits.minUniformBufferOffsetAlignment;
        const VkDeviceSize stride=(sizeof(Globals)+alignment-1)/alignment*alignment;
        for(std::size_t face=0;face<f.rsm_descriptors.size();++face) {
            buffers[0]={f.rsm_uniform.handle,stride*face,sizeof(Globals)};
            for(auto& write:writes)write.dstSet=f.rsm_descriptors[face];
            vkUpdateDescriptorSets(vk,std::uint32_t(writes.size()),writes.data(),0,nullptr);
        }
    bindings_dirty=false;
}
VkPipeline VulkanWorkbench::Impl::pipeline(const char* vertex,const char* fragment,const std::vector<VkFormat>& colors,bool depth_test,bool reverse,bool blend) {
    auto load=[&](const char* name,VkShaderModule& module){std::ifstream file(shaders/(std::string(name)+".spv"),std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error(std::string("Missing shader: ")+name);
        auto bytes=file.tellg();if(bytes<=0||std::uint64_t(bytes)%4)throw std::runtime_error("Invalid SPIR-V length");std::vector<std::uint32_t> code(std::size_t(bytes)/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),std::streamsize(bytes));if(!file||code[0]!=0x07230203)throw std::runtime_error("Invalid SPIR-V file");
        std::string key=name;auto reflected=reflect_spirv(code,key.ends_with(".vert")?ShaderStage::vertex:ShaderStage::fragment);auto io=shader_io(code);
        if(key=="mesh.vert"&&(!io.contains({1,5})||!io.contains({3,5})))throw std::runtime_error("Mesh shader lacks PRT location5 ABI");
        if(key=="gbuffer.frag"&&!io.contains({3,6}))throw std::runtime_error("G-buffer shader lacks seventh PRT target ABI");
        if(key=="forward.frag"||key=="deferred.frag"){
            auto find=[&](std::uint32_t set,std::uint32_t binding){return std::any_of(reflected.descriptors.begin(),reflected.descriptors.end(),[&](const auto& d){return d.set==set&&d.binding==binding;});};
            if(!find(4,0)||!find(4,1)||(key=="deferred.frag"&&!find(0,16)))throw std::runtime_error("Lighting shader lacks SH/PRT/SDF descriptor ABI");
        }
        if(auto baseline=core_abi.find(key);baseline!=core_abi.end()){
            auto compatible=compare_shader_layouts(baseline->second.resources,reflected);
            if(!compatible.compatible||baseline->second.io!=io)throw std::runtime_error("Core shader descriptor/push/Stage IO ABI changed: "+key);
        }else core_abi.emplace(key,CoreAbi{std::move(reflected),std::move(io)});
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=std::size_t(bytes);ci.pCode=code.data();check(vkCreateShaderModule(vk,&ci,nullptr,&module),"shader module");};
    DeviceHandle<VkShaderModule,vkDestroyShaderModule> vs{vk},fs{vk};load(vertex,vs.handle);load(fragment,fs.handle);
    std::array<VkPipelineShaderStageCreateInfo,2> stages{};for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs.handle;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs.handle;
    VkVertexInputBindingDescription binding{0,sizeof(Vertex),VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription,6> attrs{{{0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,position)},{1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,normal)},
        {2,0,VK_FORMAT_R32G32_SFLOAT,offsetof(Vertex,uv)},{3,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(Vertex,tangent)},{4,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(Vertex,color)},{5,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,baked_irradiance)}}};
    std::array<VkVertexInputAttributeDescription,3> shadow_attrs{attrs[0],attrs[2],attrs[4]};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};if(std::string(vertex)=="mesh.vert"||std::string(vertex)=="gpu_shadow.vert"){vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&binding;vi.vertexAttributeDescriptionCount=std::string(vertex)=="gpu_shadow.vert"?3:std::uint32_t(attrs.size());vi.pVertexAttributeDescriptions=std::string(vertex)=="gpu_shadow.vert"?shadow_attrs.data():attrs.data();}
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthTestEnable=depth_test;ds.depthWriteEnable=depth_test&&!blend;ds.depthCompareOp=reverse?VK_COMPARE_OP_GREATER_OR_EQUAL:VK_COMPARE_OP_LESS_OR_EQUAL;
    std::vector<VkPipelineColorBlendAttachmentState> attachments(colors.size());for(auto& a:attachments){a.colorWriteMask=0xf;a.blendEnable=blend;a.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;a.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;a.colorBlendOp=VK_BLEND_OP_ADD;a.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;a.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;a.alphaBlendOp=VK_BLEND_OP_ADD;}
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=std::uint32_t(attachments.size());cb.pAttachments=attachments.data();
    std::array<VkDynamicState,2> dynamics{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dyn.dynamicStateCount=2;dyn.pDynamicStates=dynamics.data();
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=std::uint32_t(colors.size());rendering.pColorAttachmentFormats=colors.data();rendering.depthAttachmentFormat=depth_test?depth_format:VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.pNext=&rendering;ci.stageCount=2;ci.pStages=stages.data();ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vp;ci.pRasterizationState=&raster;ci.pMultisampleState=&ms;ci.pDepthStencilState=&ds;ci.pColorBlendState=&cb;ci.pDynamicState=&dyn;ci.layout=pipeline_layout;
    VkPipeline result{};auto r=vkCreateGraphicsPipelines(vk,driver_cache->handle(),1,&ci,nullptr,&result);if(r!=VK_SUCCESS){if(result)vkDestroyPipeline(vk,result,nullptr);check(r,"graphics pipeline");}return result;
}
void VulkanWorkbench::Impl::create_pipelines() {
    for(int r=0;r<2;++r){forward[r]=pipeline("mesh.vert","forward.frag",{hdr_format,hdr_format},true,r!=0);transparent[r]=pipeline("mesh.vert","forward.frag",{hdr_format,hdr_format},true,r!=0,true);gbuffer[r]=pipeline("mesh.vert","gbuffer.frag",{VK_FORMAT_R32G32B32A32_SFLOAT,hdr_format,hdr_format,hdr_format,hdr_format,VK_FORMAT_R32G32B32A32_SFLOAT,hdr_format},true,r!=0);}
    shadow_pipeline=pipeline("gpu_shadow.vert","shadow.frag",{},true);sky_pipeline=pipeline("fullscreen.vert","sky.frag",{hdr_format,hdr_format});deferred_pipeline=pipeline("fullscreen.vert","deferred.frag",{hdr_format,hdr_format});
    for(const auto& shader:std::array<std::pair<const char*,VkPipeline*>,2>{{{"cull.comp.spv",&cull_pipeline},{"object_cull.comp.spv",&object_pipeline}}}) {
    std::ifstream file(shaders/shader.first,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error(std::string("Missing ")+shader.first);auto bytes=file.tellg();if(bytes<=0||std::uint64_t(bytes)%4)throw std::runtime_error("Invalid compute SPIR-V");std::vector<std::uint32_t> code(std::size_t(bytes)/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),std::streamsize(bytes));if(!file||code[0]!=0x07230203)throw std::runtime_error("Invalid compute SPIR-V");
    DeviceHandle<VkShaderModule,vkDestroyShaderModule> module{vk};VkShaderModuleCreateInfo sc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sc.codeSize=std::size_t(bytes);sc.pCode=code.data();check(vkCreateShaderModule(vk,&sc,nullptr,&module.handle),"compute module");
    VkComputePipelineCreateInfo cc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cc.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;cc.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cc.stage.module=module.handle;cc.stage.pName="main";cc.layout=pipeline_layout;check(vkCreateComputePipelines(vk,driver_cache->handle(),1,&cc,nullptr,shader.second),"culling pipeline");
    }
}
void VulkanWorkbench::Impl::destroy_pipelines() noexcept {
    for(auto* range:{&forward,&transparent,&gbuffer})for(auto& p:*range){if(p)vkDestroyPipeline(vk,p,nullptr);p={};}
    for(auto* p:{&shadow_pipeline,&sky_pipeline,&deferred_pipeline,&post_pipeline,&cull_pipeline,&object_pipeline}){if(*p)vkDestroyPipeline(vk,*p,nullptr);*p={};}
}
void VulkanWorkbench::Impl::destroy_present_resources() noexcept {
    if(icon_descriptor&&imgui_initialized){ImGui_ImplVulkan_RemoveTexture(icon_descriptor);icon_descriptor={};}
    if(imgui_initialized){ImGui::SetCurrentContext(imgui);ImGui_ImplVulkan_Shutdown();imgui_initialized=false;}
    for(auto fb:ui_framebuffers)if(fb)vkDestroyFramebuffer(vk,fb,nullptr);ui_framebuffers.clear();
    for(auto sem:presented)if(sem)vkDestroySemaphore(vk,sem,nullptr);presented.clear();
    if(ui_pass)vkDestroyRenderPass(vk,ui_pass,nullptr);ui_pass={};if(post_pipeline)vkDestroyPipeline(vk,post_pipeline,nullptr);post_pipeline={};
}
void VulkanWorkbench::Impl::create_present_resources() {
    auto extent=swapchain->extent();auto format=swapchain->image_format();
    VkAttachmentDescription a{};a.format=format;a.samples=VK_SAMPLE_COUNT_1_BIT;a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;a.initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;a.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
    VkSubpassDependency dep{};dep.srcSubpass=VK_SUBPASS_EXTERNAL;dep.dstSubpass=0;dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dep.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rc{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rc.attachmentCount=1;rc.pAttachments=&a;rc.subpassCount=1;rc.pSubpasses=&sub;rc.dependencyCount=1;rc.pDependencies=&dep;check(vkCreateRenderPass(vk,&rc,nullptr,&ui_pass),"UI render pass");
    ui_framebuffers.resize(swapchain->images().size());presented.resize(swapchain->images().size());
    for(std::size_t i=0;i<ui_framebuffers.size();++i){VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fc.renderPass=ui_pass;fc.attachmentCount=1;fc.pAttachments=&swapchain->image_views()[i];fc.width=extent.width;fc.height=extent.height;fc.layers=1;check(vkCreateFramebuffer(vk,&fc,nullptr,&ui_framebuffers[i]),"UI framebuffer");VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};check(vkCreateSemaphore(vk,&sc,nullptr,&presented[i]),"present semaphore");}
    post_pipeline=pipeline("fullscreen.vert","post.frag",{format});
    ImGui::SetCurrentContext(imgui);ImGui_ImplVulkan_InitInfo ii{};ii.Instance=context->instance();ii.PhysicalDevice=device->physical_device();ii.Device=vk;ii.QueueFamily=device->graphics_queue_family();ii.Queue=device->graphics_queue();ii.DescriptorPool=imgui_pool;ii.RenderPass=ui_pass;ii.MinImageCount=2;ii.ImageCount=std::uint32_t(swapchain->images().size());ii.MSAASamples=VK_SAMPLE_COUNT_1_BIT;ii.UseDynamicRendering=false;ii.CheckVkResultFn=[](VkResult r){check(r,"ImGui Vulkan");};
    ii.PipelineCache=driver_cache->handle();
    // 旧 ImGui 后端仅查找 KHR 动态入口；传统 UI render pass 避免依赖未启用的扩展。
    try{if(!ImGui_ImplVulkan_Init(&ii))throw std::runtime_error("ImGui Vulkan initialization failed");imgui_initialized=true;}
    catch(...){imgui_initialized=ImGui::GetIO().BackendRendererUserData!=nullptr;throw;}
    if(!ImGui_ImplVulkan_CreateFontsTexture())throw std::runtime_error("ImGui font texture failed");
    if(application_icon.view)icon_descriptor=ImGui_ImplVulkan_AddTexture(presentation_sampler,application_icon.view,application_icon.layout);
    counters.present_width=int(extent.width);counters.present_height=int(extent.height);
}
bool VulkanWorkbench::Impl::recreate_swapchain() {
    int w=0,h=0;SDL_Vulkan_GetDrawableSize(window,&w,&h);if(w<=0||h<=0||(SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED))return false;
    check(vkDeviceWaitIdle(vk),"wait resize");destroy_present_resources();
    if(!swapchain){VkSurfaceCapabilitiesKHR caps{};check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical_device(),context->surface(),&caps),"surface capabilities");
        screenshot_supported=(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)!=0;
        auto usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|(screenshot_supported?VK_IMAGE_USAGE_TRANSFER_SRC_BIT:0);
        swapchain=std::make_unique<renderer::VulkanSwapchain>(window,device->physical_device(),vk,context->surface(),device->graphics_queue_family(),device->present_queue_family(),usage);
    }else if(!swapchain->recreate())return false;
    create_present_resources();dirty=false;return true;
}
void VulkanWorkbench::Impl::targets(const Settings& settings) {
    int w=settings.render_width,h=settings.render_height,ss=std::clamp(settings.shadow_resolution,32,2048);
    if(w<1||h<1||w>4096||h>4096)throw std::invalid_argument("GPU render dimensions must be in 1..4096");
    if(std::uint64_t(w)*h>render_pixel_limit)throw std::invalid_argument("Render resolution exceeds device VRAM safety budget; reduce width/height or use --fit-viewport");
    if(width==w&&height==h&&shadow_size==ss)return;
    auto list_bytes=VkDeviceSize((w+15)/16)*((h+15)/16)*cluster_slices*65*sizeof(std::uint32_t);
    if(list_bytes>properties.limits.maxStorageBufferRange)throw std::invalid_argument("Render resolution exceeds clustered light-list storage limit");
    check(vkDeviceWaitIdle(vk),"wait render target resize");
    auto color_usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    hdr=image(w,h,hdr_format,color_usage);indirect_baseline=image(w,h,hdr_format,color_usage);depth=image(w,h,depth_format,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    for(std::size_t i=0;i<gb.size();++i)gb[i]=image(w,h,i==0||i==5?VK_FORMAT_R32G32B32A32_SFLOAT:hdr_format,color_usage);
    shadow=image(ss,ss,depth_format,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
    for(std::size_t i=0;i<rsm_gb.size();++i)rsm_gb[i]=image(rsm_face_resolution*6,rsm_face_resolution,i==0||i==5?VK_FORMAT_R32G32B32A32_SFLOAT:hdr_format,color_usage);
    rsm_depth=image(rsm_face_resolution*6,rsm_face_resolution,depth_format,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    advanced_shadows->resize(ss);effects->resize(w,h);previous_frame_valid=false;has_scene_output=false;processed={};
    light_lists=buffer(list_bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    width=w;height=h;shadow_size=ss;bindings_dirty=true;
    counters.render_width=w;counters.render_height=h;
    // 即使本帧前向/CPU 路径不读取 G-buffer，先赋予合法描述符布局。
    immediate([&](VkCommandBuffer c){for(auto& attachment:gb)transition(c,attachment,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);for(auto& attachment:rsm_gb)transition(c,attachment,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);transition(c,hdr,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);transition(c,indirect_baseline,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);transition(c,shadow,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);});
}
void VulkanWorkbench::Impl::upload_scene(const Scene& scene,FilterMode filter) {
    try {
        if(scene.asset_revision&&active_scene&&active_scene->ready&&active_scene->identity==&scene&&
           active_scene->asset_revision==scene.asset_revision&&active_scene->filter==filter&&active_scene->bake==scene.baked_resources){
            if(active_scene->revision==scene.revision&&!pending_scene)return;
            if(active_scene->revision!=scene.revision){
                // CPU 节点矩阵/灯光最终写入当前帧 Push Constants / Uniform，Vertex/Texture 不变。
                // 保留完整的最后可用场景快照，以便后续导入尚未完成时仍显示刚才的姿态。
                auto nodes=scene.nodes;auto lights=scene.lights;auto name=scene.name;
                active_scene->snapshot.nodes.swap(nodes);active_scene->snapshot.lights.swap(lights);active_scene->snapshot.name.swap(name);
                active_scene->snapshot.sky_top=scene.sky_top;active_scene->snapshot.sky_bottom=scene.sky_bottom;
                active_scene->snapshot.environment_map=scene.environment_map;
                active_scene->snapshot.environment_intensity=scene.environment_intensity;
                active_scene->snapshot.environment_rotation=scene.environment_rotation;
                active_scene->snapshot.revision=active_scene->revision=scene.revision;
            }
            pending_scene.reset();requested_identity=&scene;requested_revision=scene.revision;requested_filter=filter;requested_bake=scene.baked_resources;
            counters.displayedRevision=scene.revision;counters.uploadPending=false;counters.uploadStage="ready (pose)";
            counters.uploaded=counters.uploadSubmitted=counters.snapshotCopied=counters.total=counters.snapshotTotal=0;
            counters.uploadError.clear();return;
        }
        const bool changed=requested_identity!=&scene||requested_revision!=scene.revision||requested_filter!=filter||requested_bake!=scene.baked_resources;
        if(changed) {
            // Cancel only host work. Already-submitted slices keep this generation's
            // entire resource bundle alive until their own fences signal.
            pending_scene.reset();requested_identity=&scene;requested_revision=scene.revision;requested_filter=filter;requested_bake=scene.baked_resources;
            counters.uploadPending=true;counters.uploaded=counters.uploadSubmitted=counters.snapshotCopied=0;
            counters.uploadError.clear();counters.uploadStage="snapshot";
            auto pending=std::make_unique<PendingScene>();pending->snapshot=std::make_unique<SceneUploadSnapshot>(scene);
            pending->resources=std::make_shared<SceneResources>();auto& resources=*pending->resources;
            resources.device=vk;resources.identity=&scene;resources.revision=scene.revision;resources.asset_revision=scene.asset_revision;resources.filter=filter;
            resources.bake=scene.baked_resources;
            auto add=[&](std::size_t n,std::size_t stride){if(n>(SIZE_MAX-pending->total)/stride)throw std::length_error("GPU upload size overflow");pending->total+=n*stride;};
            add(scene.materials.size()+1,sizeof(GpuMaterial));add(2,4);
            for(const auto& mesh:scene.meshes){add(mesh.vertices.size(),sizeof(Vertex));add(mesh.indices.size(),sizeof(std::uint32_t));}
            for(const auto& texture:scene.textures)for(const auto& mip:texture.levels)add(mip.pixels.size(),4);
            // SDF 与顶点/纹理使用同一个上传环、预算、fence 与原子发布边界。
            if(resources.bake&&resources.bake->sdf){resources.sdf_upload=scene_gpu->prepare_sdf(resources.bake->sdf);auto data=resources.sdf_upload->bytes();add(data.size(),1);pending->tasks.push_back({false,resources.sdf_upload->buffer(),data.data(),data.size()});}
            counters.total=pending->total;counters.snapshotTotal=pending->snapshot->total_bytes();pending_scene=std::move(pending);
        }
        for(auto completion:upload_ring->poll())if(pending_scene&&pending_scene->first_ticket&&completion.ticket>=pending_scene->first_ticket&&completion.ticket<=pending_scene->last_ticket)pending_scene->completed+=completion.bytes;
        if(!pending_scene)return;
        auto& pending=*pending_scene;
        if(pending.snapshot) {
            bool complete=pending.snapshot->advance(scene,counters.uploadBudget);
            counters.snapshotCopied=pending.snapshot->copied_bytes();
            if(!complete)return;
            pending.resources->snapshot=pending.snapshot->take();pending.resources->snapshot.baked_resources=pending.resources->bake;pending.snapshot.reset();counters.uploadStage="allocate";
        }
        if(pending.phase<6)prepare_scene_upload(pending);
        if(pending.phase==6){counters.uploadStage="upload";transfer_scene_upload(pending);}
        counters.uploaded=pending.completed;counters.uploadSubmitted=pending.submitted;
        if(pending.phase==6&&pending.task==pending.tasks.size()) {
            counters.uploadStage="fence";
            if(upload_ring->completed(pending.last_ticket)) {
                if(pending.completed!=pending.total||pending.submitted!=pending.total)throw std::logic_error("Upload byte accounting mismatch");
                pending.resources->ready=true;active_scene=pending.resources;
                counters.displayedRevision=active_scene->revision;counters.uploadPending=false;counters.uploadStage="ready";
                pending_scene.reset();
            }
        }
    } catch(const std::exception& error) {
        upload_ring->abort();pending_scene.reset();counters.uploadPending=false;counters.uploadStage="failed";counters.uploadError=error.what();
        // The previous committed scene and all its descriptors remain usable.
    }
}
void VulkanWorkbench::Impl::prepare_scene_upload(PendingScene& p) {
    auto& r=*p.resources;const auto& scene=r.snapshot;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(2);
    unsigned records=0;
    while(p.phase<6&&records++<64&&std::chrono::steady_clock::now()<deadline) {
        if(p.phase==0) {
            const auto count=scene.materials.size()+1;
            if(count>properties.limits.maxStorageBufferRange/sizeof(GpuMaterial)||count>UINT32_MAX/5)throw std::length_error("GPU material table too large");
            if(!p.index){r.cached_materials.reserve(count);r.packed_materials.reserve(count);}
            if(p.index<count){
                Material mat=p.index<scene.materials.size()?scene.materials[p.index]:Material{};
                r.packed_materials.push_back({mat.base_color,glm::vec4(mat.emissive,mat.normal_scale),{mat.metallic,mat.roughness,mat.ao_strength,mat.alpha_cutoff},{mat.alpha_mode,mat.double_sided?1:0,mat.normal_texture>=0&&std::size_t(mat.normal_texture)<scene.textures.size()?1:0,int(r.filter)},{mat.clearcoat,mat.clearcoat_roughness,mat.anisotropy,mat.sheen}});
                r.cached_materials.push_back(std::move(mat));++p.index;
            }else{p.phase=1;p.index=0;}
        }else if(p.phase==1) {
            auto size=r.packed_materials.size()*sizeof(GpuMaterial);
            r.material_buffer=buffer(size,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            p.tasks.push_back({false,r.material_buffer.handle,reinterpret_cast<const std::byte*>(r.packed_materials.data()),size});
            p.phase=2;
        }else if(p.phase==2) {
            if(p.index==scene.meshes.size()){p.phase=3;p.index=0;continue;}
            const auto& mesh=scene.meshes[p.index];if(mesh.vertices.size()>UINT32_MAX||mesh.indices.size()>UINT32_MAX)throw std::length_error("Mesh too large");
            GpuMesh gpu;auto vertex_bytes=mesh.vertices.size()*sizeof(Vertex),index_bytes=mesh.indices.size()*sizeof(std::uint32_t);
            // 静态几何发布时计算一次；逐帧仅将局部球变换到世界空间。
            auto cache_bound=[&](Primitive primitive){auto key=std::pair{primitive.first_index,primitive.index_count};
                if(!gpu.bounds.contains(key))gpu.bounds.emplace(key,primitive_sphere(mesh,primitive));};
            if(mesh.primitives.empty())cache_bound({0,std::uint32_t(mesh.indices.size()),0});
            else for(const auto& primitive:mesh.primitives)cache_bound(primitive);
            gpu.vertices=buffer(vertex_bytes,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            gpu.indices=buffer(index_bytes,VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            if(vertex_bytes)p.tasks.push_back({false,gpu.vertices.handle,reinterpret_cast<const std::byte*>(mesh.vertices.data()),vertex_bytes});
            if(index_bytes)p.tasks.push_back({false,gpu.indices.handle,reinterpret_cast<const std::byte*>(mesh.indices.data()),index_bytes});
            r.meshes.push_back(std::move(gpu));++p.index;
        }else if(p.phase==3) {
            if(p.index==scene.textures.size()+2){p.phase=4;p.index=0;continue;}
            bool fallback=p.index>=scene.textures.size();const Texture* tex=fallback?nullptr:&scene.textures[p.index];
            auto w=tex?tex->levels[0].width:1,h=tex?tex->levels[0].height:1;
            if(w>int(properties.limits.maxImageDimension2D)||h>int(properties.limits.maxImageDimension2D))throw std::length_error("Texture exceeds GPU dimensions");
            r.textures.push_back(image(w,h,tex&&tex->srgb?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,tex?std::uint32_t(tex->levels.size()):1));
            for(std::size_t level=0;level<(tex?tex->levels.size():1);++level){UploadTask task;task.image=true;task.texture=p.index;task.level=level;task.bytes=tex?tex->levels[level].pixels.size()*4:4;p.tasks.push_back(task);}
            ++p.index;
        }else if(p.phase==4) {
            if(!r.material_pool){
                VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,std::uint32_t(r.cached_materials.size()*5)};
                VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.maxSets=std::uint32_t(r.cached_materials.size());pc.poolSizeCount=1;pc.pPoolSizes=&ps;
                check(vkCreateDescriptorPool(vk,&pc,nullptr,&r.material_pool),"pending material pool");r.samplers.resize(scene.textures.size());
                r.owned_samplers.reserve(108);r.material_sets.reserve(r.cached_materials.size());
            }
            if(p.index==r.cached_materials.size()){p.phase=5;p.index=0;r.object_capacity=0;continue;}
            VkDescriptorSet set{};VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=r.material_pool;da.descriptorSetCount=1;da.pSetLayouts=&material_layout;check(vkAllocateDescriptorSets(vk,&da,&set),"pending material descriptor");
            r.material_sets.push_back(set);const auto& mat=r.cached_materials[p.index];
            std::array<int,5> indexes{mat.base_texture,mat.mr_texture,mat.normal_texture,mat.ao_texture,mat.emissive_texture};
            std::array<VkDescriptorImageInfo,5> images{};std::array<VkWriteDescriptorSet,5> writes{};
            for(std::uint32_t k=0;k<5;++k){
                int index=indexes[k];auto actual=index>=0&&std::size_t(index)<scene.textures.size()?std::size_t(index):scene.textures.size()+(k==2?1:0);
                VkSampler sampler=linear_sampler;
                if(actual<scene.textures.size()){
                    if(!r.samplers[actual]){
                        const auto& tex=scene.textures[actual];std::array<int,4> key{int(tex.wrap_s),int(tex.wrap_t),int(tex.min_filter),int(tex.mag_filter)};
                        auto it=p.sampler_cache.find(key);if(it==p.sampler_cache.end()){
                            auto info=texture_sampler_info(tex,r.filter);DeviceHandle<VkSampler,vkDestroySampler> owner{vk};
                            check(vkCreateSampler(vk,&info,nullptr,&owner.handle),"pending glTF sampler");r.owned_samplers.push_back(owner.handle);auto handle=std::exchange(owner.handle,VK_NULL_HANDLE);
                            it=p.sampler_cache.emplace(key,handle).first;
                        }r.samplers[actual]=it->second;
                    }sampler=r.samplers[actual];
                }
                images[k]={sampler,r.textures[actual].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};auto& write=writes[k];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=set;write.dstBinding=k;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&images[k];
            }
            vkUpdateDescriptorSets(vk,5,writes.data(),0,nullptr);++p.index;
        }else{
            auto count=scene.nodes.empty()?scene.meshes.size():scene.nodes.size();
            if(p.index<count){int mesh=scene.nodes.empty()?int(p.index):scene.nodes[p.index].mesh;
                if(mesh>=0){if(std::size_t(mesh)>=scene.meshes.size())throw std::invalid_argument("Node mesh out of range");r.object_capacity+=std::max<std::size_t>(1,scene.meshes[mesh].primitives.size());}++p.index;
            }else{
                // 每次只走一个父节点或退栈一个节点，深层 glTF 层级验证也受准备预算约束。
                if(!p.topology_started){p.node_state.resize(scene.nodes.size());p.topology_started=true;}
                if(p.node_walk>=0){
                    auto node=std::size_t(p.node_walk);if(node>=scene.nodes.size())throw std::invalid_argument("Scene node parent out of range");
                    if(p.node_state[node]==1)throw std::invalid_argument("Scene node cycle");
                    if(p.node_state[node]==2)p.node_walk=-1;else{p.node_state[node]=1;p.node_chain.push_back(node);p.node_walk=scene.nodes[node].parent;}
                    continue;
                }
                if(!p.node_chain.empty()){p.node_state[p.node_chain.back()]=2;p.node_chain.pop_back();continue;}
                if(p.node_start<scene.nodes.size()){if(p.node_state[p.node_start]!=2)p.node_walk=int(p.node_start);++p.node_start;continue;}
                r.object_capacity=std::max<std::size_t>(1,r.object_capacity);
                if(r.object_capacity>properties.limits.maxStorageBufferRange/(2*sizeof(VkDrawIndexedIndirectCommand)))throw std::length_error("Too many indirect objects");
                // Grow both future frame scratch sets before publication. Allocation failure
                // now rejects only this replacement; it cannot strand the committed scene.
                if(p.scratch_frame<frames.size()){
                    auto i=p.scratch_frame++;if(r.object_capacity*sizeof(GpuObject)>frames[i].objects.size){
                        auto& scratch=r.prepared_scratch[i];scratch.objects=buffer(r.object_capacity*sizeof(GpuObject),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true);
                        scratch.indirect=buffer(r.object_capacity*2*sizeof(VkDrawIndexedIndirectCommand),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
                        scratch.visibility=buffer(scratch.indirect.size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
                    }continue;
                }
                p.phase=6;p.scratch.reserve(65536);
            }
        }
    }
}
void VulkanWorkbench::Impl::transfer_scene_upload(PendingScene& p) {
    auto& r=*p.resources;std::size_t budget=counters.uploadBudget;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(2);
    while(p.task<p.tasks.size()&&budget>=4&&std::chrono::steady_clock::now()<deadline) {
        if(!upload_ring->begin(p.resources))break;
        while(p.task<p.tasks.size()&&budget>=4&&upload_ring->remaining()>=4&&std::chrono::steady_clock::now()<deadline){
            auto& task=p.tasks[p.task];auto amount=std::min({task.bytes-task.offset,budget,upload_ring->remaining(),std::size_t(65536)})&~std::size_t(3);
            if(!amount)break;
            if(!task.image)upload_ring->copy_buffer(task.buffer,task.offset,{task.source+task.offset,amount});
            else {
                auto& img=r.textures[task.texture];bool fallback=task.texture>=r.snapshot.textures.size();
                const auto* mip=fallback?nullptr:&r.snapshot.textures[task.texture].levels[task.level];
                std::size_t w=mip?std::size_t(mip->width):1,h=mip?std::size_t(mip->height):1,pixel=task.offset/4,x=pixel%w,y=pixel/w;
                std::size_t columns,rows;if(x||amount/4<w){columns=std::min(w-x,amount/4);rows=1;}else{columns=w;rows=std::min(h-y,amount/(w*4));}
                amount=columns*rows*4;p.scratch.resize(amount);
                for(std::size_t i=0;i<amount/4;++i){auto color=mip?mip->pixels[pixel+i]:(task.texture==r.snapshot.textures.size()?glm::vec4(1):glm::vec4(.5f,.5f,1,1));for(int k=0;k<4;++k)p.scratch[i*4+k]=std::byte(channel(color[k]));}
                if(img.layout==VK_IMAGE_LAYOUT_UNDEFINED)transition(upload_ring->command(),img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,std::uint32_t(task.level),0,1};copy.imageOffset={int(x),int(y),0};copy.imageExtent={std::uint32_t(columns),std::uint32_t(rows),1};
                upload_ring->copy_rgba8(img.handle,copy,p.scratch);
                if(task.offset+amount==task.bytes&&task.level+1==img.mips)transition(upload_ring->command(),img,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            task.offset+=amount;budget-=amount;if(task.offset==task.bytes)++p.task;
        }
        auto bytes=upload_ring->recorded_bytes();if(!bytes){upload_ring->abort();break;}
        auto ticket=upload_ring->submit();if(!p.first_ticket)p.first_ticket=ticket;p.last_ticket=ticket;p.submitted+=bytes;uploaded_this_draw+=bytes;
    }
}
void VulkanWorkbench::Impl::prepare_reference(Frame& frame) {
    if(frame.reference_revision==reference_revision)return;
    int w=pending_reference.empty()?1:pending_reference.width,h=pending_reference.empty()?1:pending_reference.height;
    if(w>int(properties.limits.maxImageDimension2D)||h>int(properties.limits.maxImageDimension2D))throw std::invalid_argument("Reference image exceeds GPU limit");
    const auto pixels=std::size_t(w)*h,bytes=pixels*sizeof(glm::vec4);
    // draw 已等待本帧槽 fence；同尺寸重复更新复用 Image/Buffer，不等待整台 GPU。
    if(!frame.reference.handle||frame.reference.w!=std::uint32_t(w)||frame.reference.h!=std::uint32_t(h))
        frame.reference=image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
    if(!frame.reference_staging.handle||frame.reference_staging.size<bytes)
        frame.reference_staging=buffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,true);
    auto* rgba=static_cast<glm::vec4*>(frame.reference_staging.mapped);
    for(std::size_t i=0;i<pixels;++i){
        auto p=pending_reference.empty()?glm::vec3(0):pending_reference.pixels[i];
        for(int k=0;k<3;++k)if(!std::isfinite(p[k]))p[k]=0;
        rgba[i]=glm::vec4(p,1);
    }
    check(vmaFlushAllocation(allocator,frame.reference_staging.allocation,0,bytes),"flush reference");
    frame.reference_upload_pending=true;
}
void VulkanWorkbench::Impl::record_reference_upload(VkCommandBuffer cmd,Frame& frame) {
    if(!frame.reference_upload_pending)return;
    // 复制和采样在同一正常帧中提交；transfer→shader-read 屏障建立实际可见性。
    transition(cmd,frame.reference,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageExtent={frame.reference.w,frame.reference.h,1};
    vkCmdCopyBufferToImage(cmd,frame.reference_staging.handle,frame.reference.handle,frame.reference.layout,1,&copy);
    transition(cmd,frame.reference,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    frame.reference_revision=reference_revision;frame.reference_upload_pending=false;
}
void VulkanWorkbench::Impl::upload_energy_lut() {
    const auto& lut=default_energy_lut();int w=lut.directional.width,h=lut.directional.height;
    if(w<2||h<2||lut.average.size()!=std::size_t(h))throw std::runtime_error("Invalid shared Kulla-Conty LUT");
    std::vector<glm::vec4> pixels(std::size_t(w)*h);
    // R=directional albedo E(mu,roughness), G=hemisphere-averaged Eavg(roughness).
    // GPU uses endpoint-grid interpolation identical to the CPU, not UNORM quantization.
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)pixels[std::size_t(y)*w+x]={lut.directional.at(x,y),lut.average[y],0,1};
    auto staging=buffer(pixels.size()*sizeof(glm::vec4),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,true);std::memcpy(staging.mapped,pixels.data(),pixels.size()*sizeof(glm::vec4));check(vmaFlushAllocation(allocator,staging.allocation,0,VK_WHOLE_SIZE),"flush energy LUT");
    energy_lut=image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
    immediate([&](VkCommandBuffer cmd){transition(cmd,energy_lut,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={std::uint32_t(w),std::uint32_t(h),1};vkCmdCopyBufferToImage(cmd,staging.handle,energy_lut.handle,energy_lut.layout,1,&copy);transition(cmd,energy_lut,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);});
}
void VulkanWorkbench::Impl::viewport(VkCommandBuffer cmd,std::uint32_t w,std::uint32_t h) {
    VkViewport v{0,0,float(w),float(h),0,1};VkRect2D rect{{0,0},{w,h}};vkCmdSetViewport(cmd,0,1,&v);vkCmdSetScissor(cmd,0,1,&rect);
}
void VulkanWorkbench::Impl::render_begin(VkCommandBuffer cmd,const std::vector<GpuImage*>& colors,GpuImage* z,bool clear,bool reversed) {
    std::vector<VkRenderingAttachmentInfo> attachments;attachments.reserve(colors.size());
    for(auto* img:colors){VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};a.imageView=img->view;a.imageLayout=img->layout;a.loadOp=clear?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_LOAD;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachments.push_back(a);}
    VkRenderingAttachmentInfo depth_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if(z){depth_attachment.imageView=z->view;depth_attachment.imageLayout=z->layout;depth_attachment.loadOp=clear?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_LOAD;depth_attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;depth_attachment.clearValue.depthStencil.depth=reversed?0.f:1.f;}
    auto* extent_image=colors.empty()?z:colors[0];VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea.extent={extent_image->w,extent_image->h};ri.layerCount=1;ri.colorAttachmentCount=std::uint32_t(attachments.size());ri.pColorAttachments=attachments.data();ri.pDepthAttachment=z?&depth_attachment:nullptr;vkCmdBeginRendering(cmd,&ri);viewport(cmd,extent_image->w,extent_image->h);
}
void VulkanWorkbench::Impl::fullscreen(VkCommandBuffer cmd,VkPipeline p) {vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,p);vkCmdDraw(cmd,3,1,0,0);++counters.draw_calls;}
void VulkanWorkbench::Impl::profile_event(VkCommandBuffer cmd,std::string_view name,bool begin) {
    auto& profile=frames[frame_index].profile;
    if(begin){
        if(profile.passes.size()>=profile_pass_limit)throw std::length_error("Frame profiler Pass budget exceeded");
        const auto index=profile.passes.size();
        profile.passes.push_back({std::string(name),std::uint32_t(profile_scopes.size()),0,-1});
        profile_scopes.push_back({index,ProfileClock::now()});
        if(query_pool)vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,query_pool,
            std::uint32_t(frame_index)*profile_queries_per_slot+2+std::uint32_t(index)*2);
    }else{
        if(profile_scopes.empty()||profile.passes[profile_scopes.back().index].name!=name)
            throw std::logic_error("Unbalanced frame profiling scope");
        const auto scope=profile_scopes.back();profile_scopes.pop_back();
        profile.passes[scope.index].cpu_record_ms=elapsed_ms(scope.start);
        if(query_pool)vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,query_pool,
            std::uint32_t(frame_index)*profile_queries_per_slot+3+std::uint32_t(scope.index)*2);
    }
}
void VulkanWorkbench::Impl::render_scene(VkCommandBuffer cmd,const Scene& scene,const Camera& camera,const Settings& settings,const std::vector<glm::mat4>& worlds,Buffer* readback,
                                       const std::function<void(RenderGraph&)>& append_final,VkImage present_image) {
    const auto geometry_start=ProfileClock::now();
    const auto& resources=*frames[frame_index].scene_owner;const auto& meshes=resources.meshes;const auto& material_sets=resources.material_sets;const auto& cached_materials=resources.cached_materials;
    struct Item{std::size_t mesh,material;Primitive primitive;glm::mat4 model;float depth;glm::vec4 sphere;std::size_t indirect_index=0;std::size_t node=0;};std::vector<Item> opaque,blended;
    auto add=[&](std::size_t index,const glm::mat4& model,std::size_t node){
        if(index>=scene.meshes.size())throw std::invalid_argument("Node mesh out of range");
        // 判断轴是否退化要相对于轴长，不能用绝对行列式阈值：毫米级模型的
        // 合法均匀缩放也会得到很小的行列式，误丢弃会造成“导入成功但只有背景”。
        const glm::dmat3 axes(model);
        const double axis_measure=glm::length(axes[0])*glm::length(axes[1])*glm::length(axes[2]);
        const double determinant=glm::determinant(axes);
        if(!std::isfinite(axis_measure)||axis_measure<=0||!std::isfinite(determinant)||std::abs(determinant)<=axis_measure*1e-12)return;
        const auto& mesh=scene.meshes[index];auto push=[&](Primitive p){if(p.index_count==0)return;std::size_t m=p.material<scene.materials.size()?p.material:cached_materials.size()-1;
            const auto local_sphere=bounds_cache_enabled?meshes[index].bounds.at({p.first_index,p.index_count}):primitive_sphere(mesh,p);
            const glm::vec3 center(local_sphere);const float radius=local_sphere.w;
            // Frobenius 范数是最大奇异值的上界，负缩放/剪切也不会把包围球缩小而漏剔。
            float scale=std::sqrt(glm::dot(glm::vec3(model[0]),glm::vec3(model[0]))+glm::dot(glm::vec3(model[1]),glm::vec3(model[1]))+glm::dot(glm::vec3(model[2]),glm::vec3(model[2])));
            glm::vec3 world_center(model*glm::vec4(center,1));float distance=-(camera.view()*glm::vec4(world_center,1)).z;Item item{index,m,p,model,distance,{world_center,radius*scale}};item.node=node;(cached_materials[m].alpha_mode==2?blended:opaque).push_back(item);};
        if(mesh.primitives.empty())push({0,std::uint32_t(mesh.indices.size()),0});else for(auto p:mesh.primitives)push(p);
    };
    if(scene.nodes.empty())for(std::size_t i=0;i<scene.meshes.size();++i)add(i,glm::mat4(1),i);
    else for(std::size_t i=0;i<scene.nodes.size();++i)if(scene.nodes[i].mesh>=0)add(std::size_t(scene.nodes[i].mesh),worlds[i],i);
    std::stable_sort(blended.begin(),blended.end(),[](const Item& a,const Item& b){return a.depth>b.depth;});
    auto& frame=frames[frame_index];frame.object_count=opaque.size()+blended.size();frame.opaque_count=opaque.size();
    frame.shadow_triangles=0;if(advanced_shadows->light_index()>=0)for(const auto& item:opaque)frame.shadow_triangles+=std::uint64_t(item.primitive.index_count/3)*advanced_shadows->cascade_count();
    if(frame.object_count*sizeof(GpuObject)>frame.objects.size)throw std::runtime_error("Scene instance topology changed without incrementing revision");
    auto* objects=static_cast<GpuObject*>(frame.objects.mapped);std::size_t object_index=0;
    object_motion.clear();object_motion.reserve(frame.object_count);pending_objects.clear();
    for(auto* items:{&opaque,&blended})for(auto& item:*items){
        item.indirect_index=object_index;objects[object_index++]={item.sphere,{item.primitive.index_count,item.primitive.first_index,0,0}};
        const ObjectKey key{item.node,item.mesh,item.primitive.first_index,item.primitive.index_count};
        const auto previous=previous_objects.find(key);
        const bool known=previous!=previous_objects.end()&&!effect_inputs.invalidate_history;
        object_motion.push_back(effects_make_object_motion(item.model,known?previous->second.world:item.model,known?previous->second.id:0,known));
        pending_objects.emplace(key,ObjectHistory{item.model,std::uint32_t(item.indirect_index)});
    }
    effect_inputs.object_motion=object_motion;
    check(vmaFlushAllocation(allocator,frame.objects.allocation,0,VK_WHOLE_SIZE),"flush object bounds");
    frame.profile.cpu_geometry_ms=elapsed_ms(geometry_start);
    auto graphics_bind=[&]{vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&frame.descriptors,0,nullptr);std::array<VkDescriptorSet,3> sets{advanced_shadows->descriptor_set(std::uint32_t(frame_index)),lighting->descriptor_set(std::uint32_t(frame_index)),scene_gpu->descriptor_set(std::uint32_t(frame_index))};vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,2,3,sets.data(),0,nullptr);};
    auto draw_items=[&](const std::vector<Item>& items,VkPipeline pipe,bool shadows,int cascade=-1,bool uncull=false,VkDescriptorSet globalOverride=VK_NULL_HANDLE){graphics_bind();if(globalOverride)vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&globalOverride,0,nullptr);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);for(const auto& item:items){
        auto& mesh=meshes[item.mesh];VkDeviceSize offset=0;vkCmdBindVertexBuffers(cmd,0,1,&mesh.vertices.handle,&offset);vkCmdBindIndexBuffer(cmd,mesh.indices.handle,0,VK_INDEX_TYPE_UINT32);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,1,1,&material_sets[item.material],0,nullptr);
        Push push{item.model,{int(item.material),shadows?1:0,glm::determinant(glm::mat3(item.model))<0?1:0,cascade>=0?cascade:int(item.indirect_index)}};vkCmdPushConstants(cmd,pipeline_layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        if(uncull)vkCmdDrawIndexed(cmd,item.primitive.index_count,1,item.primitive.first_index,0,0);
        else {auto command_index=item.indirect_index+(shadows?frame.object_count:0);vkCmdDrawIndexedIndirect(cmd,frame.indirect.handle,command_index*sizeof(VkDrawIndexedIndirectCommand),1,sizeof(VkDrawIndexedIndirectCommand));}++counters.draw_calls;
    }};
    using A=ResourceAccess;using S=ResourceState;RenderGraph graph;
    std::unordered_map<std::string,GpuImage*> images{{"shadow",&shadow},{"depth",&depth},{"hdr",&hdr},{"baseline",&indirect_baseline},{"position",&gb[0]},{"normal",&gb[1]},{"albedo",&gb[2]},{"emission",&gb[3]},{"tangent",&gb[4]},{"meta",&gb[5]},{"prt",&gb[6]}};
    const bool volume=settings.gi==GiMode::rsm||settings.gi==GiMode::lpv||settings.gi==GiMode::voxel;
    if(volume){for(std::size_t i=0;i<rsm_gb.size();++i)images.emplace("rsm"+std::to_string(i),&rsm_gb[i]);images.emplace("rsm-depth",&rsm_depth);}
    auto state=[](VkImageLayout l){if(l==VK_IMAGE_LAYOUT_UNDEFINED)return S::undefined;if(l==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)return S::color_attachment;if(l==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)return S::depth_attachment;return S::shader_read;};
    for(const auto& [name,img]:images)graph.add_resource(name,img->layout!=VK_IMAGE_LAYOUT_UNDEFINED,state(img->layout));
    graph.add_resource("lists",true,S::shader_read);graph.add_resource("indirect",true,S::transfer_src);graph.add_resource("visibility",true,S::host_read);
    // 上传已完成才发布；同队列 transfer->fragment barrier 由真实图执行。
    graph.add_resource("scene-distance-grid",true,S::transfer_dst);
    // 模块内部图像的精确转换由模块记录；外层依赖图记录真实生产者/消费者边。
    graph.add_resource("shadow-filtered",true,S::shader_read);graph.add_resource("screen-ao",true,S::shader_read);
    if(volume)graph.add_resource("volume-indirect");
    if(readback)graph.add_resource("readback");
    auto compute_bind=[&](VkPipeline p){vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&frame.descriptors,0,nullptr);};
    graph.add_pass("object-cull",{{"indirect",A::write,S::storage}},[&]{if(frame.object_count){compute_bind(object_pipeline);Push control;control.ids.x=int(frame.object_count);vkCmdPushConstants(cmd,pipeline_layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(control),&control);vkCmdDispatch(cmd,std::uint32_t((frame.object_count+63)/64),2,1);}});
    graph.add_pass("directional-shadow-depth",{{"shadow-filtered",A::write,S::depth_attachment}},[&]{
        advanced_shadows->prepare(cmd);
        for(std::uint32_t c=0;c<advanced_shadows->cascade_count();++c){auto target=advanced_shadows->depth_target(c);
            VkRenderingAttachmentInfo z{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};z.imageView=target.view;z.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;z.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;z.storeOp=VK_ATTACHMENT_STORE_OP_STORE;z.clearValue.depthStencil.depth=1;
            VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};info.renderArea.extent={target.width,target.height};info.layerCount=1;info.pDepthAttachment=&z;vkCmdBeginRendering(cmd,&info);viewport(cmd,target.width,target.height);
            if(advanced_shadows->light_index()>=0)draw_items(opaque,shadow_pipeline,true,int(c),true);vkCmdEndRendering(cmd);
        }
    });
    graph.add_pass("shadow-moments-SAT-and-sampling",{{"shadow-filtered",A::read_write,S::shader_read}},[&]{advanced_shadows->finish(cmd,std::uint32_t(frame_index));});
    if(settings.culling!=LightCulling::all)graph.add_pass("light-cull",{{"lists",A::write,S::storage}},[&]{compute_bind(cull_pipeline);vkCmdDispatch(cmd,(width+15)/16,(height+15)/16,settings.culling==LightCulling::clustered?cluster_slices:1);});
    auto lit_uses=[&](std::vector<ResourceUse> uses){uses.push_back({"shadow-filtered",A::read,S::shader_read});uses.push_back({"screen-ao",A::read,S::shader_read});uses.push_back({"scene-distance-grid",A::read,S::shader_read});if(settings.culling!=LightCulling::all)uses.push_back({"lists",A::read,S::shader_read});return uses;};
    int r=settings.reversed_z?1:0;
    graph.add_pass("gbuffer-seven-targets",{{"indirect",A::read,S::shader_read},{"position",A::write,S::color_attachment},{"normal",A::write,S::color_attachment},{"albedo",A::write,S::color_attachment},{"emission",A::write,S::color_attachment},{"tangent",A::write,S::color_attachment},{"meta",A::write,S::color_attachment},{"prt",A::write,S::color_attachment},{"depth",A::write,S::depth_attachment}},[&]{render_begin(cmd,{&gb[0],&gb[1],&gb[2],&gb[3],&gb[4],&gb[5],&gb[6]},&depth,true,settings.reversed_z);draw_items(opaque,gbuffer[r],false);vkCmdEndRendering(cmd);});
    auto effect_data=[&]{auto data=effect_inputs;auto input=[](const GpuImage& im){return EffectsImageInput{im.handle,im.view,im.layout};};data.position=input(gb[0]);data.normal_roughness=input(gb[1]);data.albedo_metallic=input(gb[2]);data.emission_ao=input(gb[3]);data.optional_tangent=input(gb[4]);data.optional_meta=input(gb[5]);data.hdr=input(hdr);data.indirect_baseline=input(indirect_baseline);return data;};
    graph.add_pass("screen-AO-compute",{{"position",A::read,S::shader_read},{"normal",A::read,S::shader_read},{"albedo",A::read,S::shader_read},{"emission",A::read,S::shader_read},{"tangent",A::read,S::shader_read},{"meta",A::read,S::shader_read},{"screen-ao",A::write,S::storage}},[&]{effects->record_occlusion(cmd,effect_data());});
    if(settings.path==RenderPath::deferred) {
        graph.add_pass("deferred-lighting",lit_uses({{"position",A::read,S::shader_read},{"normal",A::read,S::shader_read},{"albedo",A::read,S::shader_read},{"emission",A::read,S::shader_read},{"tangent",A::read,S::shader_read},{"meta",A::read,S::shader_read},{"prt",A::read,S::shader_read},{"hdr",A::write,S::color_attachment},{"baseline",A::write,S::color_attachment}}),[&]{graphics_bind();render_begin(cmd,{&hdr,&indirect_baseline},nullptr,true);fullscreen(cmd,deferred_pipeline);vkCmdEndRendering(cmd);});
    }else {
        graph.add_pass("sky",{{"hdr",A::write,S::color_attachment},{"baseline",A::write,S::color_attachment}},[&]{graphics_bind();render_begin(cmd,{&hdr,&indirect_baseline},nullptr,true);fullscreen(cmd,sky_pipeline);vkCmdEndRendering(cmd);});
        graph.add_pass("forward",lit_uses({{"indirect",A::read,S::shader_read},{"hdr",A::read_write,S::color_attachment},{"baseline",A::read_write,S::color_attachment},{"depth",A::read_write,S::depth_attachment}}),[&]{render_begin(cmd,{&hdr,&indirect_baseline},&depth,false,settings.reversed_z);draw_items(opaque,forward[r],false);vkCmdEndRendering(cmd);});
    }
    if(!blended.empty())graph.add_pass("transparent",lit_uses({{"indirect",A::read,S::shader_read},{"hdr",A::read_write,S::color_attachment},{"baseline",A::read_write,S::color_attachment},{"depth",A::read,S::depth_attachment}}),[&]{render_begin(cmd,{&hdr,&indirect_baseline},&depth,false,settings.reversed_z);draw_items(blended,transparent[r],false);vkCmdEndRendering(cmd);});
    if(volume){
        std::vector<ResourceUse> writes;for(int i=0;i<int(rsm_gb.size());++i)writes.push_back({"rsm"+std::to_string(i),A::write,S::color_attachment});writes.push_back({"rsm-depth",A::write,S::depth_attachment});
        graph.add_pass("RSM-light-view-geometry",writes,[&]{
            render_begin(cmd,{&rsm_gb[0],&rsm_gb[1],&rsm_gb[2],&rsm_gb[3],&rsm_gb[4],&rsm_gb[5],&rsm_gb[6]},&rsm_depth,true);
            // 单次 clear 整个 atlas；每面有自己的 viewport/scissor 和持久 UBO slice。
            // 没有发光灯时保持零 coverage，不能借用默认方向光制造间接光。
            if(frame.rsm_projection.lightIndex>=0)for(std::uint32_t face=0;face<frame.rsm_projection.faceCount;++face) {
                const float size=float(rsm_face_resolution);
                VkViewport viewport{float(face)*size,0,size,size,0,1};VkRect2D scissor{{int(face*rsm_face_resolution),0},{rsm_face_resolution,rsm_face_resolution}};
                vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
                draw_items(opaque,gbuffer[0],true,-1,true,frame.rsm_descriptors[face]);
            }
            vkCmdEndRendering(cmd);
        });
        graph.add_pass("GPU-volume-GI",{{"rsm0",A::read,S::shader_read},{"rsm1",A::read,S::shader_read},{"rsm2",A::read,S::shader_read},{"position",A::read,S::shader_read},{"normal",A::read,S::shader_read},{"albedo",A::read,S::shader_read},{"volume-indirect",A::write,S::storage}},[&]{
            auto sample=[](const GpuImage& im){return GpuVolumeGi::SampledImage{im.handle,im.view,im.layout,{im.w,im.h},VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT};};
            const auto& projection=frame.rsm_projection;
            GpuVolumeGi::Inputs input{camera,scene,settings,sample(gb[0]),sample(gb[1]),sample(gb[2]),sample(rsm_gb[0]),sample(rsm_gb[1]),sample(rsm_gb[2]),projection.viewProjection[0],std::uint32_t(projection.lightIndex),std::uint32_t(frame_index),true};
            input.lightFaceViewProjections=projection.viewProjection;input.lightFaceCount=projection.faceCount;input.rsmAtlas=true;input.cacheStaticTransport=true;
            volume_gi->record(cmd,input);
        });
    }
    std::array<ResourceUse,10> fx_uses{{{"position",A::read,S::shader_read},{"normal",A::read,S::shader_read},{"albedo",A::read,S::shader_read},{"emission",A::read,S::shader_read},{"hdr",A::read,S::shader_read},{"baseline",A::read,S::shader_read},{"tangent",A::read,S::shader_read},{"meta",A::read,S::shader_read},{volume?"volume-indirect":"",A::read,S::shader_read},{"screen-ao",A::read,S::shader_read}}};
    effects->add_post_passes(graph,cmd,effect_inputs,[&]{auto data=effect_data();if(volume){auto output=volume_gi->output(std::uint32_t(frame_index));data.volume_indirect={output.image,output.view,output.layout};}return data;},fx_uses,processed);
    const std::string processed_resource(effects->post_output_resource());
    if(readback)graph.add_pass("frame-readback",{{*pending_readback==DebugView::albedo?"albedo":processed_resource,A::read,S::transfer_src},{"readback",A::write,S::transfer_dst}},[&]{VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={std::uint32_t(width),std::uint32_t(height),1};if(*pending_readback==DebugView::albedo)vkCmdCopyImageToBuffer(cmd,gb[2].handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback->handle,1,&copy);else vkCmdCopyImageToBuffer(cmd,processed.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback->handle,1,&copy);});
    graph.add_pass("visibility-readback",{{"indirect",A::read,S::transfer_src},{"visibility",A::write,S::transfer_dst}},[&]{if(frame.object_count){VkBufferCopy copy{0,0,frame.object_count*2*sizeof(VkDrawIndexedIndirectCommand)};vkCmdCopyBuffer(cmd,frame.indirect.handle,frame.visibility.handle,1,&copy);}});
    std::vector<ResourceUse> host{{"visibility",A::read,S::host_read}};if(readback)host.push_back({"readback",A::read,S::host_read});graph.add_pass("host-visibility",host,[]{},true);
    graph.add_pass("ready-for-post",{{processed_resource,A::read,S::shader_read}},[]{},true,readback?std::vector<std::string>{"frame-readback"}:std::vector<std::string>{});
    if(append_final)append_final(graph);
    auto plan=graph.compile();last_graph=plan;counters.graph_serial=serial+1;counters.graph_revision=resources.revision;counters.graph_submitted=false;counters.graph_passes=plan.order.size();counters.graph_barriers=plan.barriers.size();
    // The compiled graph drives the actual Vulkan image layouts AND buffer memory hazards.
    // Whole resources / one graphics queue; no alias allocation or async queue claims.
    graph.execute(plan,[&](const ResourceBarrier& b){
        if(b.resource=="swapchain-color"){
            auto layout=[](ResourceState s){return s==ResourceState::undefined?VK_IMAGE_LAYOUT_UNDEFINED:
                s==ResourceState::present?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;};
            barrier_image(cmd,present_image,layout(b.before),layout(b.after),VK_IMAGE_ASPECT_COLOR_BIT);
        }else if(effects->record_graph_barrier(cmd,b)){
            // 内部 ping-pong/history 图像也由这份图驱动真实转换。
        }else if(auto it=images.find(b.resource);it!=images.end()){
            auto& img=*it->second;VkImageLayout layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if(b.after==S::color_attachment)layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            else if(b.after==S::depth_attachment)layout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            else if(b.after==S::transfer_src)layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            else if(img.format==depth_format)layout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            transition(cmd,img,layout);
        }else{
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};barrier.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;barrier.srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask=b.after==S::host_read?VK_PIPELINE_STAGE_2_HOST_BIT:VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask=b.after==S::host_read?VK_ACCESS_2_HOST_READ_BIT:VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
            VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};info.memoryBarrierCount=1;info.pMemoryBarriers=&barrier;vkCmdPipelineBarrier2(cmd,&info);
        }
    },[&](std::string_view name,bool begin){profile_event(cmd,name,begin);});
}

const Scene& VulkanWorkbench::Impl::prepare_baked_scene(const Scene& scene,const Settings& settings) {
    // 取消任务只轮询 ready future；future 析构的隐式 wait 不进入正常 draw。
    for(auto it=retired_bakes.begin();it!=retired_bakes.end();){
        if(it->future.wait_for(std::chrono::seconds(0))==std::future_status::ready){try{it->future.get();}catch(...){}it=retired_bakes.erase(it);}else ++it;
    }
    const bool needed=!cpu_path(settings.path)&&(settings.environment_diffuse==EnvironmentDiffuse::prt||settings.sdf_shadows);
    bool nodes_changed=scene.nodes.size()!=bake_nodes.size();
    if(!nodes_changed)for(std::size_t i=0;i<scene.nodes.size();++i){const auto& a=scene.nodes[i];const auto& b=bake_nodes[i];if(a.parent!=b.parent||a.mesh!=b.mesh||std::memcmp(&a.local,&b.local,sizeof(a.local))){nodes_changed=true;break;}}
    const bool source_changed=bake_identity!=&scene||bake_revision!=scene.revision||nodes_changed;
    const bool changed=source_changed||bake_input!=scene.baked_resources||bake_top!=scene.sky_top||bake_bottom!=scene.sky_bottom
        ||bake_settings.environment_diffuse!=settings.environment_diffuse||bake_settings.sdf_shadows!=settings.sdf_shadows
        ||(settings.environment_diffuse==EnvironmentDiffuse::prt&&bake_settings.bake_samples!=settings.bake_samples)
        ||(settings.sdf_shadows&&bake_settings.sdf_resolution!=settings.sdf_resolution);
    if(!needed){
        if(bake_job){bake_job->cancel->store(true);retired_bakes.push_back(std::move(*bake_job));bake_job.reset();}
        bake_snapshot.reset();counters.scene_resources_pending=false;counters.scene_resources_error.clear();
        // 标记下一次开启 PRT/SDF 必须重新检查；completed_bake 仍可显式持久化。
        bake_identity=nullptr;counters.scene_resources_geometry_hash=0;return scene;
    }
    if(changed){
        if(bake_job){bake_job->cancel->store(true);retired_bakes.push_back(std::move(*bake_job));bake_job.reset();}
        bake_snapshot.reset();++bake_generation;counters.scene_resources_error.clear();
        if(source_changed)bake_source.reset(); // 不能拿上一几何的快照响应连续天空编辑。
        bake_identity=&scene;bake_revision=scene.revision;bake_nodes=scene.nodes;bake_top=scene.sky_top;bake_bottom=scene.sky_bottom;bake_settings=settings;bake_input=scene.baked_resources;
        // 天空/质量变化复用已完成的独立源快照；没有跨 draw 借用输入数组。
        if(source_changed||!bake_source)bake_snapshot=std::make_unique<SceneUploadSnapshot>(scene);
    }
    if(bake_job&&bake_job->future.wait_for(std::chrono::seconds(0))==std::future_status::ready){
        try{auto result=bake_job->future.get();bake_source=std::move(result.source);prepared_scene=std::make_shared<Scene>(std::move(result.prepared));completed_bake=prepared_scene->baked_resources;prepared_generation=bake_generation;}
        catch(const std::exception& e){counters.scene_resources_error=e.what();}
        bake_job.reset();
    }
    if(!bake_job&&prepared_generation!=bake_generation&&counters.scene_resources_error.empty()){
        std::shared_ptr<const Scene> snapshot;
        try{
            if(bake_snapshot){
                if(bake_snapshot->advance(scene,counters.uploadBudget)){auto copy=bake_snapshot->take();copy.baked_resources=bake_input;snapshot=std::make_shared<Scene>(std::move(copy));bake_source=snapshot;bake_snapshot.reset();}
            }else snapshot=bake_source;
            if(snapshot){
                auto cancel=std::make_shared<std::atomic<bool>>(false);auto options=settings;auto top=bake_top,bottom=bake_bottom;auto input=bake_input;
                bake_job=BakeJob{cancel,std::async(std::launch::async,[snapshot,options,cancel,top,bottom,input]{
                    // 大数组复制、指纹、准备与每实例 irradiance apply 均不占用 UI 线程。
                    auto source=std::make_shared<Scene>(*snapshot);source->sky_top=top;source->sky_bottom=bottom;if(input)source->baked_resources=input;
                    auto resources=prepare_scene_resources(*source,options,cancel.get());
                    if(!resources||cancel->load())throw std::runtime_error("Scene resource preparation cancelled");
                    auto prepared=apply_scene_resources(*source,resources,options);
                    return BakeResult{std::move(source),std::move(prepared)};
                })};
            }
        }catch(const std::exception& e){counters.scene_resources_error=e.what();}
    }
    const bool prepared=prepared_scene&&prepared_generation==bake_generation;
    counters.scene_resources_pending=!prepared&&counters.scene_resources_error.empty();
    counters.scene_resources_geometry_hash=prepared&&completed_bake?completed_bake->geometry_hash:0;
    if(!prepared)return scene;
    // 不重烘焙灯光；只更新未被后台任务持有的展示 Scene 小字段。
    prepared_scene->lights=scene.lights;return *prepared_scene;
}

bool VulkanWorkbench::Impl::draw(const Scene& source_scene,const Camera& camera,const Settings& settings,const std::function<void()>& ui) {
    const auto frame_start=ProfileClock::now();
    if(failed)throw std::runtime_error("VulkanWorkbench is faulted; recreate after the previous GPU failure");
    int drawable_w=0,drawable_h=0;SDL_Vulkan_GetDrawableSize(window,&drawable_w,&drawable_h);
    if(drawable_w<=0||drawable_h<=0||(SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED))return false;
    if((dirty||!swapchain)&&!recreate_swapchain())return false;
    auto& frame=frames[frame_index];const auto wait_start=ProfileClock::now();
    check(vkWaitForFences(vk,1,&frame.fence,VK_TRUE,UINT64_MAX),"wait frame");const auto wait_ms=elapsed_ms(wait_start);
    if(frame.submitted&&frame.object_count){check(vmaInvalidateAllocation(allocator,frame.visibility.allocation,0,VK_WHOLE_SIZE),"read object visibility");auto* commands=static_cast<const VkDrawIndexedIndirectCommand*>(frame.visibility.mapped);counters.candidate_objects=frame.object_count;counters.visible_objects=0;counters.triangles=0;
        for(std::size_t i=0;i<frame.object_count;++i){counters.visible_objects+=commands[i].instanceCount;counters.triangles+=commands[i].indexCount/3*commands[i].instanceCount;}counters.triangles+=frame.shadow_triangles;
    }
    if(frame.submitted){
        frame.profile.completed=true;
        if(query_pool){std::vector<std::uint64_t> times(2+frame.profile.passes.size()*2);
            const auto r=vkGetQueryPoolResults(vk,query_pool,std::uint32_t(frame_index)*profile_queries_per_slot,
                std::uint32_t(times.size()),times.size()*sizeof(std::uint64_t),times.data(),sizeof(std::uint64_t),VK_QUERY_RESULT_64_BIT);
            if(r==VK_SUCCESS){decode_frame_timestamps(frame.profile,times,timestamp_bits,properties.limits.timestampPeriod);
                counters.gpu_ms=frame.profile.gpu_ms;counters.gpu_sample_serial=frame.serial;}
            else if(r!=VK_NOT_READY)check(r,"read frame profile timestamps");
        }
        counters.completed_profile=frame.profile;
    }
    frame.profile={};frame.profile.cpu_wait_ms=wait_ms;frame.profile.bounds_cached=bounds_cache_enabled;profile_scopes.clear();
    frame.scene_owner.reset(); // The frame fence has retired every use of its old bundle.
    const auto ui_start=ProfileClock::now();
    ImGui::SetCurrentContext(imgui);update_font_scale();ImGui_ImplVulkan_NewFrame();ImGui_ImplSDL2_NewFrame();ImGui::NewFrame();
    try{if(ui)ui();}catch(...){ImGui::EndFrame();throw;}ImGui::Render();
    frame.profile.cpu_ui_ms=elapsed_ms(ui_start);const auto prepare_start=ProfileClock::now();uploaded_this_draw=0;
    const bool reference_path=cpu_path(settings.path);
    const bool blocked=!reference_path&&!VulkanWorkbench::unsupported_modes(source_scene,settings).empty();
    const bool presentation_only=reference_path||blocked;
    // CPU 专题只展示完成的参考图，不重复准备/上传其演示场景的 GPU 几何和烘焙。
    // blocked 也只呈现旧 GPU 结果。这个临时设置仅用于取消烘焙，不执行 CPU 渲染。
    auto bake_options=settings;if(blocked)bake_options.path=RenderPath::cpu_raster;
    const Scene& scene=prepare_baked_scene(presentation_only?placeholder->snapshot:source_scene,bake_options);
    // Main computes CPU images asynchronously. Until the first image arrives, show the
    // black placeholder plus the live UI, so switching modes never kills the event loop.
    if(!blocked)targets(settings);
    else if(!hdr.handle){Settings initial;initial.render_width=initial.render_height=64;initial.shadow_resolution=32;targets(initial);}
    prepare_reference(frame);if(!presentation_only)upload_scene(scene,settings.filter);
    frame.scene_owner=presentation_only?placeholder:(active_scene?active_scene:placeholder);auto& resources=*frame.scene_owner;
    const bool current_scene=resources.ready&&resources.identity==&scene&&resources.revision==scene.revision&&resources.bake==scene.baked_resources;
    counters.displayedAssetRevision=resources.ready?resources.asset_revision:0;
    const Scene& visible_scene=current_scene?scene:resources.snapshot;
    const bool display_ready=!presentation_only&&resources.ready&&VulkanWorkbench::unsupported_modes(visible_scene,settings).empty();
    const bool keep_gpu_output=blocked&&has_scene_output;
    if(resources.object_capacity*sizeof(GpuObject)>frame.objects.size){
        // Only this fence-retired frame moves the prebuilt allocations; another in-flight
        // frame keeps its original allocations/descriptors. No allocation or wait-idle here.
        auto& scratch=resources.prepared_scratch[frame_index];
        if(!scratch.objects.handle)throw std::logic_error("Committed scene lacks prepared frame scratch");
        frame.objects=std::move(scratch.objects);frame.indirect=std::move(scratch.indirect);frame.visibility=std::move(scratch.visibility);frame.object_count=0;
    }
    auto worlds=resources.ready?world_matrices(visible_scene):std::vector<glm::mat4>{};
    if(!presentation_only){
        advanced_shadows->configure(visible_scene,camera,settings,worlds,std::uint32_t(frame_index));
        lighting->configure(visible_scene,settings,std::uint32_t(frame_index));
    }
    const bool bake_current=prepared_scene&&&scene==prepared_scene.get()&&prepared_generation==bake_generation&&current_scene;
    const bool empty_geometry=std::none_of(visible_scene.meshes.begin(),visible_scene.meshes.end(),[](const Mesh& mesh){return mesh.indices.size()>=3;});
    if(!presentation_only)scene_gpu->configure(visible_scene,settings,std::uint32_t(frame_index),resources.sdf_upload,
        bake_current&&bool(resources.bake&&resources.bake->prt),bake_current&&bool(resources.bake&&(resources.bake->sdf||empty_geometry)));
    const bool requires_bake=settings.environment_diffuse!=EnvironmentDiffuse::ibl||settings.sdf_shadows;
    counters.scene_resources_ready=!blocked&&(reference_path||!requires_bake||(current_scene&&scene_gpu->ready(settings)&&counters.scene_resources_error.empty()));
    counters.scene_resources_pending=counters.scene_resources_pending||(!counters.scene_resources_ready&&counters.scene_resources_error.empty()&&!presentation_only);
    counters.environment_ready=presentation_only||lighting->environment_ready();
    const bool needs_volume=settings.gi==GiMode::rsm||settings.gi==GiMode::lpv||settings.gi==GiMode::voxel;
    if(display_ready&&needs_volume) {
        volume_gi->configure(visible_scene,{std::uint32_t(width),std::uint32_t(height)},std::uint32_t(settings.voxel_resolution),settings.gi);
        frame.rsm_projection=GpuVolumeGi::rsm_projection(visible_scene,volume_gi->bounds_minimum(),volume_gi->bounds_maximum());
    }
    update_descriptors(frame,resources);
    Globals globals;auto extent=swapchain->extent();globals.view=camera.view();globals.projection=camera.projection(float(width)/float(height),settings.reversed_z);
    globals.projection[1][1]*=-1;
    const glm::vec2 jitter=settings.taa&&!presentation_only?effects->jitter():glm::vec2(0);
    globals.projection=GpuEffects::jittered_projection(globals.projection,jitter);globals.vp=globals.projection*globals.view;
    globals.camera_near={camera.position,camera.near_plane};globals.sky_top_far={visible_scene.sky_top,camera.far_plane};globals.sky_bottom_exposure={visible_scene.sky_bottom,std::max(settings.exposure,0.f)};
    globals.dimensions={width,height,(width+15)/16,(height+15)/16};globals.modes={std::min(int(visible_scene.lights.size()),light_limit),int(settings.culling),int(settings.debug),int(settings.shading)};
    // SSR/SSGI 使用 IBL 作为 miss fallback，由 effects 替换命中贡献，不重复累加。
    globals.options={int(settings.shadows),settings.gi==GiMode::environment||settings.gi==GiMode::ssr||settings.gi==GiMode::ssgi?0:1,settings.reversed_z?1:0,settings.energy_compensation?1:0};globals.post={0,std::max(settings.bloom_strength,0.f),std::max(settings.bloom_threshold,0.f),std::max(settings.shadow_bias,0.f)};
    globals.misc={-1,cluster_slices,srgb_format(swapchain->image_format())?1:0,display_ready||keep_gpu_output?0:1};
    if(keep_gpu_output){globals.modes.z=int(last_scene_debug);globals.sky_bottom_exposure.w=last_scene_exposure;}
    for(int i=0;i<globals.modes.x;++i){const auto& l=visible_scene.lights[i];globals.lights[i]={{l.position,std::max(l.range,0.f)},{l.direction,float(int(l.kind))},{l.color,std::max(l.intensity,0.f)}};}
    // 阴影模块统一选择方向光/矩形光；Globals 不能再独立按“只有方向光”查找。
    globals.misc.x=presentation_only?-1:advanced_shadows->light_index();
    if(display_ready)globals.light_vp=advanced_shadows->uniform(std::uint32_t(frame_index)).area_projection.x>0
        ? advanced_shadows->uniform(std::uint32_t(frame_index)).light_vp[0]:light_projection(visible_scene,worlds,globals.misc.x);
    // 相机正常移动依靠重投影；几何/灯光跳变则丢弃历史，防止把旧遮挡拖到新位置。
    std::uint64_t light_key=1469598103934665603ull;auto hash=[&](const auto& value){const auto* p=reinterpret_cast<const unsigned char*>(&value);for(std::size_t i=0;i<sizeof(value);++i){light_key^=p[i];light_key*=1099511628211ull;}};
    hash(visible_scene.sky_top);hash(visible_scene.sky_bottom);hash(environment_fingerprint(visible_scene));hash(settings.environment_diffuse);hash(settings.sdf_shadows);hash(settings.sdf_softness);for(const auto& light:visible_scene.lights){hash(light.kind);hash(light.position);hash(light.direction);hash(light.color);hash(light.intensity);hash(light.range);hash(light.size);}
    const bool assets_changed=previous_scene_owner.lock()!=frame.scene_owner;
    if(assets_changed)++temporal_epoch;
    effect_inputs={};effect_inputs.width=std::uint32_t(width);effect_inputs.height=std::uint32_t(height);effect_inputs.frame_slot=std::uint32_t(frame_index);effect_inputs.camera=camera;effect_inputs.settings=settings;effect_inputs.scene_revision=temporal_epoch;effect_inputs.sky_top=visible_scene.sky_top;effect_inputs.sky_bottom=visible_scene.sky_bottom;
    effect_inputs.current_vp=globals.vp;effect_inputs.previous_vp=previous_frame_valid?previous_vp:globals.vp;effect_inputs.current_jitter=jitter;effect_inputs.previous_jitter=previous_jitter;
    effect_inputs.invalidate_history=!previous_frame_valid||assets_changed||light_key!=previous_light_key||glm::length(camera.position-previous_camera)>std::max(1.f,camera.far_plane*.1f);
    std::memcpy(frame.uniform.mapped,&globals,sizeof(globals));check(vmaFlushAllocation(allocator,frame.uniform.allocation,0,VK_WHOLE_SIZE),"flush globals");
    if(display_ready&&needs_volume) {
        const auto alignment=properties.limits.minUniformBufferOffsetAlignment;
        const VkDeviceSize stride=(sizeof(Globals)+alignment-1)/alignment*alignment;
        for(std::uint32_t face=0;face<frame.rsm_projection.faceCount;++face) {
            Globals faceGlobals=globals;faceGlobals.light_vp=frame.rsm_projection.viewProjection[face];
            std::memcpy(static_cast<std::byte*>(frame.rsm_uniform.mapped)+stride*face,&faceGlobals,sizeof(faceGlobals));
        }
        check(vmaFlushAllocation(allocator,frame.rsm_uniform.allocation,0,VK_WHOLE_SIZE),"flush RSM face globals");
    }
    Buffer readback;bool capture=!pending_screenshot.empty();
    Buffer scene_readback;if(pending_readback&&display_ready&&current_scene&&counters.scene_resources_ready&&!counters.uploadPending&&counters.uploadError.empty()&&lighting->environment_ready())scene_readback=buffer(VkDeviceSize(width)*height*(*pending_readback==DebugView::albedo?8:16),VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
    if(capture&&(!screenshot_supported||!bmp_format(swapchain->image_format()))){counters.screenshot_error="Swapchain does not support 8-bit screenshot readback";pending_screenshot.clear();capture=false;}
    if(capture)readback=buffer(VkDeviceSize(extent.width)*extent.height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
    frame.profile.cpu_prepare_ms=elapsed_ms(prepare_start);frame.profile.width=width;frame.profile.height=height;
    frame.profile.scene_revision=resources.revision;frame.profile.upload_bytes=uploaded_this_draw;
    const auto acquire_start=ProfileClock::now();
    std::uint32_t index=0;auto acquired=vkAcquireNextImageKHR(vk,swapchain->swapchain(),UINT64_MAX,frame.acquired,VK_NULL_HANDLE,&index);
    frame.profile.cpu_acquire_ms=elapsed_ms(acquire_start);
    if(acquired==VK_ERROR_OUT_OF_DATE_KHR){dirty=true;return false;}if(acquired==VK_SUBOPTIMAL_KHR)dirty=true;else check(acquired,"acquire image");
    // Acquiring consumes external state. A later recording/submission exception faults the
    // workbench, so we never re-wait an unsignalled fence or reuse a signalled acquire sem.
    try {
        const auto record_start=ProfileClock::now();
        check(vkResetCommandPool(vk,frame.pool,0),"reset frame commands");VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(frame.command,&bi),"begin frame");auto cmd=frame.command;
        if(query_pool){vkCmdResetQueryPool(cmd,query_pool,std::uint32_t(frame_index)*profile_queries_per_slot,profile_queries_per_slot);vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,query_pool,std::uint32_t(frame_index)*profile_queries_per_slot);}
        record_reference_upload(cmd,frame);
        counters.draw_calls=0;vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&frame.descriptors,0,nullptr);
        // 最终颜色、UI 和 PRESENT 布局也是实际图节点；它们不再游离于场景图之外。
        auto record_tone=[&]{
            if(display_ready||keep_gpu_output){VkDescriptorImageInfo sampled{presentation_sampler,processed.view,processed.layout};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=frame.post_descriptors;write.dstBinding=8;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.descriptorCount=1;write.pImageInfo=&sampled;vkUpdateDescriptorSets(vk,1,&write,0,nullptr);}
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&frame.post_descriptors,0,nullptr);
            VkRenderingAttachmentInfo output{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};output.imageView=swapchain->image_views()[index];output.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;output.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;output.storeOp=VK_ATTACHMENT_STORE_OP_STORE;output.clearValue.color.float32[3]=1;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea.extent=extent;ri.layerCount=1;ri.colorAttachmentCount=1;ri.pColorAttachments=&output;vkCmdBeginRendering(cmd,&ri);
            auto box=letterbox(reference_path?frame.reference.w:std::uint32_t(width),reference_path?frame.reference.h:std::uint32_t(height),extent,viewport_inset_left,viewport_inset_right,viewport_inset_top,viewport_inset_bottom);
            if(box.extent.width&&box.extent.height){VkViewport vp{float(box.offset.x),float(box.offset.y),float(box.extent.width),float(box.extent.height),0,1};vkCmdSetViewport(cmd,0,1,&vp);vkCmdSetScissor(cmd,0,1,&box);fullscreen(cmd,post_pipeline);}vkCmdEndRendering(cmd);
        };
        auto record_ui=[&]{VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=ui_pass;rp.framebuffer=ui_framebuffers[index];rp.renderArea.extent=extent;vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),cmd);vkCmdEndRenderPass(cmd);};
        auto append_final=[&](RenderGraph& graph){
            using A=ResourceAccess;using S=ResourceState;
            // 本帧 CLEAR 丢弃旧像素；不导入上一轮呈现的内容，外部图像句柄仍由 Swapchain 拥有。
            graph.add_resource("swapchain-color",false,S::undefined);
            if(!display_ready)graph.add_resource("presentation-input",true,S::shader_read);
            graph.add_pass("tone-map-and-present-color",{{display_ready?std::string(effects->post_output_resource()):"presentation-input",A::read,S::shader_read},{"swapchain-color",A::write,S::color_attachment}},record_tone);
            graph.add_pass("editor-UI",{{"swapchain-color",A::read_write,S::color_attachment}},record_ui);
            graph.add_pass("present-layout",{{"swapchain-color",A::read,S::present}},[]{},true);
            graph.export_resource("swapchain-color");
        };
        if(display_ready){render_scene(cmd,visible_scene,camera,settings,worlds,scene_readback.handle?&scene_readback:nullptr,append_final,swapchain->images()[index]);has_scene_output=true;last_scene_debug=settings.debug;last_scene_exposure=globals.sky_bottom_exposure.w;const auto& diagnostic=effects->diagnostics();counters.effect_dispatches=diagnostic.dispatches;counters.effect_passes=diagnostic.pass_names;counters.temporal_history_valid=diagnostic.history_valid;}
        else {
            frame.object_count=0;counters.candidate_objects=counters.visible_objects=counters.triangles=0;counters.graph_submitted=false;counters.effect_dispatches=0;counters.effect_passes.clear();counters.temporal_history_valid=false;
            RenderGraph presentation;append_final(presentation);const auto plan=presentation.compile();last_graph=plan;
            counters.graph_serial=serial+1;counters.graph_revision=resources.revision;counters.graph_passes=plan.order.size();counters.graph_barriers=plan.barriers.size();
            presentation.execute(plan,[&](const ResourceBarrier& b){if(b.resource=="swapchain-color"){
                auto layout=[](ResourceState s){return s==ResourceState::undefined?VK_IMAGE_LAYOUT_UNDEFINED:s==ResourceState::present?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;};
                barrier_image(cmd,swapchain->images()[index],layout(b.before),layout(b.after),VK_IMAGE_ASPECT_COLOR_BIT);}
            },[&](std::string_view name,bool begin){profile_event(cmd,name,begin);});
        }
        if(query_pool)vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,query_pool,std::uint32_t(frame_index)*profile_queries_per_slot+1);
        if(capture){barrier_image(cmd,swapchain->images()[index],VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT);VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={extent.width,extent.height,1};vkCmdCopyImageToBuffer(cmd,swapchain->images()[index],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle,1,&copy);
            VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};host.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;host.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;host.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.memoryBarrierCount=1;dep.pMemoryBarriers=&host;vkCmdPipelineBarrier2(cmd,&dep);
            barrier_image(cmd,swapchain->images()[index],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_IMAGE_ASPECT_COLOR_BIT);}
        check(vkEndCommandBuffer(cmd),"end frame");
        frame.profile.cpu_record_ms=elapsed_ms(record_start);const auto submit_start=ProfileClock::now();
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};wait.semaphore=frame.acquired;wait.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};signal.semaphore=presented[index];signal.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=cmd;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};submit.waitSemaphoreInfoCount=1;submit.pWaitSemaphoreInfos=&wait;submit.commandBufferInfoCount=1;submit.pCommandBufferInfos=&cb;submit.signalSemaphoreInfoCount=1;submit.pSignalSemaphoreInfos=&signal;
        // Fence reset only immediately before a real submission; OUT_OF_DATE never resets it.
        check(vkResetFences(vk,1,&frame.fence),"reset frame fence");check(vkQueueSubmit2(device->graphics_queue(),1,&submit,frame.fence),"submit frame");frame.submitted=true;
        counters.graph_submitted=true;
        VkSwapchainKHR chain=swapchain->swapchain();VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&presented[index];present.swapchainCount=1;present.pSwapchains=&chain;present.pImageIndices=&index;
        auto result=vkQueuePresentKHR(device->present_queue(),&present);if(result==VK_ERROR_OUT_OF_DATE_KHR||result==VK_SUBOPTIMAL_KHR)dirty=true;else check(result,"present frame");
        ++serial;
        frame.serial=serial;
        frame.profile.serial=serial;frame.profile.cpu_submit_ms=elapsed_ms(submit_start);
        frame.profile.cpu_total_ms=elapsed_ms(frame_start);
        previous_frame_valid=display_ready;previous_vp=globals.vp;previous_jitter=jitter;previous_worlds=std::move(worlds);previous_revision=resources.revision;previous_light_key=light_key;previous_camera=camera.position;
        previous_scene_owner=frame.scene_owner;if(display_ready)previous_objects=std::move(pending_objects);else previous_objects.clear();
        if(scene_readback.handle){check(vkWaitForFences(vk,1,&frame.fence,VK_TRUE,UINT64_MAX),"wait frame readback");check(vmaInvalidateAllocation(allocator,scene_readback.allocation,0,VK_WHOLE_SIZE),"invalidate frame readback");
            FrameReadback completed;completed.requested=*pending_readback;completed.path=settings.path;completed.serial=serial;completed.image.reset(width,height);auto* source=static_cast<const std::uint16_t*>(scene_readback.mapped);
            for(std::size_t i=0;i<completed.image.pixels.size();++i){if(*pending_readback==DebugView::albedo)completed.image.pixels[i]={from_half(source[i*4]),from_half(source[i*4+1]),from_half(source[i*4+2])};else {auto* rgba=static_cast<const float*>(scene_readback.mapped);completed.image.pixels[i]={rgba[i*4],rgba[i*4+1],rgba[i*4+2]};}}completed_readback=std::move(completed);pending_readback.reset();
        }
        if(capture){check(vkWaitForFences(vk,1,&frame.fence,VK_TRUE,UINT64_MAX),"wait screenshot");check(vmaInvalidateAllocation(allocator,readback.allocation,0,VK_WHOLE_SIZE),"invalidate screenshot");auto path=std::exchange(pending_screenshot,{});
            try{auto format=swapchain->image_format();write_bmp(path,int(extent.width),int(extent.height),static_cast<const std::uint8_t*>(readback.mapped),format==VK_FORMAT_B8G8R8A8_SRGB||format==VK_FORMAT_B8G8R8A8_UNORM);counters.last_screenshot=path;counters.screenshot_error.clear();}catch(const std::exception& e){counters.screenshot_error=e.what();}}
        frame_index=(frame_index+1)%frames.size();VmaTotalStatistics statistics{};vmaCalculateStatistics(allocator,&statistics);counters.allocated_bytes=std::size_t(statistics.total.statistics.allocationBytes);frame.profile.allocated_bytes=counters.allocated_bytes;
        return result!=VK_ERROR_OUT_OF_DATE_KHR;
    }catch(...){failed=true;vkDeviceWaitIdle(vk);throw;}
}
void VulkanWorkbench::Impl::cleanup() noexcept {
    if(bake_job)bake_job->cancel->store(true);
    for(auto& job:retired_bakes)job.cancel->store(true);
    if(vk)vkDeviceWaitIdle(vk);
    if(imgui)ImGui::SetCurrentContext(imgui);
    destroy_present_resources();
    if(sdl_initialized){ImGui_ImplSDL2_Shutdown();sdl_initialized=false;}
    if(imgui){ImGui::DestroyContext(imgui);imgui=nullptr;}
    if(vk) {
        destroy_pipelines();
        if(descriptor_pool)vkDestroyDescriptorPool(vk,descriptor_pool,nullptr);
        if(imgui_pool)vkDestroyDescriptorPool(vk,imgui_pool,nullptr);
        if(pipeline_layout)vkDestroyPipelineLayout(vk,pipeline_layout,nullptr);
        if(global_layout)vkDestroyDescriptorSetLayout(vk,global_layout,nullptr);
        if(material_layout)vkDestroyDescriptorSetLayout(vk,material_layout,nullptr);
        for(auto sampler:{linear_sampler,nearest_sampler,bilinear_sampler,shadow_sampler,presentation_sampler})if(sampler)vkDestroySampler(vk,sampler,nullptr);
        if(query_pool)vkDestroyQueryPool(vk,query_pool,nullptr);
        for(auto& f:frames){f.scene_owner.reset();f.reference.reset();f.reference_staging.reset();f.uniform.reset();f.rsm_uniform.reset();f.objects.reset();f.indirect.reset();f.visibility.reset();if(f.pool)vkDestroyCommandPool(vk,f.pool,nullptr);if(f.fence)vkDestroyFence(vk,f.fence,nullptr);if(f.acquired)vkDestroySemaphore(vk, f.acquired,nullptr);}
        if(upload_pool)vkDestroyCommandPool(vk,upload_pool,nullptr);
    }
    scene_gpu.reset();volume_gi.reset();effects.reset();lighting.reset();advanced_shadows.reset();
    upload_ring.reset();pending_scene.reset();active_scene.reset();placeholder.reset();light_lists.reset();application_icon.reset();energy_lut.reset();hdr.reset();indirect_baseline.reset();depth.reset();shadow.reset();rsm_depth.reset();for(auto& g:gb)g.reset();for(auto& g:rsm_gb)g.reset();
    if(allocator){vmaDestroyAllocator(allocator);allocator={};}
    driver_cache.reset();swapchain.reset();device.reset();context.reset();vk={};
}

VulkanWorkbench::VulkanWorkbench(SDL_Window* w,const std::filesystem::path& shaders):impl_(std::make_unique<Impl>()){impl_->initialize(w,shaders);}
VulkanWorkbench::~VulkanWorkbench()=default;
void VulkanWorkbench::process_event(const SDL_Event& e){ImGui::SetCurrentContext(impl_->imgui);ImGui_ImplSDL2_ProcessEvent(&e);if(e.type==SDL_WINDOWEVENT&&e.window.windowID==SDL_GetWindowID(impl_->window)&&(e.window.event==SDL_WINDOWEVENT_SIZE_CHANGED||e.window.event==SDL_WINDOWEVENT_RESIZED||e.window.event==SDL_WINDOWEVENT_RESTORED))resize();}
void VulkanWorkbench::resize(){impl_->dirty=true;}
void VulkanWorkbench::set_viewport_inset_left(std::uint32_t pixels) noexcept{set_viewport_insets(pixels,0,0,0);}
void VulkanWorkbench::set_viewport_insets(std::uint32_t left,std::uint32_t right,std::uint32_t top,std::uint32_t bottom) noexcept{
    impl_->viewport_inset_left=left;impl_->viewport_inset_right=right;impl_->viewport_inset_top=top;impl_->viewport_inset_bottom=bottom;
}
void VulkanWorkbench::set_upload_budget(std::size_t bytes){if(bytes<65536||bytes>12*1024*1024)throw std::invalid_argument("Upload budget must be 64 KiB..12 MiB");impl_->counters.uploadBudget=bytes&~std::size_t(3);}
void VulkanWorkbench::set_bounds_cache_enabled(bool enabled) noexcept {impl_->bounds_cache_enabled=enabled;}
bool VulkanWorkbench::draw(const Scene& s,const Camera& c,const Settings& settings,const std::function<void()>& ui){return impl_->draw(s,c,settings,ui);}
const WorkbenchStats& VulkanWorkbench::stats() const{return impl_->counters;}
std::uint64_t VulkanWorkbench::max_render_tiles() const noexcept{return impl_->properties.limits.maxStorageBufferRange/(cluster_slices*65*sizeof(std::uint32_t));}
std::uint64_t VulkanWorkbench::max_render_pixels() const noexcept{return impl_->render_pixel_limit;}
std::uintptr_t VulkanWorkbench::application_icon_texture() const noexcept{return reinterpret_cast<std::uintptr_t>(impl_->icon_descriptor);}
const GraphPlan& VulkanWorkbench::render_graph() const{return impl_->last_graph;}
std::shared_ptr<const SceneBakeResources> VulkanWorkbench::scene_bake_resources() const{return impl_->completed_bake;}
void VulkanWorkbench::wait_idle(){check(vkDeviceWaitIdle(impl_->vk),"workbench wait idle");}
void VulkanWorkbench::set_reference_image(const Image<glm::vec3>& img){
    if(img.width<0||img.height<0||img.pixels.size()!=std::size_t(img.width)*img.height)throw std::invalid_argument("Invalid reference image storage");
    if(impl_->reference_revision==UINT64_MAX)throw std::overflow_error("Reference image revision exhausted");
    impl_->pending_reference=img;++impl_->reference_revision;
}
void VulkanWorkbench::request_screenshot(const std::filesystem::path& path){if(path.empty())throw std::invalid_argument("Empty screenshot filename");impl_->pending_screenshot=path;impl_->counters.screenshot_error.clear();}
void VulkanWorkbench::request_frame_readback(DebugView requested){if(requested!=DebugView::final_color&&requested!=DebugView::albedo)throw std::invalid_argument("Readback supports HDR/final_color or linear albedo");impl_->pending_readback=requested;}
std::optional<FrameReadback> VulkanWorkbench::take_frame_readback(){return std::exchange(impl_->completed_readback,std::nullopt);}
bool VulkanWorkbench::reload_pipelines(const std::filesystem::path& new_spv_dir,std::string* error) {
    auto& self=*impl_;std::array<VkPipeline*,12> slots{&self.forward[0],&self.forward[1],&self.transparent[0],&self.transparent[1],&self.gbuffer[0],&self.gbuffer[1],&self.shadow_pipeline,&self.sky_pipeline,&self.deferred_pipeline,&self.post_pipeline,&self.cull_pipeline,&self.object_pipeline};
    std::array<VkPipeline,12> old{};auto previous_path=self.shaders;
    for(std::size_t i=0;i<slots.size();++i)old[i]=std::exchange(*slots[i],VK_NULL_HANDLE);
    try {
        std::string preparation_error;
        auto effects_candidate=self.effects->prepare_pipelines(new_spv_dir,&preparation_error);if(!effects_candidate)throw std::runtime_error(preparation_error);
        auto shadows_candidate=self.advanced_shadows->prepare_pipelines(new_spv_dir,&preparation_error);if(!shadows_candidate)throw std::runtime_error(preparation_error);
        auto volume_candidate=self.volume_gi->prepare_pipelines(new_spv_dir,&preparation_error);if(!volume_candidate)throw std::runtime_error(preparation_error);
        self.shaders=new_spv_dir;self.create_pipelines();if(self.swapchain)self.post_pipeline=self.pipeline("fullscreen.vert","post.frag",{self.swapchain->image_format()});
        check(vkDeviceWaitIdle(self.vk),"wait pipeline replacement");
        // token 在同模块 prepare 后提交；commit 不分配、不等待、不抛异常。
        self.effects->commit_pipelines(std::move(effects_candidate));self.advanced_shadows->commit_pipelines(std::move(shadows_candidate));self.volume_gi->commit_pipelines(std::move(volume_candidate));self.previous_frame_valid=false;
        for(auto p:old)if(p)vkDestroyPipeline(self.vk,p,nullptr);self.counters.shader_error.clear();if(error)error->clear();return true;
    }catch(const std::exception& e){
        for(std::size_t i=0;i<slots.size();++i){if(*slots[i])vkDestroyPipeline(self.vk,*slots[i],nullptr);*slots[i]=old[i];}
        self.shaders=std::move(previous_path);self.counters.shader_error=e.what();if(error)*error=self.counters.shader_error;return false;
    }catch(...){
        for(std::size_t i=0;i<slots.size();++i){if(*slots[i])vkDestroyPipeline(self.vk,*slots[i],nullptr);*slots[i]=old[i];}
        self.shaders=std::move(previous_path);self.counters.shader_error="Unknown shader reload failure";if(error)*error=self.counters.shader_error;return false;
    }
}
std::vector<std::string> VulkanWorkbench::unsupported_modes(const Settings& s) {
    std::vector<std::string> out;if(cpu_path(s.path))return out;
    if(s.voxel_resolution!=16&&s.voxel_resolution!=32&&(s.gi==GiMode::rsm||s.gi==GiMode::lpv||s.gi==GiMode::voxel))out.emplace_back("GPU volume GI resolution must be 16 or 32");
    return out;
}
std::vector<std::string> VulkanWorkbench::unsupported_modes(const Scene& scene,const Settings& s) {
    auto out=unsupported_modes(s);if(cpu_path(s.path))return out;
    if(scene.lights.size()>light_limit)out.emplace_back("GPU supports at most 64 lights; no silent list truncation");
    if(s.shadows==ShadowMode::csm&&std::none_of(scene.lights.begin(),scene.lights.end(),[](const auto& l){return l.kind==LightKind::directional;}))
        out.emplace_back("CSM requires a directional source; use rectangle center hard/PCF/PCSS/moment shadows for an area light");
    if(s.sdf_shadows&&std::none_of(scene.lights.begin(),scene.lights.end(),[](const auto& l){return l.kind==LightKind::directional;}))
        out.emplace_back("GPU SDF visibility requires a directional source; rectangle center visibility uses its perspective shadow map");
    if(s.gi==GiMode::rsm||s.gi==GiMode::lpv||s.gi==GiMode::voxel){
        std::size_t count=0;auto mesh_count=[&](const Mesh& mesh){std::size_t n=0;if(mesh.primitives.empty())n=(mesh.indices.empty()?mesh.vertices.size():mesh.indices.size())/3;else for(const auto& p:mesh.primitives)n+=p.index_count/3;return n;};
        if(scene.nodes.empty())for(const auto& mesh:scene.meshes)count+=mesh_count(mesh);else for(const auto& node:scene.nodes)if(node.mesh>=0&&std::size_t(node.mesh)<scene.meshes.size())count+=mesh_count(scene.meshes[node.mesh]);
        if(count>GpuVolumeGi::max_triangles)out.emplace_back("GPU GI geometry exceeds shader index range; no geometry truncation or CPU fallback");
    }
    return out;
}
std::string VulkanWorkbench::unsupported_reason(const Settings& s){auto reasons=unsupported_modes(s);std::string out;for(const auto& r:reasons){if(!out.empty())out+="; ";out+=r;}return out;}

TestResults test_vulkan_workbench() {
    TestResults results=test_scene_upload_snapshot();auto cache_tests=test_pipeline_cache_format();results.insert(results.end(),cache_tests.begin(),cache_tests.end());auto test=[&](const char* name,const std::function<bool()>& f,const char* detail){try{results.push_back({name,f(),detail});}catch(const std::exception& e){results.push_back({name,false,e.what()});}};
    test("Vulkan reverse-Z near/far and monotonic depth",[]{Camera c;auto p=c.projection(1.7f,true);auto depth=[&](float z){auto q=p*glm::vec4(0,0,-z,1);return q.z/q.w;};return std::abs(depth(c.near_plane)-1)<1e-5f&&std::abs(depth(c.far_plane))<1e-5f&&depth(1)>depth(10);},"Uses the actual shared camera projection; near maps to 1, far to 0");
    test("Vulkan hierarchical world transform",[]{Scene s;s.nodes.resize(3);s.nodes[0].parent=2;s.nodes[1].parent=-1;s.nodes[2].parent=1;s.nodes[1].local=glm::translate(glm::mat4(1),glm::vec3(1,2,3));s.nodes[2].local=glm::scale(glm::mat4(1),glm::vec3(2));s.nodes[0].local=glm::translate(glm::mat4(1),glm::vec3(1,0,0));return glm::length(glm::vec3(world_matrices(s)[0]*glm::vec4(0,0,0,1))-glm::vec3(3,2,3))<1e-6f;},"Unordered parents, parent scale and local translation");
    test("Vulkan rejects cyclic scene hierarchy",[]{Scene s;s.nodes.resize(2);s.nodes[0].parent=1;s.nodes[1].parent=0;try{world_matrices(s);}catch(const std::runtime_error&){return true;}return false;},"Must throw before traversing forever");
    test("Vulkan logarithmic cluster boundaries",[]{float n=.1f,f=100;return depth_slice(n,n,f)==0&&depth_slice(f,n,f)==15&&depth_slice(std::sqrt(n*f)*1.0001f,n,f)==8&&depth_slice(-1,n,f)==0;},"Near/far/midpoint/clamping match cull.comp partition");
    test("Vulkan timestamp wraparound",[]{return timestamp_delta(250,4,8)==10&&timestamp_delta(100,150,64)==50;},"Masks timestampValidBits instead of subtracting unrestricted counters");
    test("Vulkan shadow projection contains scene",[]{Scene s;Mesh m;m.vertices.resize(2);m.vertices[0].position={-2,-1,-3};m.vertices[1].position={2,4,3};s.meshes.push_back(m);s.lights.emplace_back();auto p=light_projection(s,{},0);for(int i=0;i<8;++i){auto q=p*glm::vec4(i&1?2:-2,i&2?4:-1,i&4?3:-3,1);if(std::abs(q.x)>1||std::abs(q.y)>1||q.z<0||q.z>1)return false;}return true;},"All eight AABB corners fit the directional-light projection");
    test("Vulkan advanced modes and bounded volume configuration",[]{Settings s;s.taa=true;s.ao=AoMode::gtao;s.shadows=ShadowMode::msm;s.shading=ShadingMode::disney;if(!VulkanWorkbench::unsupported_modes(s).empty())return false;s.gi=GiMode::voxel;s.voxel_resolution=64;if(VulkanWorkbench::unsupported_modes(s).empty())return false;s.path=RenderPath::path_trace;return VulkanWorkbench::unsupported_modes(s).empty();},"Advanced GPU modes accepted; volume limits explicit; CPU reference remains unrestricted");
    test("Vulkan bounded light list contract",[]{Settings s;s.energy_compensation=false;Scene scene;scene.lights.resize(64);if(!VulkanWorkbench::unsupported_modes(scene,s).empty())return false;scene.lights.emplace_back();return !VulkanWorkbench::unsupported_modes(scene,s).empty();},"64 accepted, 65 explicitly rejected rather than truncated");
    test("Vulkan RGBA upload channel conversion",[]{return channel(-1)==0&&channel(.5f)==128&&channel(2)==255&&channel(std::numeric_limits<float>::quiet_NaN())==0;},"Clamped rounded UNORM upload including NaN handling");
    test("Vulkan half-float HDR readback",[]{return from_half(0x3c00)==1&&from_half(0xbc00)==-1&&from_half(0x4000)==2&&from_half(1)==std::ldexp(1.f,-24)&&std::isinf(from_half(0x7c00))&&std::isnan(from_half(0x7e00));},"Normals, subnormals, signs, infinity and NaN");
    test("Vulkan letterbox preserves reference aspect",[]{auto box=letterbox(640,360,{1440,900});return box.extent.width==1440&&box.extent.height==810&&box.offset.x==0&&box.offset.y==45;},"16:9 render target centered in a 16:10 window");
    test("Vulkan sidebar inset letterbox and narrow window",[]{auto box=letterbox(1280,720,{1280,720},340);auto narrow=letterbox(1280,720,{320,200},340);return box.offset.x==340&&box.offset.y==95&&box.extent.width==940&&box.extent.height==529&&narrow.offset.x==320&&narrow.extent.width==0&&narrow.extent.height==0;},"Scene fits to the right of 340 drawable pixels; a fully covered window skips the post draw");
    test("Vulkan editor four-edge letterbox",[]{auto box=letterbox(1280,720,{1440,900},224,340,62,26);return box.offset.x==224&&box.offset.y==221&&box.extent.width==876&&box.extent.height==493;},"Presentation fits between scene tree, inspector, toolbar and status without changing projection");
    test("Vulkan editor insets clamp without unsigned overflow",[]{auto box=letterbox(1280,720,{320,200},224,UINT32_MAX,UINT32_MAX,UINT32_MAX);return box.offset.x==224&&box.offset.y==200&&box.extent.width==0&&box.extent.height==0;},"A fully covered or minimized drawable safely skips scene presentation");
    test("Editor layout wide and compact preserves viewport",[]{auto wide=editor::layout(1440,900,true,true,224,340);auto compact=editor::layout(800,600,true,true,224,340);return wide.left==224&&wide.right==340&&wide.viewport_width()==876&&compact.left==0&&compact.right==340&&compact.viewport_width()==460&&compact.viewport_height()==504;},"Scene dock auto-collapses below 1100 logical pixels; inspector and viewport remain usable");
    test("Editor layout hidden panels restore full viewport",[]{auto full=editor::layout(1440,900,false,false,224,340);auto small=editor::layout(100,80,true,true,224,340);return full.viewport_width()==1440&&full.viewport_height()==804&&small.right==0&&small.viewport_width()==100&&small.viewport_height()==0;},"Hidden docks release space; tiny dimensions cannot produce negative rectangles");
    test("Editor native physical pixels and bounded high DPI",[]{auto native=editor::fit_resolution(876,812,1);auto dpi=editor::fit_resolution(2190,2030,1);auto large=editor::fit_resolution(3504,3248,1.5f);return native.width==876&&native.height==812&&dpi.width==2190&&dpi.height==2030&&large.width>2048&&large.width<=4096&&large.height<=4096&&std::uint64_t(large.width)*large.height<=8294400&&std::abs(float(large.width)/large.height-3504.f/3248)<.001f;},"1x fills physical viewport; explicit 4K-pixel budget preserves aspect instead of an invisible 2048 edge cap");
    test("Editor render size honors clustered storage and VRAM budget",[]{auto size=editor::fit_resolution(3840,2160,1,32263,3000000);return std::uint64_t((size.width+15)/16)*((size.height+15)/16)<=32263&&std::uint64_t(size.width)*size.height<=3000000;},"Both padded tile count and pixel budget bound high-DPI targets");
    test("Embedded titlebar protects buttons and supports resize/drag",[]{using namespace platform;std::array<WindowDragRegion,1> drag{{{460,0,840,36}}};std::array<WindowDragRegion,2> controls{{{0,0,460,36},{1300,0,138,36}}};const auto flags=std::uint32_t(SDL_WINDOW_BORDERLESS|SDL_WINDOW_RESIZABLE);auto hit=[&](int x,int y,std::uint32_t extra=0){return detail::hit_test_window_chrome({1438,900},{x,y},flags|extra,drag,controls);};return hit(1400,1)==SDL_HITTEST_NORMAL&&hit(100,1)==SDL_HITTEST_NORMAL&&hit(700,20)==SDL_HITTEST_DRAGGABLE&&hit(1437,200)==SDL_HITTEST_RESIZE_RIGHT&&hit(3,899)==SDL_HITTEST_RESIZE_BOTTOMLEFT&&hit(3,899,SDL_WINDOW_MAXIMIZED)==SDL_HITTEST_NORMAL&&hit(700,20,SDL_WINDOW_FULLSCREEN_DESKTOP)==SDL_HITTEST_NORMAL;},"Logical hit regions never swallow close/menu input; edges resize only in restored mode");
    test("Vulkan glTF wrap and filter preservation",[]{Texture t;t.wrap_s=Texture::Wrap::mirrored_repeat;t.wrap_t=Texture::Wrap::clamp_to_edge;t.mag_filter=Texture::Filter::nearest;t.min_filter=Texture::Filter::nearest_mipmap_linear;auto s=texture_sampler_info(t,FilterMode::trilinear);if(s.addressModeU!=VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT||s.addressModeV!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE||s.minFilter!=VK_FILTER_NEAREST||s.magFilter!=VK_FILTER_NEAREST||s.mipmapMode!=VK_SAMPLER_MIPMAP_MODE_LINEAR||s.anisotropyEnable)return false;t.min_filter=Texture::Filter::linear;return texture_sampler_info(t,FilterMode::trilinear).maxLod==0;},"Imported nearest/linear/mipmap semantics and nonrepeat addressing");
    auto shadow_formulas=test_gpu_shadow_formulas();results.insert(results.end(),shadow_formulas.begin(),shadow_formulas.end());
    return results;
}
TestResults test_vulkan_uploads(VulkanWorkbench& workbench){
    auto& p=*workbench.impl_;auto results=test_gpu_upload_ring(p.vk,p.allocator,p.device->graphics_queue(),p.device->graphics_queue_family());auto cache_tests=test_gpu_pipeline_cache(p.vk,p.properties,p.shaders/"cache-test",p.driver_cache->handle());results.insert(results.end(),cache_tests.begin(),cache_tests.end());
    auto shadow_checks=test_gpu_shadows(p.device->physical_device(),p.vk,p.allocator,p.device->graphics_queue(),p.device->graphics_queue_family(),p.shaders,p.driver_cache->handle());results.insert(results.end(),shadow_checks.begin(),shadow_checks.end());
#ifdef EMBERFRAME_VULKAN_WORKBENCH_SELF_TEST
    auto effect_checks=test_gpu_effects(p.device->physical_device(),p.vk,p.allocator,p.device->graphics_queue(),p.device->graphics_queue_family(),p.shaders);results.insert(results.end(),effect_checks.begin(),effect_checks.end());
#endif
    auto volume_checks=test_gpu_volume(p.device->physical_device(),p.vk,p.allocator,p.device->graphics_queue(),p.device->graphics_queue_family(),p.shaders,p.driver_cache->handle());results.insert(results.end(),volume_checks.begin(),volume_checks.end());
    auto original_budget=workbench.stats().uploadBudget;
    try{
        Scene scene;scene.revision=100001;scene.materials.emplace_back();scene.materials[0].double_sided=true;scene.materials[0].base_color={1,0,0,1};scene.meshes.emplace_back();auto& mesh=scene.meshes[0];mesh.vertices.resize(4);
        mesh.vertices[0].position={-2,-2,0};mesh.vertices[1].position={2,-2,0};mesh.vertices[2].position={2,2,0};mesh.vertices[3].position={-2,2,0};for(auto& v:mesh.vertices){v.normal={0,0,1};v.uv={.5f,.5f};}mesh.indices={0,1,2,0,2,3};
        Camera camera;camera.position={0,0,5};camera.target={0,0,0};Settings settings;settings.render_width=settings.render_height=64;settings.gi=GiMode::none;settings.bloom=false;settings.energy_compensation=false;
        workbench.set_upload_budget(65536);std::size_t last_submitted=0,last_snapshot=0;std::uint64_t last_revision=0;int callbacks=0;
        auto step=[&]{if(last_revision!=scene.revision){last_revision=scene.revision;last_submitted=last_snapshot=0;}workbench.draw(scene,camera,settings,[&]{++callbacks;});const auto& stats=workbench.stats();if(stats.uploadSubmitted<last_submitted||stats.uploadSubmitted-last_submitted>65536||stats.snapshotCopied<last_snapshot||stats.snapshotCopied-last_snapshot>65536)throw std::runtime_error("Per-frame upload or snapshot budget violated");last_submitted=stats.uploadSubmitted;last_snapshot=stats.snapshotCopied;};
        auto ready=[&]{for(int i=0;i<4000;++i){step();if(!workbench.stats().uploadError.empty())throw std::runtime_error(workbench.stats().uploadError);if(!workbench.stats().uploadPending&&workbench.stats().displayedRevision==scene.revision)return;}throw std::runtime_error("Budgeted scene upload timed out");};
        auto sample=[&]{workbench.request_frame_readback(DebugView::albedo);for(int i=0;i<4000;++i){step();if(auto result=workbench.take_frame_readback())return result->image.at(32,32);}throw std::runtime_error("Async scene readback timed out");};
        ready();if(glm::length(sample()-glm::vec3(1,0,0))>1e-4f)throw std::runtime_error("Initial committed scene color differs");
        auto baseline=scene.revision;scene.textures.emplace_back();scene.textures[0].levels.emplace_back(1025,257,glm::vec4(0,1,0,1));scene.textures[0].min_filter=scene.textures[0].mag_filter=Texture::Filter::nearest;scene.materials[0].base_color=glm::vec4(1);scene.materials[0].base_texture=0;mesh.vertices.resize(10000);++scene.revision;
        bool submitted=false;for(int i=0;i<4000;++i){step();const auto& stats=workbench.stats();if(!stats.uploadError.empty())throw std::runtime_error(stats.uploadError);if(stats.displayedRevision!=baseline)throw std::runtime_error("Incomplete upload replaced the old scene");if(stats.uploadSubmitted>0){if(!stats.uploadPending||stats.uploaded>=stats.total)throw std::runtime_error("Large upload did not remain pending");submitted=true;break;}}
        if(!submitted)throw std::runtime_error("No asynchronous transfer submitted");
        results.push_back({"GPU scene replacement keeps old scene and UI responsive",callbacks>4,"64 KiB/frame cap checked for snapshot and GPU submission; prior revision retained"});
        auto cancelled=scene.revision;++scene.revision;scene.materials[0].base_color={1,.5f,1,1};int pending_frames=0;
        for(int i=0;i<4000;++i){step();const auto& stats=workbench.stats();if(!stats.uploadError.empty())throw std::runtime_error(stats.uploadError);if(stats.displayedRevision==cancelled)throw std::runtime_error("Cancelled generation became visible");if(stats.uploadPending){++pending_frames;if(stats.displayedRevision!=baseline)throw std::runtime_error("Partial replacement became visible");}else if(stats.displayedRevision==scene.revision)break;}
        const auto& done=workbench.stats();if(done.uploadPending||done.displayedRevision!=scene.revision||done.uploaded!=done.total||done.uploadSubmitted!=done.total||pending_frames<2)throw std::runtime_error("Atomic scene publication/fence accounting failed");
        if(glm::length(sample()-glm::vec3(0,.5f,0))>.002f)throw std::runtime_error("Newest scene texture/material bytes not visible after swap");
        results.push_back({"GPU budgeted cancellation and atomic publication",true,"Cancelled submitted generation never displayed; latest odd-width texture and geometry read back correctly"});
        baseline=scene.revision;++scene.revision;mesh.indices[1]=UINT32_MAX;
        bool rejected=false;for(int i=0;i<4000;++i){step();if(!workbench.stats().uploadError.empty()){rejected=true;break;}}
        if(!rejected||workbench.stats().uploadPending||workbench.stats().displayedRevision!=baseline||!p.active_scene||p.active_scene->revision!=baseline)throw std::runtime_error("Invalid replacement did not preserve committed scene");
        for(int i=0;i<3;++i)step();results.push_back({"GPU failed replacement preserves rendering",true,"Bad index rejected; previous scene and UI continue for subsequent frames"});
        mesh.indices[1]=1;++scene.revision;scene.materials[0].base_color={1,.25f,1,1};ready();if(glm::length(sample()-glm::vec3(0,.25f,0))>.002f)throw std::runtime_error("Upload did not recover after rejected generation");
        results.push_back({"GPU upload recovers on next revision",true,"Fresh valid generation commits and albedo readback matches after failed upload"});
        // 选择性启用编辑器的资源版本契约；旧版仅 revision 的调用者仍经过上面的预算验证。
        Node node;node.mesh=0;scene.nodes.push_back(node);scene.asset_revision=1;++scene.revision;ready();
        const auto resident=p.active_scene;
        for(int i=0;i<8;++i){scene.nodes[0].local[3].x=.02f*float(i+1);++scene.revision;step();const auto& stats=workbench.stats();
            if(stats.uploadPending||stats.uploadSubmitted||stats.snapshotCopied||stats.displayedRevision!=scene.revision||p.active_scene!=resident)
                throw std::runtime_error("Pose edit reuploaded immutable assets or displayed stale matrices");}
        scene.nodes[0].local[3].x=20;++scene.revision;step();
        if(glm::length(sample()-glm::vec3(0,.25f,0))<.1f)throw std::runtime_error("Pose-only GPU draw ignored offscreen transform");
        scene.nodes[0].local[3].x=0;++scene.revision;step();if(glm::length(sample()-glm::vec3(0,.25f,0))>.002f)throw std::runtime_error("Pose-only GPU draw failed to restore textured mesh");
        results.push_back({"GPU pose edits reuse resident geometry and textures",true,"Eight consecutive transforms: zero snapshot/upload bytes; actual albedo moves offscreen and back"});
        scene.materials[0].base_color={1,.75f,1,1};++scene.asset_revision;++scene.revision;ready();
        if(p.active_scene==resident||glm::length(sample()-glm::vec3(0,.75f,0))>.002f)throw std::runtime_error("Static material edit was mistaken for a pose update");
        results.push_back({"GPU asset revision invalidates pose fast path",true,"Changed material gets a new resource bundle and real readback matches"});
    }catch(const std::exception& e){results.push_back({"GPU scene upload integration",false,e.what()});}
    workbench.set_upload_budget(original_budget);
    auto scene_checks=test_vulkan_scene_resources(workbench);results.insert(results.end(),scene_checks.begin(),scene_checks.end());return results;
}
TestResults test_vulkan_scene_resources(VulkanWorkbench& workbench) {
    TestResults results;
    try{
        // 测试只调用真实 draw/readback；CPU 烘焙是 GPU 输入，不是最终图像替身。
        Scene scene;scene.revision=200001;scene.sky_top=scene.sky_bottom=glm::vec3(1);
        scene.materials.emplace_back();scene.materials[0].double_sided=true;scene.materials[0].base_color={.6f,.6f,.6f,1};
        Mesh floor;constexpr int cells=12;
        for(int z=0;z<=cells;++z)for(int x=0;x<=cells;++x){Vertex v;v.position={-3+.5f*x,0,-3+.5f*z};v.normal={0,1,0};floor.vertices.push_back(v);}
        for(int z=0;z<cells;++z)for(int x=0;x<cells;++x){auto a=std::uint32_t(z*(cells+1)+x),b=a+1,c=a+cells+1,d=c+1;floor.indices.insert(floor.indices.end(),{a,c,b,b,c,d});}
        scene.meshes.push_back(std::move(floor));
        Camera camera;camera.position={0,2,7};camera.target={0,0,0};
        Settings settings;settings.render_width=160;settings.render_height=120;settings.reversed_z=false;settings.bloom=false;settings.energy_compensation=false;settings.shading=ShadingMode::toon;settings.debug=DebugView::indirect;settings.environment_diffuse=EnvironmentDiffuse::sh;settings.bake_samples=256;settings.sdf_resolution=32;settings.gi=GiMode::environment;
        auto pixel=[&](glm::vec3 position){auto projection=camera.projection(float(settings.render_width)/settings.render_height,settings.reversed_z);projection[1][1]*=-1;auto p=projection*camera.view()*glm::vec4(position,1);auto q=glm::vec2(p)/p.w*.5f+.5f;return glm::ivec2(q*glm::vec2(settings.render_width,settings.render_height));};
        auto blocked=pixel({-1.5f,0,0}),lit=pixel({1.8f,0,0});
        auto read=[&](RenderPath path){settings.path=path;workbench.request_frame_readback();for(int i=0;i<4000;++i){workbench.draw(scene,camera,settings,{});const auto& stats=workbench.stats();if(!stats.uploadError.empty())throw std::runtime_error(stats.uploadError);if(!stats.scene_resources_error.empty())throw std::runtime_error(stats.scene_resources_error);if(auto result=workbench.take_frame_readback()){if(!stats.scene_resources_ready)throw std::runtime_error("Resource readback preceded readiness");return std::move(result->image);}}throw std::runtime_error("SH/PRT/SDF readback timed out");};
        auto max_error=[](const auto& a,const auto& b){float error=0;for(std::size_t i=0;i<a.pixels.size();++i){auto d=glm::abs(a.pixels[i]-b.pixels[i]);error=std::max({error,d.x,d.y,d.z});}return error;};
        auto forward=read(RenderPath::forward),deferred=read(RenderPath::deferred);
        if(glm::length(forward.at(lit.x,lit.y)-glm::vec3(.6f))>.005f||max_error(forward,deferred)>.005f)throw std::runtime_error("SH constant radiance convolution / forward-deferred mismatch");
        results.push_back({"GPU SH9 constant sky irradiance",true,"Raw SH radiance cosine convolution gives albedo * sky; real forward/deferred HDR agrees"});
        // 世界几何遮挡：只有左侧地面顶点被上方真实三角面遮住。
        Mesh blocker;blocker.vertices.resize(4);blocker.vertices[0].position={-3,1.5f,-1};blocker.vertices[1].position={0,1.5f,-1};blocker.vertices[2].position={0,1.5f,1};blocker.vertices[3].position={-3,1.5f,1};for(auto& v:blocker.vertices)v.normal={0,1,0};blocker.indices={0,2,1,0,3,2};scene.meshes.push_back(std::move(blocker));++scene.revision;
        settings.environment_diffuse=EnvironmentDiffuse::prt;forward=read(RenderPath::forward);deferred=read(RenderPath::deferred);
        const auto dark=forward.at(blocked.x,blocked.y).x,bright=forward.at(lit.x,lit.y).x;
        if(bright-dark<.12f||bright<.4f||max_error(forward,deferred)>.01f)throw std::runtime_error("PRT geometry visibility / seventh attachment mismatch: "+std::to_string(dark)+" / "+std::to_string(bright));
        if(scene.baked_resources||glm::length(scene.meshes[0].vertices[0].baked_irradiance)>0)throw std::runtime_error("GPU preparation mutated caller Scene");
        auto first=workbench.scene_bake_resources();if(!first||!first->prt)throw std::runtime_error("Missing completed PRT getter");
        results.push_back({"GPU PRT world geometry and G-buffer preservation",true,"Blocked/lit irradiance differs; location5 survives forward/deferred; source remains read-only"});
        scene.sky_top=scene.sky_bottom=glm::vec3(.5f);auto relit=read(RenderPath::forward);auto second=workbench.scene_bake_resources();
        if(!second||first->prt!=second->prt||glm::length(relit.at(lit.x,lit.y)-forward.at(lit.x,lit.y)*.5f)>.01f)throw std::runtime_error("PRT sky update rebaked transfer or used stale irradiance");
        results.push_back({"GPU PRT sky relighting reuses transfer",true,"Sky changed without revision bump; transfer owner reused and GPU irradiance halved"});
        // 纯金属没有环境 diffuse，SH 与 PRT 必须保留完全相同的镜面近似。
        scene.materials[0].metallic=1;scene.materials[0].roughness=.6f;++scene.revision;settings.shading=ShadingMode::pbr;
        settings.environment_diffuse=EnvironmentDiffuse::sh;auto metal_sh=read(RenderPath::forward);settings.environment_diffuse=EnvironmentDiffuse::prt;auto metal_prt=read(RenderPath::deferred);
        if(max_error(metal_sh,metal_prt)>.01f||metal_prt.at(lit.x,lit.y).x<.1f)throw std::runtime_error("SH/PRT altered metallic environment specular");
        results.push_back({"GPU SH/PRT preserves environment specular",true,"Metallic receiver has zero diffuse; both selections and render paths retain the same split-sum mirror"});
        scene.materials[0].metallic=0;++scene.revision;settings.shading=ShadingMode::toon;
        settings.gi=GiMode::none;settings.environment_diffuse=EnvironmentDiffuse::ibl;settings.debug=DebugView::shadow;settings.shadows=ShadowMode::hard;settings.sdf_shadows=true;settings.sdf_softness=32;scene.lights.emplace_back();scene.lights[0].direction={0,-1,0};
        forward=read(RenderPath::forward);deferred=read(RenderPath::deferred);auto bake=workbench.scene_bake_resources();
        if(!bake||!bake->sdf||forward.at(blocked.x,blocked.y).x>.1f||forward.at(lit.x,lit.y).x<.9f||max_error(forward,deferred)>.02f)throw std::runtime_error("SDF blocker/lit visibility invalid: "+std::to_string(forward.at(blocked.x,blocked.y).x)+" / "+std::to_string(forward.at(lit.x,lit.y).x));
        // 反投影得到被测试像素的真实地面点，避免把目标点当作像素中心。
        auto ground=[&](glm::ivec2 tc){auto projection=camera.projection(float(settings.render_width)/settings.render_height,false);projection[1][1]*=-1;auto q=glm::vec2(tc)+.5f;auto clip=glm::vec4(q/glm::vec2(settings.render_width,settings.render_height)*2.f-1.f,1,1);auto point=glm::inverse(projection*camera.view())*clip;auto direction=glm::vec3(point)/point.w-camera.position;return camera.position+direction*(-camera.position.y/direction.y);};
        for(auto tc:{blocked,lit}){float cpu=scene_sdf_visibility(*bake->sdf,ground(tc),{0,1,0},{0,1,0},100,settings.sdf_softness,settings.shadow_bias);if(std::abs(cpu-forward.at(tc.x,tc.y).x)>.02f)throw std::runtime_error("CPU/GPU sampled SDF visibility differs");}
        auto grid=bake->sdf;settings.sdf_shadows=false;auto baseline=read(RenderPath::forward);
        if(baseline.at(blocked.x,blocked.y).x>.1f||baseline.at(lit.x,lit.y).x<.9f||!workbench.stats().scene_resources_ready)throw std::runtime_error("SDF and baseline hard shadow disagree / disabled readiness false");
        results.push_back({"GPU scene SDF visibility matches CPU and shadow baseline",true,"Actual world mesh grid uploads; blocker/lit and both render paths match CPU conservative query"});
        // 节点变换无 revision 变化也失效；主灯方向变化不重烘焙 SDF。
        settings.sdf_shadows=true;scene.lights[0].direction={.1f,-1,0};read(RenderPath::forward);if(workbench.scene_bake_resources()->sdf!=grid)throw std::runtime_error("Light-only update rebaked SDF");
        scene.nodes.resize(2);scene.nodes[0].mesh=0;scene.nodes[1].mesh=1;scene.nodes[1].local=glm::translate(glm::mat4(1),glm::vec3(0,0,8));auto moved=read(RenderPath::forward);
        if(workbench.scene_bake_resources()->sdf==grid||moved.at(blocked.x,blocked.y).x<.9f)throw std::runtime_error("Node transform did not invalidate world SDF");
        results.push_back({"GPU SDF light reuse and transform invalidation",true,"Directional light update keeps grid; node motion without revision rebuilds and unblocks receiver"});
        settings.sdf_resolution=16;read(RenderPath::deferred);if(workbench.scene_bake_resources()->sdf->resolution!=16)throw std::runtime_error("SDF quality update did not replace grid");
        scene.meshes.clear();scene.nodes.clear();++scene.revision;read(RenderPath::forward);
        if(!workbench.stats().scene_resources_ready||workbench.scene_bake_resources()->sdf)throw std::runtime_error("Empty SDF scene never became ready");
        results.push_back({"GPU SDF quality and empty-scene readiness",true,"Resolution change publishes new grid; an empty world completes with legal dummy descriptor and full visibility"});
    }catch(const std::exception& e){results.push_back({"GPU SH/PRT/SDF integration",false,e.what()});}
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_VULKAN_WORKBENCH_SELF_TEST
// Optional standalone verification entry. Normal library builds never define this.
#include <iostream>
#undef main
int main(int argc,char** argv) {
    using namespace emberframe::lab;
    try {
        bool passed=true;for(const auto& t:test_vulkan_workbench()){std::cout<<(t.passed?"PASS ":"FAIL ")<<t.name<<": "<<t.detail<<'\n';passed&=t.passed;}
        if(!passed)return 1;if(argc<2)return 0;
        if(SDL_Init(SDL_INIT_VIDEO)!=0)throw std::runtime_error(SDL_GetError());
        SDL_Window* window=SDL_CreateWindow("EmberFrame GPU verification",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,800,600,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE);
        if(!window)throw std::runtime_error(SDL_GetError());
        try {
            VulkanWorkbench workbench(window,argv[1]);if(argc>3)workbench.set_viewport_inset_left(std::uint32_t(std::stoul(argv[3])));Scene scene;scene.materials.emplace_back();scene.materials[0].double_sided=true;scene.materials[0].base_color={.8f,.2f,.1f,1};
            Mesh mesh;mesh.vertices.resize(4);mesh.vertices[0].position={-2,-1,0};mesh.vertices[1].position={2,-1,0};mesh.vertices[2].position={2,2,0};mesh.vertices[3].position={-2,2,0};for(auto& v:mesh.vertices)v.normal={0,0,1};mesh.indices={0,1,2,0,2,3};scene.meshes.push_back(mesh);
            scene.nodes.resize(2);scene.nodes[0].mesh=0;scene.nodes[1].mesh=0;scene.nodes[1].local=glm::translate(glm::mat4(1),glm::vec3(100,0,0));
            scene.lights.emplace_back();scene.lights[0].direction={0,-.3f,-1};scene.lights.emplace_back();scene.lights[1].kind=LightKind::point;scene.lights[1].position={1,1,2};
            Camera camera;camera.position={0,0,5};camera.target={0,0,0};Settings settings;settings.energy_compensation=true;settings.render_width=320;settings.render_height=240;
            for(int i=0;i<12;++i){settings.path=i%2?RenderPath::deferred:RenderPath::forward;settings.culling=LightCulling(i%3);settings.reversed_z=i%4<2;
                if(i==5){SDL_SetWindowSize(window,960,640);workbench.resize();}
                if(i==11&&argc>2)workbench.request_screenshot(argv[2]);
                if(!workbench.draw(scene,camera,settings,[&](){ImGui::SetNextWindowPos({10,10});ImGui::SetNextWindowSize({300,160});ImGui::Begin("验证");ImGui::TextUnformatted("中文界面：前向 / 延迟 / 灯光列表");ImGui::Text("Frame %d",i);ImGui::End();}))--i;
            }
            workbench.wait_idle();const auto& stats=workbench.stats();if(!stats.screenshot_error.empty())throw std::runtime_error(stats.screenshot_error);
            std::cout<<"GPU "<<stats.gpu_name<<" ms="<<stats.gpu_ms<<" draws="<<stats.draw_calls<<" triangles="<<stats.triangles<<" objects="<<stats.visible_objects<<"/"<<stats.candidate_objects<<" graph="<<stats.graph_passes<<"/"<<stats.graph_barriers<<'\n';
            if(stats.candidate_objects!=2||stats.visible_objects!=1)throw std::runtime_error("GPU indirect frustum culling did not reject offscreen instance");
            auto read=[&](RenderPath path,DebugView mode){settings.path=path;workbench.request_frame_readback(mode);for(int attempt=0;attempt<4000;++attempt){workbench.draw(scene,camera,settings,{});if(!workbench.stats().uploadError.empty())throw std::runtime_error(workbench.stats().uploadError);auto result=workbench.take_frame_readback();if(result){if(result->image.width!=settings.render_width||result->image.height!=settings.render_height)throw std::runtime_error("GPU frame readback dimensions differ");return std::move(result->image);}}throw std::runtime_error("GPU frame readback timed out");};
            for(const auto& test:test_vulkan_uploads(workbench)){std::cout<<(test.passed?"PASS ":"FAIL ")<<test.name<<": "<<test.detail<<'\n';if(!test.passed)throw std::runtime_error(test.name);}
            for(const auto& test:test_vulkan_colored_room(workbench,argc>2?std::filesystem::path(argv[2]).parent_path():std::filesystem::path{})){
                std::cout<<(test.passed?"PASS ":"FAIL ")<<test.name<<": "<<test.detail<<'\n';if(!test.passed)throw std::runtime_error(test.name);
            }
            auto albedo=read(RenderPath::forward,DebugView::albedo);if(glm::length(albedo.at(160,120)-glm::vec3(.8f,.2f,.1f))>.002f)throw std::runtime_error("GPU albedo readback differs from material");std::cout<<"PASS GPU linear albedo readback\n";
            std::string reload_error;if(workbench.reload_pipelines(std::filesystem::path(argv[1])/"missing-shaders",&reload_error)||reload_error.empty())throw std::runtime_error("Invalid hot reload did not report failure");
            auto survived=read(RenderPath::forward,DebugView::albedo);if(glm::length(survived.at(160,120)-albedo.at(160,120))>1e-6f)throw std::runtime_error("Failed hot reload damaged old pipelines");
            if(!workbench.reload_pipelines(argv[1],&reload_error))throw std::runtime_error(reload_error);std::cout<<"PASS GPU transactional pipeline reload rollback/success\n";
            settings.culling=LightCulling::all;auto forward=read(RenderPath::forward,DebugView::final_color);auto deferred=read(RenderPath::deferred,DebugView::final_color);
            auto error=[](const auto& a,const auto& b){float maximum=0;for(std::size_t i=0;i<a.pixels.size();++i){auto d=glm::abs(a.pixels[i]-b.pixels[i]);maximum=std::max({maximum,d.x,d.y,d.z});}return maximum;};
            float difference=error(forward,deferred);if(difference>.025f)throw std::runtime_error("GPU forward/deferred mismatch: "+std::to_string(difference));std::cout<<"PASS GPU forward/deferred max HDR delta="<<difference<<'\n';
            for(auto cull:{LightCulling::tiled,LightCulling::clustered}){settings.culling=cull;auto culled=read(RenderPath::deferred,DebugView::final_color);difference=error(deferred,culled);if(difference>.002f)throw std::runtime_error("GPU light culling lost contributing light");}std::cout<<"PASS GPU all/tiled/clustered HDR equality\n";
            Texture texture;texture.levels.emplace_back(2,2);texture.levels[0].pixels={{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,1,1}};texture.wrap_s=Texture::Wrap::clamp_to_edge;texture.min_filter=texture.mag_filter=Texture::Filter::nearest;scene.textures.push_back(texture);scene.materials[0].base_texture=0;scene.materials[0].base_color=glm::vec4(1);for(auto& v:scene.meshes[0].vertices)v.uv={1.25f,.25f};++scene.revision;
            auto clamped=read(RenderPath::deferred,DebugView::albedo);if(glm::length(clamped.at(160,120)-glm::vec3(0,1,0))>1e-4f)throw std::runtime_error("GPU glTF clamp sampler not preserved");
            scene.textures[0].wrap_s=Texture::Wrap::repeat;++scene.revision;auto repeated=read(RenderPath::deferred,DebugView::albedo);if(glm::length(repeated.at(160,120)-glm::vec3(1,0,0))>1e-4f)throw std::runtime_error("GPU glTF repeat sampler incorrect");std::cout<<"PASS GPU texture upload and glTF clamp/repeat sampling\n";
            {
                const Scene saved_scene=scene;const Camera saved_camera=camera;
                scene.materials[0].base_texture=-1;scene.materials[0].double_sided=true;
                scene.sky_top=scene.sky_bottom=glm::vec3(0);scene.lights.resize(1);scene.lights[0].direction={0,0,1};
                camera.position={0,0,-5};camera.target={0,0,0};++scene.revision;
                for(auto path:{RenderPath::forward,RenderPath::deferred}) {
                    const auto back=read(path,DebugView::final_color);
                    if(back.at(160,120).x<.05f)throw std::runtime_error("Double-sided back face incorrectly shadowed by unflipped geometric normal");
                }
                const auto revision=scene.revision+1;scene=saved_scene;scene.revision=revision;camera=saved_camera;
                std::cout<<"PASS GPU double-sided back face geometric/shading normals agree\n";
            }
            scene.materials[0].base_texture=-1;scene.materials[0].double_sided=false;scene.nodes[0].local=glm::scale(glm::mat4(1),glm::vec3(-1,1,1));++scene.revision;auto mirrored=read(RenderPath::deferred,DebugView::albedo);if(glm::length(mirrored.at(160,120)-glm::vec3(1))>1e-4f)throw std::runtime_error("GPU negative-scale front face incorrect");std::cout<<"PASS GPU mirrored transform front-face correction\n";
            {
                // 相机、裁剪面和模型同比缩小，画面应与原尺寸完全等价。
                // 同时覆盖负缩放；只检查源三角形数量会漏掉“整模型被丢弃”的问题。
                const Camera original_camera=camera;const glm::mat4 original_model=scene.nodes[0].local;
                constexpr float scale=1e-4f;
                camera.position*=scale;camera.target*=scale;camera.near_plane*=scale;camera.far_plane*=scale;
                scene.nodes[0].local=glm::scale(glm::mat4(1),glm::vec3(-scale,scale,scale));++scene.revision;
                for(auto path:{RenderPath::forward,RenderPath::deferred}){
                    settings.path=path;for(int frame=0;frame<8;++frame)workbench.draw(scene,camera,settings,{});
                    auto tiny=read(path,DebugView::albedo);
                    // HDR / G-buffer 使用 FP16；允许一个量化步长，而不是要求精确等于 1。
                    if(glm::length(tiny.at(160,120)-glm::vec3(1))>.002f)throw std::runtime_error("GPU tiny-scale albedo mismatch: path="+std::to_string(int(path))+" pixel="+std::to_string(tiny.at(160,120).x)+" visible="+std::to_string(workbench.stats().visible_objects)+" candidates="+std::to_string(workbench.stats().candidate_objects));
                    for(int frame=0;frame<4;++frame)workbench.draw(scene,camera,settings,{});
                    if(workbench.stats().candidate_objects!=2||workbench.stats().visible_objects!=1)throw std::runtime_error("GPU tiny-scale visibility counters incorrect");
                }
                camera=original_camera;scene.nodes[0].local=original_model;++scene.revision;
                std::cout<<"PASS GPU tiny-scale mirrored model remains visible in forward/deferred\n";
            }
            scene.lights.clear();scene.sky_top=scene.sky_bottom=glm::vec3(1);scene.materials[0].base_color=glm::vec4(1);scene.materials[0].metallic=1;scene.materials[0].roughness=1;++scene.revision;settings.energy_compensation=false;
            auto single=read(RenderPath::forward,DebugView::final_color);settings.energy_compensation=true;auto multiple=read(RenderPath::forward,DebugView::final_color);float a=single.at(160,120).x,b=multiple.at(160,120).x;
            if(b-a<.2f||std::abs(b-1)>.15f)throw std::runtime_error("GPU KC white-furnace energy invalid");std::cout<<"PASS GPU KC white furnace single="<<a<<" compensated="<<b<<'\n';
        }catch(...){SDL_DestroyWindow(window);SDL_Quit();throw;}
        SDL_DestroyWindow(window);SDL_Quit();return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
#endif
