#include <set>

#include <catch_amalgamated.hpp>

#include "orderpulse/feed_simulator.hpp"
#include "orderpulse/order_book.hpp"

using namespace orderpulse;

TEST_CASE("FeedSimulator generates well-formed events", "[feed]") {
    FeedSimConfig cfg;
    cfg.seed = 7;
    FeedSimulator sim(cfg);

    std::set<OrderId> seenAddIds;
    int adds = 0, cancels = 0, modifies = 0, markets = 0;

    for (int i = 0; i < 100'000; ++i) {
        FeedEvent ev = sim.next();
        switch (ev.type) {
            case FeedEventType::Add:
                ++adds;
                // Every Add must carry a fresh id we haven't seen before.
                REQUIRE(seenAddIds.insert(ev.id).second);
                if (ev.orderType == OrderType::Market) {
                    ++markets;
                } else {
                    CHECK(ev.price > 0);
                }
                CHECK(ev.qty > 0);
                break;
            case FeedEventType::Cancel:
                ++cancels;
                // A cancel must target an id we've already seen as an Add.
                CHECK(seenAddIds.count(ev.id) == 1);
                break;
            case FeedEventType::Modify:
                ++modifies;
                CHECK(seenAddIds.count(ev.id) == 1);
                CHECK(ev.qty > 0);
                CHECK(ev.price > 0);
                break;
        }
    }

    CHECK(adds > 0);
    CHECK(cancels > 0);
    CHECK(modifies > 0);
    CHECK(markets > 0);

    // Cancels should be a substantial fraction of traffic (real LOB flow is
    // cancel-heavy) but not overwhelm everything else.
    double cancelFraction = static_cast<double>(cancels) / 100'000.0;
    CHECK(cancelFraction > 0.15);
    CHECK(cancelFraction < 0.60);
}

TEST_CASE("FeedSimulator output drives a real OrderBook without crashing or corrupting it", "[feed][integration]") {
    FeedSimConfig cfg;
    cfg.seed = 99;
    FeedSimulator sim(cfg);

    OrderBook book(200'000, 200'000);
    std::vector<Trade> trades;

    for (int i = 0; i < 150'000; ++i) {
        FeedEvent ev = sim.next();
        switch (ev.type) {
            case FeedEventType::Add:
                book.addOrder(ev.id, ev.side, ev.orderType, ev.price, ev.qty, trades);
                break;
            case FeedEventType::Cancel:
                book.cancelOrder(ev.id); // may legitimately fail if already filled -- not checked
                break;
            case FeedEventType::Modify:
                book.modifyOrder(ev.id, ev.price, ev.qty, trades);
                break;
        }
        if (i % 20'000 == 0) {
            CHECK(book.poolInUse() == book.totalOrderCount());
        }
    }

    CHECK(book.poolInUse() == book.totalOrderCount());
    CHECK_FALSE(trades.empty()); // the aggressive-order fraction must produce real trades
}
