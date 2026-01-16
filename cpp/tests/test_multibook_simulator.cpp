#include "lobsim/book_id.hpp"
#include "lobsim/log_sink.hpp"
#include "lobsim/multi_log_sink.hpp"
#include "lobsim/multibook_simulator.hpp"
#include "lobsim/types.hpp"

#include <catch2/catch_test_macros.hpp>
#include <utility>
#include <vector>

namespace {

NormalizedLobEvent makeEvent(std::string symbol, std::int64_t tsExchange, std::int64_t tsReceived, Side side,
                             UpdateType type, std::int64_t price, std::int64_t qty, std::int64_t orderId) {
    NormalizedLobEvent ev{};
    ev.symbolId = std::move(symbol);
    ev.tsExchange = tsExchange;
    ev.tsReceived = tsReceived;
    ev.side = side;
    ev.updateType = type;
    ev.priceTicks = price;
    ev.quantityLots = qty;
    ev.orderId = orderId;
    ev.traderId = 1;
    ev.aggressorId = UnknownAggressorIdSentinel;
    ev.updateSource = UpdateSource::HISTORICAL;
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
        ev.symbolId = symbol;
        ev.tsExchange = raw.tsEx;
        ev.tsReceived = raw.tsRx;
        ev.side = Side::BUY;
        ev.updateType = UpdateType::ADD;
        ev.priceTicks = raw.px;
        ev.quantityLots = raw.qty;
        ev.orderId = raw.id;
        ev.traderId = 1;
        ev.aggressorId = UnknownAggressorIdSentinel;
        ev.updateSource = UpdateSource::HISTORICAL;
        return ev;
    }
    std::string symbol;
};

struct CapturingSink final : ILogSink {
    void onFill(const FillRecord& r) override { fills.push_back(r); }
    void onEventApply(const EventApplyRecord& r) override { events.push_back(r); }
    void onDiagnostic(const DiagnosticRecord& r) override { diagnostics.push_back(r); }
    std::vector<FillRecord> fills;
    std::vector<EventApplyRecord> events;
    std::vector<DiagnosticRecord> diagnostics;
};

} // namespace

TEST_CASE("MultiBookSimulator merges events across books in time order and logs bookKey") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);

    BookId a{"X", "A"};
    BookId b{"X", "B"};

    NormalizedVectorSource srcA({makeEvent("A", 10, 10, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                 makeEvent("A", 30, 30, Side::BUY, UpdateType::ADD, 110, 1, 2)});
    NormalizedVectorSource srcB(
        {makeEvent("B", 15, 20, Side::SELL, UpdateType::ADD, 200, 1, 3),
         makeEvent("B", 25, 30, Side::SELL, UpdateType::ADD, 210, 1, 4)}); // same tsReceived as A second event

    sim.addStream(a, srcA);
    sim.addStream(b, srcB);

    std::size_t steps = 0;
    while (sim.step()) {
        ++steps;
    }
    REQUIRE(steps == 4);

    auto evs = sink.events();
    REQUIRE(evs.size() == 4);
    REQUIRE(evs[0].bookKey == "X:A");
    REQUIRE(evs[1].bookKey == "X:B");
    // tsReceived tie at 30 -> tsExchange tie-breaker keeps B(25) before A(30)
    REQUIRE(evs[2].bookKey == "X:B");
    REQUIRE(evs[3].bookKey == "X:A");

    REQUIRE(sim.getBestPriceTicks(a, Side::BUY).has_value());
    REQUIRE(sim.getBestPriceTicks(a, Side::BUY).value() == 110);
    REQUIRE(sim.getBestPriceTicks(b, Side::SELL).has_value());
    REQUIRE(sim.getBestPriceTicks(b, Side::SELL).value() == 200);
}

TEST_CASE("MultiBookSimulator deduces raw type via adapter overload") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);

    BookId id{"X", "C"};
    RawVectorSource src({RawEvent{1, 1, 100, 5, 10}, RawEvent{2, 2, 105, 7, 11}});
    RawAdapter adapter{"C"};

    sim.addStream(id, src, adapter); // should compile and run without spelling RawEvent

    while (sim.step()) {
    }
    auto evs = sink.events();
    REQUIRE(evs.size() == 2);
    for (const auto& e : evs) {
        REQUIRE(e.bookKey == "X:C");
        REQUIRE(e.side == Side::BUY);
        REQUIRE(e.updateType == UpdateType::ADD);
    }
}

TEST_CASE("Non-monotonic per-stream timestamps emit diagnostics when failFast=false") {
    MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = true, .failFast = false});
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);

    BookId id{"X", "D"};
    NormalizedVectorSource src({makeEvent("D", 1, 10, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                makeEvent("D", 2, 5, Side::BUY, UpdateType::ADD, 110, 1, 2)}); // tsReceived decreases
    sim.addStream(id, src);

    REQUIRE(sim.step());       // applies first event
    REQUIRE_FALSE(sim.step()); // second is rejected; no more buffered

    auto diags = sink.diagnostics();
    REQUIRE(diags.size() == 1);
    REQUIRE(diags[0].code == DiagnosticRecordCode::NON_MONOTONIC_TS_RECEIVED_DETECTED_IN_MULTI_BOOK_SIMULATOR);
    REQUIRE(diags[0].bookKey == "X:D");

    auto best = sim.getBestPriceTicks(id, Side::BUY);
    REQUIRE(best.has_value());
    REQUIRE(best.value() == 100);
}

TEST_CASE("Unknown book queries emit diagnostics and return empty") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);

    BookId missing{"X", "MISSING"};
    auto depth = sim.depthAt(missing, Side::BUY, 100);
    auto topn = sim.l2TopN(missing, Side::BUY, 1);
    auto best = sim.getBestPriceTicks(missing, Side::BUY);

    REQUIRE_FALSE(depth.has_value());
    REQUIRE(topn.empty());
    REQUIRE_FALSE(best.has_value());

    auto diags = sink.diagnostics();
    REQUIRE(diags.size() == 3);
    REQUIRE(diags[0].bookKey == "X:MISSING");
    REQUIRE(diags[1].bookKey == "X:MISSING");
    REQUIRE(diags[2].bookKey == "X:MISSING");
}

TEST_CASE("stepUntil/stepFor advance correct counts and currentTime updates") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);

    BookId id{"X", "STEP"};
    NormalizedVectorSource src({makeEvent("STEP", 1, 5, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                makeEvent("STEP", 2, 10, Side::BUY, UpdateType::ADD, 101, 1, 2),
                                makeEvent("STEP", 3, 15, Side::BUY, UpdateType::ADD, 102, 1, 3)});
    sim.addStream(id, src);

    REQUIRE(sim.stepUntil(5) == 1);
    REQUIRE(sim.currentTime().has_value());
    REQUIRE(sim.currentTime().value() == 5);

    REQUIRE(sim.stepFor(4) == 0);    // no events in (5,9]
    REQUIRE(sim.stepUntil(12) == 1); // picks tsReceived=10
    REQUIRE(sim.currentTime().value() == 10);

    REQUIRE(sim.step()); // last event
    REQUIRE(sim.currentTime().value() == 15);
    REQUIRE_FALSE(sim.step()); // exhausted
}

TEST_CASE("apply rejects non-monotonic tsReceived and emits diagnostic") {
    MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = true, .failFast = true});
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);

    BookId id{"X", "FAIL"};
    NormalizedVectorSource src({makeEvent("FAIL", 1, 10, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    sim.addStream(id, src);

    REQUIRE(sim.step()); // establishes currentTsReceived_ == 10

    NormalizedLobEvent ev = makeEvent("FAIL", 1, 0, Side::BUY, UpdateType::ADD, 101, 1, 2);
    sim.apply(id, ev); // should be rejected but not throw

    auto diags = sink.diagnostics();
    REQUIRE_FALSE(diags.empty());
    REQUIRE(diags.back().code == DiagnosticRecordCode::NON_MONOTONIC_TS_RECEIVED_DETECTED_IN_MULTI_BOOK_SIMULATOR);
    REQUIRE(diags.back().bookKey == "X:FAIL");

    auto best = sim.getBestPriceTicks(id, Side::BUY);
    REQUIRE(best.has_value());
    REQUIRE(best.value() == 100);
}

TEST_CASE("apply on unknown book emits diagnostic and does not crash") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);

    BookId id{"X", "NOBOOK"};
    NormalizedLobEvent ev = makeEvent("NOBOOK", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1);
    sim.apply(id, ev);

    auto diags = sink.diagnostics();
    REQUIRE(diags.size() == 1);
    REQUIRE(diags[0].code == DiagnosticRecordCode::APPLY_EVENT_REQUESTED_FOR_UNKNOWN_BOOK);
    REQUIRE(diags[0].bookKey == "X:NOBOOK");
}

TEST_CASE("Adapter failures emit diagnostics or throw depending on failFast") {
    struct BadAdapter {
        std::string symbol;
        NormalizedLobEvent normalize(const RawEvent&) const { throw std::runtime_error("bad"); }
    };
    RawVectorSource src({RawEvent{1, 1, 100, 1, 1}});

    // failFast=false: skip and emit no event
    {
        MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = true, .failFast = false});
        InMemoryMultiLogSink sink;
        sim.setMultiLogSink(&sink);
        sim.addStream(BookId{"X", "BAD"}, src, BadAdapter{"BAD"});
        REQUIRE_FALSE(sim.step()); // normalization failed, nothing applied
        REQUIRE(sink.events().empty());
    }

    // failFast=true: throws
    {
        RawVectorSource src2({RawEvent{1, 1, 100, 1, 1}});
        MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = true, .failFast = true});
        InMemoryMultiLogSink sink;
        sim.setMultiLogSink(&sink);
        sim.addStream(BookId{"X", "BAD"}, src2, BadAdapter{"BAD"});
        REQUIRE_THROWS(sim.step());
    }
}

TEST_CASE("Mixed normalized and raw streams merge correctly") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);

    BookId norm{"X", "NORM"};
    BookId raw{"X", "RAW"};

    NormalizedVectorSource srcNorm({makeEvent("NORM", 1, 5, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                    makeEvent("NORM", 2, 15, Side::BUY, UpdateType::ADD, 101, 1, 2)});
    RawVectorSource srcRaw({RawEvent{1, 10, 200, 1, 10}, RawEvent{2, 20, 201, 1, 11}});
    RawAdapter adapter{"RAW"};

    sim.addStream(norm, srcNorm);
    sim.addStream(raw, srcRaw, adapter);

    std::vector<std::string> seen;
    while (sim.step()) {
        auto evs = sink.events();
        if (!evs.empty()) {
            seen.push_back(evs.back().bookKey);
        }
    }
    REQUIRE(seen == std::vector<std::string>{"X:NORM", "X:RAW", "X:NORM", "X:RAW"});
}

TEST_CASE("Per-book filtering APIs return only matching records") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);
    BookId a{"X", "A2"};
    BookId b{"X", "B2"};

    NormalizedVectorSource srcA({makeEvent("A2", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    NormalizedVectorSource srcB({makeEvent("B2", 1, 2, Side::SELL, UpdateType::ADD, 200, 1, 2)});
    sim.addStream(a, srcA);
    sim.addStream(b, srcB);
    while (sim.step()) {
    }
    auto fillsA = sink.fillsFor("X:A2");
    auto fillsB = sink.fillsFor("X:B2");
    auto eventsA = sink.eventsFor("X:A2");
    auto eventsB = sink.eventsFor("X:B2");
    REQUIRE(eventsA.size() == 1);
    REQUIRE(eventsB.size() == 1);
    REQUIRE(eventsA[0].bookKey == "X:A2");
    REQUIRE(eventsB[0].bookKey == "X:B2");
    REQUIRE(fillsA.empty());
    REQUIRE(fillsB.empty());
}

TEST_CASE("No multi sink attached does not crash and no diagnostics are emitted") {
    MultiBookSimulator sim;
    BookId id{"X", "NOSINK"};
    NormalizedVectorSource src({makeEvent("NOSINK", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    sim.addStream(id, src);
    // No sink set; step should still advance without dereferencing null
    REQUIRE(sim.step());
}

TEST_CASE("setLogSink overrides multi sink for a single book") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink multi;
    sim.setMultiLogSink(&multi);

    BookId a{"X", "SA"};
    BookId b{"X", "SB"};
    NormalizedVectorSource srcA({makeEvent("SA", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    NormalizedVectorSource srcB({makeEvent("SB", 1, 2, Side::BUY, UpdateType::ADD, 200, 1, 2)});
    sim.addStream(a, srcA);
    sim.addStream(b, srcB);

    CapturingSink localA;
    sim.setLogSink(a, &localA);

    while (sim.step()) {
    }

    REQUIRE(localA.events.size() == 1);
    REQUIRE(localA.events[0].bookKey == "X:SA");

    REQUIRE(multi.eventsFor("X:SA").empty());
    REQUIRE(multi.eventsFor("X:SB").size() == 1);
    REQUIRE(multi.eventsFor("X:SB")[0].bookKey == "X:SB");
}

TEST_CASE("setMultiLogSink(nullptr) detaches wrappers safely") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink multi;
    sim.setMultiLogSink(&multi);

    BookId id{"X", "DETACH"};
    NormalizedVectorSource src({makeEvent("DETACH", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                makeEvent("DETACH", 2, 2, Side::BUY, UpdateType::ADD, 101, 1, 2)});
    sim.addStream(id, src);

    REQUIRE(sim.step());
    REQUIRE(multi.events().size() == 1);

    sim.setMultiLogSink(nullptr);

    REQUIRE(sim.step()); // still applies, but no more logging to multi
    REQUIRE(multi.events().size() == 1);

    auto best = sim.getBestPriceTicks(id, Side::BUY);
    REQUIRE(best.has_value());
    REQUIRE(best.value() == 101);
}

TEST_CASE("Raw stream can skip invalid events and still apply later valid events (tryNormalize path)") {
    struct SelectiveAdapter {
        std::string symbol;
        bool tryNormalize(const RawEvent& raw, NormalizedLobEvent& out) const {
            if (raw.id == 1) {
                return false;
            }
            out = NormalizedLobEvent{};
            out.symbolId = symbol;
            out.tsExchange = raw.tsEx;
            out.tsReceived = raw.tsRx;
            out.side = Side::BUY;
            out.updateType = UpdateType::ADD;
            out.priceTicks = raw.px;
            out.quantityLots = raw.qty;
            out.orderId = raw.id;
            out.traderId = 1;
            out.aggressorId = UnknownAggressorIdSentinel;
            out.updateSource = UpdateSource::HISTORICAL;
            return true;
        }
        NormalizedLobEvent normalize(const RawEvent& raw) const {
            NormalizedLobEvent out{};
            if (!tryNormalize(raw, out)) {
                throw std::runtime_error("bad");
            }
            return out;
        }
    };

    RawVectorSource src({RawEvent{1, 1, 100, 1, 1}, RawEvent{2, 2, 101, 1, 2}});
    MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = true, .failFast = false});
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);
    sim.addStream(BookId{"X", "SKIP"}, src, SelectiveAdapter{"SKIP"});

    std::size_t steps = 0;
    while (sim.step()) {
        ++steps;
    }
    REQUIRE(steps == 1);
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].orderId == 2);
}

TEST_CASE("Raw stream can skip invalid events and still apply later valid events (throwing normalize path)") {
    struct ThrowingAdapter {
        std::string symbol;
        NormalizedLobEvent normalize(const RawEvent& raw) const {
            if (raw.id == 1) {
                throw std::runtime_error("bad");
            }
            NormalizedLobEvent ev{};
            ev.symbolId = symbol;
            ev.tsExchange = raw.tsEx;
            ev.tsReceived = raw.tsRx;
            ev.side = Side::BUY;
            ev.updateType = UpdateType::ADD;
            ev.priceTicks = raw.px;
            ev.quantityLots = raw.qty;
            ev.orderId = raw.id;
            ev.traderId = 1;
            ev.aggressorId = UnknownAggressorIdSentinel;
            ev.updateSource = UpdateSource::HISTORICAL;
            return ev;
        }
    };

    RawVectorSource src({RawEvent{1, 1, 100, 1, 1}, RawEvent{2, 2, 101, 1, 2}});
    MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = true, .failFast = false});
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);
    sim.addStream(BookId{"X", "SKIP2"}, src, ThrowingAdapter{"SKIP2"});

    std::size_t steps = 0;
    while (sim.step()) {
        ++steps;
    }
    REQUIRE(steps == 1);
    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].orderId == 2);
}

TEST_CASE("stepFor before any step uses base=0") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);
    BookId id{"X", "BASE0"};
    NormalizedVectorSource src({makeEvent("BASE0", 1, 5, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                makeEvent("BASE0", 1, 10, Side::BUY, UpdateType::ADD, 101, 1, 2)});
    sim.addStream(id, src);

    REQUIRE(sim.stepFor(7) == 1);
    REQUIRE(sim.currentTime().has_value());
    REQUIRE(sim.currentTime().value() == 5);
    REQUIRE(sink.events().size() == 1);
}

TEST_CASE("addBook returns false on duplicate book key") {
    MultiBookSimulator sim;
    REQUIRE(sim.addBook(BookId{"X", "DUP"}));
    REQUIRE_FALSE(sim.addBook(BookId{"X", "DUP"}));
}

TEST_CASE("Strategy events merge with feeds honoring latency and ordering") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);

    BookId id{"X", "STRAT"};
    NormalizedVectorSource src({makeEvent("STRAT", 1, 5, Side::SELL, UpdateType::ADD, 100, 5, 1)});
    sim.addStream(id, src);

    NormalizedLobEvent strat1 = makeEvent("STRAT", 1, 6, Side::BUY, UpdateType::ADD, 100, 2, 10);
    NormalizedLobEvent strat2 = makeEvent("STRAT", 1, 7, Side::BUY, UpdateType::ADD, 100, 3, 11);
    sim.submitStrategyEvent(id, strat1);
    sim.submitStrategyEvent(id, strat2);

    std::vector<std::string> order;
    while (sim.step()) {
        auto evs = sink.events();
        order.push_back(evs.back().bookKey + ":" + std::to_string(evs.back().orderId));
    }
    REQUIRE(order.size() == 3);
    REQUIRE(order[0] == "X:STRAT:1");
    REQUIRE(order[1] == "X:STRAT:10");
    REQUIRE(order[2] == "X:STRAT:11");
}

TEST_CASE("Strategy event into unknown book emits diagnostic and optional throw") {
    {
        MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = true, .failFast = false});
        InMemoryMultiLogSink sink;
        sim.setMultiLogSink(&sink);
        NormalizedLobEvent ev = makeEvent("MISSING", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1);
        sim.submitStrategyEvent(BookId{"X", "MISSING"}, ev);
        auto diags = sink.diagnostics();
        REQUIRE(diags.size() == 1);
        REQUIRE(diags[0].code == DiagnosticRecordCode::SUBMIT_STRATEGY_EVENT_FOR_UNKNOWN_BOOK);
    }
    {
        MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = true, .failFast = true});
        InMemoryMultiLogSink sink;
        sim.setMultiLogSink(&sink);
        NormalizedLobEvent ev = makeEvent("MISSING", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1);
        REQUIRE_THROWS(sim.submitStrategyEvent(BookId{"X", "MISSING"}, ev));
    }
}

TEST_CASE("Strategy event time travel emits diagnostic") {
    MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = true, .failFast = false});
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);
    BookId id{"X", "TIME"};
    NormalizedVectorSource src({makeEvent("TIME", 1, 5, Side::SELL, UpdateType::ADD, 100, 1, 1)});
    sim.addStream(id, src);
    REQUIRE(sim.step());

    NormalizedLobEvent ev = makeEvent("TIME", 1, 3, Side::BUY, UpdateType::ADD, 101, 1, 2);
    sim.submitStrategyEvent(id, ev, 0); // allowed
    ev.tsReceived = 1;                  // force time travel
    sim.submitStrategyEvent(id, ev, 0);

    auto diags = sink.diagnostics();
    REQUIRE_FALSE(diags.empty());
    REQUIRE(diags.back().code == DiagnosticRecordCode::STRATEGY_EVENT_TIME_TRAVEL);
    REQUIRE(diags.back().bookKey == "X:TIME");
}

TEST_CASE("Duplicate stream registration emits diagnostic and is rejected") {
    MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = true, .failFast = false});
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);

    BookId id{"X", "ONE"};
    NormalizedVectorSource srcA({makeEvent("ONE", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    NormalizedVectorSource srcB({makeEvent("ONE", 1, 2, Side::BUY, UpdateType::ADD, 101, 1, 2)});

    sim.addStream(id, srcA);
    sim.addStream(id, srcB); // should be rejected

    while (sim.step()) {
    }

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].orderId == 1);

    auto diags = sink.diagnostics();
    REQUIRE(diags.size() == 1);
    REQUIRE(diags[0].code == DiagnosticRecordCode::DUPLICATE_STREAM_FOR_BOOK_IN_MULTI_BOOK_SIMULATOR);
    REQUIRE(diags[0].bookKey == "X:ONE");
}

TEST_CASE("Duplicate stream registration throws when failFast=true") {
    MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = true, .failFast = true});
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);

    BookId id{"X", "ONE2"};
    NormalizedVectorSource srcA({makeEvent("ONE2", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    NormalizedVectorSource srcB({makeEvent("ONE2", 1, 2, Side::BUY, UpdateType::ADD, 101, 1, 2)});

    sim.addStream(id, srcA);
    REQUIRE_THROWS(sim.addStream(id, srcB));
}

TEST_CASE("MultiBookSimulator emits fills with correct bookKey across multiple books") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);

    BookId a{"X", "FA"};
    BookId b{"X", "FB"};

    NormalizedVectorSource srcA({makeEvent("FA", 1, 1, Side::SELL, UpdateType::ADD, 100, 5, 1),
                                 makeEvent("FA", 1, 3, Side::BUY, UpdateType::ADD, 100, 2, 2)});
    NormalizedVectorSource srcB({makeEvent("FB", 1, 2, Side::SELL, UpdateType::ADD, 200, 4, 3),
                                 makeEvent("FB", 1, 4, Side::BUY, UpdateType::ADD, 200, 1, 4)});

    sim.addStream(a, srcA);
    sim.addStream(b, srcB);

    while (sim.step()) {
    }

    auto fillsA = sink.fillsFor("X:FA");
    auto fillsB = sink.fillsFor("X:FB");
    REQUIRE(fillsA.size() == 1);
    REQUIRE(fillsB.size() == 1);
    REQUIRE(fillsA[0].bookKey == "X:FA");
    REQUIRE(fillsB[0].bookKey == "X:FB");
}

TEST_CASE("setMultiLogSink can switch between sinks without crashing") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink1;
    InMemoryMultiLogSink sink2;

    sim.setMultiLogSink(&sink1);

    BookId id{"X", "SWITCH"};
    NormalizedVectorSource src({makeEvent("SWITCH", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                makeEvent("SWITCH", 1, 2, Side::BUY, UpdateType::ADD, 101, 1, 2)});
    sim.addStream(id, src);

    REQUIRE(sim.step());
    REQUIRE(sink1.events().size() == 1);
    REQUIRE(sink2.events().empty());

    sim.setMultiLogSink(&sink2);

    REQUIRE(sim.step());
    REQUIRE(sink1.events().size() == 1);
    REQUIRE(sink2.events().size() == 1);
}

TEST_CASE("Heap tie-breaking uses tsReceived, tsExchange, then stream index deterministically") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);
    BookId a{"X", "TA"};
    BookId b{"X", "TB"};

    // Same tsReceived and tsExchange, registered A then B, so A should come first
    NormalizedVectorSource srcA({makeEvent("TA", 1, 10, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    NormalizedVectorSource srcB({makeEvent("TB", 1, 10, Side::SELL, UpdateType::ADD, 200, 1, 2)});
    sim.addStream(a, srcA);
    sim.addStream(b, srcB);

    while (sim.step()) {
    }
    auto evs = sink.events();
    REQUIRE(evs.size() == 2);
    REQUIRE(evs[0].bookKey == "X:TA");
    REQUIRE(evs[1].bookKey == "X:TB");
}

TEST_CASE("SymbolId mismatch in event payload is overwritten with book key") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);
    BookId id{"X", "FIX"};
    // Wrong symbolId in payload
    NormalizedVectorSource src({makeEvent("WRONG", 1, 1, Side::BUY, UpdateType::ADD, 100, 1, 1)});
    sim.addStream(id, src);
    REQUIRE(sim.step());
    auto evs = sink.events();
    REQUIRE(evs.size() == 1);
    REQUIRE(evs[0].bookKey == "X:FIX");
    REQUIRE(evs[0].side == Side::BUY);
}

TEST_CASE("tryNormalize returning false is skipped or throws based on failFast") {
    struct TryAdapter {
        std::string symbol;
        bool tryNormalize(const RawEvent&, NormalizedLobEvent&) const { return false; }
        NormalizedLobEvent normalize(const RawEvent& raw) const { return normalizeFallback(raw); }
        NormalizedLobEvent normalizeFallback(const RawEvent& raw) const {
            NormalizedLobEvent ev{};
            ev.symbolId = symbol;
            ev.tsExchange = raw.tsEx;
            ev.tsReceived = raw.tsRx;
            ev.side = Side::BUY;
            ev.updateType = UpdateType::ADD;
            ev.priceTicks = raw.px;
            ev.quantityLots = raw.qty;
            ev.orderId = raw.id;
            ev.traderId = 1;
            ev.aggressorId = UnknownAggressorIdSentinel;
            ev.updateSource = UpdateSource::HISTORICAL;
            return ev;
        }
    };

    // failFast = false: skipped, no events applied
    {
        RawVectorSource src({RawEvent{1, 1, 100, 1, 1}});
        MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = true, .failFast = false});
        InMemoryMultiLogSink sink;
        sim.setMultiLogSink(&sink);
        sim.addStream(BookId{"X", "TRY"}, src, TryAdapter{"TRY"});
        REQUIRE_FALSE(sim.step());
        REQUIRE(sink.events().empty());
    }
    // failFast = true: throws on tryNormalize failure
    {
        RawVectorSource src({RawEvent{1, 1, 100, 1, 1}});
        MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = true, .failFast = true});
        InMemoryMultiLogSink sink;
        sim.setMultiLogSink(&sink);
        sim.addStream(BookId{"X", "TRY"}, src, TryAdapter{"TRY"});
        REQUIRE_THROWS(sim.step());
    }
}

TEST_CASE("requireMonotonicTsReceived=false allows out-of-order per stream") {
    MultiBookSimulator sim(MultiBookSimulator::Config{.requireMonotonicTsReceived = false, .failFast = true});
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);
    BookId id{"X", "OOO"};
    NormalizedVectorSource src({makeEvent("OOO", 1, 10, Side::BUY, UpdateType::ADD, 100, 1, 1),
                                makeEvent("OOO", 1, 5, Side::BUY, UpdateType::ADD, 101, 1, 2)});
    sim.addStream(id, src);
    REQUIRE(sim.step()); // applies tsReceived 10 first
    REQUIRE(sim.step()); // then tsReceived 5 (allowed)
    auto evs = sink.events();
    REQUIRE(evs.size() == 2);
    REQUIRE(evs[0].tsReceived == 10);
    REQUIRE(evs[1].tsReceived == 5);
}

TEST_CASE("setLogSink on unknown book emits diagnostic") {
    MultiBookSimulator sim;
    InMemoryMultiLogSink sink;
    sim.setMultiLogSink(&sink);
    sim.setLogSink(BookId{"X", "UNKNOWN"}, nullptr);
    auto diags = sink.diagnostics();
    REQUIRE(diags.size() == 1);
    REQUIRE(diags[0].code == DiagnosticRecordCode::SET_LOG_SINK_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR);
    REQUIRE(diags[0].bookKey == "X:UNKNOWN");
}
