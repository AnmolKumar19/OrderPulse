// OrderPulse latency benchmark: a real feed-simulator thread and a real
// matching thread, connected by the actual SpscQueue, generating actual
// TSC-cycle measurements of per-event matching-engine latency. Nothing
// here is estimated or hardcoded -- every number in the report below is
// measured on whatever machine this binary runs on.
//
// Usage: orderpulse_bench [num_events] [warmup_events]

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "orderpulse/feed_event.hpp"
#include "orderpulse/feed_simulator.hpp"
#include "orderpulse/order_book.hpp"
#include "orderpulse/spsc_queue.hpp"

#include "latency.hpp"
#include "thread_affinity.hpp"

using namespace orderpulse;
using namespace orderpulse::bench;

namespace {

bool hostHasInvariantTsc() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("flags", 0) == 0 && line.find("constant_tsc") != std::string::npos) {
            return true;
        }
    }
    return false; // couldn't confirm -- not necessarily absent, just unverified
}

void printStats(const char* label, const LatencyStats& s) {
    std::printf("  %-28s p50=%8.1f  p90=%8.1f  p99=%8.1f  p99.9=%9.1f  p99.99=%9.1f  max=%10.1f  mean=%8.1f  (ns, n=%zu)\n",
                label, s.p50Ns, s.p90Ns, s.p99Ns, s.p999Ns, s.p9999Ns, s.maxNs, s.meanNs, s.count);
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t numEvents = 2'000'000;
    std::uint64_t warmupEvents = 20'000;
    if (argc > 1) numEvents = std::stoull(argv[1]);
    if (argc > 2) warmupEvents = std::stoull(argv[2]);

    std::printf("OrderPulse latency benchmark\n");
    std::printf("  events: %llu (warmup: %llu excluded from stats)\n",
                (unsigned long long)numEvents, (unsigned long long)warmupEvents);
    std::printf("  invariant TSC (constant_tsc): %s\n", hostHasInvariantTsc() ? "yes" : "unverified");

    TscCalibration cal = TscCalibration::measure();
    std::printf("  TSC calibration: %.4f cycles/ns\n", cal.cyclesPerNs);

    const unsigned hwThreads = std::thread::hardware_concurrency();
    std::printf("  hardware_concurrency: %u\n\n", hwThreads);

    SpscQueue<FeedEvent> queue(1 << 16);
    std::atomic<bool> producerDone{false};

    LatencyHistogram processingHist(numEvents);
    LatencyHistogram endToEndHist(numEvents);
    // Per-event-type breakdown: cancels and adds exercise very different
    // code paths (cancel is a pure unlink; add may walk/match several
    // levels), and profiling showed cancel's cost was disproportionately
    // tied to the price-level container -- this isolates that claim with
    // the same low-overhead rdtscp technique used above, rather than
    // relying on a statistical sampling profiler's run-to-run noise.
    LatencyHistogram addHist(numEvents);
    LatencyHistogram cancelHist(numEvents);
    LatencyHistogram modifyHist(numEvents);

    std::uint64_t totalTrades = 0;
    std::uint64_t poolCapacityUsed = 0;
    bool invariantOk = true;

    std::thread producer([&]() {
        FeedSimConfig cfg;
        cfg.seed = 12345;
        FeedSimulator sim(cfg);
        for (std::uint64_t i = 0; i < numEvents; ++i) {
            FeedEvent ev = sim.next();
            ev.enqueueTsc = rdtscpNow();
            while (!queue.tryPush(ev)) {
                std::this_thread::yield();
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        OrderBook book(numEvents, numEvents + 10);
        std::vector<Trade> trades;
        std::uint64_t processed = 0;
        std::uint64_t tradesGenerated = 0;

        auto handle = [&](const FeedEvent& ev) {
            const std::uint64_t start = rdtscpNow();
            switch (ev.type) {
                case FeedEventType::Add:
                    book.addOrder(ev.id, ev.side, ev.orderType, ev.price, ev.qty, trades);
                    break;
                case FeedEventType::Cancel:
                    book.cancelOrder(ev.id);
                    break;
                case FeedEventType::Modify:
                    book.modifyOrder(ev.id, ev.price, ev.qty, trades);
                    break;
            }
            const std::uint64_t end = rdtscpNow();
            // Reused across the whole run to avoid allocating per event, but
            // must be cleared here (outside the timed window -- this is
            // harness bookkeeping, not matching-engine work) or it grows
            // unbounded for 2M events and its rare reallocations land as
            // multi-microsecond spikes inside whichever event's timing
            // window happens to trigger them, polluting the tail with
            // noise that has nothing to do with the engine.
            tradesGenerated += trades.size();
            trades.clear();

            ++processed;
            if (processed > warmupEvents) {
                processingHist.record(end - start);
                endToEndHist.record(end - ev.enqueueTsc);
                switch (ev.type) {
                    case FeedEventType::Add: addHist.record(end - start); break;
                    case FeedEventType::Cancel: cancelHist.record(end - start); break;
                    case FeedEventType::Modify: modifyHist.record(end - start); break;
                }
            }
            if (processed % 100'000 == 0 && book.poolInUse() != book.totalOrderCount()) {
                invariantOk = false;
            }
        };

        FeedEvent ev;
        while (!producerDone.load(std::memory_order_acquire)) {
            if (queue.tryPop(ev)) handle(ev);
        }
        while (queue.tryPop(ev)) handle(ev);

        totalTrades = tradesGenerated;
        poolCapacityUsed = book.poolInUse();
        if (book.poolInUse() != book.totalOrderCount()) invariantOk = false;
    });

    if (hwThreads >= 2) {
        pinThreadToCore(producer, 0);
        pinThreadToCore(consumer, 1);
    }

    producer.join();
    consumer.join();

    std::printf("Results:\n");
    printStats("processing latency", processingHist.compute(cal));
    printStats("  - Add", addHist.compute(cal));
    printStats("  - Cancel", cancelHist.compute(cal));
    printStats("  - Modify", modifyHist.compute(cal));
    printStats("end-to-end (feed->processed)", endToEndHist.compute(cal));
    std::printf("  note: the producer is unthrottled (pushes as fast as it can generate\n"
                "  events) to stress-test sustained peak throughput, so this run saturates\n"
                "  the queue -- end-to-end latency is dominated by queue backlog\n"
                "  (~queue_capacity x processing time), not per-event handoff cost. See\n"
                "  README.md \"Benchmark methodology\" for the arithmetic. \"processing\n"
                "  latency\" above is the matching-engine-isolated number.\n");

    std::printf("\nBook sanity: trades=%llu  resting-orders=%llu  invariant=%s\n",
                (unsigned long long)totalTrades, (unsigned long long)poolCapacityUsed,
                invariantOk ? "OK" : "VIOLATED");

    return invariantOk ? 0 : 1;
}
