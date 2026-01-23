#include "lobsim/in_memory_sink.hpp"
#include "lobsim/paper_trading_simulator.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

static EventApplyRecord make_event_apply(std::uint64_t seq, UpdateType update_type, UpdateSource source,
                                         std::int64_t price_ticks, std::int64_t qty_lots, std::int64_t order_id,
                                         Side side = Side::BUY, std::int64_t tsEx = 1, std::int64_t tsRecv = 2,
                                         std::int64_t trader_id = UnknownTraderIdSentinel,
                                         std::int64_t aggressor_id = NoAggressorNeededSentinel,
                                         std::string book_key = {}) {
    return EventApplyRecord{seq,         tsEx,     tsRecv,   side,      update_type,  source,
                            price_ticks, qty_lots, order_id, trader_id, aggressor_id, std::move(book_key)};
}

static FillRecord make_fill(std::uint64_t seq, std::int64_t price_ticks, std::int64_t qty_lots,
                            std::int64_t maker_order_id, UpdateSource maker_source, std::int64_t taker_order_id,
                            UpdateSource taker_source, Side maker_side = Side::BUY, Side taker_side = Side::SELL,
                            std::int64_t tsEx = 1, std::int64_t tsRecv = 2,
                            std::int64_t maker_trader_id = UnknownTraderIdSentinel,
                            std::int64_t taker_trader_id = UnknownTraderIdSentinel, std::string book_key = {}) {
    return FillRecord{seq,
                      tsEx,
                      tsRecv,
                      price_ticks,
                      qty_lots,
                      maker_side,
                      maker_order_id,
                      maker_trader_id,
                      maker_source,
                      taker_side,
                      taker_order_id,
                      taker_trader_id,
                      taker_source,
                      std::move(book_key)};
}

TEST_CASE("Ledger tracks strategy add and maker fills") {
    InMemoryLogSink sink;
    sink.on_event_apply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 5, 9001));

    sink.on_fill(make_fill(2, 100, 2, 9001, UpdateSource::STRATEGY, 2001, UpdateSource::HISTORICAL));
    sink.on_fill(make_fill(3, 100, 3, 9001, UpdateSource::STRATEGY, 2002, UpdateSource::HISTORICAL));

    const auto* entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.initial_qty == 5);
    CHECK(entry->state.remaining_qty == 0);
    CHECK(entry->state.filled_qty == 5);
    CHECK(entry->state.status == PaperOrderLedgerStatus::FILLED);
    CHECK(entry->state.created_seq == 1);
    CHECK(entry->state.last_update_seq == 3);
    REQUIRE(entry->fills.size() == 2);
    CHECK(entry->fills[0].role == PaperOrderFillRole::MAKER);
    CHECK(entry->fills[1].role == PaperOrderFillRole::MAKER);
}

TEST_CASE("Ledger tracks strategy taker fills") {
    InMemoryLogSink sink;
    sink.on_event_apply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 101, 4, 9002));

    sink.on_fill(make_fill(2, 101, 1, 3001, UpdateSource::HISTORICAL, 9002, UpdateSource::STRATEGY));

    const auto* entry = sink.find_paper_order(9002);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remaining_qty == 3);
    CHECK(entry->state.filled_qty == 1);
    CHECK(entry->state.status == PaperOrderLedgerStatus::PARTIALLY_FILLED);
    REQUIRE(entry->fills.size() == 1);
    CHECK(entry->fills[0].role == PaperOrderFillRole::TAKER);
}

TEST_CASE("Ledger ignores non-strategy fills and unknown order ids") {
    InMemoryLogSink sink;
    sink.on_event_apply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 2, 9001));

    sink.on_fill(make_fill(2, 100, 1, 9001, UpdateSource::HISTORICAL, 2001, UpdateSource::HISTORICAL));
    sink.on_fill(make_fill(3, 100, 1, UnknownOrderIdSentinel, UpdateSource::STRATEGY, 2002, UpdateSource::HISTORICAL));

    const auto* entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.filled_qty == 0);
    CHECK(entry->fills.empty());
}

TEST_CASE("Ledger ignores zero-qty add and duplicate adds") {
    InMemoryLogSink sink;
    sink.on_event_apply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 0, 9001));
    CHECK(sink.get_paper_ledger().empty());
    REQUIRE(sink.get_rejected_strategy_events().size() == 1);

    sink.on_event_apply(make_event_apply(2, UpdateType::ADD, UpdateSource::STRATEGY, 100, 3, 9001));
    sink.on_event_apply(make_event_apply(3, UpdateType::ADD, UpdateSource::STRATEGY, 100, 5, 9001));

    const auto* entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.initial_qty == 3);
    CHECK(entry->state.remaining_qty == 3);
    REQUIRE(sink.get_rejected_strategy_events().size() == 2);
}

TEST_CASE("Ledger handles subtract edge cases") {
    InMemoryLogSink sink;
    sink.on_event_apply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 5, 9001));

    sink.on_event_apply(make_event_apply(2, UpdateType::SUBTRACT, UpdateSource::STRATEGY, 100, -1, 9001));
    auto entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remaining_qty == 5);
    REQUIRE(sink.get_rejected_strategy_events().size() == 1);

    sink.on_event_apply(make_event_apply(3, UpdateType::SUBTRACT, UpdateSource::STRATEGY, 100, 0, 9001));
    entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.last_update_seq == 3);

    sink.on_event_apply(make_event_apply(4, UpdateType::SUBTRACT, UpdateSource::STRATEGY, 100, 2, 9001));
    entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remaining_qty == 3);
    CHECK(entry->state.status == PaperOrderLedgerStatus::OPEN);

    sink.on_event_apply(make_event_apply(5, UpdateType::SUBTRACT, UpdateSource::STRATEGY, 100, 10, 9001));
    entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remaining_qty == 0);
    CHECK(entry->state.status == PaperOrderLedgerStatus::CANCELLED);
}

TEST_CASE("Ledger handles set, delete, and match updates") {
    InMemoryLogSink sink;
    sink.on_event_apply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 5, 9001));

    sink.on_event_apply(make_event_apply(2, UpdateType::SET, UpdateSource::STRATEGY, 100, 3, 9001));
    auto entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remaining_qty == 3);
    CHECK(entry->state.status == PaperOrderLedgerStatus::OPEN);

    sink.on_event_apply(make_event_apply(3, UpdateType::SET, UpdateSource::STRATEGY, 100, -5, 9001));
    entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remaining_qty == 0);
    CHECK(entry->state.status == PaperOrderLedgerStatus::CANCELLED);

    sink.on_event_apply(make_event_apply(4, UpdateType::MATCH, UpdateSource::STRATEGY, 100, 1, 9001));
    entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.status == PaperOrderLedgerStatus::CANCELLED);

    sink.on_event_apply(make_event_apply(5, UpdateType::ADD, UpdateSource::STRATEGY, 100, 2, 9002));
    sink.on_fill(make_fill(6, 100, 2, 9002, UpdateSource::STRATEGY, 2001, UpdateSource::HISTORICAL));
    sink.on_event_apply(make_event_apply(7, UpdateType::DELETE, UpdateSource::STRATEGY, 100, 0, 9002));
    entry = sink.find_paper_order(9002);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.status == PaperOrderLedgerStatus::FILLED);
}

TEST_CASE("Taker fills leave remainder open across seq changes") {
    InMemoryLogSink sink;
    sink.on_event_apply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 5, 9001));

    sink.on_fill(make_fill(1, 100, 2, 1001, UpdateSource::HISTORICAL, 9001, UpdateSource::STRATEGY));

    sink.on_event_apply(make_event_apply(2, UpdateType::ADD, UpdateSource::HISTORICAL, 101, 1, 2001));

    const auto* entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remaining_qty == 3);
    CHECK(entry->state.status == PaperOrderLedgerStatus::PARTIALLY_FILLED);
}

TEST_CASE("Filled order stays filled after seq change") {
    InMemoryLogSink sink;
    sink.on_event_apply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 2, 9001));
    sink.on_fill(make_fill(1, 100, 2, 1001, UpdateSource::HISTORICAL, 9001, UpdateSource::STRATEGY));

    sink.on_event_apply(make_event_apply(2, UpdateType::ADD, UpdateSource::HISTORICAL, 101, 1, 2001));

    const auto* entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remaining_qty == 0);
    CHECK(entry->state.status == PaperOrderLedgerStatus::FILLED);
}

TEST_CASE("Ledger ignores fills after cancel") {
    InMemoryLogSink sink;
    sink.on_event_apply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 4, 9001));
    sink.on_event_apply(make_event_apply(2, UpdateType::DELETE, UpdateSource::STRATEGY, 100, 0, 9001));
    sink.on_fill(make_fill(3, 100, 2, 9001, UpdateSource::STRATEGY, 2001, UpdateSource::HISTORICAL));

    const auto* entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.status == PaperOrderLedgerStatus::CANCELLED);
    CHECK(entry->state.filled_qty == 0);
    CHECK(entry->fills.empty());
}

TEST_CASE("Cancelled order ignores set and subtract") {
    InMemoryLogSink sink;
    sink.on_event_apply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 4, 9001));
    sink.on_event_apply(make_event_apply(2, UpdateType::DELETE, UpdateSource::STRATEGY, 100, 0, 9001));

    sink.on_event_apply(make_event_apply(3, UpdateType::SET, UpdateSource::STRATEGY, 100, 5, 9001));
    sink.on_event_apply(make_event_apply(4, UpdateType::SUBTRACT, UpdateSource::STRATEGY, 100, 1, 9001));

    const auto* entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.status == PaperOrderLedgerStatus::CANCELLED);
    CHECK(entry->state.remaining_qty == 0);
    CHECK(entry->state.last_update_seq == 2);
}

TEST_CASE("SET increase updates initial qty invariant") {
    InMemoryLogSink sink;
    sink.on_event_apply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 5, 9001));
    sink.on_fill(make_fill(2, 100, 2, 9001, UpdateSource::STRATEGY, 2001, UpdateSource::HISTORICAL));

    sink.on_event_apply(make_event_apply(3, UpdateType::SET, UpdateSource::STRATEGY, 100, 6, 9001));

    const auto* entry = sink.find_paper_order(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remaining_qty == 6);
    CHECK(entry->state.filled_qty == 2);
    CHECK(entry->state.initial_qty == 8);
}

TEST_CASE("Ledger reset clears paper state") {
    InMemoryLogSink sink;
    sink.on_event_apply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 2, 9001));
    CHECK_FALSE(sink.get_paper_ledger().empty());
    sink.reset();
    CHECK(sink.get_paper_ledger().empty());
}
