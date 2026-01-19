#if __has_include(<pybind11/pybind11.h>)

#include "lobsim/book_id.hpp"
#include "lobsim/lob_event.hpp"
#include "lobsim/multi_log_sink.hpp"
#include "lobsim/multibook_simulator.hpp"
#include "lobsim/paper_trading_simulator.hpp"
#include "lobsim/replay_session.hpp"
#include "lobsim/types.hpp"

#include <optional>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <span>
#include <string>
#include <unordered_map>
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

struct PyIteratorSource {
    explicit PyIteratorSource(py::object source) : source_(std::move(source)) {}

    bool nextObject(py::object& out) {
        py::gil_scoped_acquire gil;
        if (!iter_ && py::hasattr(source_, "__iter__")) {
            iter_ = py::iter(source_);
        }

        if (iter_) {
            py::object item = py::reinterpret_steal<py::object>(PyIter_Next(iter_.ptr()));
            if (!item) {
                if (PyErr_Occurred()) {
                    throw py::error_already_set();
                }
                return false;
            }
            out = std::move(item);
            return true;
        }

        if (py::hasattr(source_, "next")) {
            py::object item = source_.attr("next")();
            if (item.is_none()) {
                return false;
            }
            out = std::move(item);
            return true;
        }

        throw std::runtime_error("Python source must be iterable or implement next().");
    }

private:
    py::object source_;
    py::object iter_;
};

struct PyNormalizedStreamSource {
    explicit PyNormalizedStreamSource(py::object source) : source_(std::move(source)) {}

    bool next(NormalizedLobEvent& out) {
        py::object item;
        if (!source_.nextObject(item)) {
            return false;
        }
        out = item.cast<NormalizedLobEvent>();
        return true;
    }

private:
    PyIteratorSource source_;
};

struct PyRawStreamSource {
    explicit PyRawStreamSource(py::object source) : source_(std::move(source)) {}

    bool next(py::object& out) { return source_.nextObject(out); }

private:
    PyIteratorSource source_;
};

struct PyAdapter {
    explicit PyAdapter(py::object adapter) : adapter_(std::move(adapter)) {}

    NormalizedLobEvent normalize(const py::object& raw) const {
        py::gil_scoped_acquire gil;
        try {
            if (py::hasattr(adapter_, "normalize")) {
                return adapter_.attr("normalize")(raw).cast<NormalizedLobEvent>();
            }
            return adapter_(raw).cast<NormalizedLobEvent>();
        } catch (py::error_already_set& e) {
            const std::string msg = e.what();
            PyErr_Clear();
            throw std::runtime_error(msg);
        }
    }

private:
    py::object adapter_;
};

struct PyMultiBookWrapper {
    explicit PyMultiBookWrapper(const MultiBookSimulator::Config& cfg) : sim(cfg) {}
    PyMultiBookWrapper(const PyMultiBookWrapper&) = delete;
    PyMultiBookWrapper& operator=(const PyMultiBookWrapper&) = delete;
    PyMultiBookWrapper(PyMultiBookWrapper&&) = default;
    PyMultiBookWrapper& operator=(PyMultiBookWrapper&&) = default;

    MultiBookSimulator sim;
    std::vector<std::shared_ptr<PyNormalizedVectorSource>> heldSources; // keep sources alive
    std::vector<std::shared_ptr<PyNormalizedStreamSource>> heldNormalizedStreams;
    std::vector<std::shared_ptr<PyRawStreamSource>> heldRawStreams;
    std::vector<std::shared_ptr<PyAdapter>> heldAdapters;
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
        .value("SET", UpdateType::SET)
        .value("AGGRESSIVE_TRADE", UpdateType::AGGRESSIVE_TRADE);

    py::enum_<UpdateSource>(types, "UpdateSource")
        .value("HISTORICAL", UpdateSource::HISTORICAL)
        .value("STRATEGY", UpdateSource::STRATEGY);

    py::enum_<DiagnosticRecordCode>(types, "DiagnosticRecordCode")
        .value("ADD_DUPLICATE_ORDER_ID", DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID)
        .value("DELETE_NON_EXISTING_PAPER_ORDER_ID", DiagnosticRecordCode::DELETE_NON_EXISTING_PAPER_ORDER_ID)
        .value("DELETE_NON_EXISTING_HISTORICAL_ORDER_ID", DiagnosticRecordCode::DELETE_NON_EXISTING_HISTORICAL_ORDER_ID)
        .value("PROVIDED_SIDE_ON_DELETE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID",
               DiagnosticRecordCode::PROVIDED_SIDE_ON_DELETE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID)
        .value("PROVIDED_PRICE_ON_DELETE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID",
               DiagnosticRecordCode::PROVIDED_PRICE_ON_DELETE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID)
        .value("SET_WITH_NEGATIVE_LIQUIDITY_REQUESTED_WAS_SET_TO_ZERO",
               DiagnosticRecordCode::SET_WITH_NEGATIVE_LIQUIDITY_REQUESTED_WAS_SET_TO_ZERO)
        .value("SET_NON_EXISTING_ORDER_ID_IS_REJECTED", DiagnosticRecordCode::SET_NON_EXISTING_ORDER_ID_IS_REJECTED)
        .value("PROVIDED_SIDE_ON_SET_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID",
               DiagnosticRecordCode::PROVIDED_SIDE_ON_SET_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID)
        .value("PROVIDED_PRICE_ON_SET_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID",
               DiagnosticRecordCode::PROVIDED_PRICE_ON_SET_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID)
        .value("REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY", DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY)
        .value("REQUESTED_REDUCE_NON_EXISTING_ORDER_ID", DiagnosticRecordCode::REQUESTED_REDUCE_NON_EXISTING_ORDER_ID)
        .value("PROVIDED_SIDE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID",
               DiagnosticRecordCode::PROVIDED_SIDE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID)
        .value("PROVIDED_PRICE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID",
               DiagnosticRecordCode::PROVIDED_PRICE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID)
        .value("REQUESTED_ORDER_REDUCE_WITH_VOLUME_LARGER_THAN_AVAILABLE_FOR_ORDER_ID",
               DiagnosticRecordCode::REQUESTED_ORDER_REDUCE_WITH_VOLUME_LARGER_THAN_AVAILABLE_FOR_ORDER_ID)
        .value("PAPER_ORDER_INVOKES_PASSIVE_MATCH_INSTEAD_OF_AGGRESSIVE_TRADE",
               DiagnosticRecordCode::PAPER_ORDER_INVOKES_PASSIVE_MATCH_INSTEAD_OF_AGGRESSIVE_TRADE)
        .value("INVALID_UPDATE_TYPE", DiagnosticRecordCode::INVALID_UPDATE_TYPE)
        .value("ADD_INVOKED_WITH_NEGATIVE_QUANTITY", DiagnosticRecordCode::ADD_INVOKED_WITH_NEGATIVE_QUANTITY)
        .value("REQUESTED_REDUCE_PAPER_ORDER_BY_NEGATIVE_QUANTITY",
               DiagnosticRecordCode::REQUESTED_REDUCE_PAPER_ORDER_BY_NEGATIVE_QUANTITY)
        .value("CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK",
               DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK)
        .value("REQUESTED_REDUCE_ORDER_BY_NEGATIVE_QUANTITY",
               DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_NEGATIVE_QUANTITY)
        .value("SET_LOG_SINK_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR",
               DiagnosticRecordCode::SET_LOG_SINK_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR)
        .value("DEPTH_AT_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR",
               DiagnosticRecordCode::DEPTH_AT_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR)
        .value("L2_TOP_N_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR",
               DiagnosticRecordCode::L2_TOP_N_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR)
        .value("GET_BEST_PRICE_TICKS_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR",
               DiagnosticRecordCode::GET_BEST_PRICE_TICKS_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR)
        .value("HEAP_ENTRY_WITHOUT_BUFFERED_EVENT", DiagnosticRecordCode::HEAP_ENTRY_WITHOUT_BUFFERED_EVENT)
        .value("APPLY_EVENT_REQUESTED_FOR_UNKNOWN_BOOK", DiagnosticRecordCode::APPLY_EVENT_REQUESTED_FOR_UNKNOWN_BOOK)
        .value("NON_MONOTONIC_TS_RECEIVED_DETECTED_IN_MULTI_BOOK_SIMULATOR",
               DiagnosticRecordCode::NON_MONOTONIC_TS_RECEIVED_DETECTED_IN_MULTI_BOOK_SIMULATOR)
        .value("DUPLICATE_STREAM_FOR_BOOK_IN_MULTI_BOOK_SIMULATOR",
               DiagnosticRecordCode::DUPLICATE_STREAM_FOR_BOOK_IN_MULTI_BOOK_SIMULATOR)
        .value("SUBMIT_STRATEGY_EVENT_FOR_UNKNOWN_BOOK", DiagnosticRecordCode::SUBMIT_STRATEGY_EVENT_FOR_UNKNOWN_BOOK)
        .value("STRATEGY_EVENT_TIME_TRAVEL", DiagnosticRecordCode::STRATEGY_EVENT_TIME_TRAVEL)
        .value("REQUESTED_REDUCE_PAPER_ORDER_BY_ZERO_QUANTITY",
               DiagnosticRecordCode::REQUESTED_REDUCE_PAPER_ORDER_BY_ZERO_QUANTITY);

    py::enum_<DiagnosticRecordSeverity>(types, "DiagnosticRecordSeverity")
        .value("INFO", DiagnosticRecordSeverity::INFO)
        .value("WARNING", DiagnosticRecordSeverity::WARNING)
        .value("ERROR", DiagnosticRecordSeverity::ERROR);

    types.def("diagnostic_code_names", []() {
        return std::unordered_map<int, std::string>{
            {static_cast<int>(DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID), "ADD_DUPLICATE_ORDER_ID"},
            {static_cast<int>(DiagnosticRecordCode::DELETE_NON_EXISTING_PAPER_ORDER_ID),
             "DELETE_NON_EXISTING_PAPER_ORDER_ID"},
            {static_cast<int>(DiagnosticRecordCode::DELETE_NON_EXISTING_HISTORICAL_ORDER_ID),
             "DELETE_NON_EXISTING_HISTORICAL_ORDER_ID"},
            {static_cast<int>(DiagnosticRecordCode::PROVIDED_SIDE_ON_DELETE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID),
             "PROVIDED_SIDE_ON_DELETE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID"},
            {static_cast<int>(DiagnosticRecordCode::PROVIDED_PRICE_ON_DELETE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID),
             "PROVIDED_PRICE_ON_DELETE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID"},
            {static_cast<int>(DiagnosticRecordCode::SET_WITH_NEGATIVE_LIQUIDITY_REQUESTED_WAS_SET_TO_ZERO),
             "SET_WITH_NEGATIVE_LIQUIDITY_REQUESTED_WAS_SET_TO_ZERO"},
            {static_cast<int>(DiagnosticRecordCode::SET_NON_EXISTING_ORDER_ID_IS_REJECTED),
             "SET_NON_EXISTING_ORDER_ID_IS_REJECTED"},
            {static_cast<int>(DiagnosticRecordCode::PROVIDED_SIDE_ON_SET_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID),
             "PROVIDED_SIDE_ON_SET_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID"},
            {static_cast<int>(DiagnosticRecordCode::PROVIDED_PRICE_ON_SET_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID),
             "PROVIDED_PRICE_ON_SET_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID"},
            {static_cast<int>(DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY),
             "REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY"},
            {static_cast<int>(DiagnosticRecordCode::REQUESTED_REDUCE_NON_EXISTING_ORDER_ID),
             "REQUESTED_REDUCE_NON_EXISTING_ORDER_ID"},
            {static_cast<int>(
                 DiagnosticRecordCode::PROVIDED_SIDE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID),
             "PROVIDED_SIDE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID"},
            {static_cast<int>(
                 DiagnosticRecordCode::PROVIDED_PRICE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID),
             "PROVIDED_PRICE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID"},
            {static_cast<int>(
                 DiagnosticRecordCode::REQUESTED_ORDER_REDUCE_WITH_VOLUME_LARGER_THAN_AVAILABLE_FOR_ORDER_ID),
             "REQUESTED_ORDER_REDUCE_WITH_VOLUME_LARGER_THAN_AVAILABLE_FOR_ORDER_ID"},
            {static_cast<int>(DiagnosticRecordCode::PAPER_ORDER_INVOKES_PASSIVE_MATCH_INSTEAD_OF_AGGRESSIVE_TRADE),
             "PAPER_ORDER_INVOKES_PASSIVE_MATCH_INSTEAD_OF_AGGRESSIVE_TRADE"},
            {static_cast<int>(DiagnosticRecordCode::INVALID_UPDATE_TYPE), "INVALID_UPDATE_TYPE"},
            {static_cast<int>(DiagnosticRecordCode::ADD_INVOKED_WITH_NEGATIVE_QUANTITY),
             "ADD_INVOKED_WITH_NEGATIVE_QUANTITY"},
            {static_cast<int>(DiagnosticRecordCode::REQUESTED_REDUCE_PAPER_ORDER_BY_NEGATIVE_QUANTITY),
             "REQUESTED_REDUCE_PAPER_ORDER_BY_NEGATIVE_QUANTITY"},
            {static_cast<int>(DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK),
             "CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK"},
            {static_cast<int>(DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_NEGATIVE_QUANTITY),
             "REQUESTED_REDUCE_ORDER_BY_NEGATIVE_QUANTITY"},
            {static_cast<int>(DiagnosticRecordCode::SET_LOG_SINK_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR),
             "SET_LOG_SINK_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR"},
            {static_cast<int>(DiagnosticRecordCode::DEPTH_AT_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR),
             "DEPTH_AT_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR"},
            {static_cast<int>(DiagnosticRecordCode::L2_TOP_N_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR),
             "L2_TOP_N_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR"},
            {static_cast<int>(DiagnosticRecordCode::GET_BEST_PRICE_TICKS_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR),
             "GET_BEST_PRICE_TICKS_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR"},
            {static_cast<int>(DiagnosticRecordCode::HEAP_ENTRY_WITHOUT_BUFFERED_EVENT),
             "HEAP_ENTRY_WITHOUT_BUFFERED_EVENT"},
            {static_cast<int>(DiagnosticRecordCode::APPLY_EVENT_REQUESTED_FOR_UNKNOWN_BOOK),
             "APPLY_EVENT_REQUESTED_FOR_UNKNOWN_BOOK"},
            {static_cast<int>(DiagnosticRecordCode::NON_MONOTONIC_TS_RECEIVED_DETECTED_IN_MULTI_BOOK_SIMULATOR),
             "NON_MONOTONIC_TS_RECEIVED_DETECTED_IN_MULTI_BOOK_SIMULATOR"},
            {static_cast<int>(DiagnosticRecordCode::DUPLICATE_STREAM_FOR_BOOK_IN_MULTI_BOOK_SIMULATOR),
             "DUPLICATE_STREAM_FOR_BOOK_IN_MULTI_BOOK_SIMULATOR"},
            {static_cast<int>(DiagnosticRecordCode::SUBMIT_STRATEGY_EVENT_FOR_UNKNOWN_BOOK),
             "SUBMIT_STRATEGY_EVENT_FOR_UNKNOWN_BOOK"},
            {static_cast<int>(DiagnosticRecordCode::STRATEGY_EVENT_TIME_TRAVEL), "STRATEGY_EVENT_TIME_TRAVEL"},
            {static_cast<int>(DiagnosticRecordCode::REQUESTED_REDUCE_PAPER_ORDER_BY_ZERO_QUANTITY),
             "REQUESTED_REDUCE_PAPER_ORDER_BY_ZERO_QUANTITY"},
        };
    });

    types.def("diagnostic_severity_names", []() {
        return std::unordered_map<int, std::string>{
            {static_cast<int>(DiagnosticRecordSeverity::INFO), "INFO"},
            {static_cast<int>(DiagnosticRecordSeverity::WARNING), "WARNING"},
            {static_cast<int>(DiagnosticRecordSeverity::ERROR), "ERROR"},
        };
    });

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

    py::class_<DiagnosticRecord>(m, "DiagnosticRecord")
        .def_readonly("seq", &DiagnosticRecord::seq)
        .def_readonly("tsExchange", &DiagnosticRecord::tsExchange)
        .def_readonly("tsReceived", &DiagnosticRecord::tsReceived)
        .def_readonly("code", &DiagnosticRecord::code)
        .def_readonly("severity", &DiagnosticRecord::severity)
        .def_readonly("bookKey", &DiagnosticRecord::bookKey);

    py::enum_<PaperOrderLedgerStatus>(m, "PaperOrderLedgerStatus")
        .value("OPEN", PaperOrderLedgerStatus::OPEN)
        .value("PARTIALLY_FILLED", PaperOrderLedgerStatus::PARTIALLY_FILLED)
        .value("FILLED", PaperOrderLedgerStatus::FILLED)
        .value("CANCELLED", PaperOrderLedgerStatus::CANCELLED)
        .value("REJECTED", PaperOrderLedgerStatus::REJECTED);

    py::enum_<PaperOrderFillRole>(m, "PaperOrderFillRole")
        .value("MAKER", PaperOrderFillRole::MAKER)
        .value("TAKER", PaperOrderFillRole::TAKER);

    py::class_<PaperOrderFill>(m, "PaperOrderFill")
        .def_readonly("seq", &PaperOrderFill::seq)
        .def_readonly("tsExchange", &PaperOrderFill::tsExchange)
        .def_readonly("tsReceived", &PaperOrderFill::tsReceived)
        .def_readonly("priceTicks", &PaperOrderFill::priceTicks)
        .def_readonly("qtyLots", &PaperOrderFill::qtyLots)
        .def_readonly("role", &PaperOrderFill::role);

    py::class_<PaperOrderState>(m, "PaperOrderState")
        .def_readonly("orderId", &PaperOrderState::orderId)
        .def_readonly("side", &PaperOrderState::side)
        .def_readonly("priceTicks", &PaperOrderState::priceTicks)
        .def_readonly("initialQty", &PaperOrderState::initialQty)
        .def_readonly("remainingQty", &PaperOrderState::remainingQty)
        .def_readonly("filledQty", &PaperOrderState::filledQty)
        .def_readonly("status", &PaperOrderState::status)
        .def_readonly("createdSeq", &PaperOrderState::createdSeq)
        .def_readonly("lastUpdateSeq", &PaperOrderState::lastUpdateSeq);

    py::class_<PaperOrderLedgerEntry>(m, "PaperOrderLedgerEntry")
        .def_readonly("state", &PaperOrderLedgerEntry::state)
        .def_readonly("fills", &PaperOrderLedgerEntry::fills);

    py::class_<InMemoryLogSink>(m, "InMemoryLogSink")
        .def(py::init<>())
        .def("reset", &InMemoryLogSink::reset)
        .def("get_fills", [](const InMemoryLogSink& s) { return s.getFills(); })
        .def("get_events", [](const InMemoryLogSink& s) { return s.getEvents(); })
        .def("get_diagnostics", [](const InMemoryLogSink& s) { return s.getDiagnostics(); })
        .def("get_paper_ledger", [](const InMemoryLogSink& s) { return s.getPaperLedger(); })
        .def("find_paper_order",
             [](const InMemoryLogSink& s, std::int64_t orderId) -> std::optional<PaperOrderLedgerEntry> {
                 if (auto* entry = s.findPaperOrder(orderId)) {
                     return *entry;
                 }
                 return std::nullopt;
             })
        .def("get_rejected_strategy_events", [](const InMemoryLogSink& s) { return s.getRejectedStrategyEvents(); });

    py::class_<InMemoryMultiLogSink>(m, "InMemoryMultiLogSink")
        .def(py::init<>())
        .def("reset", &InMemoryMultiLogSink::reset)
        .def("fills", [](const InMemoryMultiLogSink& s) { return s.fills(); })
        .def("events", [](const InMemoryMultiLogSink& s) { return s.events(); })
        .def("diagnostics", [](const InMemoryMultiLogSink& s) { return s.diagnostics(); })
        .def("fills_for", [](const InMemoryMultiLogSink& s, const std::string& key) { return s.fillsFor(key); })
        .def("events_for", [](const InMemoryMultiLogSink& s, const std::string& key) { return s.eventsFor(key); })
        .def("diagnostics_for",
             [](const InMemoryMultiLogSink& s, const std::string& key) { return s.diagnosticsFor(key); })
        .def("paper_ledger", [](const InMemoryMultiLogSink& s) { return s.paperLedger(); })
        .def("paper_ledger_for",
             [](const InMemoryMultiLogSink& s, const std::string& key) { return s.paperLedgerFor(key); })
        .def("find_paper_order",
             [](const InMemoryMultiLogSink& s, const std::string& key,
                std::int64_t orderId) -> std::optional<PaperOrderLedgerEntry> {
                 if (auto* entry = s.findPaperOrder(key, orderId)) {
                     return *entry;
                 }
                 return std::nullopt;
             })
        .def("rejected_strategy_events_for",
             [](const InMemoryMultiLogSink& s, const std::string& key) { return s.rejectedStrategyEventsFor(key); });

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
        .def(
            "add_book", [](PyMultiBookWrapper& w, const BookId& id) { return w.sim.addBook(id); }, py::arg("book_id"))
        .def(
            "has_book", [](const PyMultiBookWrapper& w, const BookId& id) { return w.sim.hasBook(id); },
            py::arg("book_id"))
        .def(
            "set_log_sink",
            [](PyMultiBookWrapper& w, const BookId& id, InMemoryLogSink& sink) { w.sim.setLogSink(id, &sink); },
            py::arg("book_id"), py::arg("sink"), py::keep_alive<1, 3>())
        .def(
            "set_multi_log_sink",
            [](PyMultiBookWrapper& w, InMemoryMultiLogSink& sink) { w.sim.setMultiLogSink(&sink); }, py::arg("sink"),
            py::keep_alive<1, 2>())
        .def(
            "add_normalized_stream",
            [](PyMultiBookWrapper& w, const BookId& id, std::vector<NormalizedLobEvent> events) {
                auto src = std::make_shared<PyNormalizedVectorSource>(std::move(events));
                w.sim.addStream(id, *src);
                w.heldSources.push_back(std::move(src));
            },
            py::arg("book_id"), py::arg("events"))
        .def(
            "add_stream",
            [](PyMultiBookWrapper& w, const BookId& id, py::object source, py::object adapter) {
                if (adapter.is_none()) {
                    auto src = std::make_shared<PyNormalizedStreamSource>(std::move(source));
                    w.sim.addStream(id, *src);
                    w.heldNormalizedStreams.push_back(std::move(src));
                    return;
                }
                auto src = std::make_shared<PyRawStreamSource>(std::move(source));
                auto ad = std::make_shared<PyAdapter>(std::move(adapter));
                w.sim.addStream(id, *src, *ad);
                w.heldRawStreams.push_back(std::move(src));
                w.heldAdapters.push_back(std::move(ad));
            },
            py::arg("book_id"), py::arg("source"), py::arg("adapter") = py::none())
        .def(
            "init_from_l2_snapshot",
            [](PyMultiBookWrapper& w, const BookId& id, const std::vector<Side>& sides,
               const std::vector<std::int64_t>& prices, const std::vector<std::int64_t>& quantities) {
                w.sim.initFromL2Snapshot(id, sides, prices, quantities);
            },
            py::arg("book_id"), py::arg("sides"), py::arg("prices"), py::arg("quantities"))
        .def(
            "init_from_l3_snapshot",
            [](PyMultiBookWrapper& w, const BookId& id, const std::vector<Side>& sides,
               const std::vector<std::int64_t>& prices, const std::vector<std::int64_t>& quantities,
               const std::vector<std::int64_t>& order_ids, const std::vector<std::int64_t>& trader_ids) {
                w.sim.initFromL3Snapshot(id, sides, prices, quantities, order_ids, trader_ids);
            },
            py::arg("book_id"), py::arg("sides"), py::arg("prices"), py::arg("quantities"), py::arg("order_ids"),
            py::arg("trader_ids"))
        .def(
            "apply", [](PyMultiBookWrapper& w, const BookId& id, const NormalizedLobEvent& ev) { w.sim.apply(id, ev); },
            py::arg("book_id"), py::arg("event"))
        .def(
            "submit_strategy_event",
            [](PyMultiBookWrapper& w, const BookId& id, const NormalizedLobEvent& ev,
               std::optional<std::int64_t> latency) { w.sim.submitStrategyEvent(id, ev, latency); },
            py::arg("book_id"), py::arg("event"), py::arg("latency") = std::nullopt)
        .def("step", [](PyMultiBookWrapper& w) { return w.sim.step(); })
        .def(
            "step_until", [](PyMultiBookWrapper& w, std::int64_t ts) { return w.sim.stepUntil(ts); }, py::arg("ts"))
        .def(
            "step_for", [](PyMultiBookWrapper& w, std::int64_t delta) { return w.sim.stepFor(delta); },
            py::arg("delta_ts"))
        .def("current_time", [](const PyMultiBookWrapper& w) { return w.sim.currentTime(); })
        .def(
            "get_best_price_ticks",
            [](const PyMultiBookWrapper& w, const BookId& id, Side side) { return w.sim.getBestPriceTicks(id, side); },
            py::arg("book_id"), py::arg("side"))
        .def(
            "l2_top_n",
            [](const PyMultiBookWrapper& w, const BookId& id, Side side, std::uint32_t n) {
                return w.sim.l2TopN(id, side, n);
            },
            py::arg("book_id"), py::arg("side"), py::arg("n"))
        .def(
            "depth_at",
            [](const PyMultiBookWrapper& w, const BookId& id, Side side, std::int64_t price) {
                return w.sim.depthAt(id, side, price);
            },
            py::arg("book_id"), py::arg("side"), py::arg("price_ticks"));
}

#else
#pragma message("pybind11 headers not found; python bindings are disabled for this configuration.")
#endif
