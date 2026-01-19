#pragma once

#include "lobsim/in_memory_sink.hpp"
#include "lobsim/lob_event.hpp"
#include "lobsim/log_sink.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class IMultiLogSink : public ILogSink {
public:
    ~IMultiLogSink() override = default;

    virtual std::vector<FillRecord> fillsFor(std::string_view bookKey) const = 0;
    virtual std::vector<EventApplyRecord> eventsFor(std::string_view bookKey) const = 0;
    virtual std::vector<DiagnosticRecord> diagnosticsFor(std::string_view bookKey) const = 0;
};

class BookScopedSink final : public ILogSink {
public:
    BookScopedSink(std::string bookKey, IMultiLogSink* parent) : bookKey_(std::move(bookKey)), parent_(parent) {}

    void onFill(const FillRecord& r) override {
        FillRecord tagged = r;
        if (tagged.bookKey.empty()) {
            tagged.bookKey = bookKey_;
        }
        parent_->onFill(tagged);
    }
    void onEventApply(const EventApplyRecord& r) override {
        EventApplyRecord tagged = r;
        if (tagged.bookKey.empty()) {
            tagged.bookKey = bookKey_;
        }
        parent_->onEventApply(tagged);
    }
    void onDiagnostic(const DiagnosticRecord& r) override {
        DiagnosticRecord tagged = r;
        if (tagged.bookKey.empty()) {
            tagged.bookKey = bookKey_;
        }
        parent_->onDiagnostic(tagged);
    }
    void reset() override {}

private:
    std::string bookKey_;
    IMultiLogSink* parent_{nullptr};
};

class InMemoryMultiLogSink final : public IMultiLogSink {
public:
    void onFill(const FillRecord& r) override {
        fillIndex_[r.bookKey].push_back(fills_.size());
        fills_.push_back(r);
        applyStrategyFill(r.bookKey, r.makerOrderId, r.makerSource, PaperOrderFillRole::MAKER, r);
        applyStrategyFill(r.bookKey, r.takerOrderId, r.takerSource, PaperOrderFillRole::TAKER, r);
    }
    void onEventApply(const EventApplyRecord& r) override {
        eventIndex_[r.bookKey].push_back(events_.size());
        events_.push_back(r);
        updateStrategyOrder(r);
    }
    void onDiagnostic(const DiagnosticRecord& r) override {
        diagIndex_[r.bookKey].push_back(diagnostics_.size());
        diagnostics_.push_back(r);
    }
    void reset() override {
        fills_.clear();
        events_.clear();
        diagnostics_.clear();
        fillIndex_.clear();
        eventIndex_.clear();
        diagIndex_.clear();
        paperLedger_.clear();
        rejectedStrategyEvents_.clear();
    }

    std::vector<FillRecord> fillsFor(std::string_view bookKey) const override {
        std::vector<FillRecord> out;
        auto it = fillIndex_.find(std::string(bookKey));
        if (it == fillIndex_.end()) {
            return out;
        }
        out.reserve(it->second.size());
        for (auto idx : it->second) {
            out.push_back(fills_[idx]);
        }
        return out;
    }

    std::vector<EventApplyRecord> eventsFor(std::string_view bookKey) const override {
        std::vector<EventApplyRecord> out;
        auto it = eventIndex_.find(std::string(bookKey));
        if (it == eventIndex_.end()) {
            return out;
        }
        out.reserve(it->second.size());
        for (auto idx : it->second) {
            out.push_back(events_[idx]);
        }
        return out;
    }

    std::vector<DiagnosticRecord> diagnosticsFor(std::string_view bookKey) const override {
        std::vector<DiagnosticRecord> out;
        auto it = diagIndex_.find(std::string(bookKey));
        if (it == diagIndex_.end()) {
            return out;
        }
        out.reserve(it->second.size());
        for (auto idx : it->second) {
            out.push_back(diagnostics_[idx]);
        }
        return out;
    }

    const std::vector<FillRecord>& fills() const { return fills_; }
    const std::vector<EventApplyRecord>& events() const { return events_; }
    const std::vector<DiagnosticRecord>& diagnostics() const { return diagnostics_; }
    const std::unordered_map<std::string, std::unordered_map<std::int64_t, PaperOrderLedgerEntry>>&
    paperLedger() const {
        return paperLedger_;
    }

    std::unordered_map<std::int64_t, PaperOrderLedgerEntry> paperLedgerFor(std::string_view bookKey) const {
        auto it = paperLedger_.find(std::string(bookKey));
        if (it == paperLedger_.end()) {
            return {};
        }
        return it->second;
    }

    const PaperOrderLedgerEntry* findPaperOrder(std::string_view bookKey, std::int64_t orderId) const {
        auto it = paperLedger_.find(std::string(bookKey));
        if (it == paperLedger_.end()) {
            return nullptr;
        }
        auto entry = it->second.find(orderId);
        if (entry == it->second.end()) {
            return nullptr;
        }
        return &entry->second;
    }

    std::vector<EventApplyRecord> rejectedStrategyEventsFor(std::string_view bookKey) const {
        auto it = rejectedStrategyEvents_.find(std::string(bookKey));
        if (it == rejectedStrategyEvents_.end()) {
            return {};
        }
        return it->second;
    }

    std::vector<FillRecord> drainFills() {
        std::vector<FillRecord> out;
        out.swap(fills_);
        fillIndex_.clear();
        return out;
    }

    std::vector<EventApplyRecord> drainEvents() {
        std::vector<EventApplyRecord> out;
        out.swap(events_);
        eventIndex_.clear();
        return out;
    }

    std::vector<DiagnosticRecord> drainDiagnostics() {
        std::vector<DiagnosticRecord> out;
        out.swap(diagnostics_);
        diagIndex_.clear();
        return out;
    }

private:
    void applyStrategyFill(std::string_view bookKey, std::int64_t orderId, UpdateSource source, PaperOrderFillRole role,
                           const FillRecord& r) {
        if (source != UpdateSource::STRATEGY) {
            return;
        }
        if (orderId == UnknownOrderIdSentinel) {
            return;
        }
        auto it = paperLedger_.find(std::string(bookKey));
        if (it == paperLedger_.end()) {
            return;
        }
        auto entryIt = it->second.find(orderId);
        if (entryIt == it->second.end()) {
            return;
        }
        auto& entry = entryIt->second;
        auto& state = entry.state;
        if (state.status == PaperOrderLedgerStatus::CANCELLED || state.status == PaperOrderLedgerStatus::REJECTED) {
            return;
        }

        PaperOrderFill fill{r.seq, r.tsExchange, r.tsReceived, r.priceTicks, r.qtyLots, role};
        entry.fills.push_back(fill);

        if (r.qtyLots > 0) {
            state.filledQty += r.qtyLots;
            state.remainingQty = std::max<std::int64_t>(state.remainingQty - r.qtyLots, 0);
            state.status =
                state.remainingQty == 0 ? PaperOrderLedgerStatus::FILLED : PaperOrderLedgerStatus::PARTIALLY_FILLED;
            state.lastUpdateSeq = r.seq;
        }
    }

    void updateStrategyOrder(const EventApplyRecord& r) {
        if (r.source != UpdateSource::STRATEGY) {
            return;
        }
        auto& ledger = paperLedger_[r.bookKey];
        auto& rejected = rejectedStrategyEvents_[r.bookKey];

        switch (r.updateType) {
        case UpdateType::ADD: {
            if (r.qtyLots <= 0) {
                rejected.push_back(r);
                return;
            }
            if (ledger.contains(r.orderId)) {
                rejected.push_back(r);
                return;
            }
            PaperOrderLedgerEntry entry{};
            entry.state = PaperOrderState{
                r.orderId, r.side, r.priceTicks, r.qtyLots, r.qtyLots, 0, PaperOrderLedgerStatus::OPEN, r.seq, r.seq};
            ledger.emplace(r.orderId, std::move(entry));
            return;
        }
        case UpdateType::SUBTRACT: {
            if (r.qtyLots < 0) {
                rejected.push_back(r);
                return;
            }
            auto it = ledger.find(r.orderId);
            if (it == ledger.end()) {
                return;
            }
            auto& state = it->second.state;
            if (state.status == PaperOrderLedgerStatus::FILLED || state.status == PaperOrderLedgerStatus::CANCELLED ||
                state.status == PaperOrderLedgerStatus::REJECTED) {
                return;
            }
            if (r.qtyLots == 0) {
                state.lastUpdateSeq = r.seq;
                return;
            }
            const auto take = std::min(state.remainingQty, r.qtyLots);
            state.remainingQty -= take;
            if (state.remainingQty == 0) {
                state.status = PaperOrderLedgerStatus::CANCELLED;
            }
            state.lastUpdateSeq = r.seq;
            return;
        }
        case UpdateType::SET: {
            auto it = ledger.find(r.orderId);
            if (it == ledger.end()) {
                return;
            }
            auto& state = it->second.state;
            if (state.status == PaperOrderLedgerStatus::FILLED || state.status == PaperOrderLedgerStatus::CANCELLED ||
                state.status == PaperOrderLedgerStatus::REJECTED) {
                return;
            }
            auto newQty = r.qtyLots < 0 ? 0 : r.qtyLots;
            state.remainingQty = newQty;
            const auto totalQty = state.filledQty + state.remainingQty;
            if (totalQty > state.initialQty) {
                state.initialQty = totalQty;
            }
            if (state.remainingQty == 0) {
                state.status = PaperOrderLedgerStatus::CANCELLED;
            } else if (state.filledQty > 0) {
                state.status = PaperOrderLedgerStatus::PARTIALLY_FILLED;
            } else {
                state.status = PaperOrderLedgerStatus::OPEN;
            }
            state.lastUpdateSeq = r.seq;
            return;
        }
        case UpdateType::DELETE: {
            auto it = ledger.find(r.orderId);
            if (it == ledger.end()) {
                return;
            }
            auto& state = it->second.state;
            if (state.status != PaperOrderLedgerStatus::FILLED) {
                state.remainingQty = 0;
                state.status = PaperOrderLedgerStatus::CANCELLED;
                state.lastUpdateSeq = r.seq;
            }
            return;
        }
        case UpdateType::MATCH:
            return;
        }
    }

    std::vector<FillRecord> fills_;
    std::vector<EventApplyRecord> events_;
    std::vector<DiagnosticRecord> diagnostics_;
    std::unordered_map<std::string, std::vector<std::size_t>> fillIndex_;
    std::unordered_map<std::string, std::vector<std::size_t>> eventIndex_;
    std::unordered_map<std::string, std::vector<std::size_t>> diagIndex_;
    std::unordered_map<std::string, std::unordered_map<std::int64_t, PaperOrderLedgerEntry>> paperLedger_;
    std::unordered_map<std::string, std::vector<EventApplyRecord>> rejectedStrategyEvents_;
};
