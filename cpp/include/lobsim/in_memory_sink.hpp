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
    std::int64_t ts_exchange;
    std::int64_t ts_received;
    std::int64_t price_ticks;
    std::int64_t qty_lots;
    PaperOrderFillRole role;
};

struct PaperOrderState {
    std::int64_t order_id;
    Side side;
    std::int64_t price_ticks;
    std::int64_t initial_qty;
    std::int64_t remaining_qty;
    std::int64_t filled_qty;
    PaperOrderLedgerStatus status;
    std::uint64_t created_seq;
    std::uint64_t last_update_seq;
};

struct PaperOrderLedgerEntry {
    PaperOrderState state;
    std::vector<PaperOrderFill> fills;
};

class InMemoryLogSink final : public ILogSink {
public:
    void on_fill(const FillRecord& r) override;
    void on_event_apply(const EventApplyRecord& r) override;
    void on_diagnostic(const DiagnosticRecord& r) override;
    void reset() override;

    const std::vector<FillRecord>& get_fills() const;
    const std::vector<EventApplyRecord>& get_events() const;
    const std::vector<DiagnosticRecord>& get_diagnostics() const;
    const std::unordered_map<std::int64_t, PaperOrderLedgerEntry>& get_paper_ledger() const;
    const PaperOrderLedgerEntry* find_paper_order(std::int64_t order_id) const;
    const std::vector<EventApplyRecord>& get_rejected_strategy_events() const;

    std::vector<FillRecord> drain_fills();
    std::vector<EventApplyRecord> drain_events();

private:
    void apply_strategy_fill(std::int64_t order_id, UpdateSource source, PaperOrderFillRole role, const FillRecord& r);
    void update_strategy_order(const EventApplyRecord& r);

    std::vector<FillRecord> fills;
    std::vector<EventApplyRecord> events;
    std::vector<DiagnosticRecord> diagnostics;
    std::unordered_map<std::int64_t, PaperOrderLedgerEntry> paper_ledger;
    std::vector<EventApplyRecord> rejected_strategy_events;
};
