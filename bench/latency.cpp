#include "latency.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace orderpulse::bench {

TscCalibration TscCalibration::measure(int warmupMs) {
    const auto wallStart = std::chrono::steady_clock::now();
    const std::uint64_t tscStart = rdtscpNow();

    std::this_thread::sleep_for(std::chrono::milliseconds(warmupMs));

    const std::uint64_t tscEnd = rdtscpNow();
    const auto wallEnd = std::chrono::steady_clock::now();

    const double elapsedNs = std::chrono::duration<double, std::nano>(wallEnd - wallStart).count();
    const double elapsedCycles = static_cast<double>(tscEnd - tscStart);

    TscCalibration cal;
    cal.cyclesPerNs = elapsedCycles / elapsedNs;
    return cal;
}

namespace {
double percentileOf(const std::vector<std::uint64_t>& sorted, double p, const TscCalibration& cal) {
    if (sorted.empty()) {
        return 0.0;
    }
    // p in (0, 100]; index = ceil(p/100 * N) - 1, clamped into range.
    std::size_t n = sorted.size();
    std::size_t idx = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(n))) - 1;
    idx = std::min(idx, n - 1);
    return cal.cyclesToNs(sorted[idx]);
}
} // namespace

LatencyStats LatencyHistogram::compute(const TscCalibration& cal) {
    LatencyStats stats;
    stats.count = samples_.size();
    if (samples_.empty()) {
        return stats;
    }

    std::sort(samples_.begin(), samples_.end());

    std::uint64_t sum = 0;
    for (auto c : samples_) {
        sum += c;
    }
    stats.meanNs = cal.cyclesToNs(sum / samples_.size());

    stats.p50Ns = percentileOf(samples_, 50.0, cal);
    stats.p90Ns = percentileOf(samples_, 90.0, cal);
    stats.p99Ns = percentileOf(samples_, 99.0, cal);
    stats.p999Ns = percentileOf(samples_, 99.9, cal);
    stats.p9999Ns = percentileOf(samples_, 99.99, cal);
    stats.maxNs = cal.cyclesToNs(samples_.back());

    return stats;
}

} // namespace orderpulse::bench
