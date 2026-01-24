#include "lobsim/metrics_sink.hpp"

namespace lobsim {

MetricsSink::MetricsSink(const PaperTradingSimulator& engine) : engine_(engine) {}

void MetricsSink::on_fill(const FillRecord&) {
    // Metrics are updated on event apply to capture the state after the full transition
}

void MetricsSink::on_event_apply(const EventApplyRecord& r) {
    MetricsRecord metric;
    metric.seq = r.seq;
    metric.ts_exchange = r.ts_exchange;
    metric.ts_received = r.ts_received;

    auto top_bids = engine_.l2_top_n(Side::BUY, 1);
    auto top_asks = engine_.l2_top_n(Side::SELL, 1);

    if (!top_bids.empty()) {
        metric.best_bid = top_bids[0].first;
    }
    if (!top_asks.empty()) {
        metric.best_ask = top_asks[0].first;
    }

    if (metric.best_bid && metric.best_ask) {
        metric.spread = *metric.best_ask - *metric.best_bid;
        metric.mid_price = (static_cast<double>(*metric.best_bid) + static_cast<double>(*metric.best_ask)) / 2.0;

        double bid_qty = static_cast<double>(top_bids[0].second);
        double ask_qty = static_cast<double>(top_asks[0].second);
        double total_qty = bid_qty + ask_qty;
        
        if (total_qty > 0) {
            metric.imbalance = (bid_qty - ask_qty) / total_qty;
        }
    }

    metrics_.push_back(metric);
}

void MetricsSink::on_diagnostic(const DiagnosticRecord& r) {
    diagnostics_.push_back(r);
}

void MetricsSink::reset() {
    metrics_.clear();
    diagnostics_.clear();
}

const std::vector<MetricsRecord>& MetricsSink::get_metrics() const {
    return metrics_;
}

} // namespace lobsim
