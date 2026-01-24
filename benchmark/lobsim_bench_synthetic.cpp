#include "lobsim/log_sink.hpp"
#include "lobsim/paper_trading_simulator.hpp"
#include "lobsim/lob_event.hpp"
#include "lobsim/types.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>
#include <random>
#include <iomanip>

struct CountingSink final : public ILogSink {
    void on_fill(const FillRecord&) override { ++fills; }
    void on_event_apply(const EventApplyRecord&) override { ++events; }
    void on_diagnostic(const DiagnosticRecord&) override { ++diagnostics; }
    void reset() override {
        fills = 0;
        events = 0;
        diagnostics = 0;
    }

    std::uint64_t fills{0};
    std::uint64_t events{0};
    std::uint64_t diagnostics{0};
};

int main(int argc, char** argv) {
    std::int64_t num_events = 1'000'000;
    if (argc > 1) {
        num_events = std::stoll(argv[1]);
    }

    std::cout << "Running synthetic benchmark with " << num_events << " events...\n";

    PaperTradingSimulator engine;
    auto sink = std::make_shared<CountingSink>();
    engine.set_log_sink(sink);

    // Pre-generate events to measure engine speed only
    std::vector<NormalizedLobEvent> events;
    events.reserve(num_events);

    std::mt19937 rng(42);
    std::uniform_int_distribution<std::int64_t> price_dist(90000, 110000);
    std::uniform_int_distribution<std::int64_t> qty_dist(1, 100);
    std::uniform_int_distribution<int> side_dist(0, 1);
    
    std::uint64_t order_id_seq = 1;

    for (std::int64_t i = 0; i < num_events; ++i) {
        NormalizedLobEvent ev{};
        ev.ts_exchange = i * 1000;
        ev.ts_received = i * 1000;
        ev.side = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
        ev.update_type = UpdateType::ADD;
        ev.price_ticks = price_dist(rng);
        ev.quantity_lots = qty_dist(rng);
        ev.order_id = order_id_seq++;
        ev.update_source = UpdateSource::HISTORICAL;
        ev.symbol_id = "BENCH";
        
        events.push_back(ev);
    }

    auto start = std::chrono::steady_clock::now();

    for (const auto& ev : events) {
        engine.update(ev);
    }

    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> diff = end - start;

    double events_per_sec = num_events / diff.count();

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "benchmark=synthetic_cpp\n";
    std::cout << "events=" << num_events << "\n";
    std::cout << "wall_seconds=" << diff.count() << "\n";
    std::cout << "events_per_sec=" << events_per_sec << "\n";
    std::cout << "fill_count=" << sink->fills << "\n";
    std::cout << "event_apply_count=" << sink->events << "\n";
    std::cout << "diagnostic_count=" << sink->diagnostics << "\n";

    return 0;
}
