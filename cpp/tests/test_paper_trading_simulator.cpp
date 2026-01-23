#include "lobsim/in_memory_sink.hpp"
#define private public
#include "lobsim/paper_trading_simulator.hpp"
#undef private

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

static NormalizedLobEvent make_event(std::int64_t tsEx, std::int64_t tsRecv, Side side, UpdateType ut,
                                     std::int64_t price_ticks, std::int64_t qty_lots, std::int64_t order_id,
                                     std::int64_t trader_id = UnknownTraderIdSentinel,
                                     std::int64_t aggressor_id = NoAggressorNeededSentinel,
                                     UpdateSource src = UpdateSource::HISTORICAL) {
    return NormalizedLobEvent{tsEx,     tsRecv,    side,         ut,  price_ticks, qty_lots,
                              order_id, trader_id, aggressor_id, src, ""};
}

static std::int64_t sum_fill_qty(const std::vector<FillRecord>& fills) {
    std::int64_t s = 0;
    for (const auto& f : fills)
        s += f.qty_lots;
    return s;
}

static std::vector<FillRecord> strategy_maker_fills(const std::vector<FillRecord>& fills) {
    std::vector<FillRecord> out;
    for (const auto& f : fills) {
        if (f.maker_source == UpdateSource::STRATEGY) {
            out.push_back(f);
        }
    }
    return out;
}

static const DiagnosticRecord* last_diagnostic(const InMemoryLogSink& sink) {
    const auto& diags = sink.get_diagnostics();
    if (diags.empty()) {
        return nullptr;
    }
    return &diags.back();
}

static void assert_diagnostic_matches_event(const DiagnosticRecord& diag, const EventApplyRecord& ev,
                                            DiagnosticRecordCode code, DiagnosticRecordSeverity severity) {
    CHECK(diag.code == code);
    CHECK(diag.severity == severity);
    CHECK(diag.seq == ev.seq);
    CHECK(diag.ts_exchange == ev.ts_exchange);
    CHECK(diag.ts_received == ev.ts_received);
}

static void assert_last_diagnostic_matches_event(const InMemoryLogSink& sink, DiagnosticRecordCode code,
                                                 DiagnosticRecordSeverity severity) {
    const auto* diag = last_diagnostic(sink);
    REQUIRE(diag != nullptr);
    const auto& events = sink.get_events();
    REQUIRE_FALSE(events.empty());
    const auto& ev = events.back();
    assert_diagnostic_matches_event(*diag, ev, code, severity);
}

void seed_l3(PaperTradingSimulator& sim, const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
             const std::vector<std::int64_t>& qtys, const std::vector<std::int64_t>& order_ids,
             const std::vector<std::int64_t>& trader_ids) {
    sim.init_from_l3_snapshot(sides, prices, qtys, order_ids, trader_ids);
}

TEST_CASE("Valid initialization from L2 snapshot + depth at levels") {
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 123, 131};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};

    PaperTradingSimulator sim{};
    sim.init_from_l2_snapshot(sides, prices, quantities);

    for (int i = 0; i < static_cast<int>(sides.size()); ++i) {
        auto depth = sim.depth_at(sides[i], prices[i]);
        REQUIRE(depth != std::nullopt);
        REQUIRE(depth.value() == quantities[i]);
        auto noDepth = sim.depth_at(sides[i], 345678);
        REQUIRE(noDepth == std::nullopt);
    }
}

TEST_CASE("Duplicate price levels on L2 initialization") {
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 120, 131};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    PaperTradingSimulator sim{};
    REQUIRE_THROWS_AS(sim.init_from_l2_snapshot(sides, prices, quantities), std::runtime_error);
}

TEST_CASE("Different array sizes on L2 initialization") {
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 121, 131, 140};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    PaperTradingSimulator sim{};
    REQUIRE_THROWS_AS(sim.init_from_l2_snapshot(sides, prices, quantities), std::runtime_error);
}

TEST_CASE("Valid initialization from L3 snapshot + depth at levels") {
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 123, 131};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    std::vector<std::int64_t> order_ids{1, 2, 3, 4};
    std::vector<std::int64_t> trader_ids{1, 2, 1, 3};

    PaperTradingSimulator sim{};
    sim.init_from_l3_snapshot(sides, prices, quantities, order_ids, trader_ids);

    for (int i = 0; i < static_cast<int>(sides.size()); ++i) {
        auto depth = sim.depth_at(sides[i], prices[i]);
        REQUIRE(depth != std::nullopt);
        REQUIRE(depth.value() == quantities[i]);
        auto noDepth = sim.depth_at(sides[i], 345678);
        REQUIRE(noDepth == std::nullopt);
    }
}

TEST_CASE("Different array sizes on L3 initialization") {
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 121, 131, 140};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    std::vector<std::int64_t> order_ids{1, 2, 3, 4};
    std::vector<std::int64_t> trader_ids{1, 2, 3, 4};
    PaperTradingSimulator sim{};
    REQUIRE_THROWS_AS(sim.init_from_l3_snapshot(sides, prices, quantities, order_ids, trader_ids), std::runtime_error);
}

TEST_CASE("TopN L2 view") {
    std::vector<Side> sides{Side::BUY, Side::SELL, Side::BUY, Side::SELL};
    std::vector<std::int64_t> prices{120, 129, 123, 131};
    std::vector<std::int64_t> quantities{79, 53, 88, 64};
    PaperTradingSimulator sim{};

    REQUIRE(sim.l2_top_n(Side::BUY, 1).empty());
    REQUIRE(sim.l2_top_n(Side::SELL, 1).empty());
    sim.init_from_l2_snapshot(sides, prices, quantities);

    auto top2Buy = sim.l2_top_n(Side::BUY, 2);
    REQUIRE(static_cast<int>(top2Buy.size()) == 2);
    REQUIRE(top2Buy[0].first == 123);
    REQUIRE(top2Buy[0].second == 88);
    REQUIRE(top2Buy[1].first == 120);
    REQUIRE(top2Buy[1].second == 79);

    auto top2Sell = sim.l2_top_n(Side::SELL, 2);
    REQUIRE(static_cast<int>(top2Sell.size()) == 2);
    REQUIRE(top2Sell[0].first == 129);
    REQUIRE(top2Sell[0].second == 53);
    REQUIRE(top2Sell[1].first == 131);
    REQUIRE(top2Sell[1].second == 64);

    auto top7Sell = sim.l2_top_n(Side::SELL, 7);
    REQUIRE(static_cast<int>(top7Sell.size()) == 2);
}

TEST_CASE("Paper strategy ADD crossing emmits fills but does not mutate book") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    const std::vector<Side> sides{Side::SELL, Side::SELL};
    const std::vector<std::int64_t> prices{105, 105};
    const std::vector<std::int64_t> qtys{6, 4};
    const std::vector<std::int64_t> order_ids{1001, 1002};
    const std::vector<std::int64_t> trader_ids{501, 502};

    sim.init_from_l3_snapshot(sides, prices, qtys, order_ids, trader_ids);

    REQUIRE(sink.get_fills().empty());
    REQUIRE(sink.get_events().empty());

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 110, 8, 9001, 900, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    const auto& fills = sink.get_fills();
    REQUIRE(fills.size() == 2);

    CHECK(fills[0].price_ticks == 105);
    CHECK(fills[0].qty_lots == 6);
    CHECK(fills[0].maker_order_id == 1001);
    CHECK(fills[0].maker_trader_id == 501);
    CHECK(fills[0].taker_order_id == 9001);
    CHECK(fills[0].taker_trader_id == 900);

    CHECK(fills[1].price_ticks == 105);
    CHECK(fills[1].qty_lots == 2);
    CHECK(fills[1].maker_order_id == 1002);
    CHECK(fills[1].maker_trader_id == 502);

    // 2) Paper mode must NOT mutate the historical book
    // total ask liquidity at 105 should remain 10
    auto depth = sim.depth_at(Side::SELL, 105);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 10);
}

TEST_CASE("Historical crossing ADD consumes book and emits fills") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    const std::vector<Side> sides{Side::SELL, Side::SELL};
    const std::vector<std::int64_t> prices{100, 101};
    const std::vector<std::int64_t> qtys{5, 5};
    const std::vector<std::int64_t> order_ids{2001, 2002};
    const std::vector<std::int64_t> trader_ids{601, 602};
    sim.init_from_l3_snapshot(sides, prices, qtys, order_ids, trader_ids);

    // Historical BUY crosses: BUY 7 @ 101 (should take 5@100 + 2@101) and rest 0 (since fully filled)
    sim.update(make_event(10, 11, Side::BUY, UpdateType::ADD, 101, 7, 3001, 700, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto& fills = sink.get_fills();
    REQUIRE(fills.size() == 2);
    CHECK(fills[0].price_ticks == 100);
    CHECK(fills[0].qty_lots == 5);
    CHECK(fills[1].price_ticks == 101);
    CHECK(fills[1].qty_lots == 2);

    // Now remaining ask depth: 0 @ 100 (gone), 3 @ 101
    CHECK_FALSE(sim.depth_at(Side::SELL, 100).has_value());
    auto d101 = sim.depth_at(Side::SELL, 101);
    REQUIRE(d101.has_value());
    CHECK(d101.value() == 3);
}

TEST_CASE("HISTORICAL ADD non-crossing to empty book rests order, no fills") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {}, {}, {}, {}, {});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 10, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    CHECK(sink.get_fills().empty());

    auto d = sim.depth_at(Side::BUY, 100);
    REQUIRE(d.has_value());
    CHECK(d.value() == 10);
}

TEST_CASE("STRATEGY ADD non-crossing does nothing (no fills, no resting, no mutation)") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {Side::SELL}, {105}, {10}, {1001}, {501});

    // Strategy BUY below best ask -> no cross -> paper returns without inserting
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 9001, 900, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    CHECK(sink.get_fills().empty());

    // Book unchanged
    auto d = sim.depth_at(Side::SELL, 105);
    REQUIRE(d.has_value());
    CHECK(d.value() == 10);
    CHECK_FALSE(sim.depth_at(Side::BUY, 100).has_value());
}

TEST_CASE("STRATEGY ADD crossing emits FIFO fills and does not mutate book") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {Side::SELL, Side::SELL}, {105, 105}, {6, 4}, {1001, 1002}, {501, 502});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 110, 8, 9001, 900, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    const auto& fills = sink.get_fills();
    REQUIRE(fills.size() == 2);

    // FIFO: maker 1001 then 1002
    CHECK(fills[0].price_ticks == 105);
    CHECK(fills[0].qty_lots == 6);
    CHECK(fills[0].maker_order_id == 1001);
    CHECK(fills[0].maker_trader_id == 501);
    CHECK(fills[0].taker_order_id == 9001);
    CHECK(fills[0].taker_trader_id == 900);
    CHECK(fills[0].taker_source == UpdateSource::STRATEGY);

    CHECK(fills[1].price_ticks == 105);
    CHECK(fills[1].qty_lots == 2);
    CHECK(fills[1].maker_order_id == 1002);
    CHECK(fills[1].maker_trader_id == 502);

    // Book unchanged (still 10 @105)
    auto d = sim.depth_at(Side::SELL, 105);
    REQUIRE(d.has_value());
    CHECK(d.value() == 10);
}

TEST_CASE("HISTORICAL ADD crossing partially fills and rests remainder on own side") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {Side::SELL}, {105}, {4}, {1001}, {501});

    // BUY 10 @110 consumes 4 @105, rests 6 @110
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 110, 10, 9001, 900, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto& fills = sink.get_fills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].price_ticks == 105);
    CHECK(fills[0].qty_lots == 4);
    CHECK(fills[0].maker_order_id == 1001);
    CHECK(fills[0].taker_order_id == 9001);
    CHECK(fills[0].taker_source == UpdateSource::HISTORICAL);

    CHECK_FALSE(sim.depth_at(Side::SELL, 105).has_value());

    auto d = sim.depth_at(Side::BUY, 110);
    REQUIRE(d.has_value());
    CHECK(d.value() == 6);
}

TEST_CASE("HISTORICAL ADD crossing fully fills and does not rest") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {Side::SELL, Side::SELL}, {105, 105}, {6, 4}, {1001, 1002}, {501, 502});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 110, 10, 9001, 900, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto& fills = sink.get_fills();
    REQUIRE(fills.size() == 2);
    CHECK(sum_fill_qty(fills) == 10);

    CHECK_FALSE(sim.depth_at(Side::SELL, 105).has_value());
    CHECK_FALSE(sim.depth_at(Side::BUY, 110).has_value());
}

TEST_CASE("HISTORICAL ADD crossing consumes multiple levels in price priority") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    // asks: 3@100, 4@101, 5@103
    seed_l3(sim, {Side::SELL, Side::SELL, Side::SELL}, {100, 101, 103}, {3, 4, 5}, {2001, 2002, 2003}, {601, 602, 603});

    // BUY 10 @103 => 3@100 + 4@101 + 3@103
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, /*price*/ 103, /*qty*/ 10,
                          /*order_id*/ 9001, /*trader_id*/ 900, NoAggressorNeededSentinel, UpdateSource::HISTORICAL));

    const auto& fills = sink.get_fills();
    REQUIRE(sum_fill_qty(fills) == 10);

    // First fill price should be best ask (100), then 101, then 103
    REQUIRE(fills.size() >= 3);
    CHECK(fills[0].price_ticks == 100);
    CHECK(fills[1].price_ticks == 101);
    CHECK(fills.back().price_ticks == 103);

    CHECK_FALSE(sim.depth_at(Side::SELL, 100).has_value());
    CHECK_FALSE(sim.depth_at(Side::SELL, 101).has_value());

    auto d103 = sim.depth_at(Side::SELL, 103);
    REQUIRE(d103.has_value());
    CHECK(d103.value() == 2); // 5 - 3
}

TEST_CASE("FIFO within a level: older resting order is consumed first") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    // Two bids at same price, FIFO by insertion order
    seed_l3(sim, {Side::BUY, Side::BUY}, {100, 100}, {5, 7}, {3001, 3002}, {701, 702});

    // SELL 6 @90 crosses best bid (100). Should take 5 from 3001 then 1 from 3002
    sim.update(make_event(1, 2, Side::SELL, UpdateType::ADD, 90, 6, 4001, 800, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto& fills = sink.get_fills();
    REQUIRE(fills.size() == 2);

    CHECK(fills[0].maker_order_id == 3001);
    CHECK(fills[0].qty_lots == 5);
    CHECK(fills[1].maker_order_id == 3002);
    CHECK(fills[1].qty_lots == 1);
}

TEST_CASE("ADD with duplicate order_id is ignored (no fills, no state change)") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);
    seed_l3(sim, {Side::SELL}, {105}, {10}, {1001}, {501});
    // Attempt ADD with same order_id (should early-return)
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 110, 5, 1001, 900, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    CHECK(sink.get_fills().empty());

    auto d = sim.depth_at(Side::SELL, 105);
    REQUIRE(d.has_value());
    CHECK(d.value() == 10);
}

TEST_CASE("ADD with qty=0 does nothing (no fills, no state change)") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);
    seed_l3(sim, {}, {}, {}, {}, {});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 0, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    CHECK(sink.get_fills().empty());
    CHECK_FALSE(sim.depth_at(Side::BUY, 100).has_value());
}

TEST_CASE("ADD with negative quantity emits diagnostic") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);
    seed_l3(sim, {}, {}, {}, {}, {});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, -5, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::ADD_INVOKED_WITH_NEGATIVE_QUANTITY,
                                         DiagnosticRecordSeverity::ERROR);
}

TEST_CASE("Stale heap entry is popped and does not prevent matching next level") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto& fills = sink.get_fills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].price_ticks == 101);
    CHECK(fills[0].qty_lots == 2);
}

TEST_CASE("Paper sweep does not double-count liquidity when heap has duplicate price entries") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto& fills = sink.get_fills();
    CHECK(sum_fill_qty(fills) == 5);

    // Book unchanged by paper mode (still 5 @100)
    auto d = sim.depth_at(Side::SELL, 100);
    REQUIRE(d.has_value());
    CHECK(d.value() == 5);
}

TEST_CASE("Paper order fills after trades exceed market ahead") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 10, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(5, 6, Side::SELL, UpdateType::ADD, 100, 12, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 2);
}

TEST_CASE("Multiple paper orders fill FIFO when trade volume is sufficient") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 2);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 1);
    CHECK(strat[1].maker_order_id == 9002);
    CHECK(strat[1].qty_lots == 1);
}

TEST_CASE("Resting strategy order fills when future historical orders cross its price") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {}, {}, {}, {}, {});

    // Strategy BUY rests (book is empty, so no crossing at insert time)
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 10, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    CHECK(sink.get_fills().empty());

    // Later historical SELL arrives below the resting bid -> should trade against the paper order
    sim.update(make_event(3, 4, Side::SELL, UpdateType::ADD, 95, 4, 1001, 10001, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    // Another historical SELL at the bid price finishes the remainder
    sim.update(make_event(5, 6, Side::SELL, UpdateType::ADD, 100, 6, 1002, 10002, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto& fills = sink.get_fills();
    REQUIRE(fills.size() == 2);
    CHECK(sum_fill_qty(fills) == 10);

    // Both fills should show strategy as maker (passive) and historical as taker (aggressor)
    CHECK(fills[0].maker_order_id == 9001);
    CHECK(fills[0].maker_source == UpdateSource::STRATEGY);
    CHECK(fills[0].taker_source == UpdateSource::HISTORICAL);
    // Trades occur at the resting (maker) price, i.e., the paper bid of 100
    CHECK(fills[0].price_ticks == 100);
    CHECK(fills[0].qty_lots == 4);

    CHECK(fills[1].maker_order_id == 9001);
    CHECK(fills[1].maker_source == UpdateSource::STRATEGY);
    CHECK(fills[1].taker_source == UpdateSource::HISTORICAL);
    CHECK(fills[1].price_ticks == 100);
    CHECK(fills[1].qty_lots == 6);

    const auto& ledger = sink.get_paper_ledger();
    auto it = ledger.find(9001);
    REQUIRE(it != ledger.end());
    CHECK(it->second.state.status == PaperOrderLedgerStatus::FILLED);
    CHECK(it->second.state.remaining_qty == 0);
}

TEST_CASE("Strategy aggressive trade sweeps historical book without resting") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    // asks: 3@100 (id 1001), 4@101 (id 1002)
    seed_l3(sim, {Side::SELL, Side::SELL}, {100, 101}, {3, 4}, {1001, 1002}, {501, 502});

    // Strategy AGGRESSIVE_TRADE BUY qty=5 (no price specified)
    auto ev = make_event(1, 2, Side::BUY, UpdateType::AGGRESSIVE_TRADE, 0, 5, 9001, 900, NoAggressorNeededSentinel,
                         UpdateSource::STRATEGY);
    sim.update(ev);

    const auto& fills = sink.get_fills();
    REQUIRE(fills.size() == 2);
    CHECK(sum_fill_qty(fills) == 5);

    // Should fill 3 @100 then 2 @101; book not mutated
    CHECK(fills[0].price_ticks == 100);
    CHECK(fills[0].qty_lots == 3);
    CHECK(fills[1].price_ticks == 101);
    CHECK(fills[1].qty_lots == 2);

    auto d100 = sim.depth_at(Side::SELL, 100);
    REQUIRE(d100.has_value());
    CHECK(d100.value() == 3);
    auto d101 = sim.depth_at(Side::SELL, 101);
    REQUIRE(d101.has_value());
    CHECK(d101.value() == 4);
}

TEST_CASE("Canceling a market order behind paper does not advance paper") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    CHECK(strat.empty());
}

TEST_CASE("Canceling a market order ahead advances paper") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 2);
}

TEST_CASE("Paper partial fill persists and completes on later trades") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 3, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 1, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(5, 6, Side::SELL, UpdateType::ADD, 100, 6, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].qty_lots == 1);

    sim.update(make_event(7, 8, Side::BUY, UpdateType::ADD, 100, 2, 1003, 503, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    sink.reset();
    sim.update(make_event(9, 10, Side::SELL, UpdateType::ADD, 100, 2, 2002, 602, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].qty_lots == 2);
    CHECK(strat[0].maker_order_id == 9001);
}

TEST_CASE("Paper SUBTRACT reduces size used for future fills") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 2);
}

TEST_CASE("Paper DELETE removes order and prevents future fills") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    CHECK(strat.empty());
}

TEST_CASE("Paper fills work on the ask side too") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {Side::SELL}, {100}, {5}, {1001}, {501});
    sim.update(make_event(1, 2, Side::SELL, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::SELL, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(5, 6, Side::BUY, UpdateType::ADD, 100, 12, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 2);
}

TEST_CASE("Paper order ahead of later market orders fills at an initially empty level") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {}, {}, {}, {}, {});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1001, 501, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(5, 6, Side::SELL, UpdateType::ADD, 100, 3, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 2);
}

TEST_CASE("Paper orders respect interleaved market orders in queue priority") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 2);
}

TEST_CASE("Market SUBTRACT ahead advances paper while preserving priority") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 2);
}

TEST_CASE("Market SET behind does not advance paper") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    CHECK(strat.empty());
}

TEST_CASE("Paper SET increases size used for later fills") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 3);
}

TEST_CASE("Paper SUBTRACT beyond remaining cancels and prevents fills") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    CHECK(strat.empty());
}

TEST_CASE("Paper SET to zero cancels and prevents fills") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    CHECK(strat.empty());
}

TEST_CASE("MATCH ahead advances paper for later trades") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 1);
}

TEST_CASE("MATCH behind does not advance paper") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    CHECK(strat.empty());
}

TEST_CASE("SET ahead increase pushes paper back") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    CHECK(strat.empty());
}

TEST_CASE("SET ahead decrease advances paper") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 2);
}

TEST_CASE("SUBTRACT behind does not advance paper") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    CHECK(strat.empty());
}

TEST_CASE("Paper partial fill blocks later paper when volume is insufficient") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 2);
}

TEST_CASE("Canceling first paper advances second paper") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9002);
    CHECK(strat[0].qty_lots == 2);
}

TEST_CASE("Increasing first paper delays second paper fills") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 2);
}

TEST_CASE("Decreasing first paper advances second paper fills") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 2);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 1);
    CHECK(strat[1].maker_order_id == 9002);
    CHECK(strat[1].qty_lots == 1);
}

TEST_CASE("Paper ADD with qty 0 is ignored") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {Side::BUY}, {100}, {1}, {1001}, {501});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 0, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 5, 1002, 502, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(5, 6, Side::SELL, UpdateType::ADD, 100, 3, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.get_fills());
    CHECK(strat.empty());
}

TEST_CASE("Paper SUBTRACT with negative qty emits diagnostic") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    sim.update(make_event(1, 2, Side::BUY, UpdateType::SUBTRACT, 100, -1, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_PAPER_ORDER_BY_NEGATIVE_QUANTITY,
                                         DiagnosticRecordSeverity::ERROR);
}

TEST_CASE("Paper SET with negative qty cancels order") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    CHECK(strat.empty());
}

TEST_CASE("Duplicate paper ADD is ignored") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

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

    const auto strat = strategy_maker_fills(sink.get_fills());
    CHECK(sum_fill_qty(strat) == 2);
}

TEST_CASE("ensure_paper_level builds from existing market book") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {Side::BUY, Side::BUY}, {100, 100}, {2, 2}, {1001, 1002}, {501, 502});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    sink.reset();
    sim.update(make_event(3, 4, Side::SELL, UpdateType::ADD, 100, 3, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.get_fills());
    CHECK(strat.empty());
}

TEST_CASE("Paper orders only fill at their own price level") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {Side::BUY, Side::BUY}, {101, 100}, {2, 1}, {1001, 1002}, {501, 502});
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 2, 9001, 901, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));
    sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 2, 1003, 503, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    sink.reset();
    sim.update(make_event(5, 6, Side::SELL, UpdateType::ADD, 101, 2, 2001, 601, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    CHECK(strategy_maker_fills(sink.get_fills()).empty());

    sink.reset();
    sim.update(make_event(7, 8, Side::SELL, UpdateType::ADD, 100, 3, 2002, 602, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto strat = strategy_maker_fills(sink.get_fills());
    REQUIRE(strat.size() == 1);
    CHECK(strat[0].maker_order_id == 9001);
    CHECK(strat[0].qty_lots == 2);
}

TEST_CASE("L2-seeded liquidity (order_id sentinel) can be consumed and emits fills with sentinel maker") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    // L2 snapshot uses UnknownOrderIdSentinel (-1)
    std::vector<Side> sides{Side::SELL};
    std::vector<std::int64_t> prices{100};
    std::vector<std::int64_t> qtys{5};
    sim.init_from_l2_snapshot(sides, prices, qtys);

    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 3, 9001, 900, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto& fills = sink.get_fills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].maker_order_id == UnknownOrderIdSentinel);
    CHECK(fills[0].qty_lots == 3);

    auto depth = sim.depth_at(Side::SELL, 100);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 2);
}

TEST_CASE("SUBTRACT/DELETE/MATCH targeting L2-seeded sentinel order_id are treated as missing") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    sim.init_from_l2_snapshot({Side::SELL}, {101}, {4});

    // SUBTRACT
    sim.update(make_event(1, 2, Side::SELL, UpdateType::SUBTRACT, 101, 2, UnknownOrderIdSentinel,
                          UnknownTraderIdSentinel, NoAggressorNeededSentinel, UpdateSource::HISTORICAL));
    auto depth = sim.depth_at(Side::SELL, 101);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 4); // unchanged

    // DELETE
    sim.update(make_event(3, 4, Side::SELL, UpdateType::DELETE, 101, 0, UnknownOrderIdSentinel, UnknownTraderIdSentinel,
                          NoAggressorNeededSentinel, UpdateSource::HISTORICAL));
    depth = sim.depth_at(Side::SELL, 101);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 4);

    // MATCH
    sim.update(make_event(5, 6, Side::SELL, UpdateType::MATCH, 101, 2, UnknownOrderIdSentinel, UnknownTraderIdSentinel,
                          NoAggressorNeededSentinel, UpdateSource::HISTORICAL));
    depth = sim.depth_at(Side::SELL, 101);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 4);
    CHECK(sink.get_fills().empty());
}

TEST_CASE("init_from_l3_snapshot throws on duplicate order_id") {
    PaperTradingSimulator sim{};

    const std::vector<Side> sides{Side::SELL, Side::SELL};
    const std::vector<std::int64_t> prices{100, 101};
    const std::vector<std::int64_t> qtys{1, 1};
    const std::vector<std::int64_t> order_ids{1, 1}; // duplicate
    const std::vector<std::int64_t> trader_ids{10, 11};

    REQUIRE_THROWS_AS(sim.init_from_l3_snapshot(sides, prices, qtys, order_ids, trader_ids), std::runtime_error);
}

TEST_CASE("update with unknown UpdateType emits diagnostic") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    sim.update(make_event(1, 2, Side::BUY, static_cast<UpdateType>(999), 100, 1, 1, 1, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::INVALID_UPDATE_TYPE,
                                         DiagnosticRecordSeverity::ERROR);
}

TEST_CASE("SUBTRACT reduces quantity without emitting fills") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {Side::SELL}, {100}, {10}, {1}, {11});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::SUBTRACT, 100, 4, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    CHECK(sink.get_fills().empty());
    auto depth = sim.depth_at(Side::SELL, 100);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 6);
}

TEST_CASE("SUBTRACT removing the top level updates best price despite stale heap") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    // best bid 101, next 99
    seed_l3(sim, {Side::BUY, Side::BUY}, {101, 99}, {5, 7}, {10, 11}, {21, 22});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::SUBTRACT, 101, 5, 10, 21, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    auto best = sim.get_best_price_ticks(Side::BUY);
    REQUIRE(best.has_value());
    CHECK(best.value() == 99);
}

TEST_CASE("SUBTRACT with quantity exceeding liquidity clamps to zero and removes order") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);

    seed_l3(sim, {Side::SELL}, {105}, {3}, {1001}, {501});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::SUBTRACT, 105, 10, 1001, 501, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    CHECK_FALSE(sim.depth_at(Side::SELL, 105).has_value());
    CHECK_FALSE(sim.get_best_price_ticks(Side::SELL).has_value());
}

TEST_CASE("SUBTRACT with qty 0 and negative qty emit diagnostics") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);
    seed_l3(sim, {Side::BUY}, {100}, {4}, {1}, {1});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::SUBTRACT, 100, 0, 1, 1, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    auto depth = sim.depth_at(Side::BUY, 100);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 4);
    assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY,
                                         DiagnosticRecordSeverity::WARNING);

    sink.reset();
    sim.update(make_event(3, 4, Side::BUY, UpdateType::SUBTRACT, 100, -1, 1, 1, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_NEGATIVE_QUANTITY,
                                         DiagnosticRecordSeverity::ERROR);
}

TEST_CASE("SUBTRACT on missing order_id does nothing") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);
    seed_l3(sim, {Side::SELL}, {105}, {5}, {10}, {20});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::SUBTRACT, 105, 2, 9999, 123, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    auto depth = sim.depth_at(Side::SELL, 105);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 5);
}

TEST_CASE("DELETE removes order regardless of provided side/price") {
    PaperTradingSimulator sim{};
    seed_l3(sim, {Side::SELL, Side::SELL}, {101, 103}, {2, 4}, {1, 2}, {11, 12});

    // Mismatch side/price but correct order_id
    sim.update(make_event(1, 2, Side::BUY, UpdateType::DELETE, 9999, 0, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    CHECK_FALSE(sim.depth_at(Side::SELL, 101).has_value());
    auto depth = sim.depth_at(Side::SELL, 103);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 4);
}

TEST_CASE("DELETE on missing order_id is a no-op") {
    PaperTradingSimulator sim{};
    seed_l3(sim, {Side::SELL}, {101}, {2}, {1}, {11});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::DELETE, 101, 0, 999, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    auto depth = sim.depth_at(Side::SELL, 101);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 2);
}

TEST_CASE("DELETE of best level leaves heap stale but best price still resolves") {
    PaperTradingSimulator sim{};
    seed_l3(sim, {Side::SELL, Side::SELL}, {100, 101}, {5, 6}, {10, 11}, {20, 21});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::DELETE, 100, 0, 10, 20, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    auto best = sim.get_best_price_ticks(Side::SELL);
    REQUIRE(best.has_value());
    CHECK(best.value() == 101);
}

TEST_CASE("MATCH partially fills passive order and emits fill with maker metadata") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);
    seed_l3(sim, {Side::SELL}, {101}, {5}, {5001}, {9001});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::MATCH, 101, 3, 5001, 9001, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto& fills = sink.get_fills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].qty_lots == 3);
    CHECK(fills[0].maker_order_id == 5001);
    CHECK(fills[0].maker_trader_id == 9001);
    CHECK(fills[0].maker_source == UpdateSource::HISTORICAL);
    CHECK(fills[0].taker_source == UpdateSource::HISTORICAL);

    auto depth = sim.depth_at(Side::SELL, 101);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 2);
}

TEST_CASE("MATCH over-aggressive qty fills remaining, removes order, and best price moves on") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);
    seed_l3(sim, {Side::BUY, Side::BUY}, {100, 99}, {2, 4}, {1, 2}, {11, 22});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::MATCH, 100, 10, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::STRATEGY));

    const auto& fills = sink.get_fills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].qty_lots == 2); // only existing liquidity filled
    CHECK_FALSE(sim.depth_at(Side::BUY, 100).has_value());

    auto best = sim.get_best_price_ticks(Side::BUY);
    REQUIRE(best.has_value());
    CHECK(best.value() == 99);
}

TEST_CASE("MATCH with qty 0/negative qty emit diagnostics; missing order_id ignored") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink;
    sim.set_log_sink(&sink);
    seed_l3(sim, {Side::SELL}, {101}, {5}, {10}, {20});

    sink.reset();
    sim.update(make_event(1, 2, Side::SELL, UpdateType::MATCH, 101, 0, 10, 20, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    CHECK(sink.get_fills().empty());
    assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY,
                                         DiagnosticRecordSeverity::WARNING);

    sink.reset();
    sim.update(make_event(1, 2, Side::SELL, UpdateType::MATCH, 101, -1, 10, 20, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_NEGATIVE_QUANTITY,
                                         DiagnosticRecordSeverity::ERROR);

    sink.reset();
    sim.update(make_event(1, 2, Side::SELL, UpdateType::MATCH, 101, 2, 9999, 20, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    CHECK(sink.get_fills().empty());
    auto depth = sim.depth_at(Side::SELL, 101);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 5);
}

TEST_CASE("SET reduces quantity to exact value and can increase it") {
    PaperTradingSimulator sim{};
    seed_l3(sim, {Side::SELL}, {105}, {10}, {1}, {11});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::SET, 105, 4, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    auto depth = sim.depth_at(Side::SELL, 105);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 4);

    sim.update(make_event(3, 4, Side::SELL, UpdateType::SET, 105, 12, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    depth = sim.depth_at(Side::SELL, 105);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 12);
}

TEST_CASE("SET to zero or negative removes order and advances best price") {
    PaperTradingSimulator sim{};
    seed_l3(sim, {Side::SELL, Side::SELL}, {100, 105}, {5, 6}, {1, 2}, {11, 22});

    sim.update(make_event(1, 2, Side::SELL, UpdateType::SET, 100, 0, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    auto best = sim.get_best_price_ticks(Side::SELL);
    REQUIRE(best.has_value());
    CHECK(best.value() == 105);

    sim.update(make_event(3, 4, Side::SELL, UpdateType::SET, 105, -10, 2, 22, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    CHECK_FALSE(sim.get_best_price_ticks(Side::SELL).has_value());
}

TEST_CASE("SET on missing order_id is ignored") {
    PaperTradingSimulator sim{};
    seed_l3(sim, {Side::BUY}, {100}, {5}, {1}, {11});

    sim.update(make_event(1, 2, Side::BUY, UpdateType::SET, 100, 2, 9999, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    auto depth = sim.depth_at(Side::BUY, 100);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 5);
}

TEST_CASE("Diagnostics: duplicate add order_id scenarios") {
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 1, 1, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 100, 1, 1, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID,
                                             DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 99, 1, 2, 11, NoAggressorNeededSentinel,
                              UpdateSource::STRATEGY));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 99, 1, 2, 11, NoAggressorNeededSentinel,
                              UpdateSource::STRATEGY));
        assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID,
                                             DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 98, 1, 3, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 98, 1, 3, 11, NoAggressorNeededSentinel,
                              UpdateSource::STRATEGY));
        assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID,
                                             DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 97, 1, 4, 11, NoAggressorNeededSentinel,
                              UpdateSource::STRATEGY));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::ADD, 97, 1, 4, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID,
                                             DiagnosticRecordSeverity::WARNING);
    }
}

TEST_CASE("Diagnostics: delete missing order ids") {
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::DELETE, 100, 0, 10, 11, NoAggressorNeededSentinel,
                              UpdateSource::STRATEGY));
        assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::DELETE_NON_EXISTING_PAPER_ORDER_ID,
                                             DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::DELETE, 100, 0, 11, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::DELETE_NON_EXISTING_HISTORICAL_ORDER_ID,
                                             DiagnosticRecordSeverity::WARNING);
    }
}

TEST_CASE("Diagnostics: delete side/price mismatch emits both codes") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink{};
    sim.set_log_sink(&sink);
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    sim.update(make_event(3, 4, Side::SELL, UpdateType::DELETE, 101, 0, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto& diags = sink.get_diagnostics();
    REQUIRE(diags.size() == 2);
    const auto& ev = sink.get_events().back();
    assert_diagnostic_matches_event(
        diags[0], ev, DiagnosticRecordCode::PROVIDED_SIDE_ON_DELETE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
        DiagnosticRecordSeverity::WARNING);
    assert_diagnostic_matches_event(
        diags[1], ev, DiagnosticRecordCode::PROVIDED_PRICE_ON_DELETE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
        DiagnosticRecordSeverity::WARNING);
}

TEST_CASE("Diagnostics: set negative quantity and set missing order") {
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 1, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::SET, 100, -5, 1, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(
            sink, DiagnosticRecordCode::SET_WITH_NEGATIVE_LIQUIDITY_REQUESTED_WAS_SET_TO_ZERO,
            DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::SET, 200, 5, 999, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::SET_NON_EXISTING_ORDER_ID_IS_REJECTED,
                                             DiagnosticRecordSeverity::WARNING);
    }
}

TEST_CASE("Diagnostics: set side/price mismatch emits both codes") {
    PaperTradingSimulator sim{};
    InMemoryLogSink sink{};
    sim.set_log_sink(&sink);
    sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 100, 5, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));
    sim.update(make_event(3, 4, Side::SELL, UpdateType::SET, 101, 4, 1, 11, NoAggressorNeededSentinel,
                          UpdateSource::HISTORICAL));

    const auto& diags = sink.get_diagnostics();
    REQUIRE(diags.size() == 2);
    const auto& ev = sink.get_events().back();
    assert_diagnostic_matches_event(diags[0], ev,
                                    DiagnosticRecordCode::PROVIDED_SIDE_ON_SET_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
                                    DiagnosticRecordSeverity::WARNING);
    assert_diagnostic_matches_event(
        diags[1], ev, DiagnosticRecordCode::PROVIDED_PRICE_ON_SET_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
        DiagnosticRecordSeverity::WARNING);
}

TEST_CASE("Diagnostics: subtract reduce warnings") {
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::SUBTRACT, 100, 0, 1, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY,
                                             DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::SUBTRACT, 100, 1, 2, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_NON_EXISTING_ORDER_ID,
                                             DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 101, 5, 3, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::SELL, UpdateType::SUBTRACT, 102, 1, 3, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        const auto& diags = sink.get_diagnostics();
        REQUIRE(diags.size() == 2);
        const auto& ev = sink.get_events().back();
        assert_diagnostic_matches_event(
            diags[0], ev, DiagnosticRecordCode::PROVIDED_SIDE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
            DiagnosticRecordSeverity::WARNING);
        assert_diagnostic_matches_event(
            diags[1], ev, DiagnosticRecordCode::PROVIDED_PRICE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
            DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 103, 3, 5, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::SUBTRACT, 103, 5, 5, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(
            sink, DiagnosticRecordCode::REQUESTED_ORDER_REDUCE_WITH_VOLUME_LARGER_THAN_AVAILABLE_FOR_ORDER_ID,
            DiagnosticRecordSeverity::WARNING);
    }
}

TEST_CASE("Diagnostics: match reduce warnings") {
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::MATCH, 110, 0, 10, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY,
                                             DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::MATCH, 110, 1, 11, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(sink, DiagnosticRecordCode::REQUESTED_REDUCE_NON_EXISTING_ORDER_ID,
                                             DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 110, 5, 12, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::SELL, UpdateType::MATCH, 111, 1, 12, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        const auto& diags = sink.get_diagnostics();
        REQUIRE(diags.size() == 2);
        const auto& ev = sink.get_events().back();
        assert_diagnostic_matches_event(
            diags[0], ev, DiagnosticRecordCode::PROVIDED_SIDE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
            DiagnosticRecordSeverity::WARNING);
        assert_diagnostic_matches_event(
            diags[1], ev, DiagnosticRecordCode::PROVIDED_PRICE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
            DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 113, 3, 14, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::MATCH, 113, 5, 14, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(
            sink, DiagnosticRecordCode::REQUESTED_ORDER_REDUCE_WITH_VOLUME_LARGER_THAN_AVAILABLE_FOR_ORDER_ID,
            DiagnosticRecordSeverity::WARNING);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::ADD, 114, 3, 15, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        sim.update(make_event(3, 4, Side::BUY, UpdateType::MATCH, 114, 1, 15, 11, NoAggressorNeededSentinel,
                              UpdateSource::STRATEGY));
        assert_last_diagnostic_matches_event(
            sink, DiagnosticRecordCode::PAPER_ORDER_INVOKES_PASSIVE_MATCH_INSTEAD_OF_AGGRESSIVE_TRADE,
            DiagnosticRecordSeverity::ERROR);
    }
}

TEST_CASE("Diagnostics: corrupt book price triggers on delete/set/subtract/match") {
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        seed_l3(sim, {Side::BUY}, {100}, {5}, {1}, {11});
        sim.bids.erase(100);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::DELETE, 100, 0, 1, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(sink,
                                             DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                                             DiagnosticRecordSeverity::ERROR);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        seed_l3(sim, {Side::SELL}, {101}, {5}, {2}, {11});
        sim.asks.erase(101);
        sim.update(make_event(1, 2, Side::SELL, UpdateType::SET, 101, 3, 2, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(sink,
                                             DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                                             DiagnosticRecordSeverity::ERROR);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        seed_l3(sim, {Side::BUY}, {102}, {5}, {3}, {11});
        sim.bids.erase(102);
        sim.update(make_event(1, 2, Side::BUY, UpdateType::SUBTRACT, 102, 1, 3, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(sink,
                                             DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                                             DiagnosticRecordSeverity::ERROR);
    }
    {
        PaperTradingSimulator sim{};
        InMemoryLogSink sink{};
        sim.set_log_sink(&sink);
        seed_l3(sim, {Side::SELL}, {103}, {5}, {4}, {11});
        sim.asks.erase(103);
        sim.update(make_event(1, 2, Side::SELL, UpdateType::MATCH, 103, 1, 4, 11, NoAggressorNeededSentinel,
                              UpdateSource::HISTORICAL));
        assert_last_diagnostic_matches_event(sink,
                                             DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                                             DiagnosticRecordSeverity::ERROR);
    }
}
