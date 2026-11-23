#include "orderpulse/price_ladder.hpp"

namespace orderpulse {

PriceLadder::PriceLadder(Price minPrice, Price maxPrice, int betterDirection)
    : minPrice_(minPrice),
      maxPrice_(maxPrice),
      betterDirection_(betterDirection),
      levels_(static_cast<std::size_t>(maxPrice - minPrice) + 1) {}

void PriceLadder::noteActive(std::size_t idx) noexcept {
    if (bestIdx_ == kNone) {
        bestIdx_ = idx;
        return;
    }
    if (betterDirection_ > 0) {
        if (idx > bestIdx_) {
            bestIdx_ = idx;
        }
    } else {
        if (idx < bestIdx_) {
            bestIdx_ = idx;
        }
    }
}

void PriceLadder::noteEmptied(std::size_t idx) noexcept {
    if (idx != bestIdx_) {
        return; // wasn't the best level -- the cursor is still correct
    }

    if (betterDirection_ > 0) {
        // Bids: better = higher index. The new best (if any) must be at a
        // lower index than the one that just emptied, so walk down.
        std::size_t i = idx;
        while (i != 0) {
            --i;
            if (!levels_[i].empty()) {
                bestIdx_ = i;
                return;
            }
        }
    } else {
        // Asks: better = lower index. Walk up.
        std::size_t i = idx;
        std::size_t n = levels_.size();
        while (i + 1 < n) {
            ++i;
            if (!levels_[i].empty()) {
                bestIdx_ = i;
                return;
            }
        }
    }
    bestIdx_ = kNone; // nothing left active on this side
}

} // namespace orderpulse
