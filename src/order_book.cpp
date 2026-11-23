#include "orderpulse/order_book.hpp"

#include <algorithm>

namespace orderpulse {

OrderBook::OrderBook(std::size_t orderPoolCapacity, OrderId maxOrderId, Price priceBandMin, Price priceBandMax)
    : pool_(orderPoolCapacity),
      bids_(priceBandMin, priceBandMax, /*betterDirection=*/+1),
      asks_(priceBandMin, priceBandMax, /*betterDirection=*/-1) {
    if (maxOrderId > 0) {
        idIndex_.resize(static_cast<std::size_t>(maxOrderId) + 1, nullptr);
    }
}

void OrderBook::ensureIdIndexCapacity(OrderId id) {
    if (static_cast<std::size_t>(id) < idIndex_.size()) {
        return;
    }
    std::size_t newSize = idIndex_.empty() ? 1 : idIndex_.size();
    while (newSize <= static_cast<std::size_t>(id)) {
        newSize *= 2; // geometric growth: amortized O(1) even for sequential ids
    }
    idIndex_.resize(newSize, nullptr);
}

void OrderBook::matchAgainst(PriceLadder& opposite, Order* incoming, std::vector<Trade>& outTrades) {
    while (incoming->qty > 0 && !opposite.empty()) {
        std::size_t idx = opposite.bestIndex();
        Price levelPrice = opposite.priceAt(idx);

        if (incoming->type == OrderType::Limit && !opposite.crosses(incoming->price, levelPrice)) {
            break; // best remaining level is no longer marketable
        }

        PriceLevel& level = opposite.at(idx);
        while (incoming->qty > 0 && level.head != nullptr) {
            Order* resting = level.head;
            Qty fill = std::min(incoming->qty, resting->qty);

            outTrades.push_back(Trade{incoming->id, resting->id, levelPrice, fill});

            incoming->qty -= fill;
            resting->qty -= fill;
            level.totalQty -= fill;

            if (resting->qty == 0) {
                level.spliceOut(resting);
                idIndex_[resting->id] = nullptr;
                pool_.release(resting);
            }
        }

        if (level.empty()) {
            opposite.noteEmptied(idx);
        }
    }
}

void OrderBook::rest(Order* incoming) {
    ensureIdIndexCapacity(incoming->id);
    PriceLadder& ladder = ladderFor(incoming->side);
    std::size_t idx = ladder.indexOf(incoming->price);
    ladder.at(idx).pushTail(incoming);
    ladder.noteActive(idx);
    idIndex_[incoming->id] = incoming;
}

bool OrderBook::addOrder(OrderId id, Side side, OrderType type, Price price, Qty qty,
                          std::vector<Trade>& outTrades) {
    if (static_cast<std::size_t>(id) < idIndex_.size() && idIndex_[id] != nullptr) {
        return false; // duplicate id already active
    }
    if (type == OrderType::Limit && !ladderFor(side).priceInRange(price)) {
        return false; // outside the configured price band
    }

    Order* o = pool_.acquire();
    if (o == nullptr) {
        return false; // pool exhausted
    }

    o->id = id;
    o->side = side;
    o->type = type;
    o->price = price;
    o->qty = qty;

    matchAgainst(ladderFor(opposite(side)), o, outTrades);

    if (o->qty > 0 && type == OrderType::Limit) {
        rest(o);
    } else {
        // Fully filled, or an unfilled Market order's remainder — Market
        // orders never rest, matching standard exchange behavior.
        pool_.release(o);
    }
    return true;
}

bool OrderBook::cancelOrder(OrderId id) {
    if (static_cast<std::size_t>(id) >= idIndex_.size()) {
        return false;
    }
    Order* o = idIndex_[id];
    if (o == nullptr) {
        return false;
    }

    PriceLadder& ladder = ladderFor(o->side);
    std::size_t idx = ladder.indexOf(o->price);
    PriceLevel& level = ladder.at(idx);

    level.totalQty -= o->qty;
    level.spliceOut(o);
    idIndex_[id] = nullptr;
    pool_.release(o);

    if (level.empty()) {
        ladder.noteEmptied(idx);
    }
    return true;
}

bool OrderBook::modifyOrder(OrderId id, Price newPrice, Qty newQty, std::vector<Trade>& outTrades) {
    if (static_cast<std::size_t>(id) >= idIndex_.size()) {
        return false;
    }
    Order* o = idIndex_[id];
    if (o == nullptr) {
        return false;
    }

    if (newPrice == o->price && newQty <= o->qty) {
        // Quantity-only decrease at an unchanged price keeps queue
        // position: adjust in place, no requeue.
        PriceLadder& ladder = ladderFor(o->side);
        std::size_t idx = ladder.indexOf(o->price);
        PriceLevel& level = ladder.at(idx);

        level.totalQty -= (o->qty - newQty);
        o->qty = newQty;
        if (newQty == 0) {
            // Reduced to nothing: equivalent to a cancel.
            level.spliceOut(o);
            idIndex_[id] = nullptr;
            pool_.release(o);
            if (level.empty()) {
                ladder.noteEmptied(idx);
            }
        }
        return true;
    }

    // Price change, or a quantity increase: loses time priority.
    // Implemented as cancel + re-add, which also lets it match immediately
    // if the new price is marketable.
    Side side = o->side;
    OrderType type = o->type;
    cancelOrder(id);
    return addOrder(id, side, type, newPrice, newQty, outTrades);
}

bool OrderBook::bestBid(Price& outPrice, Qty& outQty) const {
    if (bids_.empty()) {
        return false;
    }
    outPrice = bids_.bestPrice();
    outQty = bids_.at(bids_.bestIndex()).totalQty;
    return true;
}

bool OrderBook::bestAsk(Price& outPrice, Qty& outQty) const {
    if (asks_.empty()) {
        return false;
    }
    outPrice = asks_.bestPrice();
    outQty = asks_.at(asks_.bestIndex()).totalQty;
    return true;
}

Qty OrderBook::qtyAtPrice(Side side, Price price) const {
    const PriceLadder& ladder = ladderFor(side);
    if (!ladder.priceInRange(price)) {
        return 0;
    }
    return ladder.at(ladder.indexOf(price)).totalQty;
}

std::uint32_t OrderBook::orderCountAtPrice(Side side, Price price) const {
    const PriceLadder& ladder = ladderFor(side);
    if (!ladder.priceInRange(price)) {
        return 0;
    }
    return ladder.at(ladder.indexOf(price)).orderCount;
}

Qty OrderBook::totalRestingQty() const {
    Qty total = 0;
    for (std::size_t i = 0; i < bids_.size(); ++i) {
        total += bids_.at(i).totalQty;
    }
    for (std::size_t i = 0; i < asks_.size(); ++i) {
        total += asks_.at(i).totalQty;
    }
    return total;
}

std::uint32_t OrderBook::totalOrderCount() const {
    std::uint32_t total = 0;
    for (std::size_t i = 0; i < bids_.size(); ++i) {
        total += bids_.at(i).orderCount;
    }
    for (std::size_t i = 0; i < asks_.size(); ++i) {
        total += asks_.at(i).orderCount;
    }
    return total;
}

std::vector<OrderInfo> OrderBook::liveOrders() const {
    std::vector<OrderInfo> result;
    result.reserve(totalOrderCount());
    for (const PriceLadder* ladder : {&bids_, &asks_}) {
        for (std::size_t i = 0; i < ladder->size(); ++i) {
            const PriceLevel& level = ladder->at(i);
            for (const Order* o = level.head; o != nullptr; o = o->next) {
                result.push_back(OrderInfo{o->id, o->side, o->price, o->qty});
            }
        }
    }
    return result;
}

} // namespace orderpulse
