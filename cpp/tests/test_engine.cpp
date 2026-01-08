#include "simex/engine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

TEST_CASE("Valid initialization from L2 snapshot + depth at levels")
{
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 123, 131};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};

    PaperTradingSimulatorCore sim{};
    sim.initFromL2Snapshot(sides, prices, quantities);

    for (int i = 0; i < static_cast<int>(sides.size()); ++i)
    {
        auto depth = sim.depthAt(sides[i], prices[i]);
        REQUIRE(depth != std::nullopt);
        REQUIRE(depth.value() == quantities[i]);
        auto noDepth = sim.depthAt(sides[i], 345678);
        REQUIRE(noDepth == std::nullopt);
    }
}

TEST_CASE("Duplicate price levels on L2 initialization")
{
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 120, 131};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    PaperTradingSimulatorCore sim{};
    REQUIRE_THROWS_AS(sim.initFromL2Snapshot(sides, prices, quantities), std::runtime_error);
}

TEST_CASE("Different array sizes on L2 initialization")
{
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 121, 131, 140};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    PaperTradingSimulatorCore sim{};
    REQUIRE_THROWS_AS(sim.initFromL2Snapshot(sides, prices, quantities), std::runtime_error);
}

TEST_CASE("Valid initialization from L3 snapshot + depth at levels")
{
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 123, 131};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    std::vector<std::int64_t> orderIds{1, 2, 3, 4};
    std::vector<std::int64_t> traderIds{1, 2, 1, 3};

    PaperTradingSimulatorCore sim{};
    sim.initFromL3Snapshot(sides, prices, quantities, orderIds, traderIds);

    for (int i = 0; i < static_cast<int>(sides.size()); ++i)
    {
        auto depth = sim.depthAt(sides[i], prices[i]);
        REQUIRE(depth != std::nullopt);
        REQUIRE(depth.value() == quantities[i]);
        auto noDepth = sim.depthAt(sides[i], 345678);
        REQUIRE(noDepth == std::nullopt);
    }
}

TEST_CASE("Different array sizes on L3 initialization")
{
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 121, 131, 140};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    std::vector<std::int64_t> orderIds{1, 2, 3, 4};
    std::vector<std::int64_t> traderIds{1, 2, 3, 4};
    PaperTradingSimulatorCore sim{};
    REQUIRE_THROWS_AS(sim.initFromL3Snapshot(sides, prices, quantities, orderIds, traderIds), std::runtime_error);
}
