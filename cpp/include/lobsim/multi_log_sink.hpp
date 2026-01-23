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

    virtual std::vector<FillRecord> fills_for(std::string_view book_key) const = 0;
    virtual std::vector<EventApplyRecord> events_for(std::string_view book_key) const = 0;
    virtual std::vector<DiagnosticRecord> diagnostics_for(std::string_view book_key) const = 0;
};

class BookScopedSink final : public ILogSink {
public:
    BookScopedSink(std::string book_key, IMultiLogSink* parent) : book_key_(std::move(book_key)), parent_(parent) {}

    void on_fill(const FillRecord& r) override {
        FillRecord tagged = r;
        if (tagged.book_key.empty()) {
            tagged.book_key = book_key_;
        }
        parent_->on_fill(tagged);
    }
    void on_event_apply(const EventApplyRecord& r) override {
        EventApplyRecord tagged = r;
        if (tagged.book_key.empty()) {
            tagged.book_key = book_key_;
        }
        parent_->on_event_apply(tagged);
    }
    void on_diagnostic(const DiagnosticRecord& r) override {
        DiagnosticRecord tagged = r;
        if (tagged.book_key.empty()) {
            tagged.book_key = book_key_;
        }
        parent_->on_diagnostic(tagged);
    }
    void reset() override {}

private:
    std::string book_key_;
    IMultiLogSink* parent_{nullptr};
};

class InMemoryMultiLogSink final : public IMultiLogSink {
public:
    void on_fill(const FillRecord& r) override {
        fill_index_[r.book_key].push_back(fills_.size());
        fills_.push_back(r);
        apply_strategy_fill(r.book_key, r.maker_order_id, r.maker_source, PaperOrderFillRole::MAKER, r);
        apply_strategy_fill(r.book_key, r.taker_order_id, r.taker_source, PaperOrderFillRole::TAKER, r);
    }
    void on_event_apply(const EventApplyRecord& r) override {
        event_index_[r.book_key].push_back(events_.size());
        events_.push_back(r);
        update_strategy_order(r);
    }
    void on_diagnostic(const DiagnosticRecord& r) override {
        diag_index_[r.book_key].push_back(diagnostics_.size());
        diagnostics_.push_back(r);
    }
    void reset() override {
        fills_.clear();
        events_.clear();
        diagnostics_.clear();
        fill_index_.clear();
        event_index_.clear();
        diag_index_.clear();
        paper_ledger_.clear();
        rejected_strategy_events_.clear();
    }

    std::vector<FillRecord> fills_for(std::string_view book_key) const override {
        std::vector<FillRecord> out;
        auto it = fill_index_.find(std::string(book_key));
        if (it == fill_index_.end()) {
            return out;
        }
        out.reserve(it->second.size());
        for (auto idx : it->second) {
            out.push_back(fills_[idx]);
        }
        return out;
    }

    std::vector<EventApplyRecord> events_for(std::string_view book_key) const override {
        std::vector<EventApplyRecord> out;
        auto it = event_index_.find(std::string(book_key));
        if (it == event_index_.end()) {
            return out;
        }
        out.reserve(it->second.size());
        for (auto idx : it->second) {
            out.push_back(events_[idx]);
        }
        return out;
    }

    std::vector<DiagnosticRecord> diagnostics_for(std::string_view book_key) const override {
        std::vector<DiagnosticRecord> out;
        auto it = diag_index_.find(std::string(book_key));
        if (it == diag_index_.end()) {
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
    paper_ledger() const {
        return paper_ledger_;
    }

    std::unordered_map<std::int64_t, PaperOrderLedgerEntry> paper_ledger_for(std::string_view book_key) const {
        auto it = paper_ledger_.find(std::string(book_key));
        if (it == paper_ledger_.end()) {
            return {};
        }
        return it->second;
    }

    const PaperOrderLedgerEntry* find_paper_order(std::string_view book_key, std::int64_t order_id) const {
        auto it = paper_ledger_.find(std::string(book_key));
        if (it == paper_ledger_.end()) {
            return nullptr;
        }
        auto entry = it->second.find(order_id);
        if (entry == it->second.end()) {
            return nullptr;
        }
        return &entry->second;
    }

    std::vector<EventApplyRecord> rejected_strategy_events_for(std::string_view book_key) const {
        auto it = rejected_strategy_events_.find(std::string(book_key));
        if (it == rejected_strategy_events_.end()) {
            return {};
        }
        return it->second;
    }

    std::vector<FillRecord> drain_fills() {
        std::vector<FillRecord> out;
        out.swap(fills_);
        fill_index_.clear();
        return out;
    }

    std::vector<EventApplyRecord> drain_events() {
        std::vector<EventApplyRecord> out;
        out.swap(events_);
        event_index_.clear();
        return out;
    }

    std::vector<DiagnosticRecord> drain_diagnostics() {
        std::vector<DiagnosticRecord> out;
        out.swap(diagnostics_);
        diag_index_.clear();
        return out;
    }

private:
    void apply_strategy_fill(std::string_view book_key, std::int64_t order_id, UpdateSource source,
                             PaperOrderFillRole role, const FillRecord& r) {
        if (source != UpdateSource::STRATEGY) {
            return;
        }
        if (order_id == UnknownOrderIdSentinel) {
            return;
        }
        auto it = paper_ledger_.find(std::string(book_key));
        if (it == paper_ledger_.end()) {
            return;
        }
        auto entry_it = it->second.find(order_id);
        if (entry_it == it->second.end()) {
            return;
        }
        auto& entry = entry_it->second;
        auto& state = entry.state;
        if (state.status == PaperOrderLedgerStatus::CANCELLED || state.status == PaperOrderLedgerStatus::REJECTED) {
            return;
        }

        PaperOrderFill fill{r.seq, r.ts_exchange, r.ts_received, r.price_ticks, r.qty_lots, role};
        entry.fills.push_back(fill);

        if (r.qty_lots > 0) {
            state.filled_qty += r.qty_lots;
            state.remaining_qty = std::max<std::int64_t>(state.remaining_qty - r.qty_lots, 0);
            state.status =
                state.remaining_qty == 0 ? PaperOrderLedgerStatus::FILLED : PaperOrderLedgerStatus::PARTIALLY_FILLED;
            state.last_update_seq = r.seq;
        }
    }

    void update_strategy_order(const EventApplyRecord& r) {
        if (r.source != UpdateSource::STRATEGY) {
            return;
        }
        auto& ledger = paper_ledger_[r.book_key];
        auto& rejected = rejected_strategy_events_[r.book_key];

        switch (r.update_type) {
        case UpdateType::ADD: {
            if (r.qty_lots <= 0) {
                rejected.push_back(r);
                return;
            }
            if (ledger.contains(r.order_id)) {
                rejected.push_back(r);
                return;
            }
            PaperOrderLedgerEntry entry{};
            entry.state = PaperOrderState{
                r.order_id, r.side, r.price_ticks, r.qty_lots, r.qty_lots, 0, PaperOrderLedgerStatus::OPEN,
                r.seq,      r.seq};
            ledger.emplace(r.order_id, std::move(entry));
            return;
        }
        case UpdateType::SUBTRACT: {
            if (r.qty_lots < 0) {
                rejected.push_back(r);
                return;
            }
            auto it = ledger.find(r.order_id);
            if (it == ledger.end()) {
                return;
            }
            auto& state = it->second.state;
            if (state.status == PaperOrderLedgerStatus::FILLED || state.status == PaperOrderLedgerStatus::CANCELLED ||
                state.status == PaperOrderLedgerStatus::REJECTED) {
                return;
            }
            if (r.qty_lots == 0) {
                state.last_update_seq = r.seq;
                return;
            }
            const auto take = std::min(state.remaining_qty, r.qty_lots);
            state.remaining_qty -= take;
            if (state.remaining_qty == 0) {
                state.status = PaperOrderLedgerStatus::CANCELLED;
            }
            state.last_update_seq = r.seq;
            return;
        }
        case UpdateType::SET: {
            auto it = ledger.find(r.order_id);
            if (it == ledger.end()) {
                return;
            }
            auto& state = it->second.state;
            if (state.status == PaperOrderLedgerStatus::FILLED || state.status == PaperOrderLedgerStatus::CANCELLED ||
                state.status == PaperOrderLedgerStatus::REJECTED) {
                return;
            }
            auto new_qty = r.qty_lots < 0 ? 0 : r.qty_lots;
            state.remaining_qty = new_qty;
            const auto total_qty = state.filled_qty + state.remaining_qty;
            if (total_qty > state.initial_qty) {
                state.initial_qty = total_qty;
            }
            if (state.remaining_qty == 0) {
                state.status = PaperOrderLedgerStatus::CANCELLED;
            } else if (state.filled_qty > 0) {
                state.status = PaperOrderLedgerStatus::PARTIALLY_FILLED;
            } else {
                state.status = PaperOrderLedgerStatus::OPEN;
            }
            state.last_update_seq = r.seq;
            return;
        }
        case UpdateType::DELETE: {
            auto it = ledger.find(r.order_id);
            if (it == ledger.end()) {
                return;
            }
            auto& state = it->second.state;
            if (state.status != PaperOrderLedgerStatus::FILLED) {
                state.remaining_qty = 0;
                state.status = PaperOrderLedgerStatus::CANCELLED;
                state.last_update_seq = r.seq;
            }
            return;
        }
        case UpdateType::MATCH:
            return;
        case UpdateType::AGGRESSIVE_TRADE:
            return;
        }
    }

    std::vector<FillRecord> fills_;
    std::vector<EventApplyRecord> events_;
    std::vector<DiagnosticRecord> diagnostics_;
    std::unordered_map<std::string, std::vector<std::size_t>> fill_index_;
    std::unordered_map<std::string, std::vector<std::size_t>> event_index_;
    std::unordered_map<std::string, std::vector<std::size_t>> diag_index_;
    std::unordered_map<std::string, std::unordered_map<std::int64_t, PaperOrderLedgerEntry>> paper_ledger_;
    std::unordered_map<std::string, std::vector<EventApplyRecord>> rejected_strategy_events_;
};
