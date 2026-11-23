#pragma once

#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#define ORDERPULSE_HAVE_LINUX_AFFINITY 1
#endif

namespace orderpulse::bench {

// Pins `t` to a single CPU core. Reduces scheduler-induced jitter (cross-
// core migration, cache-cold rescheduling) in the tail latencies -- a
// migrated thread pays for a cold cache on its new core, which shows up as
// a p99.9 spike that has nothing to do with the matching engine itself.
// No-op (returns false) off Linux or if the platform call fails; the
// caller decides whether that's fatal.
inline bool pinThreadToCore(std::thread& t, int core) {
#if defined(ORDERPULSE_HAVE_LINUX_AFFINITY)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    return pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset) == 0;
#else
    (void)t;
    (void)core;
    return false;
#endif
}

} // namespace orderpulse::bench
