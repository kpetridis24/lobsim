#include "blocking_queue.hpp"
#include "diagnostic_queue_sink.hpp"
#include "lobsim/paper_trading_simulator.hpp"
#include "normalized_event_parser.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace {
struct Args {
    double rate = 10000.0;
    double duration = 10.0;
    int symbols = 10;
    int seed = 1;
    double cancel_pct = 0.30;
    double market_pct = 0.05;
    double cross_pct = 0.02;
    double mean_offset = 2.0;
    int mid_start = 10000;
    double mid_move_prob = 0.20;
    int mid_move_max = 3;
    int max_qty = 1000;
    double qty_mu = 3.0;
    double qty_sigma = 0.7;
    std::uint64_t max_events = 10'000'000;
    std::uint64_t max_mem_mb = 2048;
    bool header = false;
};

bool parse_flag(const char* arg, const char* name, std::string_view& out) {
    if (std::string_view(arg) == name) {
        out = name;
        return true;
    }
    return false;
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::string_view flag;
        if (parse_flag(argv[i], "--rate", flag) && i + 1 < argc) {
            args.rate = std::stod(argv[++i]);
        } else if (parse_flag(argv[i], "--duration", flag) && i + 1 < argc) {
            args.duration = std::stod(argv[++i]);
        } else if (parse_flag(argv[i], "--symbols", flag) && i + 1 < argc) {
            args.symbols = std::stoi(argv[++i]);
        } else if (parse_flag(argv[i], "--seed", flag) && i + 1 < argc) {
            args.seed = std::stoi(argv[++i]);
        } else if (parse_flag(argv[i], "--cancel-pct", flag) && i + 1 < argc) {
            args.cancel_pct = std::stod(argv[++i]);
        } else if (parse_flag(argv[i], "--market-pct", flag) && i + 1 < argc) {
            args.market_pct = std::stod(argv[++i]);
        } else if (parse_flag(argv[i], "--cross-pct", flag) && i + 1 < argc) {
            args.cross_pct = std::stod(argv[++i]);
        } else if (parse_flag(argv[i], "--mean-offset", flag) && i + 1 < argc) {
            args.mean_offset = std::stod(argv[++i]);
        } else if (parse_flag(argv[i], "--mid-start", flag) && i + 1 < argc) {
            args.mid_start = std::stoi(argv[++i]);
        } else if (parse_flag(argv[i], "--mid-move-prob", flag) && i + 1 < argc) {
            args.mid_move_prob = std::stod(argv[++i]);
        } else if (parse_flag(argv[i], "--mid-move-max", flag) && i + 1 < argc) {
            args.mid_move_max = std::stoi(argv[++i]);
        } else if (parse_flag(argv[i], "--max-qty", flag) && i + 1 < argc) {
            args.max_qty = std::stoi(argv[++i]);
        } else if (parse_flag(argv[i], "--qty-mu", flag) && i + 1 < argc) {
            args.qty_mu = std::stod(argv[++i]);
        } else if (parse_flag(argv[i], "--qty-sigma", flag) && i + 1 < argc) {
            args.qty_sigma = std::stod(argv[++i]);
        } else if (parse_flag(argv[i], "--max-events", flag) && i + 1 < argc) {
            args.max_events = static_cast<std::uint64_t>(std::stoull(argv[++i]));
        } else if (parse_flag(argv[i], "--max-mem-mb", flag) && i + 1 < argc) {
            args.max_mem_mb = static_cast<std::uint64_t>(std::stoull(argv[++i]));
        } else if (parse_flag(argv[i], "--header", flag)) {
            args.header = true;
        }
    }
    return args;
}

struct LiveOrders {
    std::vector<std::int64_t> ids;
    std::unordered_map<std::int64_t, std::size_t> index;
    std::unordered_map<std::int64_t, std::int64_t> trader_by_id;
    std::unordered_map<std::int64_t, std::int64_t> qty_by_id;
    std::unordered_map<std::int64_t, std::int64_t> price_by_id;
    std::unordered_map<std::int64_t, std::uint8_t> side_by_id;
    std::unordered_map<std::int64_t, std::size_t> symbol_by_id;

    void add(std::int64_t order_id, std::int64_t trader_id, std::int64_t price_ticks, std::int64_t qty_lots,
             std::uint8_t side, std::size_t symbol_idx) {
        index[order_id] = ids.size();
        ids.push_back(order_id);
        trader_by_id[order_id] = trader_id;
        qty_by_id[order_id] = qty_lots;
        price_by_id[order_id] = price_ticks;
        side_by_id[order_id] = side;
        symbol_by_id[order_id] = symbol_idx;
    }

    bool empty() const { return ids.empty(); }

    std::int64_t pick(std::mt19937_64& rng) const {
        std::uniform_int_distribution<std::size_t> dist(0, ids.size() - 1);
        return ids[dist(rng)];
    }

    void remove(std::int64_t order_id) {
        auto it = index.find(order_id);
        if (it == index.end()) {
            return;
        }
        std::size_t idx = it->second;
        std::int64_t last = ids.back();
        ids.pop_back();
        if (idx < ids.size()) {
            ids[idx] = last;
            index[last] = idx;
        }
        index.erase(it);
        trader_by_id.erase(order_id);
        qty_by_id.erase(order_id);
        price_by_id.erase(order_id);
        side_by_id.erase(order_id);
        symbol_by_id.erase(order_id);
    }
};

int sample_offset(std::mt19937_64& rng, double mean_offset) {
    if (mean_offset <= 0.0) {
        return 1;
    }
    std::exponential_distribution<double> dist(1.0 / mean_offset);
    int off = static_cast<int>(dist(rng));
    return std::max(1, off);
}

int sample_qty(std::mt19937_64& rng, double mu, double sigma, int max_qty) {
    std::lognormal_distribution<double> dist(mu, sigma);
    int qty = static_cast<int>(dist(rng));
    if (qty < 1) {
        qty = 1;
    } else if (qty > max_qty) {
        qty = max_qty;
    }
    return qty;
}

struct ThreadUsage {
    long utime_us = -1;
    long stime_us = -1;
};

ThreadUsage read_thread_usage() {
    ThreadUsage usage{};
#if defined(__linux__)
    rusage ru{};
    if (getrusage(RUSAGE_THREAD, &ru) == 0) {
        usage.utime_us = static_cast<long>(ru.ru_utime.tv_sec) * 1000000L + ru.ru_utime.tv_usec;
        usage.stime_us = static_cast<long>(ru.ru_stime.tv_sec) * 1000000L + ru.ru_stime.tv_usec;
    }
#endif
    return usage;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    const bool metrics_enabled = std::getenv("METRICS") != nullptr;

    std::cout << "Generating events..." << std::endl;
    std::mt19937_64 rng(static_cast<std::uint64_t>(args.seed));

    std::vector<std::string> symbols;
    symbols.reserve(args.symbols);
    for (int i = 0; i < args.symbols; ++i) {
        symbols.push_back("SYM" + std::to_string(i));
    }

    std::vector<double> weights;
    weights.reserve(args.symbols);
    double sum = 0.0;
    for (int i = 0; i < args.symbols; ++i) {
        double w = 1.0 / (i + 1.0);
        weights.push_back(w);
        sum += w;
    }
    for (double& w : weights) {
        w /= sum;
    }
    std::vector<double> prefix;
    prefix.reserve(weights.size());
    double acc = 0.0;
    for (double w : weights) {
        acc += w;
        prefix.push_back(acc);
    }
    std::vector<int> mids(args.symbols, args.mid_start);
    LiveOrders live;

    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::uniform_int_distribution<int> trader_dist(1, 64);

    std::int64_t next_order_id = 1;
    std::uint64_t intended = static_cast<std::uint64_t>(args.rate * args.duration);
    std::uint64_t max_events = args.max_events > 0 ? args.max_events : intended;
    std::uint64_t cap = std::min(intended, max_events);

    const std::uint64_t max_bytes =
        args.max_mem_mb > 0 ? args.max_mem_mb * 1024ULL * 1024ULL : std::numeric_limits<std::uint64_t>::max();
    std::uint64_t total_bytes = 0;

    std::vector<std::string> events;
    events.reserve(static_cast<std::size_t>(cap));

    auto append_int = [](std::string& out, std::int64_t value) {
        char buf[32];
        auto res = std::to_chars(buf, buf + sizeof(buf), value);
        out.append(buf, static_cast<std::size_t>(res.ptr - buf));
    };

    std::string line;
    line.reserve(192);
    const auto gen_start = std::chrono::steady_clock::now();
    bool truncated = false;

    if (args.header) {
        std::string header = "ts_exchange,ts_received,side,update_type,price_ticks,quantity_lots,order_id,trader_id,"
                             "aggressor_id,update_source,symbol_id";
        total_bytes += header.size();
        events.push_back(std::move(header));
    }

    for (std::uint64_t i = 0; i < cap; ++i) {
        std::int64_t ts = static_cast<std::int64_t>(i);
        bool do_cancel = (uni(rng) < args.cancel_pct) && !live.empty();
        bool do_market = (!do_cancel) && (uni(rng) < args.market_pct) && !live.empty();

        std::int64_t price_ticks = 0;
        std::int64_t qty = 0;
        std::int64_t order_id = 0;
        std::int64_t trader_id = 0;
        std::uint8_t side = 0;
        std::uint8_t update_type = static_cast<std::uint8_t>(UpdateType::ADD);
        std::uint8_t update_source = static_cast<std::uint8_t>(UpdateSource::HISTORICAL);
        std::string_view symbol = "SYM0";

        if (do_cancel) {
            order_id = live.pick(rng);
            side = live.side_by_id[order_id];
            update_type = static_cast<std::uint8_t>(UpdateType::DELETE);
            price_ticks = live.price_by_id[order_id];
            qty = 0;
            trader_id = live.trader_by_id[order_id];
            symbol = symbols[live.symbol_by_id[order_id]];
            live.remove(order_id);
        } else if (do_market) {
            order_id = live.pick(rng);
            side = live.side_by_id[order_id];
            update_type = static_cast<std::uint8_t>(UpdateType::MATCH);
            price_ticks = live.price_by_id[order_id];
            qty = sample_qty(rng, args.qty_mu, args.qty_sigma, args.max_qty);
            qty = std::min<std::int64_t>(qty, live.qty_by_id[order_id]);
            if (qty <= 0) {
                qty = 1;
            }
            trader_id = live.trader_by_id[order_id];
            symbol = symbols[live.symbol_by_id[order_id]];
            live.qty_by_id[order_id] -= qty;
            if (live.qty_by_id[order_id] <= 0) {
                live.remove(order_id);
            }
        } else {
            double r = uni(rng);
            auto it = std::lower_bound(prefix.begin(), prefix.end(), r);
            std::size_t sym_idx = static_cast<std::size_t>(
                std::max<std::ptrdiff_t>(0, std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(prefix.size() - 1),
                                                                     std::distance(prefix.begin(), it))));

            if (uni(rng) < args.mid_move_prob) {
                int step = std::max(1, args.mid_move_max);
                std::uniform_int_distribution<int> step_dist(1, step);
                int delta = step_dist(rng);
                mids[sym_idx] = std::max(1, mids[sym_idx] + (uni(rng) < 0.5 ? -delta : delta));
            }

            int mid = mids[sym_idx];
            side = uni(rng) < 0.5 ? 1 : 0;
            qty = sample_qty(rng, args.qty_mu, args.qty_sigma, args.max_qty);
            trader_id = trader_dist(rng);
            int offset = sample_offset(rng, args.mean_offset);
            if (uni(rng) < args.cross_pct) {
                price_ticks = side == 1 ? mid + offset : std::max(1, mid - offset);
            } else {
                price_ticks = side == 1 ? std::max(1, mid - offset) : mid + offset;
            }
            order_id = next_order_id++;
            symbol = symbols[sym_idx];
            live.add(order_id, trader_id, price_ticks, qty, side, sym_idx);
        }

        line.clear();
        append_int(line, ts);
        line.push_back(',');
        append_int(line, ts);
        line.push_back(',');
        append_int(line, side);
        line.push_back(',');
        append_int(line, update_type);
        line.push_back(',');
        append_int(line, price_ticks);
        line.push_back(',');
        append_int(line, qty);
        line.push_back(',');
        append_int(line, order_id);
        line.push_back(',');
        append_int(line, trader_id);
        line.push_back(',');
        append_int(line, -1);
        line.push_back(',');
        append_int(line, update_source);
        line.push_back(',');
        line.append(symbol);

        total_bytes += line.size();
        if (total_bytes > max_bytes) {
            truncated = true;
            break;
        }
        events.push_back(line);
    }

    auto gen_elapsed =
        std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - gen_start).count();
    std::cout << "Generated " << events.size() << " events in " << gen_elapsed << "s";
    if (truncated) {
        std::cout << " (truncated by memory cap)";
    }
    std::cout << std::endl;

    constexpr std::size_t kInboundQueueCapacity = 1'000'000;
    constexpr std::size_t kDiagnosticsQueueCapacity = 4096;
    BlockingQueue<std::string> inbound(kInboundQueueCapacity);
    BlockingQueue<DiagnosticRecord> diagnostics(kDiagnosticsQueueCapacity);

    DiagnosticQueueSink sink(&diagnostics);
    PaperTradingSimulator engine;
    engine.set_log_sink(&sink);

    ThreadUsage input_usage{};
    ThreadUsage processing_usage{};
    ThreadUsage diagnostics_usage{};

    std::int64_t replay_sent = 0;
    std::int64_t replay_elapsed_ns = 0;
    std::int64_t processing_elapsed_ns = 0;
    std::uint64_t parse_failures = 0;

    std::atomic<bool> shutdown_requested{false};

    auto input_thread_job = [&] {
        using clock = std::chrono::steady_clock;
        std::int64_t sent = 0;
        const auto start = clock::now();
        const std::int64_t total = static_cast<std::int64_t>(events.size());
        while (sent < total) {
            auto now = clock::now();
            double elapsed = std::chrono::duration<double>(now - start).count();
            std::int64_t target = args.rate > 0.0 ? static_cast<std::int64_t>(elapsed * args.rate) : total;
            if (sent >= target) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                continue;
            }
            std::int64_t batch = std::min<std::int64_t>(target - sent, 4096);
            for (std::int64_t i = 0; i < batch && sent < total; ++i, ++sent) {
                inbound.push(std::move(events[static_cast<std::size_t>(sent)]));
            }
        }
        replay_sent = sent;
        replay_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
        inbound.close();
        input_usage = read_thread_usage();
    };

    auto processing_thread_job = [&] {
        using clock = std::chrono::steady_clock;
        auto proc_start = clock::time_point{};
        bool proc_started = false;
        while (auto raw_event = inbound.pop()) {
            if (!proc_started) {
                proc_start = clock::now();
                proc_started = true;
            }
            if (raw_event->rfind("ts_exchange", 0) == 0) {
                continue;
            }
            auto event = parsing::parse_normalized_event(*raw_event);
            if (event) {
                engine.update(*event);
            } else if (metrics_enabled) {
                ++parse_failures;
            }
        }
        if (proc_started) {
            processing_elapsed_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - proc_start).count();
        }
        diagnostics.close();
        processing_usage = read_thread_usage();
    };

    auto diagnostics_thread_job = [&] {
        std::uint64_t diag_count = 0;
        while (auto diag = diagnostics.pop()) {
            (void)diag;
            ++diag_count;
        }
        if (metrics_enabled) {
            std::cerr << "DIAGNOSTICS total=" << diag_count << "\n";
        }
        diagnostics_usage = read_thread_usage();
    };

    std::thread input_thread(input_thread_job);
    std::thread processing_thread(processing_thread_job);
    std::thread diagnostics_thread(diagnostics_thread_job);

    input_thread.join();
    processing_thread.join();
    diagnostics_thread.join();

    if (metrics_enabled) {
        const double replay_elapsed_sec = replay_elapsed_ns > 0 ? static_cast<double>(replay_elapsed_ns) / 1e9 : 0.0;
        const double replay_rate = replay_elapsed_sec > 0 ? static_cast<double>(replay_sent) / replay_elapsed_sec : 0.0;
        const double proc_elapsed_sec =
            processing_elapsed_ns > 0 ? static_cast<double>(processing_elapsed_ns) / 1e9 : 0.0;
        const double proc_rate = proc_elapsed_sec > 0 ? static_cast<double>(replay_sent) / proc_elapsed_sec : 0.0;
        auto in_stats = inbound.snapshot();
        auto diag_stats = diagnostics.snapshot();
        std::cerr << "METRICS "
                  << "pregen_count=" << events.size() << " pregen_elapsed_sec=" << gen_elapsed
                  << " replay_sent=" << replay_sent << " replay_elapsed_sec=" << replay_elapsed_sec
                  << " replay_rate=" << replay_rate << " proc_elapsed_sec=" << proc_elapsed_sec
                  << " proc_rate=" << proc_rate << " parse_failures=" << parse_failures
                  << " in_pushes=" << in_stats.pushes << " in_pops=" << in_stats.pops
                  << " in_dropped=" << in_stats.push_dropped << " in_max_depth=" << in_stats.max_depth
                  << " in_cur_depth=" << in_stats.current_depth << " diag_pushes=" << diag_stats.pushes
                  << " diag_pops=" << diag_stats.pops;
        if (input_usage.utime_us >= 0) {
            std::cerr << " input_utime_us=" << input_usage.utime_us << " input_stime_us=" << input_usage.stime_us
                      << " processing_utime_us=" << processing_usage.utime_us
                      << " processing_stime_us=" << processing_usage.stime_us
                      << " diagnostics_utime_us=" << diagnostics_usage.utime_us
                      << " diagnostics_stime_us=" << diagnostics_usage.stime_us;
        }
        std::cerr << "\n";
    }

    return 0;
}
