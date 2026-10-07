#include "profiling/profiler.h"

#include <algorithm>
#include <numeric>

namespace emberframe::profiling {

Summary summarize(const std::vector<double>& samples_ms)
{
    Summary result;
    result.count = samples_ms.size();
    if (samples_ms.empty()) {
        return result;
    }

    std::vector<double> sorted = samples_ms;
    std::sort(sorted.begin(), sorted.end());
    result.mean_ms = std::accumulate(sorted.begin(), sorted.end(), 0.0)
        / static_cast<double>(sorted.size());
    const std::size_t middle = sorted.size() / 2;
    result.median_ms = sorted.size() % 2 == 0
        ? (sorted[middle - 1] + sorted[middle]) * 0.5
        : sorted[middle];
    const std::size_t p95_index = (sorted.size() * 95 + 99) / 100 - 1;
    result.p95_ms = sorted[p95_index];
    return result;
}

void CpuProfiler::add(const std::string_view scope, const double milliseconds)
{
    samples_[std::string(scope)].push_back(milliseconds);
}

void CpuProfiler::clear() noexcept
{
    samples_.clear();
}

const std::unordered_map<std::string, std::vector<double>>& CpuProfiler::samples() const noexcept
{
    return samples_;
}

CpuScope::CpuScope(CpuProfiler* const profiler, const std::string_view name) noexcept
    : profiler_(profiler)
    , name_(name)
    , started_(Clock::now())
{
}

CpuScope::~CpuScope()
{
    if (profiler_ != nullptr) {
        const std::chrono::duration<double, std::milli> elapsed = Clock::now() - started_;
        profiler_->add(name_, elapsed.count());
    }
}

} // namespace emberframe::profiling
