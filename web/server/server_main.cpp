// OrderPulse web demo server: an HTTP server (cpp-httplib) that links
// directly against the real orderpulse_core library -- every order the
// browser submits goes through the exact same OrderBook/PriceLadder code
// that the correctness tests and the latency benchmark exercise. This is
// not a reimplementation of the matching logic in JavaScript.
//
// Concurrency note: OrderBook is single-threaded by design (see
// order_book.hpp) -- the benchmark's SPSC design is how OrderPulse achieves
// concurrency without locking the matching path. This demo server takes
// the opposite, deliberately simpler tradeoff: httplib dispatches each
// HTTP request on a pool thread, so every handler that touches shared
// state (the book, the feed simulator, the trade tape) takes a single
// std::mutex for its duration. That's the right call for a low-traffic
// demo serving occasional browser clicks, not a claim about how you'd
// scale a real matching engine -- the actual latency-critical design is
// the point of the benchmark, not this server.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "orderpulse/feed_simulator.hpp"
#include "orderpulse/order_book.hpp"

using json = nlohmann::json;
using namespace orderpulse;

namespace {

constexpr Price kPriceBandMin = 1;
constexpr Price kPriceBandMax = 100'000;
constexpr Price kStartMid = 10'000; // displayed as $100.00 (price is in cents)
constexpr std::size_t kPoolCapacity = 200'000;
constexpr std::size_t kMaxTradeTapeLen = 60;

// User-submitted order ids start well above the feed simulator's own
// (which counts up from 1), so the two id spaces never collide without
// either one needing to know about the other.
constexpr OrderId kUserOrderIdStart = 1'000'000;

std::mutex gMutex;
std::unique_ptr<OrderBook> gBook;
std::unique_ptr<FeedSimulator> gSim;
std::atomic<OrderId> gNextUserOrderId{kUserOrderIdStart};
std::deque<json> gTradeTape; // newest at front, capped at kMaxTradeTapeLen

void resetState() {
    std::lock_guard<std::mutex> lock(gMutex);
    gBook = std::make_unique<OrderBook>(kPoolCapacity, /*maxOrderId=*/2'000'000, kPriceBandMin, kPriceBandMax);
    FeedSimConfig cfg;
    cfg.startMid = kStartMid;
    gSim = std::make_unique<FeedSimulator>(cfg);
    gNextUserOrderId.store(kUserOrderIdStart);
    gTradeTape.clear();
}

json tradeToJson(const Trade& t) {
    return json{{"price", t.price}, {"qty", t.qty}, {"takerId", t.takerId}, {"makerId", t.makerId}};
}

// Caller must hold gMutex.
void recordTrades(const std::vector<Trade>& trades) {
    for (auto it = trades.rbegin(); it != trades.rend(); ++it) {
        gTradeTape.push_front(tradeToJson(*it));
    }
    while (gTradeTape.size() > kMaxTradeTapeLen) {
        gTradeTape.pop_back();
    }
}

json levelsToJson(const OrderBook& book, Side side, bool bestFirstDescending) {
    // Rebuild a compact price -> {qty, count} view straight from
    // liveOrders() -- the same real, authoritative per-order state the
    // rest of the API exposes, not a separately-maintained shadow.
    std::map<Price, std::pair<Qty, int>, std::less<Price>> agg;
    for (const auto& o : book.liveOrders()) {
        if (o.side != side) continue;
        auto& slot = agg[o.price];
        slot.first += o.qty;
        slot.second += 1;
    }
    json levels = json::array();
    auto emit = [&](Price p, Qty q, int c) {
        levels.push_back(json{{"price", p}, {"qty", q}, {"count", c}});
    };
    if (bestFirstDescending) {
        for (auto it = agg.rbegin(); it != agg.rend(); ++it) emit(it->first, it->second.first, it->second.second);
    } else {
        for (auto it = agg.begin(); it != agg.end(); ++it) emit(it->first, it->second.first, it->second.second);
    }
    return levels;
}

json orderToJson(const OrderInfo& o) {
    return json{
        {"id", o.id},
        {"side", o.side == Side::Bid ? "bid" : "ask"},
        {"price", o.price},
        {"qty", o.qty},
    };
}

// Caller must hold gMutex.
json bookSnapshotJson() {
    json j;
    j["bids"] = levelsToJson(*gBook, Side::Bid, /*bestFirstDescending=*/true);
    j["asks"] = levelsToJson(*gBook, Side::Ask, /*bestFirstDescending=*/false);
    json trades = json::array();
    for (const auto& t : gTradeTape) trades.push_back(t);
    j["trades"] = std::move(trades);

    json orders = json::array();
    for (const auto& o : gBook->liveOrders()) {
        orders.push_back(orderToJson(o));
    }
    j["orders"] = std::move(orders);

    Price p; Qty q;
    j["bestBid"] = gBook->bestBid(p, q) ? json{{"price", p}, {"qty", q}} : json(nullptr);
    j["bestAsk"] = gBook->bestAsk(p, q) ? json{{"price", p}, {"qty", q}} : json(nullptr);
    j["totalOrders"] = gBook->totalOrderCount();
    return j;
}

bool parseSide(const std::string& s, Side& out) {
    if (s == "bid" || s == "buy") { out = Side::Bid; return true; }
    if (s == "ask" || s == "sell") { out = Side::Ask; return true; }
    return false;
}

bool parseType(const std::string& s, OrderType& out) {
    if (s == "limit") { out = OrderType::Limit; return true; }
    if (s == "market") { out = OrderType::Market; return true; }
    return false;
}

} // namespace

int main() {
    resetState();

    httplib::Server svr;

    // Deployed behind Render's Cloudflare edge, requests observed roughly
    // 1-in-5 landing on cpp-httplib's generic 404 fallback for routes that
    // are definitely registered (e.g. GET /api/book), with no pattern
    // pointing at any one route -- consistent with a connection-reuse edge
    // case rather than a routing bug. Fix in two parts: upgrade
    // cpp-httplib (was pinned to a several-years-old v0.18.3; picked up
    // ~30 releases of fixes to v0.49.0), and force each proxied connection
    // to carry exactly one request rather than being kept alive and
    // reused, removing persistent-connection state as a variable
    // entirely. The extra per-request handshake cost is irrelevant at this
    // demo's traffic level.
    svr.set_keep_alive_max_count(1);

    svr.set_mount_point("/", "./web/public");

    svr.Get("/api/book", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gMutex);
        res.set_content(bookSnapshotJson().dump(), "application/json");
    });

    svr.Post("/api/orders", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(json{{"error", "invalid JSON"}}.dump(), "application/json");
            return;
        }

        Side side;
        OrderType type;
        if (!body.contains("side") || !parseSide(body.value("side", ""), side) ||
            !body.contains("type") || !parseType(body.value("type", ""), type) ||
            !body.contains("qty")) {
            res.status = 400;
            res.set_content(json{{"error", "expected {side: bid|ask, type: limit|market, price, qty}"}}.dump(),
                             "application/json");
            return;
        }
        Qty qty = body.value("qty", 0u);
        Price price = body.value("price", 0);

        OrderId id = gNextUserOrderId.fetch_add(1);
        std::vector<Trade> trades;

        std::lock_guard<std::mutex> lock(gMutex);
        bool ok = gBook->addOrder(id, side, type, price, qty, trades);
        if (!ok) {
            res.status = 422;
            res.set_content(json{{"error", "order rejected (duplicate id, price outside band, or pool exhausted)"}}.dump(),
                             "application/json");
            return;
        }
        recordTrades(trades);

        json j;
        j["id"] = id;
        j["trades"] = json::array();
        for (const auto& t : trades) j["trades"].push_back(tradeToJson(t));
        j["book"] = bookSnapshotJson();
        res.set_content(j.dump(), "application/json");
    });

    svr.Delete(R"(/api/orders/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        OrderId id = std::strtoull(req.matches[1].str().c_str(), nullptr, 10);

        std::lock_guard<std::mutex> lock(gMutex);
        if (!gBook->cancelOrder(id)) {
            res.status = 404;
            res.set_content(json{{"error", "no such resting order"}}.dump(), "application/json");
            return;
        }
        res.set_content(bookSnapshotJson().dump(), "application/json");
    });

    svr.Post("/api/simulate", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body.empty() ? "{}" : req.body); } catch (...) { body = json::object(); }
        int events = body.value("events", 100);
        events = std::max(1, std::min(events, 5000));

        std::lock_guard<std::mutex> lock(gMutex);
        std::vector<Trade> trades;
        for (int i = 0; i < events; ++i) {
            FeedEvent ev = gSim->next();
            switch (ev.type) {
                case FeedEventType::Add:
                    gBook->addOrder(ev.id, ev.side, ev.orderType, ev.price, ev.qty, trades);
                    break;
                case FeedEventType::Cancel:
                    gBook->cancelOrder(ev.id);
                    break;
                case FeedEventType::Modify:
                    gBook->modifyOrder(ev.id, ev.price, ev.qty, trades);
                    break;
            }
            if (!trades.empty()) {
                recordTrades(trades);
                trades.clear();
            }
        }
        res.set_content(bookSnapshotJson().dump(), "application/json");
    });

    svr.Post("/api/reset", [](const httplib::Request&, httplib::Response& res) {
        resetState(); // takes gMutex internally, released before returning here
        std::lock_guard<std::mutex> lock(gMutex);
        res.set_content(bookSnapshotJson().dump(), "application/json");
    });

    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"status", "ok"}}.dump(), "application/json");
    });

    const char* portEnv = std::getenv("PORT"); // Render sets $PORT
    int port = portEnv ? std::atoi(portEnv) : 8080;
    std::printf("OrderPulse web demo listening on port %d\n", port);
    svr.listen("0.0.0.0", port);
    return 0;
}
