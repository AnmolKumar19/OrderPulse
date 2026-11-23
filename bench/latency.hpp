#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define ORDERPULSE_HAVE_RDTSCP 1
#endif

namespace orderpulse::bench {

// RDTSCP rather than plain RDTSC: RDTSCP waits for all prior instructions
// to retire before it samples the counter (a partial serializing barrier on
// entry), which keeps the CPU's out-of-order execution from smearing
// unrelated work across the boundary of what's being timed. The fully
// serializing alternative (CPUID immediately before RDTSC) was rejected --
// CPUID costs on the order of 20-40 cycles, which is a large fraction of a
// matching operation that itself may only take a few hundred cycles, and
// would distort exactly the tail latencies this benchmark exists to
// measure.
inline std::uint64_t rdtscpNow() noexcept {
#if defined(ORDERPULSE_HAVE_RDTSCP)
    unsigned aux;
    return __rdtscp(&aux);
#else
    return 0;
#endif
}

// Measures TSC cycles per nanosecond by comparing a TSC delta against a
// std::chrono::steady_clock delta over a short wall-clock interval. Do this
// once at startup, not per-sample: converting every recorded sample would
// put a floating-point division on the hot path we're trying to measure.
struct TscCalibration {
    double cyclesPerNs = 1.0;

    static TscCalibration measure(int warmupMs = 100);

    double cyclesToNs(std::uint64_t cycles) const noexcept {
        return static_cast<double>(cycles) / cyclesPerNs;
    }
};

struct LatencyStats {
    std::size_t count = 0;
    double meanNs = 0;
    double p50Ns = 0;
    double p90Ns = 0;
    double p99Ns = 0;
    double p999Ns = 0;
    double p9999Ns = 0;
    double maxNs = 0;
};

// Records raw TSC-cycle deltas (no conversion, no allocation) during the
// timed run; compute() sorts and converts to nanoseconds once, afterward.
class LatencyHistogram {
public:
    explicit LatencyHistogram(std::size_t expectedSamples) { samples_.reserve(expectedSamples); }

    void record(std::uint64_t cyclesDelta) noexcept { samples_.push_back(cyclesDelta); }

    std::size_t size() const noexcept { return samples_.size(); }

    // Sorts samples_ in place (destructive) and returns percentile stats in
    // nanoseconds using the given calibration.
    LatencyStats compute(const TscCalibration& cal);

private:
    std::vector<std::uint64_t> samples_;
};

} // namespace orderpulse::bench
