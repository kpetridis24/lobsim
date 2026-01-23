#include "lobsim/in_memory_sink.hpp"

#include "lobsim/lob_event.hpp"

#include <algorithm>

void InMemoryLogSink::on_fill(const FillRecord& r) {
    fills.push_back(r);
    apply_strategy_fill(r.maker_order_id, r.maker_source, PaperOrderFillRole::MAKER, r);
    apply_strategy_fill(r.taker_order_id, r.taker_source, PaperOrderFillRole::TAKER, r);
}

void InMemoryLogSink::on_event_apply(const EventApplyRecord& r) {
    events.push_back(r);
    update_strategy_order(r);
}

void InMemoryLogSink::on_diagnostic(const DiagnosticRecord& r) {
    diagnostics.push_back(r);
}

void InMemoryLogSink::reset() {
    fills.clear();
    events.clear();
    diagnostics.clear();
    paper_ledger.clear();
    rejected_strategy_events.clear();
}

std::vector<FillRecord> InMemoryLogSink::drain_fills() {
    std::vector<FillRecord> out;
    out.swap(fills);
    return out;
}

std::vector<EventApplyRecord> InMemoryLogSink::drain_events() {
    std::vector<EventApplyRecord> out;
    out.swap(events);
    return out;
}

const std::vector<FillRecord>& InMemoryLogSink::get_fills() const {
    return fills;
}

const std::vector<EventApplyRecord>& InMemoryLogSink::get_events() const {
    return events;
}

const std::vector<DiagnosticRecord>& InMemoryLogSink::get_diagnostics() const {
    return diagnostics;
}

const std::unordered_map<std::int64_t, PaperOrderLedgerEntry>& InMemoryLogSink::get_paper_ledger() const {
    return paper_ledger;
}

const PaperOrderLedgerEntry* InMemoryLogSink::find_paper_order(std::int64_t order_id) const {
    auto it = paper_ledger.find(order_id);
    if (it == paper_ledger.end()) {
        return nullptr;
    }
    return &it->second;
}

const std::vector<EventApplyRecord>& InMemoryLogSink::get_rejected_strategy_events() const {
    return rejected_strategy_events;
}

void InMemoryLogSink::apply_strategy_fill(std::int64_t order_id, UpdateSource source, PaperOrderFillRole role,
                                          const FillRecord& r) {
    if (source != UpdateSource::STRATEGY) {
        return;
    }
    if (order_id == UnknownOrderIdSentinel) {
        return;
    }

    auto it = paper_ledger.find(order_id);
    if (it == paper_ledger.end()) {
        return;
    }

    auto& entry = it->second;
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

void InMemoryLogSink::update_strategy_order(const EventApplyRecord& r) {
    if (r.source != UpdateSource::STRATEGY) {
        return;
    }

    switch (r.update_type) {
    case UpdateType::ADD: {
        if (r.qty_lots <= 0) {
            rejected_strategy_events.push_back(r);
            return;
        }
        if (paper_ledger.contains(r.order_id)) {
            rejected_strategy_events.push_back(r);
            return;
        }
        PaperOrderLedgerEntry entry{};
        entry.state = PaperOrderState{
            r.order_id, r.side, r.price_ticks, r.qty_lots, r.qty_lots, 0, PaperOrderLedgerStatus::OPEN, r.seq, r.seq};
        paper_ledger.emplace(r.order_id, std::move(entry));
        return;
    }
    case UpdateType::SUBTRACT: {
        if (r.qty_lots < 0) {
            rejected_strategy_events.push_back(r);
            return;
        }
        auto it = paper_ledger.find(r.order_id);
        if (it == paper_ledger.end()) {
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
        auto it = paper_ledger.find(r.order_id);
        if (it == paper_ledger.end()) {
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
        auto it = paper_ledger.find(r.order_id);
        if (it == paper_ledger.end()) {
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
