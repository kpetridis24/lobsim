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
    spec.tickSize = Rational{1, 100};      // $0.01
    spec.lotSize = Rational{1, 100000000}; // 1e-8 BTC
    spec.pricePolicy = RoundingPolicy::Strict;
    spec.qtyPolicy = RoundingPolicy::Strict;
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
        std::cout << "  [" << f.bookKey << "] seq=" << f.seq << " px=" << f.priceTicks << " qty=" << f.qtyLots
                  << " makerSide=" << static_cast<int>(f.makerSide) << " maker=" << f.makerOrderId
                  << " taker=" << f.takerOrderId << " src m/t=" << static_cast<int>(f.makerSource) << "/"
                  << static_cast<int>(f.takerSource) << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? std::string(argv[1]) : "sample_data/coinbase_btcusdt_sample.parquet";

    MultiBookSimulator sim(MultiBookSimulator::Config{
        .requireMonotonicTsReceived = true,
        .failFast = false,
    });

    InMemoryMultiLogSink multiSink;
    sim.setMultiLogSink(&multiSink);

    // Register two books (could be different symbols; we reuse BTC-USDT for brevity).
    BookId spot{"coinbase", "BTC-USDT-spot"};
    BookId perp{"coinbase", "BTC-USDT-perp"};
    sim.addBook(spot);
    sim.addBook(perp);

    auto spec = make_btcusdt_spec();
    CoinbaseBTCUSDTAdapter adapter(spec);

    CoinbaseBTCUSDTParquetSource sourceSpot(path);
    CoinbaseBTCUSDTParquetSource sourcePerp(path);

    sim.addStream(spot, sourceSpot, adapter);
    sim.addStream(perp, sourcePerp, adapter);

    std::size_t steps = 0;
    const std::size_t maxSteps = 2000;

    while (steps < maxSteps && sim.step()) {
        ++steps;
        if (steps % 500 == 0) {
            auto bestBidSpot = sim.getBestPriceTicks(spot, Side::BUY);
            auto bestAskSpot = sim.getBestPriceTicks(spot, Side::SELL);
            auto bestBidPerp = sim.getBestPriceTicks(perp, Side::BUY);
            auto bestAskPerp = sim.getBestPriceTicks(perp, Side::SELL);

            std::cout << "Step " << steps << " @ t=" << sim.currentTime().value_or(-1) << "\n";
            std::cout << "  Spot best: bid=" << (bestBidSpot ? std::to_string(*bestBidSpot) : "NA")
                      << " ask=" << (bestAskSpot ? std::to_string(*bestAskSpot) : "NA") << "\n";
            std::cout << "  Perp best: bid=" << (bestBidPerp ? std::to_string(*bestBidPerp) : "NA")
                      << " ask=" << (bestAskPerp ? std::to_string(*bestAskPerp) : "NA") << "\n";

            // Inject a simple strategy order into spot when we sample.
            NormalizedLobEvent strat{};
            strat.tsExchange = sim.currentTime().value_or(0);
            strat.tsReceived = strat.tsExchange + 1; // slightly after feed event
            strat.side = Side::BUY;
            strat.updateType = UpdateType::ADD;
            strat.priceTicks = bestAskSpot.value_or(0);
            strat.quantityLots = 2;
            strat.orderId = 123456789 + static_cast<std::int64_t>(steps);
            strat.traderId = 42;
            strat.aggressorId = UnknownAggressorIdSentinel;
            strat.updateSource = UpdateSource::STRATEGY;
            strat.symbolId = bookKey(spot);
            sim.submitStrategyEvent(spot, strat);

            // Peek at L2 for both books.
            auto top2Spot = sim.l2TopN(spot, Side::BUY, 2);
            auto top2Perp = sim.l2TopN(perp, Side::SELL, 2);
            std::cout << "  Spot top2 bids: ";
            print_levels(top2Spot);
            std::cout << " | Perp top2 asks: ";
            print_levels(top2Perp);
            std::cout << "\n";
        }
    }

    std::cout << "\nReplay finished after " << steps << " steps\n";
    dump_fills(multiSink);

    return 0;
}
