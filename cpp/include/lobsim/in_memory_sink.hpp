#pragma once
#include "lobsim/log_sink.hpp"

#include <unordered_map>
#include <vector>

enum class PaperOrderLedgerStatus : std::uint8_t {
    OPEN = 0,
    PARTIALLY_FILLED = 1,
    FILLED = 2,
    CANCELLED = 3,
    REJECTED = 4,
};

enum class PaperOrderFillRole : std::uint8_t {
    MAKER = 0,
    TAKER = 1,
};

struct PaperOrderFill {
    std::uint64_t seq;
    std::int64_t tsExchange;
    std::int64_t tsReceived;
    std::int64_t priceTicks;
    std::int64_t qtyLots;
    PaperOrderFillRole role;
};

struct PaperOrderState {
    std::int64_t orderId;
    Side side;
    std::int64_t priceTicks;
    std::int64_t initialQty;
    std::int64_t remainingQty;
    std::int64_t filledQty;
    PaperOrderLedgerStatus status;
    std::uint64_t createdSeq;
    std::uint64_t lastUpdateSeq;
};

struct PaperOrderLedgerEntry {
    PaperOrderState state;
    std::vector<PaperOrderFill> fills;
};

class InMemoryLogSink final : public ILogSink {
public:
    void onFill(const FillRecord& r) override;
    void onEventApply(const EventApplyRecord& r) override;
    void onDiagnostic(const DiagnosticRecord& r) override;
    void reset() override;

    const std::vector<FillRecord>& getFills() const;
    const std::vector<EventApplyRecord>& getEvents() const;
    const std::vector<DiagnosticRecord>& getDiagnostics() const;
    const std::unordered_map<std::int64_t, PaperOrderLedgerEntry>& getPaperLedger() const;
    const PaperOrderLedgerEntry* findPaperOrder(std::int64_t orderId) const;
    const std::vector<EventApplyRecord>& getRejectedStrategyEvents() const;

    std::vector<FillRecord> drainFills();
    std::vector<EventApplyRecord> drainEvents();

private:
    void applyStrategyFill(std::int64_t orderId, UpdateSource source, PaperOrderFillRole role, const FillRecord& r);
    void updateStrategyOrder(const EventApplyRecord& r);

    std::vector<FillRecord> fills;
    std::vector<EventApplyRecord> events;
    std::vector<DiagnosticRecord> diagnostics;
    std::unordered_map<std::int64_t, PaperOrderLedgerEntry> paperLedger;
    std::vector<EventApplyRecord> rejectedStrategyEvents;
};
