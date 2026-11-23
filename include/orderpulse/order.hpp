#pragma once

#include "orderpulse/types.hpp"

namespace orderpulse {

// A resting or in-flight order. Orders are never individually new/delete'd —
// they live in an OrderPool and are threaded into a PriceLevel's FIFO queue
// via the intrusive prev/next links below. See order_pool.hpp.
struct Order {
    OrderId   id    = 0;
    Price     price = 0;   // meaningless for Market orders
    Qty       qty   = 0;   // remaining (live) quantity
    Side      side  = Side::Bid;
    OrderType type  = OrderType::Limit;

    // Intrusive doubly-linked list within a single PriceLevel's FIFO queue.
    // nullptr on both ends when the order is not currently resting in a book.
    Order* prev = nullptr;
    Order* next = nullptr;
};

} // namespace orderpulse
