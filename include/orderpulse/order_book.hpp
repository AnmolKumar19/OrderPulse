#pragma once

#include <cstddef>
#include <vector>

#include "orderpulse/order.hpp"
#include "orderpulse/order_pool.hpp"
#include "orderpulse/price_ladder.hpp"
#include "orderpulse/price_level.hpp"
#include "orderpulse/trade.hpp"
#include "orderpulse/types.hpp"

namespace orderpulse {

// A read-only snapshot of one resting order, for introspection/UI use
// (e.g. the web demo's order list). Not used anywhere on the matching
// hot path.
struct OrderInfo {
    OrderId id;
    Side side;
    Price price;
    Qty qty; // remaining
};

// Single-instrument limit order book with price-time priority matching.
//
// Book representation: bids and asks are each a PriceLadder — a flat,
// pre-allocated array of PriceLevel indexed directly by price, O(1) per
// touch. This replaced an earlier std::map<Price, PriceLevel> baseline
// after profiling a real benchmark run showed a disproportionate share of
// time inside cancelOrder, almost entirely "self" time from the inlined
// RB-tree lower_bound/erase logic (every price-level create/destroy was a
// heap-allocated tree node). See README.md ("Profiling & optimization")
// for the before/after numbers. PriceLadder requires a bounded price
// range, configured at construction (defaults cover a wide realistic
// range) — a Limit order priced outside that range is rejected up front,
// the same failure path as pool exhaustion.
//
// Threading: an OrderBook instance is single-threaded — exactly one thread
// (the matching thread) may call into it. See README.md ("Concurrency
// design") for how the feed thread hands events across via SpscQueue.
class OrderBook {
public:
    // orderPoolCapacity: max resting+in-flight orders at once.
    // maxOrderId: sizes the id->Order* index up front (it still grows on
    // demand, geometrically, if exceeded).
    // priceBandMin/Max: the inclusive price range PriceLadder pre-allocates
    // for each side. Defaults comfortably cover this project's tests and
    // benchmark; a real deployment would size this to the instrument's
    // actual realistic tick range.
    OrderBook(std::size_t orderPoolCapacity, OrderId maxOrderId = 0,
              Price priceBandMin = 1, Price priceBandMax = 100'000);

    // Submits a new order. For a marketable order (Market, or a Limit that
    // crosses the book) this matches immediately against resting orders,
    // appending one Trade per fill to outTrades (never cleared by this
    // call — caller controls that, so it can reuse one buffer across many
    // calls with zero reallocation). Any unfilled remainder of a Limit
    // order rests on the book; a Market order's unfilled remainder is
    // dropped (no resting market orders, matching real exchange behavior).
    //
    // Returns false (and does nothing) if `id` is already active in this
    // book, if a Limit order's price falls outside [priceBandMin,
    // priceBandMax], or if the order pool is exhausted.
    bool addOrder(OrderId id, Side side, OrderType type, Price price, Qty qty,
                  std::vector<Trade>& outTrades);

    // Removes a resting order entirely. Returns false if `id` is not
    // currently resting (unknown, already filled, or already canceled).
    bool cancelOrder(OrderId id);

    // Changes a resting order's price and/or quantity.
    //   - Quantity decrease at the same price keeps the order's queue
    //     position (in place, no requeue) — matches real exchange semantics.
    //   - Any price change, or a quantity increase, loses time priority:
    //     implemented as cancel + re-add at the new price/qty, which also
    //     means it can immediately match if the new price is marketable
    //     (trades appended to outTrades, same as addOrder).
    // Returns false if `id` is not currently resting.
    bool modifyOrder(OrderId id, Price newPrice, Qty newQty, std::vector<Trade>& outTrades);

    bool bestBid(Price& outPrice, Qty& outQty) const;
    bool bestAsk(Price& outPrice, Qty& outQty) const;

    // 0 if there is no resting quantity at that exact price.
    Qty qtyAtPrice(Side side, Price price) const;
    std::uint32_t orderCountAtPrice(Side side, Price price) const;

    // Sum of live (resting) quantity across the whole book. Together with
    // pool bookkeeping this is what the conservation tests check: nothing
    // is created or destroyed except by an explicit fill or cancel.
    Qty totalRestingQty() const;

    // Number of resting orders across the whole book. Together with
    // poolInUse(), this is the "no orders lost or duplicated" check: every
    // pool-acquired Order is either resting (counted here) or has been
    // released — the two must always be equal.
    std::uint32_t totalOrderCount() const;

    std::size_t poolInUse() const noexcept { return pool_.inUse(); }
    std::size_t poolCapacity() const noexcept { return pool_.capacity(); }

    // Every resting order across both sides. No particular ordering is
    // guaranteed across price levels (a caller wanting the book sorted by
    // price should group/sort the result itself) — within a single price
    // level, orders come out oldest-first. O(total live orders): a full
    // walk of the intrusive lists, not something the matching hot path
    // ever calls; exists for introspection/UI use (see web/server/).
    std::vector<OrderInfo> liveOrders() const;

private:
    void matchAgainst(PriceLadder& opposite, Order* incoming, std::vector<Trade>& outTrades);
    void rest(Order* incoming);
    void ensureIdIndexCapacity(OrderId id);

    PriceLadder& ladderFor(Side side) noexcept { return side == Side::Bid ? bids_ : asks_; }
    const PriceLadder& ladderFor(Side side) const noexcept { return side == Side::Bid ? bids_ : asks_; }

    OrderPool pool_;
    std::vector<Order*> idIndex_; // dense OrderId -> Order*, nullptr if inactive
    PriceLadder bids_;            // betterDirection = +1 (higher price = better)
    PriceLadder asks_;            // betterDirection = -1 (lower price = better)
};

} // namespace orderpulse
