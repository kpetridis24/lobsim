#include "lobsim/coinapi_coinbase_btcusdt_adapter.hpp"
#include "lobsim/coinapi_coinbase_btcusdt_parquet_source.hpp"
#include "lobsim/in_memory_sink.hpp"
#include "lobsim/instrument.hpp"
#include "lobsim/lob_event.hpp"
#include "lobsim/paper_trading_simulator.hpp"
#include "lobsim/replay_session.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

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

lobsim::replay::InstrumentSpec make_btcusdt_spec() {
    lobsim::replay::InstrumentSpec spec{};
    spec.symbol = "BTC-USDT";
    spec.venue = "coinbase";
    spec.tickSize = lobsim::replay::Rational{1, 100};
    spec.lotSize = lobsim::replay::Rational{1, 100000000};
    // Parquet floats can land slightly off-grid; use nearest for the example.
    spec.pricePolicy = lobsim::replay::RoundingPolicy::Nearest;
    spec.qtyPolicy = lobsim::replay::RoundingPolicy::Nearest;
    return spec;
}

void write_fills_csv(const InMemoryLogSink& sink, const std::string& path) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Failed to open output file: " + path);
    }
    out << "seq,ts_exchange,ts_received,price_ticks,qty_lots,maker_side,maker_order_id,maker_trader_id,maker_source,"
           "taker_side,taker_order_id,taker_trader_id,taker_source\n";
    for (const auto& r : sink.getFills()) {
        out << r.seq << ',' << r.tsExchange << ',' << r.tsReceived << ',' << r.priceTicks << ',' << r.qtyLots << ','
            << static_cast<int>(r.makerSide) << ',' << r.makerOrderId << ',' << r.makerTraderId << ','
            << static_cast<int>(r.makerSource) << ',' << static_cast<int>(r.takerSide) << ',' << r.takerOrderId
            << ',' << r.takerTraderId << ',' << static_cast<int>(r.takerSource) << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string path = "../sample_data/coinapi_coinbase_btcusdt_sample.parquet";
    std::string dump_fills_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dump-fills") {
            if (i + 1 >= argc) {
                std::cerr << "Missing path after --dump-fills\n";
                return 2;
            }
            dump_fills_path = argv[++i];
            continue;
        }
        path = std::move(arg);
    }

    if (!dump_fills_path.empty()) {
        PaperTradingSimulator engine;
        InMemoryLogSink sink;
        engine.setLogSink(&sink);

        auto spec = make_btcusdt_spec();
        lobsim::replay::CoinapiCoinbaseBTCUSDTAdapter adapter(spec);
        lobsim::replay::CoinapiCoinbaseBTCUSDTParquetSource source(path);

        lobsim::replay::ReplayConfig cfg{};
        cfg.requireMonotonicTsReceived = true;
        cfg.failFast = true;
        lobsim::replay::ReplaySession replay(engine, cfg);
        using RawEvent = CoinapiCoinbaseBTCUSDTRawEvent;
        replay.run<lobsim::replay::CoinapiCoinbaseBTCUSDTParquetSource, lobsim::replay::CoinapiCoinbaseBTCUSDTAdapter,
                   RawEvent>(source, adapter, cfg);
        write_fills_csv(sink, dump_fills_path);
        return 0;
    }

    {
        // Replay all events using the ReplaySession API
        PaperTradingSimulator engine;
        InMemoryLogSink sink;
        engine.setLogSink(&sink);

        auto spec = make_btcusdt_spec();
        lobsim::replay::CoinapiCoinbaseBTCUSDTAdapter adapter(spec);
        lobsim::replay::CoinapiCoinbaseBTCUSDTParquetSource source(path);

        lobsim::replay::ReplayConfig cfg{.requireMonotonicTsReceived = true, .failFast = true};
        lobsim::replay::ReplaySession replay(engine, cfg);
        using RawEvent = CoinapiCoinbaseBTCUSDTRawEvent;
        auto summary = replay.run<lobsim::replay::CoinapiCoinbaseBTCUSDTParquetSource,
                                  lobsim::replay::CoinapiCoinbaseBTCUSDTAdapter, RawEvent>(source, adapter, cfg);
        std::cout << "Replay summary: first_ts=" << summary.firstTsReceived << " last_ts=" << summary.lastTsReceived
                  << " raw=" << summary.numRawEvents << " normalized=" << summary.numNormalizedEvents
                  << " adapter_failures=" << summary.numAdapterFailures << " num_fills=" << sink.getFills().size()
                  << "\n";
    }

    {
        PaperTradingSimulator engine;
        InMemoryLogSink sink;
        engine.setLogSink(&sink);

        auto spec = make_btcusdt_spec();
        lobsim::replay::CoinapiCoinbaseBTCUSDTAdapter adapter(spec);
        lobsim::replay::CoinapiCoinbaseBTCUSDTParquetSource source(path);

        CoinapiCoinbaseBTCUSDTRawEvent raw{};
        std::int64_t i = 0;

        while (source.next(raw)) {
            NormalizedLobEvent ev{};
            if (!adapter.tryNormalize(raw, ev)) {
                ++i;
                continue;
            }
            engine.update(ev);

            if (i % 500 == 0) {
                auto top2b = engine.l2TopN(Side::SELL, 2);
                auto top2a = engine.l2TopN(Side::BUY, 2);
                auto bestAsk = engine.getBestPriceTicks(Side::SELL);

                std::cout << "Top 2 - BID: ";
                print_levels(top2b);
                std::cout << " | ASK: ";
                print_levels(top2a);
                if (bestAsk.has_value()) {
                    std::cout << " | best_ask=" << bestAsk.value();
                }
                std::cout << "\n";

                NormalizedLobEvent strat{};
                strat.tsExchange = ev.tsExchange;
                strat.tsReceived = ev.tsReceived + 1;
                strat.side = Side::BUY;
                strat.updateType = UpdateType::ADD;
                strat.priceTicks = ev.priceTicks;
                strat.quantityLots = 2;
                strat.orderId = 123456789;
                strat.traderId = 123456789;
                strat.aggressorId = UnknownAggressorIdSentinel;
                strat.updateSource = UpdateSource::STRATEGY;
                strat.symbolId = spec.symbol;
                engine.update(strat);
            }
            ++i;
        }

        std::cout << "num_fills=" << sink.getFills().size() << "\n";
    }

    return 0;
}
