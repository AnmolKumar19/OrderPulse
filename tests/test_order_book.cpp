#include <catch_amalgamated.hpp>

#include "orderpulse/order_book.hpp"

using namespace orderpulse;

namespace {
constexpr std::size_t kPoolCap = 1024;
}

TEST_CASE("time priority within a single price level", "[matching][priority]") {
    OrderBook book(kPoolCap);
    std::vector<Trade> trades;

    // Two resting bids at the same price, submitted in order 1 then 2.
    REQUIRE(book.addOrder(1, Side::Bid, OrderType::Limit, 100, 10, trades));
    REQUIRE(book.addOrder(2, Side::Bid, OrderType::Limit, 100, 10, trades));
    REQUIRE(trades.empty());

    // An incoming marketable ask should fill the OLDER order (id 1) first,
    // even though both are at the same price.
    REQUIRE(book.addOrder(3, Side::Ask, OrderType::Limit, 100, 5, trades));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].takerId == 3);
    CHECK(trades[0].makerId == 1);
    CHECK(trades[0].qty == 5);

    // Order 1 has 5 left and is still at the head of the queue.
    CHECK(book.orderCountAtPrice(Side::Bid, 100) == 2);
    CHECK(book.qtyAtPrice(Side::Bid, 100) == 15); // 5 (order 1) + 10 (order 2)
}

TEST_CASE("partial fill of a single resting order", "[matching][partial-fill]") {
    OrderBook book(kPoolCap);
    std::vector<Trade> trades;

    REQUIRE(book.addOrder(1, Side::Ask, OrderType::Limit, 100, 20, trades));
    REQUIRE(book.addOrder(2, Side::Bid, OrderType::Limit, 100, 7, trades));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].qty == 7);

    // Resting order 1 partially filled: 13 left, still resting, still the
    // only order at that level.
    CHECK(book.orderCountAtPrice(Side::Ask, 100) == 1);
    CHECK(book.qtyAtPrice(Side::Ask, 100) == 13);
    CHECK(book.totalOrderCount() == 1);
    CHECK(book.poolInUse() == 1); // the aggressing order (id 2) fully filled and was freed
}

TEST_CASE("market order sweeps multiple price levels in price order", "[matching][sweep]") {
    OrderBook book(kPoolCap);
    std::vector<Trade> trades;

    REQUIRE(book.addOrder(1, Side::Ask, OrderType::Limit, 101, 10, trades));
    REQUIRE(book.addOrder(2, Side::Ask, OrderType::Limit, 100, 10, trades)); // best ask
    REQUIRE(book.addOrder(3, Side::Ask, OrderType::Limit, 102, 10, trades));

    // A market buy for 25 should take all of level 100, all of level 101,
    // then 5 from level 102 — cheapest first, regardless of submission order.
    REQUIRE(book.addOrder(4, Side::Bid, OrderType::Market, 0, 25, trades));

    REQUIRE(trades.size() == 3);
    CHECK(trades[0].makerId == 2);
    CHECK(trades[0].price == 100);
    CHECK(trades[0].qty == 10);
    CHECK(trades[1].makerId == 1);
    CHECK(trades[1].price == 101);
    CHECK(trades[1].qty == 10);
    CHECK(trades[2].makerId == 3);
    CHECK(trades[2].price == 102);
    CHECK(trades[2].qty == 5);

    Price bestAskPrice{};
    Qty bestAskQty{};
    REQUIRE(book.bestAsk(bestAskPrice, bestAskQty));
    CHECK(bestAskPrice == 102);
    CHECK(bestAskQty == 5);
}

TEST_CASE("marketable limit's unfilled remainder rests at its own price", "[matching][rest]") {
    OrderBook book(kPoolCap);
    std::vector<Trade> trades;

    REQUIRE(book.addOrder(1, Side::Ask, OrderType::Limit, 100, 5, trades));
    // Bid crosses (101 >= 100), fills 5, and has 10 left over -> rests at 101.
    REQUIRE(book.addOrder(2, Side::Bid, OrderType::Limit, 101, 15, trades));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].qty == 5);

    Price bestBidPrice{};
    Qty bestBidQty{};
    REQUIRE(book.bestBid(bestBidPrice, bestBidQty));
    CHECK(bestBidPrice == 101);
    CHECK(bestBidQty == 10);
}

TEST_CASE("cancel removes exactly the targeted order and preserves the rest", "[cancel]") {
    OrderBook book(kPoolCap);
    std::vector<Trade> trades;

    REQUIRE(book.addOrder(1, Side::Bid, OrderType::Limit, 100, 10, trades)); // oldest
    REQUIRE(book.addOrder(2, Side::Bid, OrderType::Limit, 100, 10, trades)); // middle
    REQUIRE(book.addOrder(3, Side::Bid, OrderType::Limit, 100, 10, trades)); // newest

    REQUIRE(book.cancelOrder(2));
    CHECK_FALSE(book.cancelOrder(2)); // already gone: cancel is not idempotent

    CHECK(book.orderCountAtPrice(Side::Bid, 100) == 2);
    CHECK(book.qtyAtPrice(Side::Bid, 100) == 20);
    CHECK(book.poolInUse() == 2);

    // Priority order must now be 1 then 3 (2 is gone, not just skipped).
    REQUIRE(book.addOrder(4, Side::Ask, OrderType::Limit, 100, 10, trades));
    REQUIRE(trades.size() == 1);
    CHECK(trades[0].makerId == 1);

    REQUIRE(book.addOrder(5, Side::Ask, OrderType::Limit, 100, 10, trades));
    REQUIRE(trades.size() == 2);
    CHECK(trades[1].makerId == 3);
}

TEST_CASE("market order against an empty book is a safe no-op", "[matching][edge-case]") {
    OrderBook book(kPoolCap);
    std::vector<Trade> trades;

    REQUIRE(book.addOrder(1, Side::Bid, OrderType::Market, 0, 100, trades));
    CHECK(trades.empty());
    CHECK(book.totalRestingQty() == 0);
    CHECK(book.poolInUse() == 0); // market orders never rest, even unfilled
}

TEST_CASE("market order remainder beyond available liquidity is dropped, not rested", "[matching][edge-case]") {
    OrderBook book(kPoolCap);
    std::vector<Trade> trades;

    REQUIRE(book.addOrder(1, Side::Ask, OrderType::Limit, 100, 10, trades));
    REQUIRE(book.addOrder(2, Side::Bid, OrderType::Market, 0, 50, trades));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].qty == 10);
    CHECK(book.totalRestingQty() == 0);
    CHECK_FALSE(book.cancelOrder(2)); // id 2 was never resting; nothing to cancel
}

TEST_CASE("modify: quantity decrease at the same price keeps queue position", "[modify][priority]") {
    OrderBook book(kPoolCap);
    std::vector<Trade> trades;

    REQUIRE(book.addOrder(1, Side::Bid, OrderType::Limit, 100, 10, trades)); // oldest
    REQUIRE(book.addOrder(2, Side::Bid, OrderType::Limit, 100, 10, trades)); // newest

    REQUIRE(book.modifyOrder(1, 100, 4, trades)); // shrink order 1, same price
    CHECK(book.qtyAtPrice(Side::Bid, 100) == 14);

    // A marketable ask for 4 should still fill order 1 (still at the head).
    REQUIRE(book.addOrder(3, Side::Ask, OrderType::Limit, 100, 4, trades));
    REQUIRE(trades.size() == 1);
    CHECK(trades[0].makerId == 1);
    CHECK(book.orderCountAtPrice(Side::Bid, 100) == 1); // order 1 fully consumed
}

TEST_CASE("modify: a price change loses time priority", "[modify][priority]") {
    OrderBook book(kPoolCap);
    std::vector<Trade> trades;

    REQUIRE(book.addOrder(1, Side::Bid, OrderType::Limit, 100, 10, trades)); // oldest, will move
    REQUIRE(book.addOrder(2, Side::Bid, OrderType::Limit, 100, 10, trades)); // stays put

    REQUIRE(book.modifyOrder(1, 100, 10, trades)); // same price+qty is a no-op path
    // Force an actual price round-trip to push order 1 behind order 2.
    REQUIRE(book.modifyOrder(1, 99, 10, trades));
    REQUIRE(book.modifyOrder(1, 100, 10, trades));

    REQUIRE(book.addOrder(3, Side::Ask, OrderType::Limit, 100, 10, trades));
    REQUIRE(trades.size() == 1);
    CHECK(trades[0].makerId == 2); // order 2 now fills first: it kept priority, order 1 lost it
}

TEST_CASE("duplicate order id is rejected without side effects", "[add][edge-case]") {
    OrderBook book(kPoolCap);
    std::vector<Trade> trades;

    REQUIRE(book.addOrder(1, Side::Bid, OrderType::Limit, 100, 10, trades));
    CHECK_FALSE(book.addOrder(1, Side::Bid, OrderType::Limit, 100, 5, trades));

    CHECK(book.qtyAtPrice(Side::Bid, 100) == 10); // unchanged by the rejected duplicate
    CHECK(book.poolInUse() == 1);
}

TEST_CASE("pool exhaustion is rejected gracefully, not a crash", "[add][edge-case]") {
    OrderBook book(2); // capacity for only 2 resting orders
    std::vector<Trade> trades;

    REQUIRE(book.addOrder(1, Side::Bid, OrderType::Limit, 100, 10, trades));
    REQUIRE(book.addOrder(2, Side::Bid, OrderType::Limit, 99, 10, trades));
    CHECK_FALSE(book.addOrder(3, Side::Bid, OrderType::Limit, 98, 10, trades));

    CHECK(book.poolInUse() == 2);
}

TEST_CASE("a Limit order priced outside the configured band is rejected, not UB", "[add][edge-case][price-band]") {
    // PriceLadder pre-allocates exactly [priceBandMin, priceBandMax]; a
    // price outside that range must never reach indexOf() (see
    // order_book.cpp's addOrder guard) -- this test exists specifically to
    // pin that guard down, since silently indexing out of range there
    // would be memory corruption, not a clean failure.
    OrderBook book(kPoolCap, /*maxOrderId=*/0, /*priceBandMin=*/1, /*priceBandMax=*/1000);
    std::vector<Trade> trades;

    CHECK_FALSE(book.addOrder(1, Side::Bid, OrderType::Limit, 0, 10, trades));      // below band
    CHECK_FALSE(book.addOrder(2, Side::Ask, OrderType::Limit, 1001, 10, trades));   // above band
    CHECK(book.poolInUse() == 0);
    CHECK(book.totalRestingQty() == 0);

    // The boundary prices themselves are valid (band is inclusive).
    REQUIRE(book.addOrder(3, Side::Bid, OrderType::Limit, 1, 10, trades));
    REQUIRE(book.addOrder(4, Side::Ask, OrderType::Limit, 1000, 10, trades));
    CHECK(book.poolInUse() == 2);

    // A Market order's price field is never checked against the band --
    // it's meaningless for a Market order and must not cause a rejection.
    REQUIRE(book.addOrder(5, Side::Bid, OrderType::Market, 0, 5, trades));
}

TEST_CASE("liveOrders reflects exactly the resting orders, with correct remaining qty", "[introspection]") {
    OrderBook book(kPoolCap);
    std::vector<Trade> trades;

    REQUIRE(book.addOrder(1, Side::Bid, OrderType::Limit, 100, 10, trades));
    REQUIRE(book.addOrder(2, Side::Ask, OrderType::Limit, 105, 20, trades));
    REQUIRE(book.addOrder(3, Side::Ask, OrderType::Limit, 100, 5, trades)); // partially fills order 1

    auto orders = book.liveOrders();
    REQUIRE(orders.size() == 2); // order 3 fully filled, order 1 partially filled, order 2 untouched

    bool sawOrder1 = false, sawOrder2 = false;
    for (const auto& o : orders) {
        if (o.id == 1) {
            sawOrder1 = true;
            CHECK(o.side == Side::Bid);
            CHECK(o.price == 100);
            CHECK(o.qty == 5); // 10 - 5 filled
        } else if (o.id == 2) {
            sawOrder2 = true;
            CHECK(o.side == Side::Ask);
            CHECK(o.price == 105);
            CHECK(o.qty == 20);
        }
    }
    CHECK(sawOrder1);
    CHECK(sawOrder2);

    REQUIRE(book.cancelOrder(2));
    CHECK(book.liveOrders().size() == 1);
}

TEST_CASE("conservation invariant holds across a mixed sequence of operations", "[conservation]") {
    // Not a substitute for the exact-outcome tests above — this is a
    // broader sanity net: at every checkpoint, every order the pool has
    // handed out is accounted for exactly once (resting XOR freed), never
    // zero times (lost) and never counted twice (duplicated).
    OrderBook book(kPoolCap);
    std::vector<Trade> trades;

    auto checkInvariant = [&book]() {
        CHECK(book.poolInUse() == book.totalOrderCount());
    };

    REQUIRE(book.addOrder(1, Side::Bid, OrderType::Limit, 100, 10, trades));
    checkInvariant();
    REQUIRE(book.addOrder(2, Side::Bid, OrderType::Limit, 99, 20, trades));
    checkInvariant();
    REQUIRE(book.addOrder(3, Side::Ask, OrderType::Limit, 105, 15, trades));
    checkInvariant();
    REQUIRE(book.addOrder(4, Side::Ask, OrderType::Limit, 100, 8, trades)); // crosses order 1 partially
    checkInvariant();
    REQUIRE(book.cancelOrder(2));
    checkInvariant();
    REQUIRE(book.modifyOrder(3, 106, 15, trades));
    checkInvariant();
    REQUIRE(book.addOrder(5, Side::Bid, OrderType::Market, 0, 1000, trades)); // sweeps everything asks-side
    checkInvariant();
    REQUIRE(book.addOrder(6, Side::Bid, OrderType::Limit, 50, 5, trades));
    checkInvariant();
    REQUIRE(book.cancelOrder(6));
    checkInvariant();

    checkInvariant();
}
