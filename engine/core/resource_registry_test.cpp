#include "core/resource_registry.h"

#include <cstdint>
#include <iostream>
#include <memory>

namespace {
struct CountDeletion {
    int* count;
    void operator()(int* value) const noexcept
    {
        ++*count;
        delete value;
    }
};
}

int main()
{
    using emberframe::core::DeferredReleaseQueue;
    using emberframe::core::ResourceRegistry;

    ResourceRegistry<int> registry;
    const auto first = registry.create(17);
    if (registry.get(first) == nullptr || *registry.get(first) != 17) return 1;
    auto removed = registry.release(first);
    if (!removed || *removed != 17 || registry.get(first) != nullptr) return 2;
    if (registry.release(first).has_value()) return 3; // 重复释放必须无效。
    const auto second = registry.create(29);
    if (second.index != first.index || second.generation == first.generation) return 4;
    if (registry.get(first) != nullptr || *registry.get(second) != 29) return 5;
    if (registry.get({ 0, 999, 1 }) != nullptr) return 6;
    ResourceRegistry<int> other_registry;
    const auto foreign = other_registry.create(31);
    if (registry.get(foreign) != nullptr) return 9;

    // 模拟 GPU 序号：提交 3 仍可能引用旧资源；完成到 2 时不可析构。
    int deleted = 0;
    using CountedPointer = std::unique_ptr<int, CountDeletion>;
    DeferredReleaseQueue<CountedPointer> retired;
    CountedPointer old_resource(new int(41), CountDeletion { &deleted });
    retired.retire(3, std::move(old_resource));
    retired.collect(2);
    if (retired.pending_count() != 1 || deleted != 0) return 7;
    retired.collect(3);
    if (retired.pending_count() != 0 || deleted != 1) return 8;

    std::cout << "Resource registry and deferred release: PASS\n";
    return 0;
}
