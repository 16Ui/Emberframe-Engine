#pragma once
#include "types.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <memory>
#include <span>

namespace emberframe::lab {
struct UploadCompletion { std::uint64_t ticket=0; std::size_t bytes=0; };

// 有界暂存环：共享一个持久映射的 VMA 缓冲区，固定分片各自拥有命令池和 fence。
// 仅主线程提交队列；allocator/device/queue 必须比本环活得更久。
// poll() 回收已完成分片；begin() 遇到占用分片立即返回 false，绝不等待 GPU。
// 环容量不是每帧预算：调用方累计本帧提交字节数，并结合 remaining() 分块限额。
// 每次提交强引用目标资源 owner，直到 fence 完成；取消场景也不能提前释放已提交资源。
// 不依赖 buffer-device-address 或 timeline semaphore 功能。
class GpuUploadRing {
public:
    GpuUploadRing(VkDevice,VmaAllocator,VkQueue,std::uint32_t queue_family,
                  std::size_t slice_bytes=4*1024*1024,std::size_t slices=3);
    ~GpuUploadRing();
    GpuUploadRing(const GpuUploadRing&)=delete;
    GpuUploadRing& operator=(const GpuUploadRing&)=delete;
    bool begin(std::shared_ptr<void> destination_owner={});
    std::size_t remaining() const noexcept;
    std::size_t recorded_bytes() const noexcept;
    VkCommandBuffer command() const;
    void copy_buffer(VkBuffer destination,VkDeviceSize offset,std::span<const std::byte>);
    // RGBA8 区域紧密排列，bufferOffset 由暂存环填写；调用方通过 command() 记录图像
    // 布局转换，并在 begin() 传入图像 owner，或自行保证图像存活到提交 fence 完成。
    void copy_rgba8(VkImage destination,VkBufferImageCopy region,std::span<const std::byte>);
    std::uint64_t submit();
    void abort() noexcept; // 只取消正在录制、尚未提交的批次，不影响已提交任务。
    std::vector<UploadCompletion> poll();
    bool completed(std::uint64_t ticket) const noexcept;
    void wait_all(); // 仅退出/显式诊断使用；正常渲染通过 poll() 非阻塞推进。
    std::size_t capacity() const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};

// 保持 const Scene& 接口的增量独立快照：仅在 advance() 内读取调用方数据，
// 不让后台线程跨 draw() 借用可变数组。来源指针/revision 改变时拒绝继续旧快照。
// 字节预算限制所计量的记录、几何和像素拷贝量；主机端 2 ms 是软时间限制，
// 单次分配或元数据字符串复制仍可能超时，不宣称硬实时保证。
class SceneUploadSnapshot {
public:
    explicit SceneUploadSnapshot(const Scene&);
    ~SceneUploadSnapshot();
    bool advance(const Scene&,std::size_t byte_budget);
    std::size_t copied_bytes() const noexcept;
    std::size_t total_bytes() const noexcept;
    Scene take();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
TestResults test_scene_upload_snapshot();

// 显式 GPU 诊断：真实设备本地内存复制/回读，验证环绕复用、非一致性内存
// flush/invalidate、取消后的资源保活，以及非法上传请求的拒绝路径。
TestResults test_gpu_upload_ring(VkDevice,VmaAllocator,VkQueue,std::uint32_t queue_family);
} // namespace emberframe::lab
