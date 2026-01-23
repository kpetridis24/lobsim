#include "lobsim/book_id.hpp"
#include "lobsim/in_memory_sink.hpp"
#include "lobsim/lob_event.hpp"
#include "lobsim/multi_log_sink.hpp"
#include "lobsim/multibook_simulator.hpp"
#include "lobsim/paper_trading_simulator.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace {

NormalizedLobEvent make_event() {
    NormalizedLobEvent ev{};
    ev.ts_exchange = 1;
    ev.ts_received = 1;
    ev.side = Side::BUY;
    ev.update_type = UpdateType::ADD;
    ev.price_ticks = 100;
    ev.quantity_lots = 10;
    ev.order_id = 123;
    ev.trader_id = UnknownTraderIdSentinel;
    ev.aggressor_id = NoAggressorNeededSentinel;
    ev.update_source = UpdateSource::HISTORICAL;
    ev.symbol_id = "venue:symbol";
    return ev;
}

void touch_record_fields() {
    FillRecord fill{};
    fill.seq = 0;
    fill.ts_exchange = 0;
    fill.ts_received = 0;
    fill.price_ticks = 0;
    fill.qty_lots = 0;
    fill.maker_side = Side::BUY;
    fill.maker_order_id = 0;
    fill.maker_trader_id = 0;
    fill.maker_source = UpdateSource::HISTORICAL;
    fill.taker_side = Side::SELL;
    fill.taker_order_id = 0;
    fill.taker_trader_id = 0;
    fill.taker_source = UpdateSource::HISTORICAL;
    fill.book_key = "venue:symbol";

    EventApplyRecord apply{};
    apply.seq = 0;
    apply.ts_exchange = 0;
    apply.ts_received = 0;
    apply.side = Side::BUY;
    apply.update_type = UpdateType::ADD;
    apply.source = UpdateSource::HISTORICAL;
    apply.price_ticks = 0;
    apply.qty_lots = 0;
    apply.order_id = 0;
    apply.trader_id = 0;
    apply.aggressor_id = 0;
    apply.book_key = "venue:symbol";

    DiagnosticRecord diag{};
    diag.seq = 0;
    diag.ts_exchange = 0;
    diag.ts_received = 0;
    diag.code = DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID;
    diag.severity = DiagnosticRecordSeverity::INFO;
    diag.book_key = "venue:symbol";
}

} // namespace

TEST_CASE("public api surface: paper trading simulator") {
    touch_record_fields();

    PaperTradingSimulator engine;
    InMemoryLogSink sink;
    engine.set_log_sink(&sink);

    std::vector<Side> sides{};
    std::vector<std::int64_t> prices{};
    std::vector<std::int64_t> quantities{};
    std::vector<std::int64_t> order_ids{};
    std::vector<std::int64_t> trader_ids{};

    REQUIRE_NOTHROW(engine.init_from_l2_snapshot(sides, prices, quantities));
    REQUIRE_NOTHROW(engine.init_from_l3_snapshot(sides, prices, quantities, order_ids, trader_ids));

    auto ev = make_event();
    REQUIRE_NOTHROW(engine.update(ev));

    auto depth = engine.depth_at(Side::BUY, ev.price_ticks);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == ev.quantity_lots);

    auto top = engine.l2_top_n(Side::BUY, 1);
    REQUIRE(top.size() == 1);
    CHECK(top[0].first == ev.price_ticks);
    CHECK(top[0].second == ev.quantity_lots);

    auto best = engine.get_best_price_ticks(Side::BUY);
    REQUIRE(best.has_value());
    CHECK(best.value() == ev.price_ticks);
}

TEST_CASE("public api surface: multibook simulator") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink multi_sink;
    sim.set_multi_log_sink(&multi_sink);

    BookId id{};
    id.venue = "venue";
    id.symbol = "symbol";
    REQUIRE(sim.add_book(id));
    REQUIRE(sim.has_book(id));

    NormalizedLobEvent ev_hist{};
    ev_hist.ts_exchange = 1;
    ev_hist.ts_received = 1;
    ev_hist.side = Side::BUY;
    ev_hist.update_type = UpdateType::ADD;
    ev_hist.price_ticks = 100;
    ev_hist.quantity_lots = 10;
    ev_hist.order_id = 111;
    ev_hist.update_source = UpdateSource::HISTORICAL;
    ev_hist.symbol_id = book_key(id);

    sim.apply(id, ev_hist);

    auto best = sim.get_best_price_ticks(id, Side::BUY);
    REQUIRE(best.has_value());
    CHECK(best.value() == 100);

    auto depth = sim.depth_at(id, Side::BUY, 100);
    REQUIRE(depth.has_value());
    CHECK(depth.value() == 10);

    auto top = sim.l2_top_n(id, Side::BUY, 1);
    REQUIRE(top.size() == 1);
    CHECK(top[0].first == 100);
    CHECK(top[0].second == 10);

    NormalizedLobEvent ev_strat = ev_hist;
    ev_strat.order_id = 222;
    ev_strat.update_source = UpdateSource::STRATEGY;
    sim.submit_strategy_event(id, ev_strat, 0);
    REQUIRE(sim.step());
}
