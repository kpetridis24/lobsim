#pragma once

#include "lobsim/book_id.hpp"
#include "lobsim/event_adapter.hpp"
#include "lobsim/event_source.hpp"
#include "lobsim/log_sink.hpp"
#include "lobsim/multi_log_sink.hpp"
#include "lobsim/paper_trading_simulator.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <random>

class MultiBookSimulator {
public:
    struct Config {
        bool require_monotonic_ts_received{true};
        bool fail_fast{true};
        LatencyModel latency_model{};
        std::uint64_t seed{42};
    };

    MultiBookSimulator() : rng_(42) {}
    explicit MultiBookSimulator(Config cfg) : cfg_(cfg), rng_(cfg.seed) {}
    ~MultiBookSimulator() {
        for (auto& [_, entry] : books_) {
            if (entry.engine) {
                entry.engine->set_log_sink(nullptr);
            }
        }
    }

    bool add_book(const BookId& id) { return add_book(id, std::make_unique<PaperTradingSimulator>()); }

    bool add_book(const BookId& id, std::unique_ptr<PaperTradingSimulator> engine) {
        const std::string key = book_key(id);
        if (books_.find(key) != books_.end()) {
            return false;
        }
        BookEntry entry{};
        entry.id = id;
        entry.key = key;
        entry.engine = std::move(engine);
        attach_book_sink(entry);
        books_.emplace(key, std::move(entry));
        return true;
    }

    bool has_book(const BookId& id) const { return has_book_key(book_key(id)); }
    bool has_book_key(std::string_view key) const { return books_.find(std::string(key)) != books_.end(); }

    PaperTradingSimulator* get_book(const BookId& id) { return get_book_key(book_key(id)); }
    const PaperTradingSimulator* get_book(const BookId& id) const { return get_book_key(book_key(id)); }

    void init_from_l2_snapshot(const BookId& id, std::span<const Side> sides,
                               std::span<const std::int64_t> prices, std::span<const std::int64_t> quantities) {
        auto* book = get_book(id);
        if (!book) {
            emit_diagnostic(book_key(id), DiagnosticRecordCode::APPLY_EVENT_REQUESTED_FOR_UNKNOWN_BOOK,
                            DiagnosticRecordSeverity::ERROR, 0, -1, -1);
            return;
        }
        book->init_from_l2_snapshot(sides, prices, quantities);
    }

    void init_from_l3_snapshot(const BookId& id, std::span<const Side> sides,
                               std::span<const std::int64_t> prices, std::span<const std::int64_t> quantities,
                               std::span<const std::int64_t> order_ids,
                               std::span<const std::int64_t> trader_ids) {
        auto* book = get_book(id);
        if (!book) {
            emit_diagnostic(book_key(id), DiagnosticRecordCode::APPLY_EVENT_REQUESTED_FOR_UNKNOWN_BOOK,
                            DiagnosticRecordSeverity::ERROR, 0, -1, -1);
            return;
        }
        book->init_from_l3_snapshot(sides, prices, quantities, order_ids, trader_ids);
    }

    PaperTradingSimulator* get_book_key(std::string_view key) {
        auto it = books_.find(std::string(key));
        if (it == books_.end()) {
            return nullptr;
        }
        return it->second.engine.get();
    }

    const PaperTradingSimulator* get_book_key(std::string_view key) const {
        auto it = books_.find(std::string(key));
        if (it == books_.end()) {
            return nullptr;
        }
        return it->second.engine.get();
    }

    void set_log_sink(const BookId& id, std::shared_ptr<ILogSink> sink) {
        auto* book = get_book(id);
        if (!book) {
            emit_diagnostic(book_key(id), DiagnosticRecordCode::SET_LOG_SINK_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR,
                            DiagnosticRecordSeverity::ERROR, 0, -1, -1);
            return;
        }
        book->set_log_sink(std::move(sink));
        book_sinks_.erase(book_key(id));
    }

    void set_log_sink(const BookId& id, ILogSink* sink) {
        set_log_sink(id, std::shared_ptr<ILogSink>(sink, [](ILogSink*) {}));
    }

    void set_multi_log_sink(std::shared_ptr<IMultiLogSink> sink) {
        // If we currently own per-book wrappers, detach them before destroying them.
        for (auto& [key, entry] : books_) {
            if (book_sinks_.find(key) != book_sinks_.end()) {
                entry.engine->set_log_sink(nullptr);
            }
        }
        multi_sink_ = std::move(sink);
        book_sinks_.clear();
        for (auto& [key, entry] : books_) {
            attach_book_sink(entry);
        }
    }

    void set_multi_log_sink(IMultiLogSink* sink) {
        set_multi_log_sink(std::shared_ptr<IMultiLogSink>(sink, [](IMultiLogSink*) {}));
    }

    std::optional<std::int64_t> depth_at(const BookId& id, Side side, std::int64_t price_ticks) const {
        const auto* book = get_book(id);
        if (!book) {
            emit_diagnostic(book_key(id), DiagnosticRecordCode::DEPTH_AT_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR,
                            DiagnosticRecordSeverity::ERROR, 0, -1, -1);
            return std::nullopt;
        }
        return book->depth_at(side, price_ticks);
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> l2_top_n(const BookId& id, Side side, std::uint32_t n) const {
        const auto* book = get_book(id);
        if (!book) {
            emit_diagnostic(book_key(id), DiagnosticRecordCode::L2_TOP_N_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR,
                            DiagnosticRecordSeverity::ERROR, 0, -1, -1);
            return {};
        }
        return book->l2_top_n(side, n);
    }

    std::optional<std::int64_t> get_best_price_ticks(const BookId& id, Side side) const {
        const auto* book = get_book(id);
        if (!book) {
            emit_diagnostic(book_key(id),
                            DiagnosticRecordCode::GET_BEST_PRICE_TICKS_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR,
                            DiagnosticRecordSeverity::ERROR, 0, -1, -1);
            return std::nullopt;
        }
        return book->get_best_price_ticks(side);
    }

    void apply(const BookId& id, NormalizedLobEvent ev) { apply_to_book(book_key(id), ev); }

    void submit_strategy_event(const BookId& id, NormalizedLobEvent ev, std::optional<std::int64_t> latency = {}) {
        const std::string key = book_key(id);
        if (!has_book_key(key)) {
            emit_diagnostic(key, DiagnosticRecordCode::SUBMIT_STRATEGY_EVENT_FOR_UNKNOWN_BOOK,
                            DiagnosticRecordSeverity::ERROR, 0, ev.ts_received, ev.ts_exchange);
            if (cfg_.fail_fast) {
                throw std::runtime_error("MultiBookSimulator: submit_strategy_event for unknown book.");
            }
            return;
        }
        if (ev.symbol_id.empty() || ev.symbol_id != key) {
            ev.symbol_id = key;
        }
        ev.update_source = UpdateSource::STRATEGY;
        
        std::int64_t latency_val = 0;
        if (latency.has_value()) {
            latency_val = latency.value();
        } else {
            latency_val = sample_latency();
        }

        if (ev.ts_received == 0) {
            const std::int64_t base = has_current_time_ ? current_ts_received_ : 0;
            // Allow negative latency to trigger time travel checks downstream
            ev.ts_received = base + latency_val;
            // Ensure at least 1 tick forward if latency is effectively 0 or negative but we want progress? 
            // The original code used: ev.ts_received = base + latency.value_or(1);
            // If we assume latency_val is the intended delay.
            if (ev.ts_received <= base && latency_val <= 0 && !latency.has_value()) {
                 // If using default sampling and it produced <= 0, force forward progress?
                 // But if user explicitly passed negative latency, we should honor it (and let it fail validation).
                 ev.ts_received = base + 1;
            }
        } else {
            // If ts_received is provided, we assume it is the "submission time" and we add latency?
            // Existing logic: ev.ts_received += latency.value();
            // So if user provides ts, we shift it by latency.
            ev.ts_received += latency_val;
        }
        
        if (cfg_.require_monotonic_ts_received && has_current_time_ && ev.ts_received < current_ts_received_) {
            emit_diagnostic(key, DiagnosticRecordCode::STRATEGY_EVENT_TIME_TRAVEL, DiagnosticRecordSeverity::ERROR, 0,
                            ev.ts_received, ev.ts_exchange);
            if (cfg_.fail_fast) {
                throw std::runtime_error("MultiBookSimulator: strategy event time travel.");
            }
            return;
        }
        const std::uint64_t seq = ++strategy_seq_counter_;
        if (ev.ts_exchange == 0) {
            ev.ts_exchange = std::numeric_limits<std::int64_t>::max();
        }
        strategy_events_.emplace(seq, StrategyItem{std::move(ev), seq});
        heap_.push(HeapEntry{strategy_events_.at(seq).ev.ts_received, strategy_events_.at(seq).ev.ts_exchange, 0,
                             HeapEntry::EntryKind::Strategy, seq});
    }

    std::optional<std::int64_t> current_time() const {
        if (!has_current_time_) {
            return std::nullopt;
        }
        return current_ts_received_;
    }

    bool step() {
        prime_streams();
        if (heap_.empty()) {
            return false;
        }

        const auto entry = heap_.top();
        heap_.pop();

        if (entry.kind == HeapEntry::EntryKind::Strategy) {
            auto it = strategy_events_.find(entry.strategy_seq);
            if (it == strategy_events_.end()) {
                emit_diagnostic("__multibook__", DiagnosticRecordCode::HEAP_ENTRY_WITHOUT_BUFFERED_EVENT,
                                DiagnosticRecordSeverity::ERROR, 0, current_ts_received_, -1);
                if (cfg_.fail_fast) {
                    throw std::runtime_error("MultiBookSimulator: invalid strategy heap entry.");
                }
                return false;
            }
            NormalizedLobEvent ev = std::move(it->second.ev);
            strategy_events_.erase(it);
            apply_to_book(ev.symbol_id, ev);
            return true;
        } else {
            auto& stream = streams_[entry.stream_index];
            if (!stream.has_buffered) {
                emit_diagnostic(stream.book_key, DiagnosticRecordCode::HEAP_ENTRY_WITHOUT_BUFFERED_EVENT,
                                DiagnosticRecordSeverity::ERROR, stream.seq, current_ts_received_, -1);
                if (cfg_.fail_fast) {
                    throw std::runtime_error("MultiBookSimulator: heap entry without buffered event.");
                }
                return false;
            }

            NormalizedLobEvent ev = std::move(stream.buffered);
            stream.has_buffered = false;

            apply_to_book(stream.book_key, ev);
            fill_buffer(entry.stream_index);

            return true;
        }
    }

    std::size_t step_until(std::int64_t ts_received_inclusive) {
        std::size_t applied = 0;
        prime_streams();
        while (!heap_.empty()) {
            const auto& entry = heap_.top();
            if (entry.ts_received > ts_received_inclusive) {
                break;
            }
            if (!step()) {
                break;
            }
            ++applied;
        }
        return applied;
    }

    std::size_t step_for(std::int64_t delta_ts) {
        if (delta_ts <= 0) {
            return 0;
        }
        const std::int64_t base = has_current_time_ ? current_ts_received_ : 0;
        return step_until(base + delta_ts);
    }

    template <typename Source, typename Adapter>
    void add_stream(const BookId& id, Source& src, const Adapter& adapter) {
        using RawEvent = NormalizeArgT<Adapter>;
        static_assert(IEventAdapter<Adapter, RawEvent>, "Adapter normalize signature mismatch.");
        static_assert(lobsim::replay::IEventSource<Source, RawEvent>, "Source next(raw) signature mismatch.");
        const std::string key = book_key(id);
        if (has_stream_for_book_key(key)) {
            emit_diagnostic(key, DiagnosticRecordCode::DUPLICATE_STREAM_FOR_BOOK_IN_MULTI_BOOK_SIMULATOR,
                            DiagnosticRecordSeverity::ERROR, 0, -1, -1);
            if (cfg_.fail_fast) {
                throw std::runtime_error("MultiBookSimulator: duplicate stream registration.");
            }
            return;
        }
        if (!has_book_key(key)) {
            add_book(id);
        }
        StreamState state{};
        state.book_key = key;
        state.stream = std::make_unique<Stream<Source, Adapter, RawEvent>>(src, adapter, cfg_.fail_fast);
        streams_.push_back(std::move(state));
    }

    template <typename Source>
        requires lobsim::replay::IEventSource<Source, NormalizedLobEvent>
    void add_stream(const BookId& id, Source& src) {
        const std::string key = book_key(id);
        if (has_stream_for_book_key(key)) {
            emit_diagnostic(key, DiagnosticRecordCode::DUPLICATE_STREAM_FOR_BOOK_IN_MULTI_BOOK_SIMULATOR,
                            DiagnosticRecordSeverity::ERROR, 0, -1, -1);
            if (cfg_.fail_fast) {
                throw std::runtime_error("MultiBookSimulator: duplicate stream registration.");
            }
            return;
        }
        if (!has_book_key(key)) {
            add_book(id);
        }
        StreamState state{};
        state.book_key = key;
        state.stream = std::make_unique<NormalizedStream<Source>>(src);
        streams_.push_back(std::move(state));
    }

private:
    struct BookEntry {
        BookId id{};
        std::string key{};
        std::unique_ptr<PaperTradingSimulator> engine{};
    };

    struct IStream {
        virtual ~IStream() = default;
        virtual bool next_normalized(NormalizedLobEvent& out) = 0;
    };

    template <typename Adapter, typename RawEvent>
    static constexpr bool has_try_normalize = requires(const Adapter& a, const RawEvent& raw, NormalizedLobEvent& out) {
        { a.try_normalize(raw, out) } -> std::same_as<bool>;
    };

    template <typename T> struct NormalizeArg;
    template <typename C, typename R, typename Arg> struct NormalizeArg<R (C::*)(const Arg&) const> {
        using type = Arg;
    };
    template <typename C, typename R, typename Arg> struct NormalizeArg<R (C::*)(const Arg&)> {
        using type = Arg;
    };
    template <typename Adapter> using NormalizeArgT = typename NormalizeArg<decltype(&Adapter::normalize)>::type;

    template <typename Source, typename Adapter, typename RawEvent> struct Stream final : IStream {
        Stream(Source& source, const Adapter& adapter, bool fail_fast)
            : source_(&source), adapter_(adapter), fail_fast_(fail_fast) {}

        bool next_normalized(NormalizedLobEvent& out) override {
            RawEvent raw{};
            while (source_->next(raw)) {
                if constexpr (has_try_normalize<Adapter, RawEvent>) {
                    if (!adapter_.try_normalize(raw, out)) {
                        if (fail_fast_) {
                            throw std::runtime_error("MultiBookSimulator: adapter normalization failed.");
                        }
                        continue;
                    }
                } else {
                    try {
                        out = adapter_.normalize(raw);
                    } catch (const std::exception&) {
                        if (fail_fast_) {
                            throw;
                        }
                        continue;
                    }
                }
                return true;
            }
            return false;
        }

        Source* source_{nullptr};
        Adapter adapter_;
        bool fail_fast_{true};
    };

    template <typename Source> struct NormalizedStream final : IStream {
        explicit NormalizedStream(Source& source) : source_(&source) {}

        bool next_normalized(NormalizedLobEvent& out) override { return source_->next(out); }

        Source* source_{nullptr};
    };

    struct StreamState {
        std::string book_key{};
        std::unique_ptr<IStream> stream{};
        bool exhausted{false};
        bool has_buffered{false};
        NormalizedLobEvent buffered{};
        bool has_last_ts{false};
        std::int64_t last_ts{0};
        std::uint64_t seq{0};
    };

    struct HeapEntry {
        std::int64_t ts_received{0};
        std::int64_t ts_exchange{0};
        std::size_t stream_index{0};
        enum class EntryKind : std::uint8_t { Stream = 0, Strategy = 1 } kind{EntryKind::Stream};
        std::uint64_t strategy_seq{0};
    };

    struct HeapCompare {
        bool operator()(const HeapEntry& a, const HeapEntry& b) const {
            if (a.ts_received != b.ts_received) {
                return a.ts_received > b.ts_received;
            }
            if (a.kind != b.kind) {
                return static_cast<int>(a.kind) > static_cast<int>(b.kind);
            }
            if (a.ts_exchange != b.ts_exchange) {
                return a.ts_exchange > b.ts_exchange;
            }
            if (a.kind == HeapEntry::EntryKind::Stream) {
                return a.stream_index > b.stream_index;
            }
            return a.strategy_seq > b.strategy_seq;
        }
    };

    static std::string book_key(const BookId& id) { return ::book_key(id); }

    bool has_stream_for_book_key(std::string_view key) const {
        for (const auto& stream : streams_) {
            if (stream.book_key == key) {
                return true;
            }
        }
        return false;
    }

    void apply_to_book(const std::string& key, NormalizedLobEvent& ev) {
        auto* book = get_book_key(key);
        if (!book) {
            emit_diagnostic(key, DiagnosticRecordCode::APPLY_EVENT_REQUESTED_FOR_UNKNOWN_BOOK,
                            DiagnosticRecordSeverity::ERROR, 0, ev.ts_received, ev.ts_exchange);
            return;
        }
        if (ev.symbol_id.empty() || ev.symbol_id != key) {
            ev.symbol_id = key;
        }
        if (cfg_.require_monotonic_ts_received && has_current_time_ && ev.ts_received < current_ts_received_) {
            emit_diagnostic(key, DiagnosticRecordCode::NON_MONOTONIC_TS_RECEIVED_DETECTED_IN_MULTI_BOOK_SIMULATOR,
                            DiagnosticRecordSeverity::ERROR, 0, ev.ts_received, ev.ts_exchange);
            if (cfg_.fail_fast) {
                throw std::runtime_error("MultiBookSimulator: non-monotonic ts_received.");
            }
            return;
        }
        book->update(ev);
        has_current_time_ = true;
        current_ts_received_ = ev.ts_received;
    }

    void prime_streams() {
        for (std::size_t i = 0; i < streams_.size(); ++i) {
            if (streams_[i].has_buffered || streams_[i].exhausted) {
                continue;
            }
            fill_buffer(i);
        }
    }

    void fill_buffer(std::size_t index) {
        auto& stream = streams_[index];
        if (stream.exhausted || stream.has_buffered) {
            return;
        }
        while (true) {
            NormalizedLobEvent ev{};
            if (!stream.stream->next_normalized(ev)) {
                stream.exhausted = true;
                return;
            }

            if (cfg_.require_monotonic_ts_received && stream.has_last_ts && ev.ts_received < stream.last_ts) {
                emit_diagnostic(stream.book_key,
                                DiagnosticRecordCode::NON_MONOTONIC_TS_RECEIVED_DETECTED_IN_MULTI_BOOK_SIMULATOR,
                                DiagnosticRecordSeverity::ERROR, stream.seq, ev.ts_received, ev.ts_exchange);
                if (cfg_.fail_fast) {
                    throw std::runtime_error("MultiBookSimulator: non-monotonic stream ts_received.");
                }
                continue; // skip bad event and try next
            }
            stream.has_last_ts = true;
            stream.last_ts = ev.ts_received;

            stream.buffered = std::move(ev);
            stream.has_buffered = true;
            ++stream.seq;
            break;
        }

        heap_.push(HeapEntry{stream.buffered.ts_received, stream.buffered.ts_exchange, index,
                             HeapEntry::EntryKind::Stream, 0});
    }

    Config cfg_{};
    std::unordered_map<std::string, std::shared_ptr<BookScopedSink>> book_sinks_{};
    std::unordered_map<std::string, BookEntry> books_{};
    std::shared_ptr<IMultiLogSink> multi_sink_{nullptr};
    std::vector<StreamState> streams_{};
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapCompare> heap_{};
    struct StrategyItem {
        NormalizedLobEvent ev{};
        std::uint64_t insertion_seq{0};
    };
    std::unordered_map<std::uint64_t, StrategyItem> strategy_events_{};
    std::uint64_t strategy_seq_counter_{0};
    bool has_current_time_{false};
    std::int64_t current_ts_received_{0};
    std::mt19937_64 rng_;

    std::int64_t sample_latency() {
        switch (cfg_.latency_model.type) {
            case LatencyModelType::FIXED:
                return static_cast<std::int64_t>(cfg_.latency_model.param1);
            case LatencyModelType::UNIFORM: {
                double p1 = cfg_.latency_model.param1;
                double p2 = std::max(p1, cfg_.latency_model.param2);
                std::uniform_real_distribution<double> dist(p1, p2);
                return static_cast<std::int64_t>(dist(rng_));
            }
            case LatencyModelType::LOGNORMAL: {
                double mu = cfg_.latency_model.param1;
                double sigma = std::max(1e-9, cfg_.latency_model.param2);
                std::lognormal_distribution<double> dist(mu, sigma);
                return static_cast<std::int64_t>(dist(rng_));
            }
        }
        return 0;
    }

    void attach_book_sink(BookEntry& entry) {
        if (!multi_sink_) {
            return;
        }
        auto wrapped = std::make_shared<BookScopedSink>(entry.key, multi_sink_.get());
        entry.engine->set_log_sink(wrapped);
        book_sinks_[entry.key] = std::move(wrapped);
    }

    void emit_diagnostic(const std::string& key, DiagnosticRecordCode code, DiagnosticRecordSeverity sev,
                         std::uint64_t seq, std::int64_t ts_received, std::int64_t ts_exchange) const {
        if (!multi_sink_) {
            return;
        }
        multi_sink_->on_diagnostic(DiagnosticRecord{seq, ts_exchange, ts_received, code, sev, key});
    }
};
