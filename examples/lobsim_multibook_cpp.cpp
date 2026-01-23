#include "lobsim/coinbase_btcusdt_adapter.hpp"
#include "lobsim/coinbase_btcusdt_parquet_source.hpp"
#include "lobsim/in_memory_sink.hpp"
#include "lobsim/instrument.hpp"
#include "lobsim/multi_log_sink.hpp"
#include "lobsim/multibook_simulator.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace lobsim;
using namespace lobsim::replay;

InstrumentSpec make_btcusdt_spec() {
    InstrumentSpec spec{};
    spec.symbol = "BTC-USDT";
    spec.venue = "coinbase";
    spec.tick_size = Rational{1, 100};      // $0.01
    spec.lot_size = Rational{1, 100000000}; // 1e-8 BTC
    spec.price_policy = RoundingPolicy::Strict;
    spec.qty_policy = RoundingPolicy::Strict;
    return spec;
}

void print_levels(const std::vector<std::pair<std::int64_t, std::int64_t>>& levels) {
    std::cout << "[";
    for (std::size_t i = 0; i < levels.size(); ++i) {
        std::cout << "(" << levels[i].first << ", " << levels[i].second << ")";
        if (i + 1 < levels.size()) {
            std::cout << ", ";
        }
    }
    std::cout << "]";
}

void dump_fills(const InMemoryMultiLogSink& sink) {
    std::cout << "Total fills: " << sink.fills().size() << "\n";
    for (const auto& f : sink.fills()) {
        std::cout << "  [" << f.book_key << "] seq=" << f.seq << " px=" << f.price_ticks << " qty=" << f.qty_lots
                  << " maker_side=" << static_cast<int>(f.maker_side) << " maker=" << f.maker_order_id
                  << " taker=" << f.taker_order_id << " src m/t=" << static_cast<int>(f.maker_source) << "/"
                  << static_cast<int>(f.taker_source) << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? std::string(argv[1]) : "sample_data/coinbase_btcusdt_sample.parquet";

    MultiBookSimulator sim(MultiBookSimulator::Config{
        .require_monotonic_ts_received = true,
        .fail_fast = false,
    });

    InMemoryMultiLogSink multi_sink;
    sim.set_multi_log_sink(&multi_sink);

    // Register two books (could be different symbols; we reuse BTC-USDT for brevity).
    BookId spot{"coinbase", "BTC-USDT-spot"};
    BookId perp{"coinbase", "BTC-USDT-perp"};
    sim.add_book(spot);
    sim.add_book(perp);

    auto spec = make_btcusdt_spec();
    CoinbaseBTCUSDTAdapter adapter(spec);

    CoinbaseBTCUSDTParquetSource source_spot(path);
    CoinbaseBTCUSDTParquetSource source_perp(path);

    sim.add_stream(spot, source_spot, adapter);
    sim.add_stream(perp, source_perp, adapter);

    std::size_t steps = 0;
    const std::size_t max_steps = 2000;

    while (steps < max_steps && sim.step()) {
        ++steps;
        if (steps % 500 == 0) {
            auto best_bid_spot = sim.get_best_price_ticks(spot, Side::BUY);
            auto best_ask_spot = sim.get_best_price_ticks(spot, Side::SELL);
            auto best_bid_perp = sim.get_best_price_ticks(perp, Side::BUY);
            auto best_ask_perp = sim.get_best_price_ticks(perp, Side::SELL);

            std::cout << "Step " << steps << " @ t=" << sim.current_time().value_or(-1) << "\n";
            std::cout << "  Spot best: bid=" << (best_bid_spot ? std::to_string(*best_bid_spot) : "NA")
                      << " ask=" << (best_ask_spot ? std::to_string(*best_ask_spot) : "NA") << "\n";
            std::cout << "  Perp best: bid=" << (best_bid_perp ? std::to_string(*best_bid_perp) : "NA")
                      << " ask=" << (best_ask_perp ? std::to_string(*best_ask_perp) : "NA") << "\n";

            // Inject a simple strategy order into spot when we sample.
            NormalizedLobEvent strat{};
            strat.ts_exchange = sim.current_time().value_or(0);
            strat.ts_received = strat.ts_exchange + 1; // slightly after feed event
            strat.side = Side::BUY;
            strat.update_type = UpdateType::ADD;
            strat.price_ticks = best_ask_spot.value_or(0);
            strat.quantity_lots = 2;
            strat.order_id = 123456789 + static_cast<std::int64_t>(steps);
            strat.trader_id = 42;
            strat.aggressor_id = UnknownAggressorIdSentinel;
            strat.update_source = UpdateSource::STRATEGY;
            strat.symbol_id = book_key(spot);
            sim.submit_strategy_event(spot, strat);

            // Peek at L2 for both books.
            auto top2_spot = sim.l2_top_n(spot, Side::BUY, 2);
            auto top2_perp = sim.l2_top_n(perp, Side::SELL, 2);
            std::cout << "  Spot top2 bids: ";
            print_levels(top2_spot);
            std::cout << " | Perp top2 asks: ";
            print_levels(top2_perp);
            std::cout << "\n";
        }
    }

    std::cout << "\nReplay finished after " << steps << " steps\n";
    dump_fills(multi_sink);

    return 0;
}
