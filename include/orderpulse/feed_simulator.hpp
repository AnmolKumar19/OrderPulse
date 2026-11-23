#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "orderpulse/feed_event.hpp"
#include "orderpulse/types.hpp"

namespace orderpulse {

// Tunable parameters for the synthetic order flow. Defaults are chosen to
// match commonly-cited stylized facts about real limit order book activity
// (see README.md "Feed simulator"), not picked to make the benchmark look
// good:
//   - resting liquidity clusters within a few ticks of the touch (geometric
//     decay in distance from mid, not uniform)
//   - cancels dominate raw event count (real LOB churn is cancel-heavy —
//     most orders never trade, they get pulled and replaced)
//   - most adds are passive (would rest, not cross); a minority are
//     aggressive and actually trade
struct FeedSimConfig {
    Price startMid = 10'000;

    // Fraction of events that are a Cancel of a currently-live order
    // (falls back to Add if nothing is live yet).
    double cancelProbability = 0.40;

    // Of the non-cancel events, the fraction that are a Modify of a
    // currently-live order instead of a fresh Add.
    double modifyProbability = 0.10;

    // Of fresh Adds, the fraction deliberately priced to cross the spread
    // (aggressive / marketable) rather than rest passively.
    double aggressiveProbability = 0.12;

    // Of aggressive Adds, the fraction submitted as an actual Market order
    // rather than a marketable Limit.
    double marketOrderProbability = 0.15;

    // Geometric-distribution parameter for |price - mid| in ticks for
    // passive orders: higher p = tighter clustering around the touch.
    double priceDecayP = 0.35;

    // Per-event random-walk step bound applied to the simulator's own
    // notion of mid, so the touch drifts over the course of a run instead
    // of sitting static at startMid.
    int midDriftMaxTicks = 1;

    std::uint64_t seed = 42;
};

// Generates a stateful stream of FeedEvent. Deliberately does NOT observe
// the OrderBook it's driving — a real feed handler has no visibility into
// how the matching engine resolved earlier events either. It only tracks
// which ids IT believes are still live, to pick cancel/modify targets.
// Consequence (by design, not a bug): a Cancel or Modify can legitimately
// race against an order that already fully filled and is harmlessly
// rejected by OrderBook — see README.md.
class FeedSimulator {
public:
    explicit FeedSimulator(const FeedSimConfig& config = FeedSimConfig{});

    // Produces the next event and advances internal state.
    FeedEvent next();

    std::uint64_t liveOrderCount() const noexcept { return liveIds_.size(); }

private:
    Qty sampleQty();
    Price samplePassiveOffset();
    void driftMid();

    FeedSimConfig cfg_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> unit_{0.0, 1.0};
    std::geometric_distribution<int> priceOffsetDist_;
    std::discrete_distribution<int> qtyLotDist_;

    Price mid_;
    OrderId nextId_ = 1;
    std::vector<OrderId> liveIds_;
};

} // namespace orderpulse
