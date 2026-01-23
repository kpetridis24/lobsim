#include "lobsim/book_id.hpp"
#include "lobsim/log_sink.hpp"
#include "lobsim/multi_log_sink.hpp"
#include "lobsim/multibook_simulator.hpp"
#include "lobsim/types.hpp"

#include <catch2/catch_test_macros.hpp>
#include <utility>
#include <vector>

namespace {

NormalizedLobEvent makeEvent(std::string symbol, std::int64_t ts_exchange, std::int64_t ts_received, Side side,
                             UpdateType type, std::int64_t price, std::int64_t qty, std::int64_t order_id) {
    NormalizedLobEvent ev{};
    ev.symbol_id = std::move(symbol);
    ev.ts_exchange = ts_exchange;
    ev.ts_received = ts_received;
    ev.side = side;
    ev.update_type = type;
    ev.price_ticks = price;
    ev.quantity_lots = qty;
    ev.order_id = order_id;
    ev.trader_id = 1;
    ev.aggressor_id = UnknownAggressorIdSentinel;
    ev.update_source = UpdateSource::HISTORICAL;
    return ev;
}

struct NormalizedVectorSource {
    explicit NormalizedVectorSource(std::vector<NormalizedLobEvent> events) : events_(std::move(events)) {}
    bool next(NormalizedLobEvent& out) {
        if (idx_ >= events_.size()) {
            return false;
        }
        out = events_[idx_++];
        return true;
    }
    std::vector<NormalizedLobEvent> events_;
    std::size_t idx_{0};
};

struct RawEvent {
    std::int64_t tsEx{};
    std::int64_t tsRx{};
    std::int64_t px{};
    std::int64_t qty{};
    std::int64_t id{};
};

struct RawVectorSource {
    explicit RawVectorSource(std::vector<RawEvent> events) : events_(std::move(events)) {}
    bool next(RawEvent& out) {
        if (idx_ >= events_.size()) {
            return false;
        }
        out = events_[idx_++];
        return true;
    }
    std::vector<RawEvent> events_;
    std::size_t idx_{0};
};

struct RawAdapter {
    NormalizedLobEvent normalize(const RawEvent& raw) const {
        NormalizedLobEvent ev{};
        ev.symbol_id = symbol;
        ev.ts_exchange = raw.tsEx;
        ev.ts_received = raw.tsRx;
        ev.side = Side::BUY;
        ev.update_type = UpdateType::ADD;
        ev.price_ticks = raw.px;
        ev.quantity_lots = raw.qty;
        ev.order_id = raw.id;
        ev.trader_id = 1;
        ev.aggressor_id = UnknownAggressorIdSentinel;
        ev.update_source = UpdateSource::HISTORICAL;
        return ev;
    }
    std::string symbol;
};

struct CapturingSink final : ILogSink {
    void on_fill(const FillRecord& r) override { fills.push_back(r); }
    void on_event_apply(const EventApplyRecord& r) override { events.push_back(r); }
    void on_diagnostic(const DiagnosticRecord& r) override { diagnostics.push_back(r); }
    std::vector<FillRecord> fills;
    std::vector<EventApplyRecord> events;
    std::vector<DiagnosticRecord> diagnostics;
};

} // namespace

TEST_CASE("MultiBookSimulator merges events across books in time order and logs book_key") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId a{"X", "A"};
    BookId b{"X", "B"};

    NormalizedVectorSource srcA({makeEvent("A", 10, 10, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                 makeEvent("A", 30, 30, Side::BUY, UpdateType::ADD, 110, 1, 2)});
    NormalizedVectorSource srcB(
        {makeEvent("B", 15, 20, Side::SELL, UpdateType::ADD, 200, 1, 3),
         makeEvent("B", 25, 30, Side::SELL, UpdateType::ADD, 210, 1, 4)}); // same ts_received as A second event

    sim.add_stream(a, srcA);
    sim.add_stream(b, srcB);

    std::size_t steps = 0;
    while (sim.step()) {
        ++steps;
    }
    REQUIRE(steps == 4);

    auto evs = sink.events();
    REQUIRE(evs.size() == 4);
    REQUIRE(evs[0].book_key == "X:A");
    REQUIRE(evs[1].book_key == "X:B");
    // ts_received tie at 30 -> ts_exchange tie-breaker keeps B(25) before A(30)
    REQUIRE(evs[2].book_key == "X:B");
    REQUIRE(evs[3].book_key == "X:A");

    REQUIRE(sim.get_best_price_ticks(a, Side::BUY).has_value());
    REQUIRE(sim.get_best_price_ticks(a, Side::BUY).value() == 110);
    REQUIRE(sim.get_best_price_ticks(b, Side::SELL).has_value());
    REQUIRE(sim.get_best_price_ticks(b, Side::SELL).value() == 200);
}

TEST_CASE("MultiBookSimulator deduces raw type via adapter overload") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId id{"X", "C"};
    RawVectorSource src({RawEvent{1, 1, 100, 5, 10}, RawEvent{2, 2, 105, 7, 11}});
    RawAdapter adapter{"C"};

    sim.add_stream(id, src, adapter); // should compile and run without spelling RawEvent

    while (sim.step()) {
    }
    auto evs = sink.events();
    REQUIRE(evs.size() == 2);
    for (const auto& e : evs) {
        REQUIRE(e.book_key == "X:C");
        REQUIRE(e.side == Side::BUY);
        REQUIRE(e.update_type == UpdateType::ADD);
    }
}

TEST_CASE("Non-monotonic per-stream timestamps emit diagnostics when fail_fast=false") {
    MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = false});
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId id{"X", "D"};
    NormalizedVectorSource src({makeEvent("D", 1, 10, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                makeEvent("D", 2, 5, Side::BUY, UpdateType::ADD, 110, 1, 2)}); // ts_received decreases
    sim.add_stream(id, src);

    REQUIRE(sim.step());       // applies first event
    REQUIRE_FALSE(sim.step()); // second is rejected; no more buffered

    auto diags = sink.diagnostics();
    REQUIRE(diags.size() == 1);
    REQUIRE(diags[0].code == DiagnosticRecordCode::NON_MONOTONIC_TS_RECEIVED_DETECTED_IN_MULTI_BOOK_SIMULATOR);
    REQUIRE(diags[0].book_key == "X:D");

    auto best = sim.get_best_price_ticks(id, Side::BUY);
    REQUIRE(best.has_value());
    REQUIRE(best.value() == 100);
}

TEST_CASE("Unknown book queries emit diagnostics and return empty") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId missing{"X", "MISSING"};
    auto depth = sim.depth_at(missing, Side::BUY, 100);
    auto topn = sim.l2_top_n(missing, Side::BUY, 1);
    auto best = sim.get_best_price_ticks(missing, Side::BUY);

    REQUIRE_FALSE(depth.has_value());
    REQUIRE(topn.empty());
    REQUIRE_FALSE(best.has_value());

    auto diags = sink.diagnostics();
    REQUIRE(diags.size() == 3);
    REQUIRE(diags[0].book_key == "X:MISSING");
    REQUIRE(diags[1].book_key == "X:MISSING");
    REQUIRE(diags[2].book_key == "X:MISSING");
}

TEST_CASE("step_until/step_for advance correct counts and current_time updates") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId id{"X", "STEP"};
    NormalizedVectorSource src({makeEvent("STEP", 1, 5, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                makeEvent("STEP", 2, 10, Side::BUY, UpdateType::ADD, 101, 1, 2),
                                makeEvent("STEP", 3, 15, Side::BUY, UpdateType::ADD, 102, 1, 3)});
    sim.add_stream(id, src);

    REQUIRE(sim.step_until(5) == 1);
    REQUIRE(sim.current_time().has_value());
    REQUIRE(sim.current_time().value() == 5);

    REQUIRE(sim.step_for(4) == 0);    // no events in (5,9]
    REQUIRE(sim.step_until(12) == 1); // picks ts_received=10
    REQUIRE(sim.current_time().value() == 10);

    REQUIRE(sim.step()); // last event
    REQUIRE(sim.current_time().value() == 15);
    REQUIRE_FALSE(sim.step()); // exhausted
}

TEST_CASE("apply rejects non-monotonic ts_received and emits diagnostic") {
    MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = true});
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId id{"X", "FAIL"};
    NormalizedVectorSource src({makeEvent("FAIL", 1, 10, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    sim.add_stream(id, src);

    REQUIRE(sim.step()); // establishes current_ts_received_ == 10

    NormalizedLobEvent ev = makeEvent("FAIL", 1, 0, Side::BUY, UpdateType::ADD, 101, 1, 2);
    REQUIRE_THROWS(sim.apply(id, ev)); // should be rejected and throw when fail_fast=true

    auto diags = sink.diagnostics();
    REQUIRE_FALSE(diags.empty());
    REQUIRE(diags.back().code == DiagnosticRecordCode::NON_MONOTONIC_TS_RECEIVED_DETECTED_IN_MULTI_BOOK_SIMULATOR);
    REQUIRE(diags.back().book_key == "X:FAIL");

    auto best = sim.get_best_price_ticks(id, Side::BUY);
    REQUIRE(best.has_value());
    REQUIRE(best.value() == 100);
}

TEST_CASE("apply on unknown book emits diagnostic and does not crash") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId id{"X", "NOBOOK"};
    NormalizedLobEvent ev = makeEvent("NOBOOK", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1);
    sim.apply(id, ev);

    auto diags = sink.diagnostics();
    REQUIRE(diags.size() == 1);
    REQUIRE(diags[0].code == DiagnosticRecordCode::APPLY_EVENT_REQUESTED_FOR_UNKNOWN_BOOK);
    REQUIRE(diags[0].book_key == "X:NOBOOK");
}

TEST_CASE("Adapter failures emit diagnostics or throw depending on fail_fast") {
    struct BadAdapter {
        std::string symbol;
        NormalizedLobEvent normalize(const RawEvent&) const { throw std::runtime_error("bad"); }
    };
    RawVectorSource src({RawEvent{1, 1, 100, 1, 1}});

    // fail_fast=false: skip and emit no event
    {
        MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = false});
        InMemoryMultiLogSink sink;
        sim.set_multi_log_sink(&sink);
        sim.add_stream(BookId{"X", "BAD"}, src, BadAdapter{"BAD"});
        REQUIRE_FALSE(sim.step()); // normalization failed, nothing applied
        REQUIRE(sink.events().empty());
    }

    // fail_fast=true: throws
    {
        RawVectorSource src2({RawEvent{1, 1, 100, 1, 1}});
        MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = true});
        InMemoryMultiLogSink sink;
        sim.set_multi_log_sink(&sink);
        sim.add_stream(BookId{"X", "BAD"}, src2, BadAdapter{"BAD"});
        REQUIRE_THROWS(sim.step());
    }
}

TEST_CASE("Mixed normalized and raw streams merge correctly") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId norm{"X", "NORM"};
    BookId raw{"X", "RAW"};

    NormalizedVectorSource srcNorm({makeEvent("NORM", 1, 5, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                    makeEvent("NORM", 2, 15, Side::BUY, UpdateType::ADD, 101, 1, 2)});
    RawVectorSource srcRaw({RawEvent{1, 10, 200, 1, 10}, RawEvent{2, 20, 201, 1, 11}});
    RawAdapter adapter{"RAW"};

    sim.add_stream(norm, srcNorm);
    sim.add_stream(raw, srcRaw, adapter);

    std::vector<std::string> seen;
    while (sim.step()) {
        auto evs = sink.events();
        if (!evs.empty()) {
            seen.push_back(evs.back().book_key);
        }
    }
    REQUIRE(seen == std::vector<std::string>{"X:NORM", "X:RAW", "X:NORM", "X:RAW"});
}

TEST_CASE("Per-book filtering APIs return only matching records") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);
    BookId a{"X", "A2"};
    BookId b{"X", "B2"};

    NormalizedVectorSource srcA({makeEvent("A2", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    NormalizedVectorSource srcB({makeEvent("B2", 1, 2, Side::SELL, UpdateType::ADD, 200, 1, 2)});
    sim.add_stream(a, srcA);
    sim.add_stream(b, srcB);
    while (sim.step()) {
    }
    auto fillsA = sink.fills_for("X:A2");
    auto fillsB = sink.fills_for("X:B2");
    auto eventsA = sink.events_for("X:A2");
    auto eventsB = sink.events_for("X:B2");
    REQUIRE(eventsA.size() == 1);
    REQUIRE(eventsB.size() == 1);
    REQUIRE(eventsA[0].book_key == "X:A2");
    REQUIRE(eventsB[0].book_key == "X:B2");
    REQUIRE(fillsA.empty());
    REQUIRE(fillsB.empty());
}

TEST_CASE("No multi sink attached does not crash and no diagnostics are emitted") {
    MultiBookSimulator sim;
    BookId id{"X", "NOSINK"};
    NormalizedVectorSource src({makeEvent("NOSINK", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    sim.add_stream(id, src);
    // No sink set; step should still advance without dereferencing null
    REQUIRE(sim.step());
}

TEST_CASE("set_log_sink overrides multi sink for a single book") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink multi;
    sim.set_multi_log_sink(&multi);

    BookId a{"X", "SA"};
    BookId b{"X", "SB"};
    NormalizedVectorSource srcA({makeEvent("SA", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    NormalizedVectorSource srcB({makeEvent("SB", 1, 2, Side::BUY, UpdateType::ADD, 200, 1, 2)});
    sim.add_stream(a, srcA);
    sim.add_stream(b, srcB);

    CapturingSink localA;
    sim.set_log_sink(a, &localA);

    while (sim.step()) {
    }

    REQUIRE(localA.events.size() == 1);
    REQUIRE(localA.events[0].book_key == "X:SA");

    REQUIRE(multi.events_for("X:SA").empty());
    REQUIRE(multi.events_for("X:SB").size() == 1);
    REQUIRE(multi.events_for("X:SB")[0].book_key == "X:SB");
}

TEST_CASE("set_multi_log_sink(nullptr) detaches wrappers safely") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink multi;
    sim.set_multi_log_sink(&multi);

    BookId id{"X", "DETACH"};
    NormalizedVectorSource src({makeEvent("DETACH", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                makeEvent("DETACH", 2, 2, Side::BUY, UpdateType::ADD, 101, 1, 2)});
    sim.add_stream(id, src);

    REQUIRE(sim.step());
    REQUIRE(multi.events().size() == 1);

    sim.set_multi_log_sink(nullptr);

    REQUIRE(sim.step()); // still applies, but no more logging to multi
    REQUIRE(multi.events().size() == 1);

    auto best = sim.get_best_price_ticks(id, Side::BUY);
    REQUIRE(best.has_value());
    REQUIRE(best.value() == 101);
}

TEST_CASE("Raw stream can skip invalid events and still apply later valid events (try_normalize path)") {
    struct SelectiveAdapter {
        std::string symbol;
        bool try_normalize(const RawEvent& raw, NormalizedLobEvent& out) const {
            if (raw.id == 1) {
                return false;
            }
            out = NormalizedLobEvent{};
            out.symbol_id = symbol;
            out.ts_exchange = raw.tsEx;
            out.ts_received = raw.tsRx;
            out.side = Side::BUY;
            out.update_type = UpdateType::ADD;
            out.price_ticks = raw.px;
            out.quantity_lots = raw.qty;
            out.order_id = raw.id;
            out.trader_id = 1;
            out.aggressor_id = UnknownAggressorIdSentinel;
            out.update_source = UpdateSource::HISTORICAL;
            return true;
        }
        NormalizedLobEvent normalize(const RawEvent& raw) const {
            NormalizedLobEvent out{};
            if (!try_normalize(raw, out)) {
                throw std::runtime_error("bad");
            }
            return out;
        }
    };

    RawVectorSource src({RawEvent{1, 1, 100, 1, 1}, RawEvent{2, 2, 101, 1, 2}});
    MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = false});
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);
    sim.add_stream(BookId{"X", "SKIP"}, src, SelectiveAdapter{"SKIP"});

    std::size_t steps = 0;
    while (sim.step()) {
        ++steps;
    }
    REQUIRE(steps == 1);
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].order_id == 2);
}

TEST_CASE("Raw stream can skip invalid events and still apply later valid events (throwing normalize path)") {
    struct ThrowingAdapter {
        std::string symbol;
        NormalizedLobEvent normalize(const RawEvent& raw) const {
            if (raw.id == 1) {
                throw std::runtime_error("bad");
            }
            NormalizedLobEvent ev{};
            ev.symbol_id = symbol;
            ev.ts_exchange = raw.tsEx;
            ev.ts_received = raw.tsRx;
            ev.side = Side::BUY;
            ev.update_type = UpdateType::ADD;
            ev.price_ticks = raw.px;
            ev.quantity_lots = raw.qty;
            ev.order_id = raw.id;
            ev.trader_id = 1;
            ev.aggressor_id = UnknownAggressorIdSentinel;
            ev.update_source = UpdateSource::HISTORICAL;
            return ev;
        }
    };

    RawVectorSource src({RawEvent{1, 1, 100, 1, 1}, RawEvent{2, 2, 101, 1, 2}});
    MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = false});
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);
    sim.add_stream(BookId{"X", "SKIP2"}, src, ThrowingAdapter{"SKIP2"});

    std::size_t steps = 0;
    while (sim.step()) {
        ++steps;
    }
    REQUIRE(steps == 1);
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].order_id == 2);
}

TEST_CASE("step_for before any step uses base=0") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);
    BookId id{"X", "BASE0"};
    NormalizedVectorSource src({makeEvent("BASE0", 1, 5, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                makeEvent("BASE0", 1, 10, Side::BUY, UpdateType::ADD, 101, 1, 2)});
    sim.add_stream(id, src);

    REQUIRE(sim.step_for(7) == 1);
    REQUIRE(sim.current_time().has_value());
    REQUIRE(sim.current_time().value() == 5);
    REQUIRE(sink.events().size() == 1);
}

TEST_CASE("add_book returns false on duplicate book key") {
    MultiBookSimulator sim;
    REQUIRE(sim.add_book(BookId{"X", "DUP"}));
    REQUIRE_FALSE(sim.add_book(BookId{"X", "DUP"}));
}

TEST_CASE("Strategy events merge with feeds honoring latency and ordering") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId id{"X", "STRAT"};
    NormalizedVectorSource src({makeEvent("STRAT", 1, 5, Side::SELL, UpdateType::ADD, 100, 5, 1)});
    sim.add_stream(id, src);

    NormalizedLobEvent strat1 = makeEvent("STRAT", 1, 6, Side::BUY, UpdateType::ADD, 100, 2, 10);
    NormalizedLobEvent strat2 = makeEvent("STRAT", 1, 7, Side::BUY, UpdateType::ADD, 100, 3, 11);
    sim.submit_strategy_event(id, strat1);
    sim.submit_strategy_event(id, strat2);

    std::vector<std::string> order;
    while (sim.step()) {
        auto evs = sink.events();
        order.push_back(evs.back().book_key + ":" + std::to_string(evs.back().order_id));
    }
    REQUIRE(order.size() == 3);
    REQUIRE(order[0] == "X:STRAT:1");
    REQUIRE(order[1] == "X:STRAT:10");
    REQUIRE(order[2] == "X:STRAT:11");
}

TEST_CASE("Strategy event latency shifts ts_received when provided") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId id{"X", "LAT"};
    NormalizedVectorSource src({makeEvent("LAT", 1, 10, Side::SELL, UpdateType::ADD, 100, 5, 1)});
    sim.add_stream(id, src);

    // Establish current time at 10
    REQUIRE(sim.step());
    REQUIRE(sim.current_time().has_value());
    REQUIRE(sim.current_time().value() == 10);

    // ts_received=0 should be based on current time + latency
    NormalizedLobEvent strat = makeEvent("LAT", 1, 0, Side::BUY, UpdateType::ADD, 100, 1, 2);
    sim.submit_strategy_event(id, strat, 5); // expect ts_received = 15

    // Non-zero ts_received should also get latency added
    NormalizedLobEvent strat2 = makeEvent("LAT", 1, 20, Side::BUY, UpdateType::ADD, 100, 1, 3);
    sim.submit_strategy_event(id, strat2, 5); // expect ts_received = 25

    std::vector<std::int64_t> appliedTs;
    while (sim.step()) {
        appliedTs.push_back(sink.events().back().ts_received);
    }
    REQUIRE(appliedTs.size() == 2);
    REQUIRE(appliedTs[0] == 15);
    REQUIRE(appliedTs[1] == 25);
}

TEST_CASE("Negative latency that causes time travel emits diagnostic") {
    MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = false});
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId id{"X", "LATNEG"};
    NormalizedVectorSource src({makeEvent("LATNEG", 1, 10, Side::SELL, UpdateType::ADD, 100, 5, 1)});
    sim.add_stream(id, src);
    REQUIRE(sim.step()); // current time = 10

    NormalizedLobEvent strat = makeEvent("LATNEG", 1, 0, Side::BUY, UpdateType::ADD, 100, 1, 2);
    sim.submit_strategy_event(id, strat, -6); // would backdate to 4 -> should be rejected

    // No additional events applied
    REQUIRE_FALSE(sim.step());

    auto diags = sink.diagnostics();
    REQUIRE_FALSE(diags.empty());
    REQUIRE(diags.back().code == DiagnosticRecordCode::STRATEGY_EVENT_TIME_TRAVEL);
    REQUIRE(diags.back().book_key == "X:LATNEG");
}

TEST_CASE("Strategy event into unknown book emits diagnostic and optional throw") {
    {
        MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = false});
        InMemoryMultiLogSink sink;
        sim.set_multi_log_sink(&sink);
        NormalizedLobEvent ev = makeEvent("MISSING", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1);
        sim.submit_strategy_event(BookId{"X", "MISSING"}, ev);
        auto diags = sink.diagnostics();
        REQUIRE(diags.size() == 1);
        REQUIRE(diags[0].code == DiagnosticRecordCode::SUBMIT_STRATEGY_EVENT_FOR_UNKNOWN_BOOK);
    }
    {
        MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = true});
        InMemoryMultiLogSink sink;
        sim.set_multi_log_sink(&sink);
        NormalizedLobEvent ev = makeEvent("MISSING", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1);
        REQUIRE_THROWS(sim.submit_strategy_event(BookId{"X", "MISSING"}, ev));
    }
}

TEST_CASE("Strategy event time travel emits diagnostic") {
    MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = false});
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);
    BookId id{"X", "TIME"};
    NormalizedVectorSource src({makeEvent("TIME", 1, 5, Side::SELL, UpdateType::ADD, 100, 1, 1)});
    sim.add_stream(id, src);
    REQUIRE(sim.step());

    NormalizedLobEvent ev = makeEvent("TIME", 1, 3, Side::BUY, UpdateType::ADD, 101, 1, 2);
    sim.submit_strategy_event(id, ev, 0); // allowed
    ev.ts_received = -5;                  // force time travel
    sim.submit_strategy_event(id, ev, 0);

    auto diags = sink.diagnostics();
    REQUIRE_FALSE(diags.empty());
    REQUIRE(diags.back().code == DiagnosticRecordCode::STRATEGY_EVENT_TIME_TRAVEL);
    REQUIRE(diags.back().book_key == "X:TIME");
}

TEST_CASE("Duplicate stream registration emits diagnostic and is rejected") {
    MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = false});
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId id{"X", "ONE"};
    NormalizedVectorSource srcA({makeEvent("ONE", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    NormalizedVectorSource srcB({makeEvent("ONE", 1, 2, Side::BUY, UpdateType::ADD, 101, 1, 2)});

    sim.add_stream(id, srcA);
    sim.add_stream(id, srcB); // should be rejected

    while (sim.step()) {
    }

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].order_id == 1);

    auto diags = sink.diagnostics();
    REQUIRE(diags.size() == 1);
    REQUIRE(diags[0].code == DiagnosticRecordCode::DUPLICATE_STREAM_FOR_BOOK_IN_MULTI_BOOK_SIMULATOR);
    REQUIRE(diags[0].book_key == "X:ONE");
}

TEST_CASE("Duplicate stream registration throws when fail_fast=true") {
    MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = true});
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId id{"X", "ONE2"};
    NormalizedVectorSource srcA({makeEvent("ONE2", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    NormalizedVectorSource srcB({makeEvent("ONE2", 1, 2, Side::BUY, UpdateType::ADD, 101, 1, 2)});

    sim.add_stream(id, srcA);
    REQUIRE_THROWS(sim.add_stream(id, srcB));
}

TEST_CASE("MultiBookSimulator emits fills with correct book_key across multiple books") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);

    BookId a{"X", "FA"};
    BookId b{"X", "FB"};

    NormalizedVectorSource srcA({makeEvent("FA", 1, 1, Side::SELL, UpdateType::ADD, 100, 5, 1),
                                 makeEvent("FA", 1, 3, Side::BUY, UpdateType::ADD, 100, 2, 2)});
    NormalizedVectorSource srcB({makeEvent("FB", 1, 2, Side::SELL, UpdateType::ADD, 200, 4, 3),
                                 makeEvent("FB", 1, 4, Side::BUY, UpdateType::ADD, 200, 1, 4)});

    sim.add_stream(a, srcA);
    sim.add_stream(b, srcB);

    while (sim.step()) {
    }

    auto fillsA = sink.fills_for("X:FA");
    auto fillsB = sink.fills_for("X:FB");
    REQUIRE(fillsA.size() == 1);
    REQUIRE(fillsB.size() == 1);
    REQUIRE(fillsA[0].book_key == "X:FA");
    REQUIRE(fillsB[0].book_key == "X:FB");
}

TEST_CASE("set_multi_log_sink can switch between sinks without crashing") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink1;
    InMemoryMultiLogSink sink2;

    sim.set_multi_log_sink(&sink1);

    BookId id{"X", "SWITCH"};
    NormalizedVectorSource src({makeEvent("SWITCH", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                makeEvent("SWITCH", 1, 2, Side::BUY, UpdateType::ADD, 101, 1, 2)});
    sim.add_stream(id, src);

    REQUIRE(sim.step());
    REQUIRE(sink1.events().size() == 1);
    REQUIRE(sink2.events().empty());

    sim.set_multi_log_sink(&sink2);

    REQUIRE(sim.step());
    REQUIRE(sink1.events().size() == 1);
    REQUIRE(sink2.events().size() == 1);
}

TEST_CASE("Heap tie-breaking uses ts_received, ts_exchange, then stream index deterministically") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);
    BookId a{"X", "TA"};
    BookId b{"X", "TB"};

    // Same ts_received and ts_exchange, registered A then B, so A should come first
    NormalizedVectorSource srcA({makeEvent("TA", 1, 10, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    NormalizedVectorSource srcB({makeEvent("TB", 1, 10, Side::SELL, UpdateType::ADD, 200, 1, 2)});
    sim.add_stream(a, srcA);
    sim.add_stream(b, srcB);

    while (sim.step()) {
    }
    auto evs = sink.events();
    REQUIRE(evs.size() == 2);
    REQUIRE(evs[0].book_key == "X:TA");
    REQUIRE(evs[1].book_key == "X:TB");
}

TEST_CASE("SymbolId mismatch in event payload is overwritten with book key") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);
    BookId id{"X", "FIX"};
    // Wrong symbol_id in payload
    NormalizedVectorSource src({makeEvent("WRONG", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    sim.add_stream(id, src);
    REQUIRE(sim.step());
    auto evs = sink.events();
    REQUIRE(evs.size() == 1);
    REQUIRE(evs[0].book_key == "X:FIX");
    REQUIRE(evs[0].side == Side::BUY);
}

TEST_CASE("try_normalize returning false is skipped or throws based on fail_fast") {
    struct TryAdapter {
        std::string symbol;
        bool try_normalize(const RawEvent&, NormalizedLobEvent&) const { return false; }
        NormalizedLobEvent normalize(const RawEvent& raw) const { return normalizeFallback(raw); }
        NormalizedLobEvent normalizeFallback(const RawEvent& raw) const {
            NormalizedLobEvent ev{};
            ev.symbol_id = symbol;
            ev.ts_exchange = raw.tsEx;
            ev.ts_received = raw.tsRx;
            ev.side = Side::BUY;
            ev.update_type = UpdateType::ADD;
            ev.price_ticks = raw.px;
            ev.quantity_lots = raw.qty;
            ev.order_id = raw.id;
            ev.trader_id = 1;
            ev.aggressor_id = UnknownAggressorIdSentinel;
            ev.update_source = UpdateSource::HISTORICAL;
            return ev;
        }
    };

    // fail_fast = false: skipped, no events applied
    {
        RawVectorSource src({RawEvent{1, 1, 100, 1, 1}});
        MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = false});
        InMemoryMultiLogSink sink;
        sim.set_multi_log_sink(&sink);
        sim.add_stream(BookId{"X", "TRY"}, src, TryAdapter{"TRY"});
        REQUIRE_FALSE(sim.step());
        REQUIRE(sink.events().empty());
    }
    // fail_fast = true: throws on try_normalize failure
    {
        RawVectorSource src({RawEvent{1, 1, 100, 1, 1}});
        MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = true, .fail_fast = true});
        InMemoryMultiLogSink sink;
        sim.set_multi_log_sink(&sink);
        sim.add_stream(BookId{"X", "TRY"}, src, TryAdapter{"TRY"});
        REQUIRE_THROWS(sim.step());
    }
}

TEST_CASE("require_monotonic_ts_received=false allows out-of-order per stream") {
    MultiBookSimulator sim(MultiBookSimulator::Config{.require_monotonic_ts_received = false, .fail_fast = true});
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);
    BookId id{"X", "OOO"};
    NormalizedVectorSource src({makeEvent("OOO", 1, 10, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                makeEvent("OOO", 1, 5, Side::BUY, UpdateType::ADD, 101, 1, 2)});
    sim.add_stream(id, src);
    REQUIRE(sim.step()); // applies ts_received 10 first
    REQUIRE(sim.step()); // then ts_received 5 (allowed)
    auto evs = sink.events();
    REQUIRE(evs.size() == 2);
    REQUIRE(evs[0].ts_received == 10);
    REQUIRE(evs[1].ts_received == 5);
}

TEST_CASE("set_log_sink on unknown book emits diagnostic") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.set_multi_log_sink(&sink);
    sim.set_log_sink(BookId{"X", "UNKNOWN"}, nullptr);
    auto diags = sink.diagnostics();
    REQUIRE(diags.size() == 1);
    REQUIRE(diags[0].code == DiagnosticRecordCode::SET_LOG_SINK_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR);
    REQUIRE(diags[0].book_key == "X:UNKNOWN");
}
