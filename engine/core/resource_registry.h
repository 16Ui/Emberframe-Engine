#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace emberframe::core {

// 索引定位 Slot；代数防止释放后复用同一索引时，旧 Handle 错认新资源。
struct ResourceHandle {
    std::uint64_t registry_id { 0 };
    std::uint32_t index { std::numeric_limits<std::uint32_t>::max() };
    std::uint32_t generation { 0 };

    [[nodiscard]] bool operator==(const ResourceHandle&) const = default;
};

inline std::uint64_t next_registry_id() noexcept
{
    static std::atomic<std::uint64_t> next { 1 };
    return next.fetch_add(1, std::memory_order_relaxed);
}

template <class T>
class ResourceRegistry {
public:
    static_assert(std::is_nothrow_move_constructible_v<T>,
        "Registry resources must be nothrow-movable for safe slot recycling");

    ResourceRegistry() noexcept : id_(next_registry_id()) {}
    ResourceRegistry(const ResourceRegistry&) = delete;
    ResourceRegistry& operator=(const ResourceRegistry&) = delete;
    ResourceRegistry(ResourceRegistry&&) = delete;
    ResourceRegistry& operator=(ResourceRegistry&&) = delete;
    template <class... Args>
    [[nodiscard]] ResourceHandle create(Args&&... args)
    {
        if (!free_indices_.empty()) {
            const std::uint32_t index = free_indices_.back();
            Slot& slot = slots_[index];
            slot.value.emplace(std::forward<Args>(args)...);
            free_indices_.pop_back();
            return { id_, index, slot.generation };
        }
        if (slots_.size() >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("ResourceRegistry index space exhausted");
        }
        // 每新增一个 Slot，预留相应的空闲索引容量；release() 不再需要分配内存。
        const std::size_t needed = slots_.size() + 1;
        if (free_indices_.capacity() < needed) {
            free_indices_.reserve(std::max(needed, free_indices_.capacity() * 2));
        }
        Slot slot;
        slot.value.emplace(std::forward<Args>(args)...);
        slots_.push_back(std::move(slot));
        return { id_, static_cast<std::uint32_t>(slots_.size() - 1), slots_.back().generation };
    }

    [[nodiscard]] T* get(const ResourceHandle handle) noexcept
    {
        if (handle.registry_id != id_ || handle.index >= slots_.size()) {
            return nullptr;
        }
        Slot& slot = slots_[handle.index];
        return slot.generation == handle.generation && slot.value
            ? &*slot.value : nullptr;
    }

    [[nodiscard]] const T* get(const ResourceHandle handle) const noexcept
    {
        if (handle.registry_id != id_ || handle.index >= slots_.size()) {
            return nullptr;
        }
        const Slot& slot = slots_[handle.index];
        return slot.generation == handle.generation && slot.value
            ? &*slot.value : nullptr;
    }

    [[nodiscard]] std::optional<T> release(const ResourceHandle handle)
    {
        T* const resource = get(handle);
        if (resource == nullptr) {
            return std::nullopt;
        }
        Slot& slot = slots_[handle.index];
        const bool reusable = slot.generation < std::numeric_limits<std::uint32_t>::max();
        if (reusable) {
            free_indices_.push_back(handle.index);
        }
        std::optional<T> released(std::move(*resource));
        slot.value.reset();
        // 代数耗尽时永久弃用此 Slot，不能绕回旧代数使陈旧 Handle 复活。
        if (reusable) {
            ++slot.generation;
        }
        return released;
    }

private:
    const std::uint64_t id_;
    struct Slot {
        std::optional<T> value;
        std::uint32_t generation { 1 };
    };
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_indices_;
};

// 资源从 Registry 移除后先保存在此处；只有对应 GPU 提交确实完成才销毁。
template <class T>
class DeferredReleaseQueue {
public:
    void retire(const std::uint64_t after_serial, T&& resource)
    {
        pending_.emplace_back(after_serial, std::move(resource));
    }

    void collect(const std::uint64_t completed_serial)
    {
        for (auto entry = pending_.begin(); entry != pending_.end();) {
            if (entry->after_serial <= completed_serial) {
                entry = pending_.erase(entry);
            } else {
                ++entry;
            }
        }
    }

    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }

private:
    struct Entry {
        Entry(const std::uint64_t serial, T&& retired_resource) noexcept
            : after_serial(serial), resource(std::move(retired_resource)) {}
        std::uint64_t after_serial;
        T resource;
    };
    std::list<Entry> pending_;
};

} // namespace emberframe::core
