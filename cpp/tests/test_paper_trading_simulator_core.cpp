#include "simex/in_memory_sink.hpp"
#include "simex/paper_trading_simulator_core.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <numeric>
#include <vector>

static std::int64_t sum_fill_qty(const std::vector<FillRecord>& fills) {
    std::int64_t s = 0;
    for (const auto& f : fills)
        s += f.qtyLots;
    return s;
}

void seed_l3(PaperTradingSimulatorCore& sim, const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
             const std::vector<std::int64_t>& qtys, const std::vector<std::int64_t>& orderIds,
             const std::vector<std::int64_t>& traderIds) {
    sim.initFromL3Snapshot(sides, prices, qtys, orderIds, traderIds);
}

TEST_CASE("Valid initialization from L2 snapshot + depth at levels") {
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 123, 131};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};

    PaperTradingSimulatorCore sim{};
    sim.initFromL2Snapshot(sides, prices, quantities);

    for (int i = 0; i < static_cast<int>(sides.size()); ++i) {
        auto depth = sim.depthAt(sides[i], prices[i]);
        REQUIRE(depth != std::nullopt);
        REQUIRE(depth.value() == quantities[i]);
        auto noDepth = sim.depthAt(sides[i], 345678);
        REQUIRE(noDepth == std::nullopt);
    }
}

TEST_CASE("Duplicate price levels on L2 initialization") {
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 120, 131};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    PaperTradingSimulatorCore sim{};
    REQUIRE_THROWS_AS(sim.initFromL2Snapshot(sides, prices, quantities), std::runtime_error);
}

TEST_CASE("Different array sizes on L2 initialization") {
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 121, 131, 140};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    PaperTradingSimulatorCore sim{};
    REQUIRE_THROWS_AS(sim.initFromL2Snapshot(sides, prices, quantities), std::runtime_error);
}

TEST_CASE("Valid initialization from L3 snapshot + depth at levels") {
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 123, 131};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    std::vector<std::int64_t> orderIds{1, 2, 3, 4};
    std::vector<std::int64_t> traderIds{1, 2, 1, 3};

    PaperTradingSimulatorCore sim{};
    sim.initFromL3Snapshot(sides, prices, quantities, orderIds, traderIds);

    for (int i = 0; i < static_cast<int>(sides.size()); ++i) {
        auto depth = sim.depthAt(sides[i], prices[i]);
        REQUIRE(depth != std::nullopt);
        REQUIRE(depth.value() == quantities[i]);
        auto noDepth = sim.depthAt(sides[i], 345678);
        REQUIRE(noDepth == std::nullopt);
    }
}

TEST_CASE("Different array sizes on L3 initialization") {
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 121, 131, 140};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    std::vector<std::int64_t> orderIds{1, 2, 3, 4};
    std::vector<std::int64_t> traderIds{1, 2, 3, 4};
    PaperTradingSimulatorCore sim{};
    REQUIRE_THROWS_AS(sim.initFromL3Snapshot(sides, prices, quantities, orderIds, traderIds), std::runtime_error);
}

TEST_CASE("TopN L2 view") {
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 123, 131};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    PaperTradingSimulatorCore sim{};

    REQUIRE(sim.l2TopN(Side::BUY, 1).empty());
    REQUIRE(sim.l2TopN(Side::SELL, 1).empty());
    sim.initFromL2Snapshot(sides, prices, quantities);

    auto top2Buy = sim.l2TopN(Side::BUY, 2);
    REQUIRE(static_cast<int>(top2Buy.size()) == 2);
    REQUIRE(top2Buy[0].first == 123);
    REQUIRE(top2Buy[0].second == 88);
    REQUIRE(top2Buy[1].first == 120);
    REQUIRE(top2Buy[1].second == 79);

    auto top2Sell = sim.l2TopN(Side::SELL, 2);
    REQUIRE(static_cast<int>(top2Sell.size()) == 2);
    REQUIRE(top2Sell[0].first == 129);
    REQUIRE(top2Sell[0].second == 53);
    REQUIRE(top2Sell[1].first == 131);
    REQUIRE(top2Sell[1].second == 64);

    auto top7Sell = sim.l2TopN(Side::SELL, 7);
    REQUIRE(static_cast<int>(top7Sell.size()) == 2);
}

TEST_CASE("Paper strategy ADD crossing emmits fills but does not mutate book") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    const std::vector<Side> sides{Side::SELL, Side::SELL};
    const std::vector<std::int64_t> prices{105, 105};
    const std::vector<std::int64_t> qtys{6, 4};
    const std::vector<std::int64_t> orderIds{1001, 1002};
    const std::vector<std::int64_t> traderIds{501, 502};

    sim.initFromL3Snapshot(sides, prices, qtys, orderIds, traderIds);

    REQUIRE(sink.getFills().empty());
    REQUIRE(sink.getEvents().empty());

    sim.update(1, 2, Side::BUY, UpdateType::ADD, 110, 8, 9001, 900, NoAggressorNeededSentinel, UpdateSource::STRATEGY);

    const auto& fills = sink.getFills();
    REQUIRE(fills.size() == 2);

    CHECK(fills[0].priceTicks == 105);
    CHECK(fills[0].qtyLots == 6);
    CHECK(fills[0].makerOrderId == 1001);
    CHECK(fills[0].makerTraderId == 501);
    CHECK(fills[0].takerOrderId == 9001);
    CHECK(fills[0].takerTraderId == 900);

    CHECK(fills[1].priceTicks == 105);
    CHECK(fills[1].qtyLots == 2);
    CHECK(fills[1].makerOrderId == 1002);
    CHECK(fills[1].makerTraderId == 502);

    // 2) Paper mode must NOT mutate the historical book
    // total ask liquidity at 105 should remain 10
    auto depth = sim.depthAt(Side::SELL, 105);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 10);
}

TEST_CASE("Historical crossing ADD consumes book and emits fills") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    const std::vector<Side> sides{Side::SELL, Side::SELL};
    const std::vector<std::int64_t> prices{100, 101};
    const std::vector<std::int64_t> qtys{5, 5};
    const std::vector<std::int64_t> orderIds{2001, 2002};
    const std::vector<std::int64_t> traderIds{601, 602};
    sim.initFromL3Snapshot(sides, prices, qtys, orderIds, traderIds);

    // Historical BUY crosses: BUY 7 @ 101 (should take 5@100 + 2@101) and rest 0 (since fully filled)
    sim.update(10, 11, Side::BUY, UpdateType::ADD, 101, 7, 3001, 700, NoAggressorNeededSentinel,
               UpdateSource::HISTORICAL);

    const auto& fills = sink.getFills();
    REQUIRE(fills.size() == 2);
    CHECK(fills[0].priceTicks == 100);
    CHECK(fills[0].qtyLots == 5);
    CHECK(fills[1].priceTicks == 101);
    CHECK(fills[1].qtyLots == 2);

    // Now remaining ask depth: 0 @ 100 (gone), 3 @ 101
    CHECK_FALSE(sim.depthAt(Side::SELL, 100).has_value());
    auto d101 = sim.depthAt(Side::SELL, 101);
    REQUIRE(d101.has_value());
    CHECK(d101.value() == 3);
}

TEST_CASE("HISTORICAL ADD non-crossing to empty book rests order, no fills") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {}, {}, {}, {}, {});

    sim.update(1, 2, Side::BUY, UpdateType::ADD, 100, 10, 1, 11, NoAggressorNeededSentinel, UpdateSource::HISTORICAL);

    CHECK(sink.getFills().empty());

    auto d = sim.depthAt(Side::BUY, 100);
    REQUIRE(d.has_value());
    CHECK(d.value() == 10);
}

TEST_CASE("STRATEGY ADD non-crossing does nothing (no fills, no resting, no mutation)") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::SELL}, {105}, {10}, {1001}, {501});

    // Strategy BUY below best ask -> no cross -> paper returns without inserting
    sim.update(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 9001, 900, NoAggressorNeededSentinel, UpdateSource::STRATEGY);

    CHECK(sink.getFills().empty());

    // Book unchanged
    auto d = sim.depthAt(Side::SELL, 105);
    REQUIRE(d.has_value());
    CHECK(d.value() == 10);
    CHECK_FALSE(sim.depthAt(Side::BUY, 100).has_value());
}

TEST_CASE("STRATEGY ADD crossing emits FIFO fills and does not mutate book") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::SELL, Side::SELL}, {105, 105}, {6, 4}, {1001, 1002}, {501, 502});
    sim.update(1, 2, Side::BUY, UpdateType::ADD, 110, 8, 9001, 900, NoAggressorNeededSentinel, UpdateSource::STRATEGY);

    const auto& fills = sink.getFills();
    REQUIRE(fills.size() == 2);

    // FIFO: maker 1001 then 1002
    CHECK(fills[0].priceTicks == 105);
    CHECK(fills[0].qtyLots == 6);
    CHECK(fills[0].makerOrderId == 1001);
    CHECK(fills[0].makerTraderId == 501);
    CHECK(fills[0].takerOrderId == 9001);
    CHECK(fills[0].takerTraderId == 900);
    CHECK(fills[0].takerSource == UpdateSource::STRATEGY);

    CHECK(fills[1].priceTicks == 105);
    CHECK(fills[1].qtyLots == 2);
    CHECK(fills[1].makerOrderId == 1002);
    CHECK(fills[1].makerTraderId == 502);

    // Book unchanged (still 10 @105)
    auto d = sim.depthAt(Side::SELL, 105);
    REQUIRE(d.has_value());
    CHECK(d.value() == 10);
}

TEST_CASE("HISTORICAL ADD crossing partially fills and rests remainder on own side") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::SELL}, {105}, {4}, {1001}, {501});

    // BUY 10 @110 consumes 4 @105, rests 6 @110
    sim.update(1, 2, Side::BUY, UpdateType::ADD, 110, 10, 9001, 900, NoAggressorNeededSentinel,
               UpdateSource::HISTORICAL);

    const auto& fills = sink.getFills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].priceTicks == 105);
    CHECK(fills[0].qtyLots == 4);
    CHECK(fills[0].makerOrderId == 1001);
    CHECK(fills[0].takerOrderId == 9001);
    CHECK(fills[0].takerSource == UpdateSource::HISTORICAL);

    CHECK_FALSE(sim.depthAt(Side::SELL, 105).has_value());

    auto d = sim.depthAt(Side::BUY, 110);
    REQUIRE(d.has_value());
    CHECK(d.value() == 6);
}

TEST_CASE("HISTORICAL ADD crossing fully fills and does not rest") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::SELL, Side::SELL}, {105, 105}, {6, 4}, {1001, 1002}, {501, 502});

    sim.update(1, 2, Side::BUY, UpdateType::ADD, 110, 10, 9001, 900, NoAggressorNeededSentinel,
               UpdateSource::HISTORICAL);

    const auto& fills = sink.getFills();
    REQUIRE(fills.size() == 2);
    CHECK(sum_fill_qty(fills) == 10);

    CHECK_FALSE(sim.depthAt(Side::SELL, 105).has_value());
    CHECK_FALSE(sim.depthAt(Side::BUY, 110).has_value());
}

TEST_CASE("HISTORICAL ADD crossing consumes multiple levels in price priority") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    // asks: 3@100, 4@101, 5@103
    seed_l3(sim, {Side::SELL, Side::SELL, Side::SELL}, {100, 101, 103}, {3, 4, 5}, {2001, 2002, 2003}, {601, 602, 603});

    // BUY 10 @103 => 3@100 + 4@101 + 3@103
    sim.update(1, 2, Side::BUY, UpdateType::ADD, /*price*/ 103, /*qty*/ 10,
               /*orderId*/ 9001, /*traderId*/ 900, NoAggressorNeededSentinel, UpdateSource::HISTORICAL);

    const auto& fills = sink.getFills();
    REQUIRE(sum_fill_qty(fills) == 10);

    // First fill price should be best ask (100), then 101, then 103
    REQUIRE(fills.size() >= 3);
    CHECK(fills[0].priceTicks == 100);
    CHECK(fills[1].priceTicks == 101);
    CHECK(fills.back().priceTicks == 103);

    CHECK_FALSE(sim.depthAt(Side::SELL, 100).has_value());
    CHECK_FALSE(sim.depthAt(Side::SELL, 101).has_value());

    auto d103 = sim.depthAt(Side::SELL, 103);
    REQUIRE(d103.has_value());
    CHECK(d103.value() == 2); // 5 - 3
}

TEST_CASE("FIFO within a level: older resting order is consumed first") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    // Two bids at same price, FIFO by insertion order
    seed_l3(sim, {Side::BUY, Side::BUY}, {100, 100}, {5, 7}, {3001, 3002}, {701, 702});

    // SELL 6 @90 crosses best bid (100). Should take 5 from 3001 then 1 from 3002
    sim.update(1, 2, Side::SELL, UpdateType::ADD, 90, 6, 4001, 800, NoAggressorNeededSentinel,
               UpdateSource::HISTORICAL);

    const auto& fills = sink.getFills();
    REQUIRE(fills.size() == 2);

    CHECK(fills[0].makerOrderId == 3001);
    CHECK(fills[0].qtyLots == 5);
    CHECK(fills[1].makerOrderId == 3002);
    CHECK(fills[1].qtyLots == 1);
}

TEST_CASE("ADD with duplicate orderId is ignored (no fills, no state change)") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);
    seed_l3(sim, {Side::SELL}, {105}, {10}, {1001}, {501});
    // Attempt ADD with same orderId (should early-return)
    sim.update(1, 2, Side::BUY, UpdateType::ADD, 110, 5, 1001, 900, NoAggressorNeededSentinel,
               UpdateSource::HISTORICAL);

    CHECK(sink.getFills().empty());

    auto d = sim.depthAt(Side::SELL, 105);
    REQUIRE(d.has_value());
    CHECK(d.value() == 10);
}

TEST_CASE("ADD with qty=0 does nothing (no fills, no state change)") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);
    seed_l3(sim, {}, {}, {}, {}, {});
    sim.update(1, 2, Side::BUY, UpdateType::ADD, 100, 0, 1, 11, NoAggressorNeededSentinel, UpdateSource::HISTORICAL);

    CHECK(sink.getFills().empty());
    CHECK_FALSE(sim.depthAt(Side::BUY, 100).has_value());
}

TEST_CASE("Stale heap entry is popped and does not prevent matching next level") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    // ask 100 qty 5
    seed_l3(sim, {Side::SELL}, {100}, {5}, {5001}, {901});

    // consume it fully -> level removed, heap retains stale -100
    sim.update(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 6001, 999, NoAggressorNeededSentinel,
               UpdateSource::HISTORICAL);

    sink.reset();

    // add new ask at 101
    sim.update(3, 4, Side::SELL, UpdateType::ADD, 101, 7, 5002, 902, NoAggressorNeededSentinel,
               UpdateSource::HISTORICAL);

    sink.reset();

    // buy crossing should match at 101 (not get stuck on stale 100)
    sim.update(5, 6, Side::BUY, UpdateType::ADD, 101, 2, 6002, 999, NoAggressorNeededSentinel,
               UpdateSource::HISTORICAL);

    const auto& fills = sink.getFills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].priceTicks == 101);
    CHECK(fills[0].qtyLots == 2);
}

TEST_CASE("Paper sweep does not double-count liquidity when heap has duplicate price entries") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    // ask 100 qty 5
    seed_l3(sim, {Side::SELL}, {100}, {5}, {7001}, {1001});

    // historical BUY consumes ask level -> leaves stale heap entry for 100
    sim.update(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 8001, 2001, NoAggressorNeededSentinel,
               UpdateSource::HISTORICAL);

    // re-add ask 100 qty 5 (pushes -100 again) => heap contains duplicate -100 entries
    sim.update(3, 4, Side::SELL, UpdateType::ADD, 100, 5, 7002, 1002, NoAggressorNeededSentinel,
               UpdateSource::HISTORICAL);

    sink.reset();

    // Strategy BUY should only fill 5 once (not 10)
    sim.update(5, 6, Side::BUY, UpdateType::ADD, 100, 10, 9001, 3001, NoAggressorNeededSentinel,
               UpdateSource::STRATEGY);

    const auto& fills = sink.getFills();
    CHECK(sum_fill_qty(fills) == 5);

    // Book unchanged by paper mode (still 5 @100)
    auto d = sim.depthAt(Side::SELL, 100);
    REQUIRE(d.has_value());
    CHECK(d.value() == 5);
}

TEST_CASE("initFromL3Snapshot throws on duplicate orderId") {
    PaperTradingSimulatorCore sim{};

    const std::vector<Side> sides{Side::SELL, Side::SELL};
    const std::vector<std::int64_t> prices{100, 101};
    const std::vector<std::int64_t> qtys{1, 1};
    const std::vector<std::int64_t> orderIds{1, 1}; // duplicate
    const std::vector<std::int64_t> traderIds{10, 11};

    REQUIRE_THROWS_AS(sim.initFromL3Snapshot(sides, prices, qtys, orderIds, traderIds), std::runtime_error);
}

TEST_CASE("update throws on unknown UpdateType") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    REQUIRE_THROWS_AS(sim.update(1, 2, Side::BUY, static_cast<UpdateType>(999), 100, 1, 1, 1, NoAggressorNeededSentinel,
                                 UpdateSource::HISTORICAL),
                      std::runtime_error);
}