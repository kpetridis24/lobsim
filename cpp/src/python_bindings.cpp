#if __has_include(<pybind11/pybind11.h>)

#include "lobsim/book_id.hpp"
#include "lobsim/lob_event.hpp"
#include "lobsim/multi_log_sink.hpp"
#include "lobsim/multibook_simulator.hpp"
#include "lobsim/paper_trading_simulator.hpp"
#include "lobsim/replay_session.hpp"
#include "lobsim/types.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <optional>
#include <span>
#include <string>
#include <vector>

// CHANGE THIS include to your actual sink header
#include "lobsim/in_memory_sink.hpp"

namespace py = pybind11;

namespace {
struct PyNormalizedVectorSource {
    explicit PyNormalizedVectorSource(std::vector<NormalizedLobEvent> events) : events_(std::move(events)) {}

    bool next(NormalizedLobEvent& out) {
        if (index_ >= events_.size()) {
            return false;
        }
        out = events_[index_++];
        return true;
    }

    void reset() { index_ = 0; }

private:
    std::vector<NormalizedLobEvent> events_{};
    std::size_t index_{0};
};

struct PyMultiBookWrapper {
    explicit PyMultiBookWrapper(const MultiBookSimulator::Config& cfg) : sim(cfg) {}
    PyMultiBookWrapper(const PyMultiBookWrapper&) = delete;
    PyMultiBookWrapper& operator=(const PyMultiBookWrapper&) = delete;
    PyMultiBookWrapper(PyMultiBookWrapper&&) = default;
    PyMultiBookWrapper& operator=(PyMultiBookWrapper&&) = default;

    MultiBookSimulator sim;
    std::vector<std::shared_ptr<PyNormalizedVectorSource>> heldSources; // keep sources alive
};
} // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() = "lobsim: L3 replay + paper trading engine";

    // Types submodule to group enums/constants
    auto types = m.def_submodule("types", "Enums and sentinel constants");

    py::enum_<Side>(types, "Side").value("SELL", Side::SELL).value("BUY", Side::BUY);

    py::enum_<UpdateType>(types, "UpdateType")
        .value("ADD", UpdateType::ADD)
        .value("DELETE", UpdateType::DELETE)
        .value("SUBTRACT", UpdateType::SUBTRACT)
        .value("MATCH", UpdateType::MATCH)
        .value("SET", UpdateType::SET);

    py::enum_<UpdateSource>(types, "UpdateSource")
        .value("HISTORICAL", UpdateSource::HISTORICAL)
        .value("STRATEGY", UpdateSource::STRATEGY);

    // Sentinels
    types.attr("UnknownOrderIdSentinel") = py::int_(UnknownOrderIdSentinel);
    types.attr("UnknownTraderIdSentinel") = py::int_(UnknownTraderIdSentinel);
    types.attr("UnknownAggressorIdSentinel") = py::int_(UnknownAggressorIdSentinel);
    types.attr("NoAggressorNeededSentinel") = py::int_(NoAggressorNeededSentinel);

    // Replay submodule
    auto replay = m.def_submodule("replay", "Replay session utilities");

    py::class_<lobsim::replay::ReplayConfig>(replay, "ReplayConfig")
        .def(py::init([](bool requireMonotonicTsReceived, bool failFast) {
                 lobsim::replay::ReplayConfig cfg{};
                 cfg.requireMonotonicTsReceived = requireMonotonicTsReceived;
                 cfg.failFast = failFast;
                 return cfg;
             }),
             py::arg("require_monotonic_ts_received") = true, py::arg("fail_fast") = true)
        .def_readwrite("require_monotonic_ts_received", &lobsim::replay::ReplayConfig::requireMonotonicTsReceived)
        .def_readwrite("fail_fast", &lobsim::replay::ReplayConfig::failFast);

    py::class_<lobsim::replay::RunSummary>(replay, "RunSummary")
        .def_readonly("num_raw_events", &lobsim::replay::RunSummary::numRawEvents)
        .def_readonly("num_normalized_events", &lobsim::replay::RunSummary::numNormalizedEvents)
        .def_readonly("num_engine_updates", &lobsim::replay::RunSummary::numEngineUpdates)
        .def_readonly("num_adapter_failures", &lobsim::replay::RunSummary::numAdapterFailures)
        .def_readonly("has_ts_range", &lobsim::replay::RunSummary::hasTsRange)
        .def_readonly("first_ts_received", &lobsim::replay::RunSummary::firstTsReceived)
        .def_readonly("last_ts_received", &lobsim::replay::RunSummary::lastTsReceived);

    py::class_<lobsim::replay::ReplaySession>(replay, "ReplaySession")
        .def(py::init([](PaperTradingSimulator& engine, const lobsim::replay::ReplayConfig& cfg) {
                 return lobsim::replay::ReplaySession(engine, cfg);
             }),
             py::arg("engine"), py::arg("config") = lobsim::replay::ReplayConfig{}, py::keep_alive<1, 2>())
        .def("step", &lobsim::replay::ReplaySession::step, py::arg("event"))
        .def(
            "run",
            [](lobsim::replay::ReplaySession& s, const std::vector<NormalizedLobEvent>& events,
               const lobsim::replay::ReplayConfig& cfg) {
                return s.run(std::span<const NormalizedLobEvent>(events.data(), events.size()), cfg);
            },
            py::arg("events"), py::arg("config") = lobsim::replay::ReplayConfig{})
        .def(
            "run_raw",
            [](lobsim::replay::ReplaySession& s, py::object source, py::object adapter,
               const lobsim::replay::ReplayConfig& cfg) {
                std::uint64_t rawCount = 0;
                std::uint64_t adapterFailures = 0;
                std::vector<NormalizedLobEvent> normalized;
                normalized.reserve(1024);

                py::object callTarget = adapter;
                if (py::hasattr(adapter, "normalize")) {
                    callTarget = adapter.attr("normalize");
                }
                if (!PyCallable_Check(callTarget.ptr())) {
                    throw std::runtime_error("ReplaySession.run_raw: adapter must be callable or have .normalize");
                }

                py::object iter = py::iter(source);
                while (true) {
                    py::object item = py::reinterpret_steal<py::object>(PyIter_Next(iter.ptr()));
                    if (!item) {
                        if (PyErr_Occurred()) {
                            throw py::error_already_set();
                        }
                        break;
                    }

                    ++rawCount;
                    try {
                        py::object result = callTarget(item);
                        NormalizedLobEvent ev = result.cast<NormalizedLobEvent>();
                        normalized.push_back(std::move(ev));
                    } catch (py::error_already_set& e) {
                        ++adapterFailures;
                        if (cfg.failFast) {
                            throw;
                        }
                        PyErr_Clear();
                        continue;
                    } catch (const std::exception&) {
                        ++adapterFailures;
                        if (cfg.failFast) {
                            throw;
                        }
                        PyErr_Clear();
                        continue;
                    }
                }

                auto summary = s.run(std::span<const NormalizedLobEvent>(normalized.data(), normalized.size()), cfg);
                summary.numRawEvents = rawCount;
                summary.numAdapterFailures = adapterFailures;
                summary.numNormalizedEvents = static_cast<std::uint64_t>(normalized.size());
                return summary;
            },
            py::arg("source"), py::arg("adapter"), py::arg("config") = lobsim::replay::ReplayConfig{});

    // Normalized event (handy for Python-driven loops)
    py::class_<NormalizedLobEvent>(m, "NormalizedLobEvent")
        .def(py::init([](std::int64_t tsEx, std::int64_t tsRecv, Side side, UpdateType ut, std::int64_t priceTicks,
                         std::int64_t qtyLots, std::int64_t orderId, std::int64_t traderId, std::int64_t aggressorId,
                         UpdateSource src, std::string symbolId) {
                 return NormalizedLobEvent{tsEx,    tsRecv,   side,        ut,  priceTicks,         qtyLots,
                                           orderId, traderId, aggressorId, src, std::move(symbolId)};
             }),
             py::arg("tsExchange") = 0, py::arg("tsReceived") = 0,
             py::arg_v("side", Side::BUY, "lobsim.types.Side.BUY"),
             py::arg_v("updateType", UpdateType::ADD, "lobsim.types.UpdateType.ADD"), py::arg("priceTicks") = 0,
             py::arg("quantityLots") = 0, py::arg("orderId") = UnknownOrderIdSentinel,
             py::arg("traderId") = UnknownTraderIdSentinel, py::arg("aggressorId") = NoAggressorNeededSentinel,
             py::arg_v("updateSource", UpdateSource::HISTORICAL, "lobsim.types.UpdateSource.HISTORICAL"),
             py::arg("symbolId") = std::string{})
        .def_readwrite("tsExchange", &NormalizedLobEvent::tsExchange)
        .def_readwrite("tsReceived", &NormalizedLobEvent::tsReceived)
        .def_readwrite("side", &NormalizedLobEvent::side)
        .def_readwrite("updateType", &NormalizedLobEvent::updateType)
        .def_readwrite("priceTicks", &NormalizedLobEvent::priceTicks)
        .def_readwrite("quantityLots", &NormalizedLobEvent::quantityLots)
        .def_readwrite("orderId", &NormalizedLobEvent::orderId)
        .def_readwrite("traderId", &NormalizedLobEvent::traderId)
        .def_readwrite("aggressorId", &NormalizedLobEvent::aggressorId)
        .def_readwrite("updateSource", &NormalizedLobEvent::updateSource)
        .def_readwrite("symbolId", &NormalizedLobEvent::symbolId);

    py::class_<BookId>(m, "BookId")
        .def(py::init<std::string, std::string>(), py::arg("venue") = std::string{}, py::arg("symbol") = std::string{})
        .def_readwrite("venue", &BookId::venue)
        .def_readwrite("symbol", &BookId::symbol)
        .def_property_readonly("book_key", [](const BookId& id) { return bookKey(id); });

    // Sink bindings (assumes your sink exposes fills() -> const std::vector<FillEvent>&)
    // If your names differ, keep the idea and adjust the method names/struct name.
    py::class_<FillRecord>(m, "FillRecord")
        .def_readonly("seq", &FillRecord::seq)
        .def_readonly("tsExchange", &FillRecord::tsExchange)
        .def_readonly("tsReceived", &FillRecord::tsReceived)
        .def_readonly("priceTicks", &FillRecord::priceTicks)
        .def_readonly("qtyLots", &FillRecord::qtyLots)
        .def_readonly("makerSide", &FillRecord::makerSide)
        .def_readonly("makerOrderId", &FillRecord::makerOrderId)
        .def_readonly("makerTraderId", &FillRecord::makerTraderId)
        .def_readonly("makerSource", &FillRecord::makerSource)
        .def_readonly("takerSide", &FillRecord::takerSide)
        .def_readonly("takerOrderId", &FillRecord::takerOrderId)
        .def_readonly("takerTraderId", &FillRecord::takerTraderId)
        .def_readonly("takerSource", &FillRecord::takerSource)
        .def_readonly("bookKey", &FillRecord::bookKey);

    py::class_<EventApplyRecord>(m, "EventApplyRecord")
        .def_readonly("seq", &EventApplyRecord::seq)
        .def_readonly("tsExchange", &EventApplyRecord::tsExchange)
        .def_readonly("tsReceived", &EventApplyRecord::tsReceived)
        .def_readonly("side", &EventApplyRecord::side)
        .def_readonly("updateType", &EventApplyRecord::updateType)
        .def_readonly("source", &EventApplyRecord::source)
        .def_readonly("priceTicks", &EventApplyRecord::priceTicks)
        .def_readonly("qtyLots", &EventApplyRecord::qtyLots)
        .def_readonly("orderId", &EventApplyRecord::orderId)
        .def_readonly("traderId", &EventApplyRecord::traderId)
        .def_readonly("aggressorId", &EventApplyRecord::aggressorId)
        .def_readonly("bookKey", &EventApplyRecord::bookKey);

    py::class_<InMemoryLogSink>(m, "InMemoryLogSink")
        .def(py::init<>())
        .def("reset", &InMemoryLogSink::reset)
        .def("get_fills", [](const InMemoryLogSink& s) {
            // return a copy so Python can hold it safely
            return s.getFills();
        });

    py::class_<InMemoryMultiLogSink>(m, "InMemoryMultiLogSink")
        .def(py::init<>())
        .def("reset", &InMemoryMultiLogSink::reset)
        .def("fills", [](const InMemoryMultiLogSink& s) { return s.fills(); })
        .def("events", [](const InMemoryMultiLogSink& s) { return s.events(); })
        .def("diagnostics", [](const InMemoryMultiLogSink& s) { return s.diagnostics(); })
        .def("fills_for", [](const InMemoryMultiLogSink& s, const std::string& key) { return s.fillsFor(key); })
        .def("events_for", [](const InMemoryMultiLogSink& s, const std::string& key) { return s.eventsFor(key); })
        .def("diagnostics_for",
             [](const InMemoryMultiLogSink& s, const std::string& key) { return s.diagnosticsFor(key); });

    // Engine bindings
    py::class_<PaperTradingSimulator>(m, "PaperTradingSimulator")
        .def(py::init<>())
        .def(py::init([](const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
                         const std::vector<std::int64_t>& quantities, InMemoryLogSink* sink) {
                 PaperTradingSimulator eng;
                 if (sink != nullptr) {
                     eng.setLogSink(sink);
                 }
                 eng.initFromL2Snapshot(sides, prices, quantities);
                 return eng;
             }),
             py::arg("sides"), py::arg("prices"), py::arg("quantities"), py::arg("sink") = nullptr,
             py::keep_alive<1, 5>())

        // Ensure sink lifetime: sink stays alive as long as engine lives (Python-side).
        .def(
            "set_log_sink", [](PaperTradingSimulator& eng, InMemoryLogSink& sink) { eng.setLogSink(&sink); },
            py::arg("sink"), py::keep_alive<1, 2>())

        // Apply a NormalizedLobEvent directly
        .def(
            "update", [](PaperTradingSimulator& eng, const NormalizedLobEvent& ev) { eng.update(ev); },
            py::arg("event"))

        .def(
            "init_from_l2_snapshot",
            [](PaperTradingSimulator& eng, const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
               const std::vector<std::int64_t>& quantities) { eng.initFromL2Snapshot(sides, prices, quantities); },
            py::arg("sides"), py::arg("prices"), py::arg("quantities"))

        .def(
            "init_from_l3_snapshot",
            [](PaperTradingSimulator& eng, const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
               const std::vector<std::int64_t>& quantities, const std::vector<std::int64_t>& orderIds,
               const std::vector<std::int64_t>& traderIds) {
                eng.initFromL3Snapshot(sides, prices, quantities, orderIds, traderIds);
            },
            py::arg("sides"), py::arg("prices"), py::arg("quantities"), py::arg("orderIds"), py::arg("traderIds"))

        .def("depth_at", &PaperTradingSimulator::depthAt, py::arg("side"), py::arg("price_ticks"))

        .def("l2_top_n", &PaperTradingSimulator::l2TopN, py::arg("side"), py::arg("n"))

        .def("get_best_price_ticks", &PaperTradingSimulator::getBestPriceTicks, py::arg("side"));

    // Multibook bindings
    auto multibook = m.def_submodule("multibook", "Multi-book simulator");

    py::class_<MultiBookSimulator::Config>(multibook, "Config")
        .def(py::init([](bool requireMonotonicTsReceived, bool failFast) {
                 MultiBookSimulator::Config cfg{};
                 cfg.requireMonotonicTsReceived = requireMonotonicTsReceived;
                 cfg.failFast = failFast;
                 return cfg;
             }),
             py::arg("require_monotonic_ts_received") = true, py::arg("fail_fast") = true)
        .def_readwrite("require_monotonic_ts_received", &MultiBookSimulator::Config::requireMonotonicTsReceived)
        .def_readwrite("fail_fast", &MultiBookSimulator::Config::failFast);

    py::class_<PyMultiBookWrapper>(multibook, "MultiBookSimulator")
        .def(py::init<const MultiBookSimulator::Config&>(), py::arg("config") = MultiBookSimulator::Config{})
        .def("add_book", [](PyMultiBookWrapper& w, const BookId& id) { return w.sim.addBook(id); }, py::arg("book_id"))
        .def("has_book", [](const PyMultiBookWrapper& w, const BookId& id) { return w.sim.hasBook(id); },
             py::arg("book_id"))
        .def(
            "set_log_sink",
            [](PyMultiBookWrapper& w, const BookId& id, InMemoryLogSink& sink) { w.sim.setLogSink(id, &sink); },
            py::arg("book_id"), py::arg("sink"), py::keep_alive<1, 3>())
        .def("set_multi_log_sink",
             [](PyMultiBookWrapper& w, InMemoryMultiLogSink& sink) { w.sim.setMultiLogSink(&sink); },
             py::arg("sink"), py::keep_alive<1, 2>())
        .def("add_normalized_stream",
             [](PyMultiBookWrapper& w, const BookId& id, std::vector<NormalizedLobEvent> events) {
                 auto src = std::make_shared<PyNormalizedVectorSource>(std::move(events));
                 w.sim.addStream(id, *src);
                 w.heldSources.push_back(std::move(src));
             },
             py::arg("book_id"), py::arg("events"))
        .def("apply", [](PyMultiBookWrapper& w, const BookId& id, const NormalizedLobEvent& ev) { w.sim.apply(id, ev); },
             py::arg("book_id"), py::arg("event"))
        .def("submit_strategy_event",
             [](PyMultiBookWrapper& w, const BookId& id, const NormalizedLobEvent& ev,
                std::optional<std::int64_t> latency) { w.sim.submitStrategyEvent(id, ev, latency); },
             py::arg("book_id"), py::arg("event"), py::arg("latency") = std::nullopt)
        .def("step", [](PyMultiBookWrapper& w) { return w.sim.step(); })
        .def("step_until", [](PyMultiBookWrapper& w, std::int64_t ts) { return w.sim.stepUntil(ts); }, py::arg("ts"))
        .def("step_for", [](PyMultiBookWrapper& w, std::int64_t delta) { return w.sim.stepFor(delta); },
             py::arg("delta_ts"))
        .def("current_time", [](const PyMultiBookWrapper& w) { return w.sim.currentTime(); })
        .def("get_best_price_ticks",
             [](const PyMultiBookWrapper& w, const BookId& id, Side side) { return w.sim.getBestPriceTicks(id, side); },
             py::arg("book_id"), py::arg("side"))
        .def("l2_top_n",
             [](const PyMultiBookWrapper& w, const BookId& id, Side side, std::uint32_t n) {
                 return w.sim.l2TopN(id, side, n);
             },
             py::arg("book_id"), py::arg("side"), py::arg("n"))
        .def("depth_at",
             [](const PyMultiBookWrapper& w, const BookId& id, Side side, std::int64_t price) {
                 return w.sim.depthAt(id, side, price);
             },
             py::arg("book_id"), py::arg("side"), py::arg("price_ticks"));
}

#else
#pragma message("pybind11 headers not found; python bindings are disabled for this configuration.")
#endif
