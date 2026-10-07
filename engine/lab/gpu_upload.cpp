#include "gpu_upload.h"
#include <cstring>
#include <chrono>
#include <utility>

namespace emberframe::lab {
namespace {
void upload_check(VkResult r,const char* operation){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+": VkResult "+std::to_string(r));}
struct UploadBuffer {
    VmaAllocator allocator{};VkBuffer handle{};VmaAllocation allocation{};void* mapped{};
    UploadBuffer(VmaAllocator a,std::size_t bytes,VkBufferUsageFlags usage,bool host):allocator(a){
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=bytes;ci.usage=usage;
        VmaAllocationCreateInfo ai{};ai.usage=host?VMA_MEMORY_USAGE_AUTO:VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if(host)ai.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;else ai.requiredFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VmaAllocationInfo info{};upload_check(vmaCreateBuffer(a,&ci,&ai,&handle,&allocation,&info),"upload buffer");mapped=info.pMappedData;
    }
    ~UploadBuffer(){if(handle)vmaDestroyBuffer(allocator,handle,allocation);}
    UploadBuffer(const UploadBuffer&)=delete;UploadBuffer& operator=(const UploadBuffer&)=delete;
};
}
struct GpuUploadRing::Impl {
    VkDevice device{};VmaAllocator allocator{};VkQueue queue{};
    std::unique_ptr<UploadBuffer> staging;
    struct Slice {VkCommandPool pool{};VkCommandBuffer cmd{};VkFence fence{};bool in_flight=false;std::uint64_t ticket=0;std::size_t bytes=0;std::shared_ptr<void> owner;};
    std::vector<Slice> slices;std::size_t slice_bytes=0,slice_stride=0,next=0,current=0,cursor=0;bool recording=false,faulted=false;
    std::uint64_t next_ticket=1,completed_ticket=0;
    ~Impl(){
        // Destruction must retain every destination until the last submitted transfer ends.
        if(device)for(auto& s:slices)if(s.in_flight)vkWaitForFences(device,1,&s.fence,VK_TRUE,UINT64_MAX);
        for(auto& s:slices){s.owner.reset();if(s.pool)vkDestroyCommandPool(device,s.pool,nullptr);if(s.fence)vkDestroyFence(device,s.fence,nullptr);}
    }
    std::vector<UploadCompletion> collect(){
        std::vector<UploadCompletion> result;for(auto& s:slices)if(s.in_flight){auto r=vkGetFenceStatus(device,s.fence);if(r==VK_NOT_READY)continue;upload_check(r,"poll upload fence");
            s.in_flight=false;completed_ticket=std::max(completed_ticket,s.ticket);result.push_back({s.ticket,s.bytes});s.owner.reset();}
        return result;
    }
    VkDeviceSize stage(std::span<const std::byte> bytes){
        if(!recording)throw std::logic_error("No upload batch is recording");
        if(bytes.empty()||bytes.size()%4||bytes.size()>slice_bytes-cursor)throw std::invalid_argument("Upload chunk must fit the slice and be nonempty/aligned to four bytes");
        auto offset=current*slice_stride+cursor;std::memcpy(static_cast<std::byte*>(staging->mapped)+offset,bytes.data(),bytes.size());cursor+=bytes.size();return offset;
    }
};
GpuUploadRing::GpuUploadRing(VkDevice device,VmaAllocator allocator,VkQueue queue,std::uint32_t family,std::size_t bytes,std::size_t count):impl_(std::make_unique<Impl>()){
    if(!device||!allocator||!queue||bytes<4||bytes%4||bytes>64*1024*1024||count<2||count>8)throw std::invalid_argument("Invalid staging-ring configuration");
    auto& p=*impl_;p.device=device;p.allocator=allocator;p.queue=queue;p.slice_bytes=bytes;p.slices.resize(count);
    VmaAllocatorInfo allocator_info{};vmaGetAllocatorInfo(allocator,&allocator_info);VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(allocator_info.physicalDevice,&properties);
    // 非一致性内存 flush 会按 atom 对齐；切片间加间距，不能刷新仍被 GPU 读取的邻片。
    auto atom=std::max<std::size_t>(4,properties.limits.nonCoherentAtomSize);p.slice_stride=(bytes+atom-1)/atom*atom;
    p.staging=std::make_unique<UploadBuffer>(allocator,p.slice_stride*count,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,true);
    for(auto& s:p.slices){VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;pc.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT|VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;upload_check(vkCreateCommandPool(device,&pc,nullptr,&s.pool),"upload slice pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=s.pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;upload_check(vkAllocateCommandBuffers(device,&ca,&s.cmd),"upload slice command");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;upload_check(vkCreateFence(device,&fc,nullptr,&s.fence),"upload slice fence");}
}
GpuUploadRing::~GpuUploadRing()=default;
bool GpuUploadRing::begin(std::shared_ptr<void> owner){
    auto& p=*impl_;if(p.faulted)throw std::runtime_error("Upload ring is faulted");if(p.recording)throw std::logic_error("Upload batch already recording");
    // Completions are delivered only by poll(); never silently discard progress in begin().
    auto& s=p.slices[p.next];if(s.in_flight)return false;
    upload_check(vkResetCommandPool(p.device,s.pool,0),"reset upload slice");VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;upload_check(vkBeginCommandBuffer(s.cmd,&bi),"begin upload slice");
    p.current=p.next;p.cursor=0;p.recording=true;s.owner=std::move(owner);return true;
}
std::size_t GpuUploadRing::remaining() const noexcept{return impl_->recording?impl_->slice_bytes-impl_->cursor:0;}
std::size_t GpuUploadRing::recorded_bytes() const noexcept{return impl_->recording?impl_->cursor:0;}
VkCommandBuffer GpuUploadRing::command() const{if(!impl_->recording)throw std::logic_error("No recording upload command");return impl_->slices[impl_->current].cmd;}
void GpuUploadRing::copy_buffer(VkBuffer destination,VkDeviceSize offset,std::span<const std::byte> data){
    if(!destination||offset%4)throw std::invalid_argument("Invalid upload buffer destination/offset");auto start=impl_->stage(data);VkBufferCopy copy{start,offset,data.size()};vkCmdCopyBuffer(command(),impl_->staging->handle,destination,1,&copy);
}
void GpuUploadRing::copy_rgba8(VkImage destination,VkBufferImageCopy region,std::span<const std::byte> data){
    auto expected=std::uint64_t(region.imageExtent.width)*region.imageExtent.height*4;
    if(!destination||region.bufferRowLength||region.bufferImageHeight||region.imageExtent.depth!=1||region.imageSubresource.layerCount!=1||region.imageSubresource.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT||expected!=data.size())throw std::invalid_argument("Invalid tightly-packed RGBA8 upload region");
    region.bufferOffset=impl_->stage(data);vkCmdCopyBufferToImage(command(),impl_->staging->handle,destination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
}
std::uint64_t GpuUploadRing::submit(){
    auto& p=*impl_;if(!p.recording)throw std::logic_error("No upload batch to submit");auto& s=p.slices[p.current];
    try{
        if(p.cursor)upload_check(vmaFlushAllocation(p.allocator,p.staging->allocation,p.current*p.slice_stride,p.cursor),"flush staging slice");
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};barrier.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;barrier.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.memoryBarrierCount=1;dep.pMemoryBarriers=&barrier;vkCmdPipelineBarrier2(s.cmd,&dep);upload_check(vkEndCommandBuffer(s.cmd),"end upload slice");
        VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=s.cmd;VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};submit.commandBufferInfoCount=1;submit.pCommandBufferInfos=&cb;
        upload_check(vkResetFences(p.device,1,&s.fence),"reset upload fence");upload_check(vkQueueSubmit2(p.queue,1,&submit,s.fence),"submit upload slice");
        s.in_flight=true;s.ticket=p.next_ticket++;s.bytes=p.cursor;p.recording=false;p.next=(p.current+1)%p.slices.size();return s.ticket;
    }catch(...){p.faulted=true;p.recording=false;s.owner.reset();throw;}
}
void GpuUploadRing::abort() noexcept{auto& p=*impl_;if(p.recording){p.slices[p.current].owner.reset();p.recording=false;p.cursor=0;}}
std::vector<UploadCompletion> GpuUploadRing::poll(){return impl_->collect();}
bool GpuUploadRing::completed(std::uint64_t ticket) const noexcept{return ticket==0||ticket<=impl_->completed_ticket;}
void GpuUploadRing::wait_all(){auto& p=*impl_;if(p.recording)throw std::logic_error("Cannot drain an unsubmitted upload batch");for(auto& s:p.slices)if(s.in_flight)upload_check(vkWaitForFences(p.device,1,&s.fence,VK_TRUE,UINT64_MAX),"drain upload ring");p.collect();}
std::size_t GpuUploadRing::capacity() const noexcept{return impl_->slice_bytes*impl_->slices.size();}

struct SceneUploadSnapshot::Impl {
    const Scene* source{};std::uint64_t revision=0;Scene scene;std::size_t copied=0,total=0,index=0,part=0,level=0;int phase=0;bool done=false;
    template<class T> bool copy(std::vector<T>& dst,const std::vector<T>& src,std::size_t& budget){
        if(dst.size()==src.size())return true;
        if(dst.empty())dst.reserve(src.size());
        auto count=std::min({src.size()-dst.size(),budget/sizeof(T),std::max<std::size_t>(1,65536/sizeof(T))});if(!count)return false;
        auto offset=dst.size();dst.insert(dst.end(),src.begin()+offset,src.begin()+offset+count);budget-=count*sizeof(T);copied+=count*sizeof(T);return dst.size()==src.size();
    }
};
SceneUploadSnapshot::SceneUploadSnapshot(const Scene& source):impl_(std::make_unique<Impl>()){
    auto& p=*impl_;p.source=&source;p.revision=source.revision;p.scene.name=source.name;p.scene.revision=source.revision;p.scene.asset_revision=source.asset_revision;p.scene.sky_top=source.sky_top;p.scene.sky_bottom=source.sky_bottom;
    auto add=[&](std::size_t count,std::size_t stride){if(count>(SIZE_MAX-p.total)/stride)throw std::length_error("Scene snapshot size overflow");p.total+=count*stride;};
    add(source.materials.size(),sizeof(Material));add(source.nodes.size(),sizeof(Node));add(source.lights.size(),sizeof(Light));
    for(const auto& m:source.meshes){add(m.vertices.size(),sizeof(Vertex));add(m.indices.size(),sizeof(std::uint32_t));add(m.primitives.size(),sizeof(Primitive));}
    for(const auto& t:source.textures)for(const auto& mip:t.levels)add(mip.pixels.size(),sizeof(glm::vec4));
}
SceneUploadSnapshot::~SceneUploadSnapshot()=default;
bool SceneUploadSnapshot::advance(const Scene& source,std::size_t budget){
    auto& p=*impl_;if(p.source!=&source||p.revision!=source.revision)throw std::invalid_argument("Scene changed during upload snapshot");
    if(budget<sizeof(Material)||budget<sizeof(Node))throw std::invalid_argument("Snapshot budget too small for one metadata record");
    auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(2);unsigned records=0;
    while(!p.done&&budget>=std::max(sizeof(Material),sizeof(Node))&&records++<128&&std::chrono::steady_clock::now()<deadline){
        if(p.phase==0){if(p.copy(p.scene.materials,source.materials,budget)){p.phase=1;p.index=0;}continue;}
        if(p.phase==1){if(p.copy(p.scene.nodes,source.nodes,budget)){p.phase=2;p.index=0;}continue;}
        if(p.phase==2){if(p.copy(p.scene.lights,source.lights,budget)){p.phase=3;p.index=0;}continue;}
        if(p.phase==3){
            if(p.index==source.meshes.size()){p.phase=4;p.index=0;continue;}
            const auto& src=source.meshes[p.index];if(p.scene.meshes.size()==p.index){p.scene.meshes.emplace_back();p.scene.meshes.back().name=src.name;}
            auto& dst=p.scene.meshes[p.index];
            if(p.part==0){if(p.copy(dst.vertices,src.vertices,budget))p.part=1;}
            else if(p.part==1){auto start=dst.indices.size();bool complete=p.copy(dst.indices,src.indices,budget);for(auto i=start;i<dst.indices.size();++i)if(dst.indices[i]>=src.vertices.size())throw std::invalid_argument("Scene upload index out of range");if(complete)p.part=2;}
            else {auto start=dst.primitives.size();bool complete=p.copy(dst.primitives,src.primitives,budget);for(auto i=start;i<dst.primitives.size();++i){auto primitive=dst.primitives[i];if(std::uint64_t(primitive.first_index)+primitive.index_count>src.indices.size()||primitive.index_count%3)throw std::invalid_argument("Scene upload primitive range invalid");}
                if(complete){if(src.primitives.empty()&&src.indices.size()%3)throw std::invalid_argument("Scene upload is not a triangle list");++p.index;p.part=0;}}
            continue;
        }
        if(p.index==source.textures.size()){p.done=true;break;}
        const auto& src=source.textures[p.index];if(src.levels.empty())throw std::invalid_argument("Scene upload texture has no mip levels");
        if(p.scene.textures.size()==p.index){p.scene.textures.emplace_back();auto& t=p.scene.textures.back();t.name=src.name;t.srgb=src.srgb;t.wrap_s=src.wrap_s;t.wrap_t=src.wrap_t;t.min_filter=src.min_filter;t.mag_filter=src.mag_filter;}
        auto& texture=p.scene.textures[p.index];const auto& mip=src.levels[p.level];
        if(texture.levels.size()==p.level){
            if(mip.width<1||mip.height<1||std::uint64_t(mip.width)*mip.height!=mip.pixels.size())throw std::invalid_argument("Scene upload texture storage invalid");
            if(p.level){const auto& prev=src.levels[p.level-1];if((prev.width==1&&prev.height==1)||mip.width!=std::max(prev.width/2,1)||mip.height!=std::max(prev.height/2,1))throw std::invalid_argument("Scene upload mip chain invalid");}
            texture.levels.emplace_back();texture.levels.back().width=mip.width;texture.levels.back().height=mip.height;
        }
        if(p.copy(texture.levels[p.level].pixels,mip.pixels,budget)){if(++p.level==src.levels.size()){++p.index;p.level=0;}}
    }
    return p.done;
}
std::size_t SceneUploadSnapshot::copied_bytes() const noexcept{return impl_->copied;}
std::size_t SceneUploadSnapshot::total_bytes() const noexcept{return impl_->total;}
Scene SceneUploadSnapshot::take(){if(!impl_->done)throw std::logic_error("Scene snapshot not complete");return std::move(impl_->scene);}
TestResults test_scene_upload_snapshot(){
    TestResults results;
    auto run=[&](const char* name,const std::function<bool()>& fn){try{results.push_back({name,fn(),"Bounded snapshot; no borrowed data survives advance()"});}catch(const std::exception& e){results.push_back({name,false,e.what()});}};
    run("Upload snapshot budget and deep-copy independence",[]{Scene s;s.meshes.emplace_back();s.meshes[0].vertices.resize(10000);for(std::size_t i=0;i<10000;++i)s.meshes[0].vertices[i].position.x=float(i);SceneUploadSnapshot snapshot(s);bool done=false;int steps=0;
        while(!done&&steps++<1000){auto before=snapshot.copied_bytes();done=snapshot.advance(s,65536);if(snapshot.copied_bytes()-before>65536)return false;}if(!done||steps<2)return false;auto copy=snapshot.take();s.meshes[0].vertices[123].position.x=-1;return copy.meshes[0].vertices[123].position.x==123&&snapshot.copied_bytes()==snapshot.total_bytes();});
    run("Upload snapshot rejects stale revision",[]{Scene s;SceneUploadSnapshot copy(s);++s.revision;try{copy.advance(s,65536);}catch(const std::invalid_argument&){return true;}return false;});
    run("Upload snapshot rejects corrupt indices",[]{Scene s;s.meshes.emplace_back();s.meshes[0].vertices.resize(1);s.meshes[0].indices={0,1,0};SceneUploadSnapshot copy(s);try{for(int i=0;i<10;++i)copy.advance(s,65536);}catch(const std::invalid_argument&){return true;}return false;});
    run("Upload snapshot rejects corrupt mip storage",[]{Scene s;s.textures.emplace_back();s.textures[0].levels.emplace_back(2,2);s.textures[0].levels[0].pixels.pop_back();SceneUploadSnapshot copy(s);try{for(int i=0;i<10;++i)copy.advance(s,65536);}catch(const std::invalid_argument&){return true;}return false;});
    return results;
}

TestResults test_gpu_upload_ring(VkDevice device,VmaAllocator allocator,VkQueue queue,std::uint32_t family){
    TestResults results;
    try{
        constexpr std::size_t slice=4096,total=slice*9;auto destination=std::make_shared<UploadBuffer>(allocator,total,VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,false);
        UploadBuffer readback(allocator,total,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);GpuUploadRing ring(device,allocator,queue,family,slice,2);
        std::vector<std::byte> expected(total);for(std::size_t i=0;i<total;++i)expected[i]=std::byte((i*73+i/31+17)&255);
        bool invalid=false;ring.begin(destination);try{ring.copy_buffer(destination->handle,1,std::span<const std::byte>(expected.data(),4));}catch(const std::invalid_argument&){invalid=true;}ring.abort();results.push_back({"GPU upload rejects unaligned destination",invalid,"Rejected before recording a Vulkan copy"});
        bool overflow=false;ring.begin(destination);ring.copy_buffer(destination->handle,0,std::span<const std::byte>(expected.data(),slice));try{ring.copy_buffer(destination->handle,0,std::span<const std::byte>(expected.data(),4));}catch(const std::invalid_argument&){overflow=true;}ring.abort();results.push_back({"GPU upload ring overflow/abort",overflow&&ring.recorded_bytes()==0,"Full unsubmitted slice cannot overwrite an adjacent in-flight slice"});
        std::size_t completed=0;std::uint64_t last=0;for(std::size_t offset=0;offset<total;offset+=slice){
            for(;;){for(auto c:ring.poll())completed+=c.bytes;if(ring.begin(destination))break;}
            ring.copy_buffer(destination->handle,offset,std::span<const std::byte>(expected.data()+offset,slice));last=ring.submit();
        }
        while(!ring.completed(last)){for(auto c:ring.poll())completed+=c.bytes;}
        results.push_back({"GPU upload completion byte accounting",completed==total,"Counts only signalled-fence bytes across nine slices and ring wrap"});
        ring.begin(destination);VkBufferCopy copy{0,0,total};vkCmdCopyBuffer(ring.command(),destination->handle,readback.handle,1,&copy);
        VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};host.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;host.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;host.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.memoryBarrierCount=1;dep.pMemoryBarriers=&host;vkCmdPipelineBarrier2(ring.command(),&dep);ring.submit();ring.wait_all();
        upload_check(vmaInvalidateAllocation(allocator,readback.allocation,0,VK_WHOLE_SIZE),"invalidate upload verification");results.push_back({"GPU staging ring actual device-byte readback",std::memcmp(readback.mapped,expected.data(),total)==0,"36 KiB deterministic pattern copied to device-local memory and read back byte-for-byte"});
        auto token=std::make_shared<int>(7);std::weak_ptr<int> weak=token;ring.begin(token);ring.copy_buffer(destination->handle,0,std::span<const std::byte>(expected.data(),4));ring.submit();token.reset();bool retained=!weak.expired();ring.wait_all();results.push_back({"GPU cancelled generation lifetime",retained&&weak.expired(),"Submitted batch retains its owner until fence retirement"});
    }catch(const std::exception& e){results.push_back({"GPU upload ring diagnostic",false,e.what()});}
    return results;
}
} // namespace emberframe::lab
