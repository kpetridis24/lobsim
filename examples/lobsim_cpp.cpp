#include "lobsim/coinbase_btcusdt_adapter.hpp"
#include "lobsim/coinbase_btcusdt_parquet_source.hpp"
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
    spec.tick_size = lobsim::replay::Rational{1, 100};
    spec.lot_size = lobsim::replay::Rational{1, 100000000};
    // Parquet floats can land slightly off-grid; use nearest for the example.
    spec.price_policy = lobsim::replay::RoundingPolicy::Nearest;
    spec.qty_policy = lobsim::replay::RoundingPolicy::Nearest;
    return spec;
}

void write_fills_csv(const InMemoryLogSink& sink, const std::string& path) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Failed to open output file: " + path);
    }
    out << "seq,ts_exchange,ts_received,price_ticks,qty_lots,maker_side,maker_order_id,maker_trader_id,maker_source,"
           "taker_side,taker_order_id,taker_trader_id,taker_source\n";
    for (const auto& r : sink.get_fills()) {
        out << r.seq << ',' << r.ts_exchange << ',' << r.ts_received << ',' << r.price_ticks << ',' << r.qty_lots << ','
            << static_cast<int>(r.maker_side) << ',' << r.maker_order_id << ',' << r.maker_trader_id << ','
            << static_cast<int>(r.maker_source) << ',' << static_cast<int>(r.taker_side) << ',' << r.taker_order_id
            << ',' << r.taker_trader_id << ',' << static_cast<int>(r.taker_source) << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string path = "../sample_data/coinbase_btcusdt_sample.parquet";
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
        engine.set_log_sink(&sink);

        auto spec = make_btcusdt_spec();
        lobsim::replay::CoinbaseBTCUSDTAdapter adapter(spec);
        lobsim::replay::CoinbaseBTCUSDTParquetSource source(path);

        lobsim::replay::ReplayConfig cfg{};
        cfg.require_monotonic_ts_received = true;
        cfg.fail_fast = true;
        lobsim::replay::ReplaySession replay(engine, cfg);
        using RawEvent = CoinbaseBTCUSDTRawEvent;
        replay.run<lobsim::replay::CoinbaseBTCUSDTParquetSource, lobsim::replay::CoinbaseBTCUSDTAdapter,
                   RawEvent>(source, adapter, cfg);
        write_fills_csv(sink, dump_fills_path);
        return 0;
    }

    {
        // Replay all events using the ReplaySession API
        PaperTradingSimulator engine;
        InMemoryLogSink sink;
        engine.set_log_sink(&sink);

        auto spec = make_btcusdt_spec();
        lobsim::replay::CoinbaseBTCUSDTAdapter adapter(spec);
        lobsim::replay::CoinbaseBTCUSDTParquetSource source(path);

        lobsim::replay::ReplayConfig cfg{.require_monotonic_ts_received = true, .fail_fast = true};
        lobsim::replay::ReplaySession replay(engine, cfg);
        using RawEvent = CoinbaseBTCUSDTRawEvent;
        auto summary = replay.run<lobsim::replay::CoinbaseBTCUSDTParquetSource,
                                  lobsim::replay::CoinbaseBTCUSDTAdapter, RawEvent>(source, adapter, cfg);
        std::cout << "Replay summary: first_ts=" << summary.first_ts_received << " last_ts=" << summary.last_ts_received
                  << " raw=" << summary.num_raw_events << " normalized=" << summary.num_normalized_events
                  << " adapter_failures=" << summary.num_adapter_failures << " num_fills=" << sink.get_fills().size()
                  << "\n";
    }

    {
        PaperTradingSimulator engine;
        InMemoryLogSink sink;
        engine.set_log_sink(&sink);

        auto spec = make_btcusdt_spec();
        lobsim::replay::CoinbaseBTCUSDTAdapter adapter(spec);
        lobsim::replay::CoinbaseBTCUSDTParquetSource source(path);

        CoinbaseBTCUSDTRawEvent raw{};
        std::int64_t i = 0;

        while (source.next(raw)) {
            NormalizedLobEvent ev{};
            if (!adapter.try_normalize(raw, ev)) {
                ++i;
                continue;
            }
            engine.update(ev);

            if (i % 500 == 0) {
                auto top2b = engine.l2_top_n(Side::SELL, 2);
                auto top2a = engine.l2_top_n(Side::BUY, 2);
                auto best_ask = engine.get_best_price_ticks(Side::SELL);

                std::cout << "Top 2 - BID: ";
                print_levels(top2b);
                std::cout << " | ASK: ";
                print_levels(top2a);
                if (best_ask.has_value()) {
                    std::cout << " | best_ask=" << best_ask.value();
                }
                std::cout << "\n";

                NormalizedLobEvent strat{};
                strat.ts_exchange = ev.ts_exchange;
                strat.ts_received = ev.ts_received + 1;
                strat.side = Side::BUY;
                strat.update_type = UpdateType::ADD;
                strat.price_ticks = ev.price_ticks;
                strat.quantity_lots = 2;
                strat.order_id = 123456789;
                strat.trader_id = 123456789;
                strat.aggressor_id = UnknownAggressorIdSentinel;
                strat.update_source = UpdateSource::STRATEGY;
                strat.symbol_id = spec.symbol;
                engine.update(strat);
            }
            ++i;
        }

        std::cout << "num_fills=" << sink.get_fills().size() << "\n";
    }

    return 0;
}
