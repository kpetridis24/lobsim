#include "lobsim/in_memory_sink.hpp"
#define private public
#include "lobsim/paper_trading_simulator_core.hpp"
#undef private

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

static NormalizedLobEvent make_event(std::int64_t tsEx, std::int64_t tsRecv, Side side, UpdateType ut,
                                     std::int64_t priceTicks, std::int64_t qtyLots, std::int64_t orderId,
                                     std::int64_t traderId = UnknownTraderIdSentinel,
                                     std::int64_t aggressorId = NoAggressorNeededSentinel,
                                     UpdateSource src = UpdateSource::HISTORICAL) {
    return NormalizedLobEvent{tsEx, tsRecv, side, ut, priceTicks, qtyLots, orderId, traderId, aggressorId, src, ""};
}

static std::int64_t sum_fill_qty(const std::vector<FillRecord>& fills) {
    std::int64_t s = 0;
    for (const auto& f : fills)
        s += f.qtyLots;
    return s;
}

static std::vector<FillRecord> strategy_maker_fills(const std::vector<FillRecord>& fills) {
    std::vector<FillRecord> out;
    for (const auto& f : fills) {
        if (f.makerSource == UpdateSource::STRATEGY) {
            out.push_back(f);
        }
    }
    return out;
}

static const DiagnosticRecord* last_diagnostic(const InMemoryLogSink& sink) {
    const auto& diags = sink.getDiagnostics();
    if (diags.empty()) {
        return nullptr;
    }
    return &diags.back();
}

static void assert_diag_matches_event(const DiagnosticRecord& diag, const EventApplyRecord& ev,
                                      DiagnosticRecordCode code, DiagnosticRecordSeverity severity) {
    CHECK(diag.code == code);
    CHECK(diag.severity == severity);
    CHECK(diag.seq == ev.seq);
    CHECK(diag.tsExchange == ev.tsExchange);
    CHECK(diag.tsReceived == ev.tsReceived);
}

static void assert_last_diag_matches_event(const InMemoryLogSink& sink, DiagnosticRecordCode code,
                                           DiagnosticRecordSeverity severity) {
    const auto* diag = last_diagnostic(sink);
    REQUIRE(diag != nullptr);
    const auto& events = sink.getEvents();
    REQUIRE_FALSE(events.empty());
    const auto& ev = events.back();
    assert_diag_matches_event(*diag, ev, code, severity);
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

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 110, 8, 9001, 900, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

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
    sim.update(make_event(10, 11, Side::BUY, UpdateType::ADD, 101, 7, 3001, 700, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

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

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 10, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

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
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 9001, 900, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

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
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 110, 8, 9001, 900, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

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
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 110, 10, 9001, 900, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

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

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 110, 10, 9001, 900, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

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
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, /*price*/ 103, /*qty*/ 10,
                          /*orderId*/ 9001, /*traderId*/ 900, NoAggressorNeededSentinel, UpdateSource::HISTORICAL));

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
    sim.update(make_event(1, 2, Side::SELL, UpdateType::ADD, 90, 6, 4001, 800, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

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
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 110, 5, 1001, 900, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

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
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 0, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    CHECK(sink.getFills().empty());
    CHECK_FALSE(sim.depthAt(Side::BUY, 100).has_value());
}

TEST_CASE("ADD with negative quantity emits diagnostic") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);
    seed_l3(sim, {}, {}, {}, {}, {});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, -5, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    assert_last_diag_matches_event(sink, DiagnosticRecordCode::ADD_INVOKED_WITH_NEGATIVE_QUANTITY,
                                   DiagnosticRecordSeverity::ERROR);
}

TEST_CASE("Stale heap entry is popped and does not prevent matching next level") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    // ask 100 qty 5
    seed_l3(sim, {Side::SELL}, {100}, {5}, {5001}, {901});

    // consume it fully -> level removed, heap retains stale -100
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 6001, 999, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();

    // add new ask at 101
    sim.update(make_event(3, 4, Side::SELL, UpdateType::ADD, 101, 7, 5002, 902, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();

    // buy crossing should match at 101 (not get stuck on stale 100)
    sim.update(make_event(5, 6, Side::BUY, UpdateType::ADD, 101, 2, 6002, 999, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

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
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 8001, 2001, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    // re-add ask 100 qty 5 (pushes -100 again) => heap contains duplicate -100 entries
    sim.update(make_event(3, 4, Side::SELL, UpdateType::ADD, 100, 5, 7002, 1002, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();

    // Strategy BUY should only fill 5 once (not 10)
    sim.update(make_event(5, 6, Side::BUY, UpdateType::ADD, 100, 10, 9001, 3001, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    const auto& fills = sink.getFills();
    CHECK(sum_fill_qty(fills) == 5);

    // Book unchanged by paper mode (still 5 @100)
    auto d = sim.depthAt(Side::SELL, 100);
    REQUIRE(d.has_value());
    CHECK(d.value() == 5);
}

TEST_CASE("Paper order fills after trades exceed market ahead") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 10, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(5, 6, Side::SELL, UpdateType::ADD, 100, 12, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 2);
}

TEST_CASE("Multiple paper orders fill FIFO when trade volume is sufficient") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 1, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 1, 9002, 902, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(5, 6, Side::BUY, UpdateType::ADD, 100, 10, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 12, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 2);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 1);
    CHECK(strat[1].makerOrderId == 9002);
    CHECK(strat[1].qtyLots == 1);
}

TEST_CASE("Canceling a market order behind paper does not advance paper") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::DELETE, 100, 0, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 5, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    CHECK(strat.empty());
}

TEST_CASE("Canceling a market order ahead advances paper") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::DELETE, 100, 0, 1001, 501, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 2, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 2);
}

TEST_CASE("Paper partial fill persists and completes on later trades") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 3, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 1, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(5, 6, Side::SELL, UpdateType::ADD, 100, 6, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].qtyLots == 1);

    sim.update(make_event(7, 8, Side::BUY, UpdateType::ADD, 100, 2, 1003, 503, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    sink.reset();
    sim.update(make_event(9, 10, Side::SELL, UpdateType::ADD, 100, 2, 2002, 602, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].qtyLots == 2);
    CHECK(strat[0].makerOrderId == 9001);
}

TEST_CASE("Paper SUBTRACT reduces size used for future fills") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 4, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 4, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::SUBTRACT, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 9, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 2);
}

TEST_CASE("Paper DELETE removes order and prevents future fills") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::DELETE, 100, 0, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 10, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    CHECK(strat.empty());
}

TEST_CASE("Paper fills work on the ask side too") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::SELL}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::SELL, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::SELL, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(5, 6, Side::BUY, UpdateType::ADD, 100, 12, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 2);
}

TEST_CASE("Paper order ahead of later market orders fills at an initially empty level") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {}, {}, {}, {}, {});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1001, 501, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(5, 6, Side::SELL, UpdateType::ADD, 100, 3, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 2);
}

TEST_CASE("Paper orders respect interleaved market orders in queue priority") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {3}, {1001}, {501});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 2, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    sim.update(make_event(5, 6, Side::BUY, UpdateType::ADD, 100, 2, 9002, 902, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(7, 8, Side::BUY, UpdateType::ADD, 100, 2, 1003, 503, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(9, 10, Side::SELL, UpdateType::ADD, 100, 7, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 2);
}

TEST_CASE("Market SUBTRACT ahead advances paper while preserving priority") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::SUBTRACT, 100, 3, 1001, 501, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 5, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 2);
}

TEST_CASE("Market SET behind does not advance paper") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::SET, 100, 1, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 5, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    CHECK(strat.empty());
}

TEST_CASE("Paper SET increases size used for later fills") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {2}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 1, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::SET, 100, 3, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 6, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 3);
}

TEST_CASE("Paper SUBTRACT beyond remaining cancels and prevents fills") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {1}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::SUBTRACT, 100, 5, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 6, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    CHECK(strat.empty());
}

TEST_CASE("Paper SET to zero cancels and prevents fills") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {1}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::SET, 100, 0, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 6, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    CHECK(strat.empty());
}

TEST_CASE("MATCH ahead advances paper for later trades") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::MATCH, 100, 3, 1001, 501, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 3, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 1);
}

TEST_CASE("MATCH behind does not advance paper") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::MATCH, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 2, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    CHECK(strat.empty());
}

TEST_CASE("SET ahead increase pushes paper back") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {2}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::SET, 100, 5, 1001, 501, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 4, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    CHECK(strat.empty());
}

TEST_CASE("SET ahead decrease advances paper") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::SET, 100, 1, 1001, 501, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 3, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 2);
}

TEST_CASE("SUBTRACT behind does not advance paper") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::SUBTRACT, 100, 2, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 2, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    CHECK(strat.empty());
}

TEST_CASE("Paper partial fill blocks later paper when volume is insufficient") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {1}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 9002, 902, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(5, 6, Side::BUY, UpdateType::ADD, 100, 10, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 3, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 2);
}

TEST_CASE("Canceling first paper advances second paper") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {1}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 2, 9002, 902, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(5, 6, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(7, 8, Side::BUY, UpdateType::DELETE, 100, 0, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    sink.reset();
    sim.update(make_event(9, 10, Side::SELL, UpdateType::ADD, 100, 3, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9002);
    CHECK(strat[0].qtyLots == 2);
}

TEST_CASE("Increasing first paper delays second paper fills") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {1}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 1, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 2, 9002, 902, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(5, 6, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(7, 8, Side::BUY, UpdateType::SET, 100, 3, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    sink.reset();
    sim.update(make_event(9, 10, Side::SELL, UpdateType::ADD, 100, 3, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 2);
}

TEST_CASE("Decreasing first paper advances second paper fills") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {1}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 3, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 2, 9002, 902, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(5, 6, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(7, 8, Side::BUY, UpdateType::SET, 100, 1, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    sink.reset();
    sim.update(make_event(9, 10, Side::SELL, UpdateType::ADD, 100, 3, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 2);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 1);
    CHECK(strat[1].makerOrderId == 9002);
    CHECK(strat[1].qtyLots == 1);
}

TEST_CASE("Paper ADD with qty 0 is ignored") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {1}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 0, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(5, 6, Side::SELL, UpdateType::ADD, 100, 3, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    CHECK(strat.empty());
}

TEST_CASE("Paper SUBTRACT with negative qty emits diagnostic") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    sim.update(make_event(1, 2, Side::BUY, UpdateType::SUBTRACT, 100, -1, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    assert_last_diag_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_PAPER_ORDER_BY_NEGATIVE_QUANTITY,
                                   DiagnosticRecordSeverity::ERROR);
}

TEST_CASE("Paper SET with negative qty cancels order") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {1}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sim.update(make_event(5, 6, Side::BUY, UpdateType::SET, 100, -5, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 3, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    CHECK(strat.empty());
}

TEST_CASE("Duplicate paper ADD is ignored") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {1}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 3, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(5, 6, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 4, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    CHECK(sum_fill_qty(strat) == 2);
}

TEST_CASE("ensurePaperLevel builds from existing market book") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY, Side::BUY}, {100, 100}, {2, 2}, {1001, 1002}, {501, 502});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    sink.reset();
    sim.update(make_event(3, 4, Side::SELL, UpdateType::ADD, 100, 3, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    CHECK(strat.empty());
}

TEST_CASE("Paper orders only fill at their own price level") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::BUY, Side::BUY}, {101, 100}, {2, 1}, {1001, 1002}, {501, 502});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 2, 1003, 503, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(5, 6, Side::SELL, UpdateType::ADD, 101, 2, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    CHECK(strategy_maker_fills(sink.getFills()).empty());

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 3, 2002, 602, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.getFills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].makerOrderId == 9001);
    CHECK(strat[0].qtyLots == 2);
}

TEST_CASE("L2-seeded liquidity (orderId sentinel) can be consumed and emits fills with sentinel maker") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    // L2 snapshot uses UnknownOrderIdSentinel (-1)
    std::vector<Side> sides{Side::SELL};
    std::vector<std::int64_t> prices{100};
    std::vector<std::int64_t> qtys{5};
    sim.initFromL2Snapshot(sides, prices, qtys);

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 3, 9001, 900, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto& fills = sink.getFills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].makerOrderId == UnknownOrderIdSentinel);
    CHECK(fills[0].qtyLots == 3);

    auto depth = sim.depthAt(Side::SELL, 100);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 2);
}

TEST_CASE("SUBTRACT/DELETE/MATCH targeting L2-seeded sentinel orderId are treated as missing") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    sim.initFromL2Snapshot({Side::SELL}, {101}, {4});

    // SUBTRACT
    sim.update(make_event(1, 2, Side::SELL, UpdateType::SUBTRACT, 101, 2, UnknownOrderIdSentinel,
                          UnknownTraderIdSentinel, NoAggressorNeededSentinel, UpdateSource::HISTORICAL));
    auto depth = sim.depthAt(Side::SELL, 101);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 4); // unchanged

    // DELETE
    sim.update(make_event(3, 4, Side::SELL, UpdateType::DELETE, 101, 0, UnknownOrderIdSentinel, UnknownTraderIdSentinel,
                          NoAggressorNeededSentinel, UpdateSource::HISTORICAL));
    depth = sim.depthAt(Side::SELL, 101);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 4);

    // MATCH
    sim.update(make_event(5, 6, Side::SELL, UpdateType::MATCH, 101, 2, UnknownOrderIdSentinel, UnknownTraderIdSentinel,
                          NoAggressorNeededSentinel, UpdateSource::HISTORICAL));
    depth = sim.depthAt(Side::SELL, 101);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 4);
    CHECK(sink.getFills().empty());
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

TEST_CASE("update with unknown UpdateType emits diagnostic") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    sim.update(make_event(1, 2, Side::BUY, static_cast<UpdateType>(999), 100, 1, 1, 1, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    assert_last_diag_matches_event(sink, DiagnosticRecordCode::INVALID_UPDATE_TYPE, DiagnosticRecordSeverity::ERROR);
}

TEST_CASE("SUBTRACT reduces quantity without emitting fills") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::SELL}, {100}, {10}, {1}, {11});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::SUBTRACT, 100, 4, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    CHECK(sink.getFills().empty());
    auto depth = sim.depthAt(Side::SELL, 100);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 6);
}

TEST_CASE("SUBTRACT removing the top level updates best price despite stale heap") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    // best bid 101, next 99
    seed_l3(sim, {Side::BUY, Side::BUY}, {101, 99}, {5, 7}, {10, 11}, {21, 22});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::SUBTRACT, 101, 5, 10, 21, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    auto best = sim.getBestPriceTicks(Side::BUY);
    REQUIRE(best.has_value());
    CHECK(best.value() == 99);
}

TEST_CASE("SUBTRACT with quantity exceeding liquidity clamps to zero and removes order") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);

    seed_l3(sim, {Side::SELL}, {105}, {3}, {1001}, {501});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::SUBTRACT, 105, 10, 1001, 501, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    CHECK_FALSE(sim.depthAt(Side::SELL, 105).has_value());
    CHECK_FALSE(sim.getBestPriceTicks(Side::SELL).has_value());
}

TEST_CASE("SUBTRACT with qty 0 and negative qty emit diagnostics") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);
    seed_l3(sim, {Side::BUY}, {100}, {4}, {1}, {1});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::SUBTRACT, 100, 0, 1, 1, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    auto depth = sim.depthAt(Side::BUY, 100);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 4);
    assert_last_diag_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY,
                                   DiagnosticRecordSeverity::WARNING);

    sink.reset();
    sim.update(make_event(3, 4, Side::BUY, UpdateType::SUBTRACT, 100, -1, 1, 1, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    assert_last_diag_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_NEGATIVE_QUANTITY,
                                   DiagnosticRecordSeverity::ERROR);
}

TEST_CASE("SUBTRACT on missing orderId does nothing") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);
    seed_l3(sim, {Side::SELL}, {105}, {5}, {10}, {20});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::SUBTRACT, 105, 2, 9999, 123, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    auto depth = sim.depthAt(Side::SELL, 105);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 5);
}

TEST_CASE("DELETE removes order regardless of provided side/price") {
    PaperTradingSimulatorCore sim{};
    seed_l3(sim, {Side::SELL, Side::SELL}, {101, 103}, {2, 4}, {1, 2}, {11, 12});

    // Mismatch side/price but correct orderId
    sim.update(make_event(1, 2, Side::BUY, UpdateType::DELETE, 9999, 0, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    CHECK_FALSE(sim.depthAt(Side::SELL, 101).has_value());
    auto depth = sim.depthAt(Side::SELL, 103);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 4);
}

TEST_CASE("DELETE on missing orderId is a no-op") {
    PaperTradingSimulatorCore sim{};
    seed_l3(sim, {Side::SELL}, {101}, {2}, {1}, {11});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::DELETE, 101, 0, 999, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    auto depth = sim.depthAt(Side::SELL, 101);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 2);
}

TEST_CASE("DELETE of best level leaves heap stale but best price still resolves") {
    PaperTradingSimulatorCore sim{};
    seed_l3(sim, {Side::SELL, Side::SELL}, {100, 101}, {5, 6}, {10, 11}, {20, 21});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::DELETE, 100, 0, 10, 20, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    auto best = sim.getBestPriceTicks(Side::SELL);
    REQUIRE(best.has_value());
    CHECK(best.value() == 101);
}

TEST_CASE("MATCH partially fills passive order and emits fill with maker metadata") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);
    seed_l3(sim, {Side::SELL}, {101}, {5}, {5001}, {9001});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::MATCH, 101, 3, 5001, 9001, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto& fills = sink.getFills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].qtyLots == 3);
    CHECK(fills[0].makerOrderId == 5001);
    CHECK(fills[0].makerTraderId == 9001);
    CHECK(fills[0].makerSource == UpdateSource::HISTORICAL);
    CHECK(fills[0].takerSource == UpdateSource::HISTORICAL);

    auto depth = sim.depthAt(Side::SELL, 101);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 2);
}

TEST_CASE("MATCH over-aggressive qty fills remaining, removes order, and best price moves on") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);
    seed_l3(sim, {Side::BUY, Side::BUY}, {100, 99}, {2, 4}, {1, 2}, {11, 22});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::MATCH, 100, 10, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    const auto& fills = sink.getFills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].qtyLots == 2); // only existing liquidity filled
    CHECK_FALSE(sim.depthAt(Side::BUY, 100).has_value());

    auto best = sim.getBestPriceTicks(Side::BUY);
    REQUIRE(best.has_value());
    CHECK(best.value() == 99);
}

TEST_CASE("MATCH with qty 0/negative qty emit diagnostics; missing orderId ignored") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink;
    sim.setLogSink(&sink);
    seed_l3(sim, {Side::SELL}, {101}, {5}, {10}, {20});

    sink.reset();
    sim.update(make_event(1, 2, Side::SELL, UpdateType::MATCH, 101, 0, 10, 20, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    CHECK(sink.getFills().empty());
    assert_last_diag_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY,
                                   DiagnosticRecordSeverity::WARNING);

    sink.reset();
    sim.update(make_event(1, 2, Side::SELL, UpdateType::MATCH, 101, -1, 10, 20, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    assert_last_diag_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_NEGATIVE_QUANTITY,
                                   DiagnosticRecordSeverity::ERROR);

    sink.reset();
    sim.update(make_event(1, 2, Side::SELL, UpdateType::MATCH, 101, 2, 9999, 20, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    CHECK(sink.getFills().empty());
    auto depth = sim.depthAt(Side::SELL, 101);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 5);
}

TEST_CASE("SET reduces quantity to exact value and can increase it") {
    PaperTradingSimulatorCore sim{};
    seed_l3(sim, {Side::SELL}, {105}, {10}, {1}, {11});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::SET, 105, 4, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    auto depth = sim.depthAt(Side::SELL, 105);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 4);

    sim.update(make_event(3, 4, Side::SELL, UpdateType::SET, 105, 12, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    depth = sim.depthAt(Side::SELL, 105);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 12);
}

TEST_CASE("SET to zero or negative removes order and advances best price") {
    PaperTradingSimulatorCore sim{};
    seed_l3(sim, {Side::SELL, Side::SELL}, {100, 105}, {5, 6}, {1, 2}, {11, 22});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::SET, 100, 0, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    auto best = sim.getBestPriceTicks(Side::SELL);
    REQUIRE(best.has_value());
    CHECK(best.value() == 105);

    sim.update(make_event(3, 4, Side::SELL, UpdateType::SET, 105, -10, 2, 22, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    CHECK_FALSE(sim.getBestPriceTicks(Side::SELL).has_value());
}

TEST_CASE("SET on missing orderId is ignored") {
    PaperTradingSimulatorCore sim{};
    seed_l3(sim, {Side::BUY}, {100}, {5}, {1}, {11});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::SET, 100, 2, 9999, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    auto depth = sim.depthAt(Side::BUY, 100);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 5);
}

TEST_CASE("Diagnostics: duplicate add orderId scenarios") {
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 1, 1, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 1, 1, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID,
                                       DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 99, 1, 2, 11, NoAggressorNeededSentinel,
                              UpdateSource::STRATEGY));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 99, 1, 2, 11, NoAggressorNeededSentinel,
                              UpdateSource::STRATEGY));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID,
                                       DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 98, 1, 3, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 98, 1, 3, 11, NoAggressorNeededSentinel,
                              UpdateSource::STRATEGY));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID,
                                       DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 97, 1, 4, 11, NoAggressorNeededSentinel,
                              UpdateSource::STRATEGY));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 97, 1, 4, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID,
                                       DiagnosticRecordSeverity::WARNING);
    }
}

TEST_CASE("Diagnostics: delete missing order ids") {
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::DELETE, 100, 0, 10, 11, NoAggressorNeededSentinel,
                              UpdateSource::STRATEGY));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::DELETE_NON_EXISTING_PAPER_ORDER_ID,
                                       DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::DELETE, 100, 0, 11, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::DELETE_NON_EXISTING_HISTORICAL_ORDER_ID,
                                       DiagnosticRecordSeverity::WARNING);
    }
}

TEST_CASE("Diagnostics: delete side/price mismatch emits both codes") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink{};
    sim.setLogSink(&sink);
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    sim.update(make_event(3, 4, Side::SELL, UpdateType::DELETE, 101, 0, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto& diags = sink.getDiagnostics();
    REQUIRE(diags.size() == 2);
    const auto& ev = sink.getEvents().back();
    assert_diag_matches_event(diags[0], ev,
                              DiagnosticRecordCode::PROVIDED_SIDE_ON_DELETE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
                              DiagnosticRecordSeverity::WARNING);
    assert_diag_matches_event(diags[1], ev,
                              DiagnosticRecordCode::PROVIDED_PRICE_ON_DELETE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
                              DiagnosticRecordSeverity::WARNING);
}

TEST_CASE("Diagnostics: set negative quantity and set missing order") {
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 1, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::SET, 100, -5, 1, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::SET_WITH_NEGATIVE_LIQUIDITY_REQUESTED_WAS_SET_TO_ZERO,
                                       DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::SET, 200, 5, 999, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::SET_NON_EXISTING_ORDER_ID_IS_REJECTED,
                                       DiagnosticRecordSeverity::WARNING);
    }
}

TEST_CASE("Diagnostics: set side/price mismatch emits both codes") {
    PaperTradingSimulatorCore sim{};
    InMemoryLogSink sink{};
    sim.setLogSink(&sink);
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    sim.update(make_event(3, 4, Side::SELL, UpdateType::SET, 101, 4, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto& diags = sink.getDiagnostics();
    REQUIRE(diags.size() == 2);
    const auto& ev = sink.getEvents().back();
    assert_diag_matches_event(diags[0], ev,
                              DiagnosticRecordCode::PROVIDED_SIDE_ON_SET_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
                              DiagnosticRecordSeverity::WARNING);
    assert_diag_matches_event(diags[1], ev,
                              DiagnosticRecordCode::PROVIDED_PRICE_ON_SET_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
                              DiagnosticRecordSeverity::WARNING);
}

TEST_CASE("Diagnostics: subtract reduce warnings") {
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::SUBTRACT, 100, 0, 1, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY,
                                       DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::SUBTRACT, 100, 1, 2, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_NON_EXISTING_ORDER_ID,
                                       DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 101, 5, 3, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::SELL, UpdateType::SUBTRACT, 102, 1, 3, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        const auto& diags = sink.getDiagnostics();
        REQUIRE(diags.size() == 2);
        const auto& ev = sink.getEvents().back();
        assert_diag_matches_event(diags[0], ev,
                                  DiagnosticRecordCode::PROVIDED_SIDE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
                                  DiagnosticRecordSeverity::WARNING);
        assert_diag_matches_event(diags[1], ev,
                                  DiagnosticRecordCode::PROVIDED_PRICE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
                                  DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 103, 3, 5, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::SUBTRACT, 103, 5, 5, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(
            sink, DiagnosticRecordCode::REQUESTED_ORDER_REDUCE_WITH_VOLUME_LARGER_THAN_AVAILABLE_FOR_ORDER_ID,
            DiagnosticRecordSeverity::WARNING);
    }
}

TEST_CASE("Diagnostics: match reduce warnings") {
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::MATCH, 110, 0, 10, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY,
                                       DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::MATCH, 110, 1, 11, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_NON_EXISTING_ORDER_ID,
                                       DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 110, 5, 12, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::SELL, UpdateType::MATCH, 111, 1, 12, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        const auto& diags = sink.getDiagnostics();
        REQUIRE(diags.size() == 2);
        const auto& ev = sink.getEvents().back();
        assert_diag_matches_event(diags[0], ev,
                                  DiagnosticRecordCode::PROVIDED_SIDE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
                                  DiagnosticRecordSeverity::WARNING);
        assert_diag_matches_event(diags[1], ev,
                                  DiagnosticRecordCode::PROVIDED_PRICE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
                                  DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 113, 3, 14, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::MATCH, 113, 5, 14, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(
            sink, DiagnosticRecordCode::REQUESTED_ORDER_REDUCE_WITH_VOLUME_LARGER_THAN_AVAILABLE_FOR_ORDER_ID,
            DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 114, 3, 15, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::MATCH, 114, 1, 15, 11, NoAggressorNeededSentinel,
                              UpdateSource::STRATEGY));
        assert_last_diag_matches_event(sink,
                                       DiagnosticRecordCode::PAPER_ORDER_INVOKES_PASSIVE_MATCH_INSTEAD_OF_AGGRESSIVE_TRADE,
                                       DiagnosticRecordSeverity::ERROR);
    }
}

TEST_CASE("Diagnostics: corrupt book price triggers on delete/set/subtract/match") {
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        seed_l3(sim, {Side::BUY}, {100}, {5}, {1}, {11});
        sim.bids.erase(100);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::DELETE, 100, 0, 1, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                                       DiagnosticRecordSeverity::ERROR);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        seed_l3(sim, {Side::SELL}, {101}, {5}, {2}, {11});
        sim.asks.erase(101);
        sim.update(make_event(1, 2, Side::SELL, UpdateType::SET, 101, 3, 2, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                                       DiagnosticRecordSeverity::ERROR);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        seed_l3(sim, {Side::BUY}, {102}, {5}, {3}, {11});
        sim.bids.erase(102);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::SUBTRACT, 102, 1, 3, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                                       DiagnosticRecordSeverity::ERROR);
    }
    {
        PaperTradingSimulatorCore sim{};
        InMemoryLogSink sink{};
        sim.setLogSink(&sink);
        seed_l3(sim, {Side::SELL}, {103}, {5}, {4}, {11});
        sim.asks.erase(103);
        sim.update(make_event(1, 2, Side::SELL, UpdateType::MATCH, 103, 1, 4, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diag_matches_event(sink, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                                       DiagnosticRecordSeverity::ERROR);
    }
}
