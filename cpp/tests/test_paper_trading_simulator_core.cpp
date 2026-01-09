#include "simex/in_memory_sink.hpp"
#include "simex/paper_trading_simulator_core.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

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