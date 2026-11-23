#pragma once

#include <cstddef>
#include <vector>

#include "orderpulse/price_level.hpp"
#include "orderpulse/types.hpp"

namespace orderpulse {

// One side (bids or asks) of the book: a flat, pre-allocated array of
// PriceLevel, indexed directly by `price - minPrice`. This is the
// optimization pass's replacement for the std::map<Price, PriceLevel>
// baseline (see order_book.hpp's history / README.md "Profiling &
// optimization") — profiling the map-based version showed a
// disproportionate share of time inside OrderBook::cancelOrder, almost
// entirely attributed as "self" time from the inlined RB-tree lower_bound/
// erase logic: every price-level creation is a heap-allocated tree node,
// every level teardown is a node deallocation, and every lookup is O(log L)
// pointer chasing. None of that exists here: a level "exists" simply by
// being a slot in the array (always has been, always will be), so create/
// destroy is free, and lookup by price is one array index.
//
// The trade-off, stated plainly: this pre-allocates and zeroes
// `maxPrice - minPrice + 1` PriceLevel slots up front, whether or not they
// ever get used, versus the tree's pay-only-for-what-you-touch allocation.
// That's the right trade for an instrument whose tick range is known and
// bounded (true of essentially every real, liquid instrument's practical
// trading range) -- not a general replacement for a truly unbounded key
// space.
//
// "Best price" (bestBid/bestAsk) is tracked as a cursor rather than
// scanned for on every call:
//   - Inserting into a level can only ever make it a candidate for best,
//     never worse than the current best by definition of "insert" -- so
//     noteActive() is an O(1) comparison against the cached cursor.
//   - Emptying a level only requires work if it WAS the best: then the
//     cursor walks outward (toward worse prices) until it finds the next
//     active level. Under realistic order flow (liquidity clustered near
//     the touch), this walk is short -- typically a handful of ticks, not
//     a scan of the whole array -- so it's amortized O(1) in practice, even
//     though a pathological input could in principle force a longer walk.
class PriceLadder {
public:
    // betterDirection: +1 if a higher index is a better price (bids: higher
    // price is better), -1 if a lower index is better (asks: lower price
    // is better).
    PriceLadder(Price minPrice, Price maxPrice, int betterDirection);

    bool priceInRange(Price p) const noexcept { return p >= minPrice_ && p <= maxPrice_; }
    std::size_t indexOf(Price p) const noexcept { return static_cast<std::size_t>(p - minPrice_); }
    Price priceAt(std::size_t idx) const noexcept { return minPrice_ + static_cast<Price>(idx); }

    bool empty() const noexcept { return bestIdx_ == kNone; }
    std::size_t bestIndex() const noexcept { return bestIdx_; }
    Price bestPrice() const noexcept { return priceAt(bestIdx_); }

    PriceLevel& at(std::size_t idx) noexcept { return levels_[idx]; }
    const PriceLevel& at(std::size_t idx) const noexcept { return levels_[idx]; }

    // True if an incoming order priced `incomingPrice` is marketable
    // against a resting level priced `levelPrice` on this ladder's side.
    // Derived purely from betterDirection_, so the one-line test is
    // correct for both bids and asks without the caller needing to know
    // which side it's calling into (mirrors the std::map-comparator trick
    // the earlier baseline used, kept because it's still the cleanest way
    // to write one matching loop for both sides).
    bool crosses(Price incomingPrice, Price levelPrice) const noexcept {
        return betterDirection_ * (incomingPrice - levelPrice) <= 0;
    }

    // Call after adding an order to the level at `idx` (whether that level
    // was already active or was empty before). O(1).
    void noteActive(std::size_t idx) noexcept;

    // Call after the level at `idx` has become empty. O(1) unless `idx` was
    // the current best, in which case it walks to the next active level.
    void noteEmptied(std::size_t idx) noexcept;

    std::size_t size() const noexcept { return levels_.size(); }

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    Price minPrice_;
    Price maxPrice_;
    int betterDirection_;
    std::vector<PriceLevel> levels_;
    std::size_t bestIdx_ = kNone;
};

} // namespace orderpulse
