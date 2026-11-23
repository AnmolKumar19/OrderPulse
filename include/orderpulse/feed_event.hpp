#pragma once

#include "orderpulse/types.hpp"

namespace orderpulse {

enum class FeedEventType : std::uint8_t { Add, Cancel, Modify };

// One market-data event as it crosses the feed -> matching thread boundary.
// Deliberately a flat, trivially-copyable struct: it moves through the
// SpscQueue by value, so there is no pointer, no ownership, and no
// allocation involved in handing an event from the feed thread to the
// matching thread.
struct FeedEvent {
    FeedEventType type      = FeedEventType::Add;
    OrderId       id        = 0;
    Side          side      = Side::Bid;     // Add only
    OrderType     orderType = OrderType::Limit; // Add only
    Price         price     = 0;             // Add: order price. Modify: new price.
    Qty           qty       = 0;             // Add: order qty. Modify: new qty.

    // Populated by the benchmark harness only (a raw TSC reading taken just
    // before the producer pushes this event) so the consumer can report
    // end-to-end feed-to-processed latency alongside pure processing
    // latency. Unused (0) by the matching engine and by the correctness/
    // concurrency tests -- it is not part of matching semantics.
    Ts enqueueTsc = 0;
};

} // namespace orderpulse
