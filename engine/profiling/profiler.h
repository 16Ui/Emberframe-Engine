#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace emberframe::profiling {

struct Summary {
    std::size_t count { 0 };
    double mean_ms { 0.0 };
    double median_ms { 0.0 };
    double p95_ms { 0.0 };
};

[[nodiscard]] Summary summarize(const std::vector<double>& samples_ms);

class CpuProfiler {
public:
    void add(std::string_view scope, double milliseconds);
    void clear() noexcept;
    [[nodiscard]] const std::unordered_map<std::string, std::vector<double>>& samples() const noexcept;

private:
    std::unordered_map<std::string, std::vector<double>> samples_;
};

// 作用域结束时自动计时；传 nullptr 可以在交互模式下零样本运行。
class CpuScope {
public:
    CpuScope(CpuProfiler* profiler, std::string_view name) noexcept;
    ~CpuScope();

    CpuScope(const CpuScope&) = delete;
    CpuScope& operator=(const CpuScope&) = delete;

private:
    using Clock = std::chrono::steady_clock;
    CpuProfiler* profiler_;
    std::string_view name_;
    Clock::time_point started_;
};

} // namespace emberframe::profiling
