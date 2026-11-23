#pragma once

#include "orderpulse/order.hpp"
#include "orderpulse/types.hpp"

namespace orderpulse {

// All resting orders at a single price, in strict time priority: head is
// the oldest order (fills first), tail is the newest. Backed by an
// intrusive doubly-linked list threaded through Order::prev/next, so
// push/splice are O(1) with no allocation.
//
// Deliberately "dumb": PriceLevel only maintains the list shape and the
// order count. It does NOT own totalQty bookkeeping across a partial fill
// vs. a full removal — those two cases decrement totalQty by different
// amounts (the fill amount vs. the order's full remaining qty), and having
// two call sites reach into the same field with different intent is a
// classic double-decrement bug waiting to happen. So totalQty is adjusted
// explicitly by the OrderBook at the one place each qty change happens,
// and pushTail is the only place PriceLevel itself touches it (unambiguous:
// a fresh order always adds its whole qty).
//
// No `price` field: PriceLevel lives at a known index in PriceLadder, which
// derives the price from that index (see price_ladder.hpp) — storing it
// again here would be redundant and one more place for it to go stale.
struct PriceLevel {
    Qty           totalQty   = 0;
    std::uint32_t orderCount = 0;
    Order*        head       = nullptr;
    Order*        tail       = nullptr;

    bool empty() const noexcept { return orderCount == 0; }

    void pushTail(Order* o) noexcept {
        o->prev = tail;
        o->next = nullptr;
        if (tail) {
            tail->next = o;
        } else {
            head = o;
        }
        tail = o;
        totalQty += o->qty;
        ++orderCount;
    }

    // Splices `o` out of the list. Does NOT touch totalQty — the caller is
    // responsible for that (see class comment above).
    void spliceOut(Order* o) noexcept {
        if (o->prev) {
            o->prev->next = o->next;
        } else {
            head = o->next;
        }
        if (o->next) {
            o->next->prev = o->prev;
        } else {
            tail = o->prev;
        }
        o->prev = o->next = nullptr;
        --orderCount;
    }
};

} // namespace orderpulse
