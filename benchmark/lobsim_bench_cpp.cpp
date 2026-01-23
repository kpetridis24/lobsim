#include "lobsim/coinbase_btcusdt_adapter.hpp"
#include "lobsim/coinbase_btcusdt_parquet_source.hpp"
#include "lobsim/instrument.hpp"
#include "lobsim/log_sink.hpp"
#include "lobsim/paper_trading_simulator.hpp"
#include "lobsim/replay_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <utility>

namespace {

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

struct Options {
    std::string path{"sample_data/coinbase_btcusdt_sample.parquet"};
    std::string tick_size{"0.01"};
    std::string lot_size{"0.00000001"};
    std::string symbol{"BTC-USDT"};
    std::int64_t maxEvents{0};
    bool useSink{true};
};

void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [--path FILE] [--tick-size X] [--lot-size X] [--symbol SYMBOL]\n"
              << "       [--max-events N] [--no-sink]\n";
}

std::int64_t pow10i(int exp) {
    std::int64_t result = 1;
    for (int i = 0; i < exp; ++i) {
        if (result > (std::numeric_limits<std::int64_t>::max() / 10)) {
            throw std::runtime_error("Decimal scale too large.");
        }
        result *= 10;
    }
    return result;
}

lobsim::replay::Rational parseDecimal(std::string_view s) {
    if (s.empty()) {
        throw std::runtime_error("Empty decimal string.");
    }
    std::string str(s);
    auto pos = str.find('.');
    std::int64_t num = 0;
    std::int64_t den = 1;
    if (pos == std::string::npos) {
        num = std::stoll(str);
    } else {
        const int fracDigits = static_cast<int>(str.size() - pos - 1);
        std::string digits = str;
        digits.erase(pos, 1);
        num = std::stoll(digits);
        den = pow10i(fracDigits);
    }

    if (num <= 0 || den <= 0) {
        throw std::runtime_error("Tick/lot size must be positive.");
    }

    const auto g = std::gcd(num, den);
    num /= g;
    den /= g;
    return lobsim::replay::Rational{num, den};
}

Options parseOptions(int argc, char** argv) {
    Options opt{};
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto nextValue = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("Missing value for ") + name);
            }
            return argv[++i];
        };

        if (arg == "--path") {
            opt.path = nextValue("--path");
        } else if (arg == "--tick-size") {
            opt.tick_size = nextValue("--tick-size");
        } else if (arg == "--lot-size") {
            opt.lot_size = nextValue("--lot-size");
        } else if (arg == "--symbol") {
            opt.symbol = nextValue("--symbol");
        } else if (arg == "--max-events") {
            opt.maxEvents = std::stoll(nextValue("--max-events"));
        } else if (arg == "--no-sink") {
            opt.useSink = false;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (arg.rfind("-", 0) == 0) {
            throw std::runtime_error("Unknown option: " + arg);
        } else {
            opt.path = arg;
        }
    }
    return opt;
}

double timeDiff(const timeval& a, const timeval& b) {
    return static_cast<double>(b.tv_sec - a.tv_sec) + (static_cast<double>(b.tv_usec - a.tv_usec) / 1'000'000.0);
}

std::uint64_t maxRssBytes(const rusage& usage) {
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ULL;
#endif
}

} // namespace

int main(int argc, char** argv) {
    Options opt{};
    try {
        opt = parseOptions(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n";
        printUsage(argv[0]);
        return 2;
    }

    PaperTradingSimulator engine;
    CountingSink sink;
    if (opt.useSink) {
        engine.set_log_sink(&sink);
    }

    lobsim::replay::InstrumentSpec spec{};
    spec.symbol = opt.symbol;
    spec.venue = "coinbase";
    spec.tick_size = parseDecimal(opt.tick_size);
    spec.lot_size = parseDecimal(opt.lot_size);
    spec.price_policy = lobsim::replay::RoundingPolicy::Nearest;
    spec.qty_policy = lobsim::replay::RoundingPolicy::Nearest;

    lobsim::replay::CoinbaseBTCUSDTAdapter adapter(spec);
    lobsim::replay::CoinbaseBTCUSDTParquetSource source(opt.path);
    lobsim::replay::ReplayConfig cfg{};
    cfg.require_monotonic_ts_received = true;
    cfg.fail_fast = true;
    lobsim::replay::ReplaySession replay(engine, cfg);

    CoinbaseBTCUSDTRawEvent raw{};
    lobsim::replay::RunSummary summary{};

    bool hasRange = false;

    rusage usageStart{};
    getrusage(RUSAGE_SELF, &usageStart);
    const auto wallStart = std::chrono::steady_clock::now();

    while (source.next(raw)) {
        if (opt.maxEvents > 0 && summary.num_raw_events >= static_cast<std::uint64_t>(opt.maxEvents)) {
            break;
        }
        ++summary.num_raw_events;
        NormalizedLobEvent ev{};

        try {
            ev = adapter.normalize(raw);
        } catch (const std::exception&) {
            ++summary.num_adapter_failures;
            if (cfg.fail_fast) {
                throw;
            }
            continue;
        }

        ++summary.num_normalized_events;
        if (!hasRange) {
            hasRange = true;
            summary.has_ts_range = true;
            summary.first_ts_received = ev.ts_received;
            summary.last_ts_received = ev.ts_received;
        } else {
            summary.first_ts_received = std::min(summary.first_ts_received, ev.ts_received);
            summary.last_ts_received = std::max(summary.last_ts_received, ev.ts_received);
        }

        replay.step(ev);
        ++summary.num_engine_updates;
    }

    const auto wallEnd = std::chrono::steady_clock::now();
    rusage usageEnd{};
    getrusage(RUSAGE_SELF, &usageEnd);

    const double wallSeconds = std::chrono::duration<double>(wallEnd - wallStart).count();
    const double cpuSeconds = timeDiff(usageStart.ru_utime, usageEnd.ru_utime) + timeDiff(usageStart.ru_stime, usageEnd.ru_stime);
    const std::uint64_t rssBytes = maxRssBytes(usageEnd);
    const double eventsPerSec = wallSeconds > 0.0 ? (static_cast<double>(summary.num_engine_updates) / wallSeconds) : 0.0;
    const double rawPerSec = wallSeconds > 0.0 ? (static_cast<double>(summary.num_raw_events) / wallSeconds) : 0.0;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "benchmark=c++\n";
    std::cout << "raw_events=" << summary.num_raw_events << "\n";
    std::cout << "normalized_events=" << summary.num_normalized_events << "\n";
    std::cout << "engine_updates=" << summary.num_engine_updates << "\n";
    std::cout << "adapter_failures=" << summary.num_adapter_failures << "\n";
    std::cout << "wall_seconds=" << wallSeconds << "\n";
    std::cout << "cpu_seconds=" << cpuSeconds << "\n";
    std::cout << "cpu_utilization=" << (wallSeconds > 0.0 ? (cpuSeconds / wallSeconds) : 0.0) << "\n";
    std::cout << "events_per_sec=" << eventsPerSec << "\n";
    std::cout << "raw_events_per_sec=" << rawPerSec << "\n";
    std::cout << "max_rss_bytes=" << rssBytes << "\n";

    if (opt.useSink) {
        std::cout << "fill_count=" << sink.fills << "\n";
        std::cout << "event_apply_count=" << sink.events << "\n";
        std::cout << "diagnostic_count=" << sink.diagnostics << "\n";
    } else {
        std::cout << "fill_count=0\n";
        std::cout << "event_apply_count=0\n";
        std::cout << "diagnostic_count=0\n";
    }

    return 0;
}
