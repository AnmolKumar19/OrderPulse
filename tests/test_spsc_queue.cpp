#include <catch_amalgamated.hpp>

#include "orderpulse/spsc_queue.hpp"

using orderpulse::SpscQueue;

TEST_CASE("SpscQueue preserves FIFO order", "[spsc]") {
    SpscQueue<int> q(8);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(q.tryPush(i));
    }
    for (int i = 0; i < 5; ++i) {
        int out = -1;
        REQUIRE(q.tryPop(out));
        CHECK(out == i);
    }
}

TEST_CASE("SpscQueue rounds capacity up to a power of two", "[spsc]") {
    SpscQueue<int> q(10);
    CHECK(q.capacity() == 16);
}

TEST_CASE("SpscQueue reports full and empty correctly", "[spsc]") {
    SpscQueue<int> q(4); // capacity 4 (already a power of two)

    int out = -1;
    CHECK_FALSE(q.tryPop(out)); // empty at start

    for (int i = 0; i < 4; ++i) {
        REQUIRE(q.tryPush(i));
    }
    CHECK_FALSE(q.tryPush(99)); // full

    REQUIRE(q.tryPop(out));
    CHECK(out == 0);
    REQUIRE(q.tryPush(99)); // one slot freed up
}

TEST_CASE("SpscQueue survives many wraparounds with no loss or reordering", "[spsc]") {
    // Push/pop one at a time through a small ring many times over —
    // exercises the index-wraparound (mask) logic far past capacity, which
    // is exactly where an off-by-one in head/tail arithmetic would show up.
    SpscQueue<int> q(4);
    constexpr int kIterations = 100'000;

    for (int i = 0; i < kIterations; ++i) {
        REQUIRE(q.tryPush(i));
        int out = -1;
        REQUIRE(q.tryPop(out));
        CHECK(out == i);
    }
}

TEST_CASE("SpscQueue: interleaved batch push/pop preserves order and count", "[spsc]") {
    SpscQueue<int> q(4);
    int next = 0;
    int produced = 0;
    int consumed = 0;

    while (produced < 50'000) {
        // Push until full or a batch of 3, whichever first.
        for (int i = 0; i < 3 && q.tryPush(produced); ++i) {
            ++produced;
        }
        // Pop until empty.
        int out;
        while (q.tryPop(out)) {
            CHECK(out == next);
            ++next;
            ++consumed;
        }
    }
    int out;
    while (q.tryPop(out)) {
        CHECK(out == next);
        ++next;
        ++consumed;
    }
    CHECK(consumed == produced);
}
