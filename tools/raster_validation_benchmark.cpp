// Addressed 06.1 probe: production mapping only, no raster/cache allocation or I/O.
#include "core/raster_mapping.h"

#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>

using namespace cadcontour::raster;
using Clock = std::chrono::steady_clock;

namespace {

PROCESS_MEMORY_COUNTERS_EX memory()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)))
        throw GetLastError();
    return counters;
}

double milliseconds(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Cancellation {
    unsigned calls = 0;
    Clock::time_point requested;
    static bool check(void *context) noexcept
    {
        auto &self = *static_cast<Cancellation *>(context);
        // Entry, preflight, X start, then after exactly 4096 checked X boundaries.
        if (++self.calls != 4)
            return false;
        self.requested = Clock::now();
        return true;
    }
};

} // namespace

int main()
{
    try {
        // Warm up CRT/code pages before the baseline measurement.
        if (!Mapping::create({}, {10000, 1}, 1, ValidationPolicy{10001}))
            return 1;
        std::cout << std::fixed << std::setprecision(6);
        std::cout << "width,height,checks,preflight_median_ms,validation_median_ms,"
                     "ws_before,ws_after,peak_ws,private_before,private_after\n";
        for (Index width : {Index{1000}, Index{100000}, Index{1000000}, Index{5000000}, Index{25000000}}) {
            const auto before = memory();
            std::array<double, 5> preflight{}, validation{};
            for (std::size_t repeat = 0; repeat < preflight.size(); ++repeat) {
                const auto start = Clock::now();
                const auto refused = Mapping::create({}, {static_cast<double>(width), static_cast<double>(width)},
                                                       1, ValidationPolicy{0});
                preflight[repeat] = milliseconds(start);
                if (refused || refused.error().code != ErrorCode::ResourceRefused
                    || refused.error().requiredBoundaryChecks != width * 2)
                    return 2;
                const auto validating = Clock::now();
                const auto result = Mapping::create({}, {static_cast<double>(width), static_cast<double>(width)},
                                                      1, ValidationPolicy{width * 2});
                validation[repeat] = milliseconds(validating);
                if (!result || result.value().width() != width || result.value().height() != width)
                    return 3;
            }
            const auto after = memory();
            std::sort(preflight.begin(), preflight.end());
            std::sort(validation.begin(), validation.end());
            std::cout << width << ',' << width << ',' << width * 2 << ',' << preflight[2] << ','
                      << validation[2] << ',' << before.WorkingSetSize << ',' << after.WorkingSetSize << ','
                      << after.PeakWorkingSetSize << ',' << before.PrivateUsage << ',' << after.PrivateUsage << '\n';
        }
        const auto refusalStart = Clock::now();
        const auto refused = Mapping::create({}, {0x1p53, 0x1p53}, 1,
                                               ValidationPolicy{initialBoundaryCheckBudget});
        const double refusalMs = milliseconds(refusalStart);
        if (refused || refused.error().code != ErrorCode::ResourceRefused)
            return 4;
        Cancellation cancellation;
        const auto start = Clock::now();
        const auto cancelled = Mapping::create({}, {1000000000, 1}, 1,
                                                 ValidationPolicy{1000000001},
                                                 Cancellation::check, &cancellation);
        const double cancelMs = milliseconds(start);
        const double responseMs = milliseconds(cancellation.requested);
        if (cancelled || cancelled.error().code != ErrorCode::Cancelled
            || cancelled.error().boundaryIndex != 4097)
            return 5;
        std::cout << "refused_checks=" << refused.error().requiredBoundaryChecks
                  << ",refusal_ms=" << refusalMs << ",cancel_total_ms=" << cancelMs
                  << ",cancel_response_ms=" << responseMs << ",cancel_calls=" << cancellation.calls
                  << ",sizeof_mapping=" << sizeof(Mapping) << '\n';
        return 0;
    } catch (DWORD error) {
        std::cerr << "GetProcessMemoryInfo error: " << error << '\n';
        return 6;
    }
}
