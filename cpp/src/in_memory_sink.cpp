#include "lobsim/in_memory_sink.hpp"

#include "lobsim/lob_event.hpp"

#include <algorithm>

void InMemoryLogSink::onFill(const FillRecord& r) {
    fills.push_back(r);
    applyStrategyFill(r.makerOrderId, r.makerSource, PaperOrderFillRole::MAKER, r);
    applyStrategyFill(r.takerOrderId, r.takerSource, PaperOrderFillRole::TAKER, r);
}

void InMemoryLogSink::onEventApply(const EventApplyRecord& r) {
    events.push_back(r);
    updateStrategyOrder(r);
}

void InMemoryLogSink::onDiagnostic(const DiagnosticRecord& r) {
    diagnostics.push_back(r);
}

void InMemoryLogSink::reset() {
    fills.clear();
    events.clear();
    diagnostics.clear();
    paperLedger.clear();
    rejectedStrategyEvents.clear();
}

std::vector<FillRecord> InMemoryLogSink::drainFills() {
    std::vector<FillRecord> out;
    out.swap(fills);
    return out;
}

std::vector<EventApplyRecord> InMemoryLogSink::drainEvents() {
    std::vector<EventApplyRecord> out;
    out.swap(events);
    return out;
}

const std::vector<FillRecord>& InMemoryLogSink::getFills() const {
    return fills;
}

const std::vector<EventApplyRecord>& InMemoryLogSink::getEvents() const {
    return events;
}

const std::vector<DiagnosticRecord>& InMemoryLogSink::getDiagnostics() const {
    return diagnostics;
}

const std::unordered_map<std::int64_t, PaperOrderLedgerEntry>& InMemoryLogSink::getPaperLedger() const {
    return paperLedger;
}

const PaperOrderLedgerEntry* InMemoryLogSink::findPaperOrder(std::int64_t orderId) const {
    auto it = paperLedger.find(orderId);
    if (it == paperLedger.end()) {
        return nullptr;
    }
    return &it->second;
}

const std::vector<EventApplyRecord>& InMemoryLogSink::getRejectedStrategyEvents() const {
    return rejectedStrategyEvents;
}

void InMemoryLogSink::applyStrategyFill(std::int64_t orderId, UpdateSource source, PaperOrderFillRole role,
                                        const FillRecord& r) {
    if (source != UpdateSource::STRATEGY) {
        return;
    }
    if (orderId == UnknownOrderIdSentinel) {
        return;
    }

    auto it = paperLedger.find(orderId);
    if (it == paperLedger.end()) {
        return;
    }

    auto& entry = it->second;
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

void InMemoryLogSink::updateStrategyOrder(const EventApplyRecord& r) {
    if (r.source != UpdateSource::STRATEGY) {
        return;
    }

    switch (r.updateType) {
    case UpdateType::ADD: {
        if (r.qtyLots <= 0) {
            rejectedStrategyEvents.push_back(r);
            return;
        }
        if (paperLedger.contains(r.orderId)) {
            rejectedStrategyEvents.push_back(r);
            return;
        }
        PaperOrderLedgerEntry entry{};
        entry.state = PaperOrderState{
            r.orderId, r.side, r.priceTicks, r.qtyLots, r.qtyLots, 0, PaperOrderLedgerStatus::OPEN, r.seq, r.seq};
        paperLedger.emplace(r.orderId, std::move(entry));
        return;
    }
    case UpdateType::SUBTRACT: {
        if (r.qtyLots < 0) {
            rejectedStrategyEvents.push_back(r);
            return;
        }
        auto it = paperLedger.find(r.orderId);
        if (it == paperLedger.end()) {
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
        auto it = paperLedger.find(r.orderId);
        if (it == paperLedger.end()) {
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
        auto it = paperLedger.find(r.orderId);
        if (it == paperLedger.end()) {
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
    case UpdateType::AGGRESSIVE_TRADE:
        return;
    }
}
