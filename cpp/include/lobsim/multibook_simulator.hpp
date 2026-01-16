#pragma once

#include "lobsim/book_id.hpp"
#include "lobsim/event_adapter.hpp"
#include "lobsim/event_source.hpp"
#include "lobsim/log_sink.hpp"
#include "lobsim/multi_log_sink.hpp"
#include "lobsim/paper_trading_simulator.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

class MultiBookSimulator {
public:
    struct Config {
        bool requireMonotonicTsReceived{true};
        bool failFast{true};
    };

    MultiBookSimulator() = default;
    explicit MultiBookSimulator(Config cfg) : cfg_(cfg) {}

    bool addBook(const BookId& id) { return addBook(id, std::make_unique<PaperTradingSimulator>()); }

    bool addBook(const BookId& id, std::unique_ptr<PaperTradingSimulator> engine) {
        const std::string key = bookKey(id);
        if (books_.find(key) != books_.end()) {
            return false;
        }
        BookEntry entry{};
        entry.id = id;
        entry.key = key;
        entry.engine = std::move(engine);
        attachBookSink(entry);
        books_.emplace(key, std::move(entry));
        return true;
    }

    bool hasBook(const BookId& id) const { return hasBookKey(bookKey(id)); }
    bool hasBookKey(std::string_view key) const { return books_.find(std::string(key)) != books_.end(); }

    PaperTradingSimulator* getBook(const BookId& id) { return getBookKey(bookKey(id)); }
    const PaperTradingSimulator* getBook(const BookId& id) const { return getBookKey(bookKey(id)); }

    PaperTradingSimulator* getBookKey(std::string_view key) {
        auto it = books_.find(std::string(key));
        if (it == books_.end()) {
            return nullptr;
        }
        return it->second.engine.get();
    }

    const PaperTradingSimulator* getBookKey(std::string_view key) const {
        auto it = books_.find(std::string(key));
        if (it == books_.end()) {
            return nullptr;
        }
        return it->second.engine.get();
    }

    void setLogSink(const BookId& id, ILogSink* sink) {
        auto* book = getBook(id);
        if (!book) {
            emitDiagnostic(bookKey(id), DiagnosticRecordCode::SET_LOG_SINK_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR,
                           DiagnosticRecordSeverity::ERROR, 0, -1, -1);
            return;
        }
        book->setLogSink(sink);
        bookSinks_.erase(bookKey(id));
    }

    void setMultiLogSink(IMultiLogSink* sink) {
        multiSink_ = sink;
        bookSinks_.clear();
        for (auto& [key, entry] : books_) {
            attachBookSink(entry);
        }
    }

    std::optional<std::int64_t> depthAt(const BookId& id, Side side, std::int64_t priceTicks) const {
        const auto* book = getBook(id);
        if (!book) {
            emitDiagnostic(bookKey(id), DiagnosticRecordCode::DEPTH_AT_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR,
                           DiagnosticRecordSeverity::ERROR, 0, -1, -1);
            return std::nullopt;
        }
        return book->depthAt(side, priceTicks);
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> l2TopN(const BookId& id, Side side, std::uint32_t n) const {
        const auto* book = getBook(id);
        if (!book) {
            emitDiagnostic(bookKey(id), DiagnosticRecordCode::L2_TOP_N_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR,
                           DiagnosticRecordSeverity::ERROR, 0, -1, -1);
            return {};
        }
        return book->l2TopN(side, n);
    }

    std::optional<std::int64_t> getBestPriceTicks(const BookId& id, Side side) const {
        const auto* book = getBook(id);
        if (!book) {
            emitDiagnostic(bookKey(id),
                           DiagnosticRecordCode::GET_BEST_PRICE_TICKS_FOR_UNKNOWN_BOOK_IN_MULTI_BOOK_SIMULATOR,
                           DiagnosticRecordSeverity::ERROR, 0, -1, -1);
            return std::nullopt;
        }
        return book->getBestPriceTicks(side);
    }

    void apply(const BookId& id, NormalizedLobEvent ev) { applyToBook(bookKey(id), ev); }

    std::optional<std::int64_t> currentTime() const {
        if (!hasCurrentTime_) {
            return std::nullopt;
        }
        return currentTsReceived_;
    }

    bool step() {
        primeStreams();
        if (heap_.empty()) {
            return false;
        }

        const auto entry = heap_.top();
        heap_.pop();

        auto& stream = streams_[entry.streamIndex];
        if (!stream.hasBuffered) {
            emitDiagnostic(stream.bookKey, DiagnosticRecordCode::HEAP_ENTRY_WITHOUT_BUFFERED_EVENT,
                           DiagnosticRecordSeverity::ERROR, stream.seq, currentTsReceived_, -1);
            if (cfg_.failFast) {
                throw std::runtime_error("MultiBookSimulator: heap entry without buffered event.");
            }
            return false;
        }

        NormalizedLobEvent ev = std::move(stream.buffered);
        stream.hasBuffered = false;

        applyToBook(stream.bookKey, ev);
        fillBuffer(entry.streamIndex);

        return true;
    }

    std::size_t stepUntil(std::int64_t tsReceivedInclusive) {
        std::size_t applied = 0;
        primeStreams();
        while (!heap_.empty()) {
            const auto& entry = heap_.top();
            if (entry.tsReceived > tsReceivedInclusive) {
                break;
            }
            if (!step()) {
                break;
            }
            ++applied;
        }
        return applied;
    }

    std::size_t stepFor(std::int64_t deltaTs) {
        if (deltaTs <= 0) {
            return 0;
        }
        const std::int64_t base = hasCurrentTime_ ? currentTsReceived_ : 0;
        return stepUntil(base + deltaTs);
    }

    template <typename Source, typename Adapter> void addStream(const BookId& id, Source& src, const Adapter& adapter) {
        using RawEvent = NormalizeArgT<Adapter>;
        static_assert(IEventAdapter<Adapter, RawEvent>, "Adapter normalize signature mismatch.");
        static_assert(lobsim::replay::IEventSource<Source, RawEvent>, "Source next(raw) signature mismatch.");
        const std::string key = bookKey(id);
        if (!hasBookKey(key)) {
            addBook(id);
        }
        StreamState state{};
        state.bookKey = key;
        state.stream = std::make_unique<Stream<Source, Adapter, RawEvent>>(src, adapter, cfg_.failFast);
        streams_.push_back(std::move(state));
    }

    template <typename Source>
        requires lobsim::replay::IEventSource<Source, NormalizedLobEvent>
    void addStream(const BookId& id, Source& src) {
        const std::string key = bookKey(id);
        if (!hasBookKey(key)) {
            addBook(id);
        }
        StreamState state{};
        state.bookKey = key;
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
        virtual bool nextNormalized(NormalizedLobEvent& out) = 0;
    };

    template <typename Adapter, typename RawEvent>
    static constexpr bool HasTryNormalize = requires(const Adapter& a, const RawEvent& raw, NormalizedLobEvent& out) {
        { a.tryNormalize(raw, out) } -> std::same_as<bool>;
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
        Stream(Source& source, const Adapter& adapter, bool failFast)
            : source_(&source), adapter_(&adapter), failFast_(failFast) {}

        bool nextNormalized(NormalizedLobEvent& out) override {
            RawEvent raw{};
            while (source_->next(raw)) {
                if constexpr (HasTryNormalize<Adapter, RawEvent>) {
                    if (!adapter_->tryNormalize(raw, out)) {
                        if (failFast_) {
                            throw std::runtime_error("MultiBookSimulator: adapter normalization failed.");
                        }
                        continue;
                    }
                } else {
                    try {
                        out = adapter_->normalize(raw);
                    } catch (const std::exception&) {
                        if (failFast_) {
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
        const Adapter* adapter_{nullptr};
        bool failFast_{true};
    };

    template <typename Source> struct NormalizedStream final : IStream {
        explicit NormalizedStream(Source& source) : source_(&source) {}

        bool nextNormalized(NormalizedLobEvent& out) override { return source_->next(out); }

        Source* source_{nullptr};
    };

    struct StreamState {
        std::string bookKey{};
        std::unique_ptr<IStream> stream{};
        bool exhausted{false};
        bool hasBuffered{false};
        NormalizedLobEvent buffered{};
        bool hasLastTs{false};
        std::int64_t lastTs{0};
        std::uint64_t seq{0};
    };

    struct HeapEntry {
        std::int64_t tsReceived{0};
        std::int64_t tsExchange{0};
        std::size_t streamIndex{0};
        std::uint64_t seq{0};
    };

    struct HeapCompare {
        bool operator()(const HeapEntry& a, const HeapEntry& b) const {
            if (a.tsReceived != b.tsReceived) {
                return a.tsReceived > b.tsReceived;
            }
            if (a.tsExchange != b.tsExchange) {
                return a.tsExchange > b.tsExchange;
            }
            if (a.streamIndex != b.streamIndex) {
                return a.streamIndex > b.streamIndex;
            }
            return a.seq > b.seq;
        }
    };

    static std::string bookKey(const BookId& id) { return ::bookKey(id); }

    void applyToBook(const std::string& key, NormalizedLobEvent& ev) {
        auto* book = getBookKey(key);
        if (!book) {
            emitDiagnostic(key, DiagnosticRecordCode::APPLY_EVENT_REQUESTED_FOR_UNKNOWN_BOOK,
                           DiagnosticRecordSeverity::ERROR, 0, ev.tsReceived, ev.tsExchange);
            return;
        }
        if (ev.symbolId.empty() || ev.symbolId != key) {
            ev.symbolId = key;
        }
        if (cfg_.requireMonotonicTsReceived && hasCurrentTime_ && ev.tsReceived < currentTsReceived_) {
            emitDiagnostic(key, DiagnosticRecordCode::NON_MONOTONIC_TS_RECEIVED_DETECTED_IN_MULTI_BOOK_SIMULATOR,
                           DiagnosticRecordSeverity::ERROR, 0, ev.tsReceived, ev.tsExchange);
            return;
        }
        book->update(ev);
        hasCurrentTime_ = true;
        currentTsReceived_ = ev.tsReceived;
    }

    void primeStreams() {
        for (std::size_t i = 0; i < streams_.size(); ++i) {
            if (streams_[i].hasBuffered || streams_[i].exhausted) {
                continue;
            }
            fillBuffer(i);
        }
    }

    void fillBuffer(std::size_t index) {
        auto& stream = streams_[index];
        if (stream.exhausted || stream.hasBuffered) {
            return;
        }
        NormalizedLobEvent ev{};
        if (!stream.stream->nextNormalized(ev)) {
            stream.exhausted = true;
            return;
        }

        if (cfg_.requireMonotonicTsReceived && stream.hasLastTs && ev.tsReceived < stream.lastTs) {
            emitDiagnostic(stream.bookKey,
                           DiagnosticRecordCode::NON_MONOTONIC_TS_RECEIVED_DETECTED_IN_MULTI_BOOK_SIMULATOR,
                           DiagnosticRecordSeverity::ERROR, stream.seq, ev.tsReceived, ev.tsExchange);
            return;
        }
        stream.hasLastTs = true;
        stream.lastTs = ev.tsReceived;

        stream.buffered = std::move(ev);
        stream.hasBuffered = true;
        ++stream.seq;

        heap_.push(HeapEntry{stream.buffered.tsReceived, stream.buffered.tsExchange, index, stream.seq});
    }

    Config cfg_{};
    std::unordered_map<std::string, BookEntry> books_{};
    std::unordered_map<std::string, std::unique_ptr<BookScopedSink>> bookSinks_{};
    IMultiLogSink* multiSink_{nullptr};
    std::vector<StreamState> streams_{};
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapCompare> heap_{};
    bool hasCurrentTime_{false};
    std::int64_t currentTsReceived_{0};

    void attachBookSink(BookEntry& entry) {
        if (!multiSink_) {
            return;
        }
        auto wrapped = std::make_unique<BookScopedSink>(entry.key, multiSink_);
        entry.engine->setLogSink(wrapped.get());
        bookSinks_[entry.key] = std::move(wrapped);
    }

    void emitDiagnostic(const std::string& key, DiagnosticRecordCode code, DiagnosticRecordSeverity sev,
                        std::uint64_t seq, std::int64_t tsReceived, std::int64_t tsExchange) const {
        if (!multiSink_) {
            return;
        }
        multiSink_->onDiagnostic(DiagnosticRecord{seq, tsExchange, tsReceived, code, sev, key});
    }
};
