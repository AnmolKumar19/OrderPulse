#include "orderpulse/feed_simulator.hpp"

#include <algorithm>

namespace orderpulse {

namespace {
constexpr Qty kLotSizes[] = {1, 5, 10, 20, 50, 100};
}

FeedSimulator::FeedSimulator(const FeedSimConfig& config)
    : cfg_(config),
      rng_(config.seed),
      priceOffsetDist_(config.priceDecayP),
      qtyLotDist_({30, 25, 20, 15, 7, 3}), // weights toward smaller round lots
      mid_(config.startMid) {}

Qty FeedSimulator::sampleQty() {
    int idx = qtyLotDist_(rng_);
    return kLotSizes[idx];
}

Price FeedSimulator::samplePassiveOffset() {
    return static_cast<Price>(priceOffsetDist_(rng_));
}

void FeedSimulator::driftMid() {
    if (cfg_.midDriftMaxTicks <= 0) {
        return;
    }
    std::uniform_int_distribution<int> step(-cfg_.midDriftMaxTicks, cfg_.midDriftMaxTicks);
    mid_ += step(rng_);
    mid_ = std::max<Price>(mid_, 100); // keep prices positive over long runs
}

FeedEvent FeedSimulator::next() {
    driftMid();
    FeedEvent ev;

    if (!liveIds_.empty() && unit_(rng_) < cfg_.cancelProbability) {
        std::uniform_int_distribution<std::size_t> pick(0, liveIds_.size() - 1);
        std::size_t idx = pick(rng_);
        ev.type = FeedEventType::Cancel;
        ev.id = liveIds_[idx];
        liveIds_[idx] = liveIds_.back();
        liveIds_.pop_back();
        return ev;
    }

    if (!liveIds_.empty() && unit_(rng_) < cfg_.modifyProbability) {
        std::uniform_int_distribution<std::size_t> pick(0, liveIds_.size() - 1);
        std::size_t idx = pick(rng_);
        ev.type = FeedEventType::Modify;
        ev.id = liveIds_[idx];
        ev.qty = sampleQty();
        Price offset = samplePassiveOffset();
        ev.price = mid_ + (unit_(rng_) < 0.5 ? -offset : offset);
        return ev; // id stays in liveIds_: it's still live after a modify
    }

    // Fresh add.
    ev.type = FeedEventType::Add;
    ev.id = nextId_++;
    ev.side = (unit_(rng_) < 0.5) ? Side::Bid : Side::Ask;
    ev.qty = sampleQty();

    bool aggressive = unit_(rng_) < cfg_.aggressiveProbability;
    Price offset = samplePassiveOffset();
    if (ev.side == Side::Bid) {
        ev.price = aggressive ? (mid_ + offset) : (mid_ - offset);
    } else {
        ev.price = aggressive ? (mid_ - offset) : (mid_ + offset);
    }

    if (aggressive && unit_(rng_) < cfg_.marketOrderProbability) {
        ev.orderType = OrderType::Market;
        ev.price = 0;
    } else {
        ev.orderType = OrderType::Limit;
    }

    liveIds_.push_back(ev.id);
    return ev;
}

} // namespace orderpulse
