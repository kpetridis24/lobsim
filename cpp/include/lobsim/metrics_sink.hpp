#pragma once

#include "lobsim/log_sink.hpp"
#include "lobsim/paper_trading_simulator.hpp"
#include <vector>
#include <cmath>
#include <optional>

namespace lobsim {

struct MetricsRecord {
    std::uint64_t seq;
    std::int64_t ts_exchange;
    std::int64_t ts_received;
    std::optional<std::int64_t> best_bid;
    std::optional<std::int64_t> best_ask;
    std::optional<double> mid_price;
    std::optional<std::int64_t> spread;
    std::optional<double> imbalance; // (bid_qty - ask_qty) / (bid_qty + ask_qty)
};

class MetricsSink : public ILogSink {
public:
    explicit MetricsSink(const PaperTradingSimulator& engine);
    ~MetricsSink() override = default;

    void on_fill(const FillRecord& r) override;
    void on_event_apply(const EventApplyRecord& r) override;
    void on_diagnostic(const DiagnosticRecord& r) override;
    void reset() override;

    // Added for testing/compatibility
    const std::vector<DiagnosticRecord>& get_diagnostics() const { return diagnostics_; }

    [[nodiscard]] const std::vector<MetricsRecord>& get_metrics() const;

private:
    const PaperTradingSimulator& engine_;
    std::vector<MetricsRecord> metrics_;
    std::vector<DiagnosticRecord> diagnostics_;
};

} // namespace lobsim
