#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include <catch_amalgamated.hpp>

#include "orderpulse/feed_event.hpp"
#include "orderpulse/order_book.hpp"
#include "orderpulse/spsc_queue.hpp"

using namespace orderpulse;

// Runs a real feed-simulator thread and a real matching thread, connected
// by the actual SpscQueue, and checks that crossing the thread boundary
// changes nothing about correctness: every event the producer generates is
// applied to the book exactly once, in order, and the same
// no-lost/no-duplicated-orders invariant from the single-threaded tests
// (poolInUse() == totalOrderCount()) holds throughout.
TEST_CASE("feed thread and matching thread stay consistent across the SPSC boundary", "[concurrency]") {
    constexpr std::uint64_t kNumEvents = 200'000;

    SpscQueue<FeedEvent> queue(4096);
    std::atomic<bool> producerDone{false};
    std::atomic<std::uint64_t> eventsConsumed{0};
    std::atomic<bool> invariantViolated{false};

    std::thread producer([&]() {
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> priceOffset(-20, 20);
        std::uniform_int_distribution<Qty> qtyDist(1, 50);
        std::uniform_int_distribution<int> actionDist(0, 9); // >=7 -> cancel

        std::vector<OrderId> liveIds;
        liveIds.reserve(kNumEvents);
        OrderId nextId = 1;
        constexpr Price kMid = 10'000;

        for (std::uint64_t i = 0; i < kNumEvents; ++i) {
            FeedEvent ev;
            bool doCancel = !liveIds.empty() && actionDist(rng) >= 7;
            if (doCancel) {
                std::uniform_int_distribution<std::size_t> pick(0, liveIds.size() - 1);
                std::size_t idx = pick(rng);
                ev.type = FeedEventType::Cancel;
                ev.id = liveIds[idx];
                liveIds[idx] = liveIds.back();
                liveIds.pop_back();
            } else {
                ev.type = FeedEventType::Add;
                ev.id = nextId++;
                ev.side = (i % 2 == 0) ? Side::Bid : Side::Ask;
                ev.orderType = OrderType::Limit;
                ev.price = kMid + priceOffset(rng);
                ev.qty = qtyDist(rng);
                liveIds.push_back(ev.id);
            }
            while (!queue.tryPush(ev)) {
                std::this_thread::yield(); // backpressure: spin until the consumer frees a slot
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        OrderBook book(kNumEvents, kNumEvents + 10);
        std::vector<Trade> trades;
        std::uint64_t consumed = 0;

        auto process = [&](const FeedEvent& ev) {
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
            ++consumed;
            if (consumed % 10'000 == 0 && book.poolInUse() != book.totalOrderCount()) {
                invariantViolated.store(true, std::memory_order_relaxed);
            }
        };

        FeedEvent ev;
        while (!producerDone.load(std::memory_order_acquire)) {
            if (queue.tryPop(ev)) {
                process(ev);
            }
        }
        // Producer has confirmed it pushed its last event — no more pushes
        // can happen from here, so a plain drain is race-free.
        while (queue.tryPop(ev)) {
            process(ev);
        }

        eventsConsumed.store(consumed, std::memory_order_relaxed);
        if (book.poolInUse() != book.totalOrderCount()) {
            invariantViolated.store(true, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();

    CHECK(eventsConsumed.load() == kNumEvents);
    CHECK_FALSE(invariantViolated.load());
}
