#include "lobsim/in_memory_sink.hpp"
#include "lobsim/paper_trading_simulator.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

static EventApplyRecord make_event_apply(std::uint64_t seq, UpdateType updateType, UpdateSource source,
                                         std::int64_t priceTicks, std::int64_t qtyLots, std::int64_t orderId,
                                         Side side = Side::BUY, std::int64_t tsEx = 1, std::int64_t tsRecv = 2,
                                         std::int64_t traderId = UnknownTraderIdSentinel,
                                         std::int64_t aggressorId = NoAggressorNeededSentinel,
                                         std::string bookKey = {}) {
    return EventApplyRecord{seq,        tsEx,    tsRecv,  side,     updateType, source,
                            priceTicks, qtyLots, orderId, traderId, aggressorId, std::move(bookKey)};
}

static FillRecord make_fill(std::uint64_t seq, std::int64_t priceTicks, std::int64_t qtyLots, std::int64_t makerOrderId,
                            UpdateSource makerSource, std::int64_t takerOrderId, UpdateSource takerSource,
                            Side makerSide = Side::BUY, Side takerSide = Side::SELL, std::int64_t tsEx = 1,
                            std::int64_t tsRecv = 2, std::int64_t makerTraderId = UnknownTraderIdSentinel,
                            std::int64_t takerTraderId = UnknownTraderIdSentinel, std::string bookKey = {}) {
    return FillRecord{seq,           tsEx,        tsRecv,    priceTicks,   qtyLots,       makerSide,  makerOrderId,
                      makerTraderId, makerSource, takerSide, takerOrderId, takerTraderId, takerSource,
                      std::move(bookKey)};
}

TEST_CASE("Ledger tracks strategy add and maker fills") {
    InMemoryLogSink sink;
    sink.onEventApply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 5, 9001));

    sink.onFill(make_fill(2, 100, 2, 9001, UpdateSource::STRATEGY, 2001, UpdateSource::HISTORICAL));
    sink.onFill(make_fill(3, 100, 3, 9001, UpdateSource::STRATEGY, 2002, UpdateSource::HISTORICAL));

    const auto* entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.initialQty == 5);
    CHECK(entry->state.remainingQty == 0);
    CHECK(entry->state.filledQty == 5);
    CHECK(entry->state.status == PaperOrderLedgerStatus::FILLED);
    CHECK(entry->state.createdSeq == 1);
    CHECK(entry->state.lastUpdateSeq == 3);
    REQUIRE(entry->fills.size() == 2);
    CHECK(entry->fills[0].role == PaperOrderFillRole::MAKER);
    CHECK(entry->fills[1].role == PaperOrderFillRole::MAKER);
}

TEST_CASE("Ledger tracks strategy taker fills") {
    InMemoryLogSink sink;
    sink.onEventApply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 101, 4, 9002));

    sink.onFill(make_fill(2, 101, 1, 3001, UpdateSource::HISTORICAL, 9002, UpdateSource::STRATEGY));

    const auto* entry = sink.findPaperOrder(9002);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remainingQty == 3);
    CHECK(entry->state.filledQty == 1);
    CHECK(entry->state.status == PaperOrderLedgerStatus::PARTIALLY_FILLED);
    REQUIRE(entry->fills.size() == 1);
    CHECK(entry->fills[0].role == PaperOrderFillRole::TAKER);
}

TEST_CASE("Ledger ignores non-strategy fills and unknown order ids") {
    InMemoryLogSink sink;
    sink.onEventApply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 2, 9001));

    sink.onFill(make_fill(2, 100, 1, 9001, UpdateSource::HISTORICAL, 2001, UpdateSource::HISTORICAL));
    sink.onFill(make_fill(3, 100, 1, UnknownOrderIdSentinel, UpdateSource::STRATEGY, 2002, UpdateSource::HISTORICAL));

    const auto* entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.filledQty == 0);
    CHECK(entry->fills.empty());
}

TEST_CASE("Ledger ignores zero-qty add and duplicate adds") {
    InMemoryLogSink sink;
    sink.onEventApply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 0, 9001));
    CHECK(sink.getPaperLedger().empty());
    REQUIRE(sink.getRejectedStrategyEvents().size() == 1);

    sink.onEventApply(make_event_apply(2, UpdateType::ADD, UpdateSource::STRATEGY, 100, 3, 9001));
    sink.onEventApply(make_event_apply(3, UpdateType::ADD, UpdateSource::STRATEGY, 100, 5, 9001));

    const auto* entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.initialQty == 3);
    CHECK(entry->state.remainingQty == 3);
    REQUIRE(sink.getRejectedStrategyEvents().size() == 2);
}

TEST_CASE("Ledger handles subtract edge cases") {
    InMemoryLogSink sink;
    sink.onEventApply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 5, 9001));

    sink.onEventApply(make_event_apply(2, UpdateType::SUBTRACT, UpdateSource::STRATEGY, 100, -1, 9001));
    auto entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remainingQty == 5);
    REQUIRE(sink.getRejectedStrategyEvents().size() == 1);

    sink.onEventApply(make_event_apply(3, UpdateType::SUBTRACT, UpdateSource::STRATEGY, 100, 0, 9001));
    entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.lastUpdateSeq == 3);

    sink.onEventApply(make_event_apply(4, UpdateType::SUBTRACT, UpdateSource::STRATEGY, 100, 2, 9001));
    entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remainingQty == 3);
    CHECK(entry->state.status == PaperOrderLedgerStatus::OPEN);

    sink.onEventApply(make_event_apply(5, UpdateType::SUBTRACT, UpdateSource::STRATEGY, 100, 10, 9001));
    entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remainingQty == 0);
    CHECK(entry->state.status == PaperOrderLedgerStatus::CANCELLED);
}

TEST_CASE("Ledger handles set, delete, and match updates") {
    InMemoryLogSink sink;
    sink.onEventApply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 5, 9001));

    sink.onEventApply(make_event_apply(2, UpdateType::SET, UpdateSource::STRATEGY, 100, 3, 9001));
    auto entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remainingQty == 3);
    CHECK(entry->state.status == PaperOrderLedgerStatus::OPEN);

    sink.onEventApply(make_event_apply(3, UpdateType::SET, UpdateSource::STRATEGY, 100, -5, 9001));
    entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remainingQty == 0);
    CHECK(entry->state.status == PaperOrderLedgerStatus::CANCELLED);

    sink.onEventApply(make_event_apply(4, UpdateType::MATCH, UpdateSource::STRATEGY, 100, 1, 9001));
    entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.status == PaperOrderLedgerStatus::CANCELLED);

    sink.onEventApply(make_event_apply(5, UpdateType::ADD, UpdateSource::STRATEGY, 100, 2, 9002));
    sink.onFill(make_fill(6, 100, 2, 9002, UpdateSource::STRATEGY, 2001, UpdateSource::HISTORICAL));
    sink.onEventApply(make_event_apply(7, UpdateType::DELETE, UpdateSource::STRATEGY, 100, 0, 9002));
    entry = sink.findPaperOrder(9002);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.status == PaperOrderLedgerStatus::FILLED);
}

TEST_CASE("Taker fills leave remainder open across seq changes") {
    InMemoryLogSink sink;
    sink.onEventApply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 5, 9001));

    sink.onFill(make_fill(1, 100, 2, 1001, UpdateSource::HISTORICAL, 9001, UpdateSource::STRATEGY));

    sink.onEventApply(make_event_apply(2, UpdateType::ADD, UpdateSource::HISTORICAL, 101, 1, 2001));

    const auto* entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remainingQty == 3);
    CHECK(entry->state.status == PaperOrderLedgerStatus::PARTIALLY_FILLED);
}

TEST_CASE("Filled order stays filled after seq change") {
    InMemoryLogSink sink;
    sink.onEventApply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 2, 9001));
    sink.onFill(make_fill(1, 100, 2, 1001, UpdateSource::HISTORICAL, 9001, UpdateSource::STRATEGY));

    sink.onEventApply(make_event_apply(2, UpdateType::ADD, UpdateSource::HISTORICAL, 101, 1, 2001));

    const auto* entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remainingQty == 0);
    CHECK(entry->state.status == PaperOrderLedgerStatus::FILLED);
}

TEST_CASE("Ledger ignores fills after cancel") {
    InMemoryLogSink sink;
    sink.onEventApply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 4, 9001));
    sink.onEventApply(make_event_apply(2, UpdateType::DELETE, UpdateSource::STRATEGY, 100, 0, 9001));
    sink.onFill(make_fill(3, 100, 2, 9001, UpdateSource::STRATEGY, 2001, UpdateSource::HISTORICAL));

    const auto* entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.status == PaperOrderLedgerStatus::CANCELLED);
    CHECK(entry->state.filledQty == 0);
    CHECK(entry->fills.empty());
}

TEST_CASE("Cancelled order ignores set and subtract") {
    InMemoryLogSink sink;
    sink.onEventApply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 4, 9001));
    sink.onEventApply(make_event_apply(2, UpdateType::DELETE, UpdateSource::STRATEGY, 100, 0, 9001));

    sink.onEventApply(make_event_apply(3, UpdateType::SET, UpdateSource::STRATEGY, 100, 5, 9001));
    sink.onEventApply(make_event_apply(4, UpdateType::SUBTRACT, UpdateSource::STRATEGY, 100, 1, 9001));

    const auto* entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.status == PaperOrderLedgerStatus::CANCELLED);
    CHECK(entry->state.remainingQty == 0);
    CHECK(entry->state.lastUpdateSeq == 2);
}

TEST_CASE("SET increase updates initial qty invariant") {
    InMemoryLogSink sink;
    sink.onEventApply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 5, 9001));
    sink.onFill(make_fill(2, 100, 2, 9001, UpdateSource::STRATEGY, 2001, UpdateSource::HISTORICAL));

    sink.onEventApply(make_event_apply(3, UpdateType::SET, UpdateSource::STRATEGY, 100, 6, 9001));

    const auto* entry = sink.findPaperOrder(9001);
    REQUIRE(entry != nullptr);
    CHECK(entry->state.remainingQty == 6);
    CHECK(entry->state.filledQty == 2);
    CHECK(entry->state.initialQty == 8);
}

TEST_CASE("Ledger reset clears paper state") {
    InMemoryLogSink sink;
    sink.onEventApply(make_event_apply(1, UpdateType::ADD, UpdateSource::STRATEGY, 100, 2, 9001));
    CHECK_FALSE(sink.getPaperLedger().empty());
    sink.reset();
    CHECK(sink.getPaperLedger().empty());
}
