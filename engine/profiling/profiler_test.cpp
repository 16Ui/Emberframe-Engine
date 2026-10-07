#include "profiling/profiler.h"

#include <cmath>
#include <iostream>
#include <vector>

int main()
{
    using emberframe::profiling::summarize;

    const auto empty = summarize({});
    const auto even = summarize(std::vector<double> { 4.0, 1.0, 3.0, 2.0 });
    const auto hundred = summarize([] {
        std::vector<double> values;
        for (int i = 1; i <= 100; ++i) {
            values.push_back(static_cast<double>(i));
        }
        return values;
    }());

    const bool passed = empty.count == 0
        && even.count == 4
        && std::abs(even.mean_ms - 2.5) < 0.000001
        && std::abs(even.median_ms - 2.5) < 0.000001
        && std::abs(even.p95_ms - 4.0) < 0.000001
        && std::abs(hundred.p95_ms - 95.0) < 0.000001;
    std::cout << (passed ? "Profiler statistics: PASS\n" : "Profiler statistics: FAIL\n");
    return passed ? 0 : 1;
}
