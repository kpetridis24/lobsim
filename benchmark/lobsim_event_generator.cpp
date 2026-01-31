#include <algorithm>
#include <arpa/inet.h>
#include <netdb.h>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <random>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct Args {
    std::string host = "127.0.0.1";
    int port = 1234;
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
    std::string stats_out;
    bool header = false;
    std::size_t batch_lines = 128;
    std::size_t max_datagram_bytes = 60000;
    bool enable_stats = true;
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
        if (parse_flag(argv[i], "--host", flag) && i + 1 < argc) {
            args.host = argv[++i];
        } else if (parse_flag(argv[i], "--port", flag) && i + 1 < argc) {
            args.port = std::stoi(argv[++i]);
        } else if (parse_flag(argv[i], "--rate", flag) && i + 1 < argc) {
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
        } else if (parse_flag(argv[i], "--stats-out", flag) && i + 1 < argc) {
            args.stats_out = argv[++i];
        } else if (parse_flag(argv[i], "--header", flag)) {
            args.header = true;
        } else if (parse_flag(argv[i], "--batch-lines", flag) && i + 1 < argc) {
            args.batch_lines = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (parse_flag(argv[i], "--max-datagram-bytes", flag) && i + 1 < argc) {
            args.max_datagram_bytes = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (parse_flag(argv[i], "--no-stats", flag)) {
            args.enable_stats = false;
        }
    }
    return args;
}

struct OrderInfo {
    std::int64_t order_id{};
    std::int64_t trader_id{};
    std::int64_t price_ticks{};
    std::int64_t qty_lots{};
    std::size_t symbol_idx{};
    std::uint8_t side{}; // 0=SELL, 1=BUY
};

struct LiveOrders {
    std::vector<OrderInfo> orders;
    std::unordered_map<std::int64_t, std::size_t> index;

    void add(const OrderInfo& info) {
        index[info.order_id] = orders.size();
        orders.push_back(info);
    }

    bool empty() const { return orders.empty(); }

    OrderInfo& pick(std::mt19937_64& rng) {
        std::uniform_int_distribution<std::size_t> dist(0, orders.size() - 1);
        return orders[dist(rng)];
    }

    void remove(std::int64_t order_id) {
        auto it = index.find(order_id);
        if (it == index.end()) {
            return;
        }
        std::size_t idx = it->second;
        OrderInfo last = orders.back();
        orders.pop_back();
        if (idx < orders.size()) {
            orders[idx] = last;
            index[last.order_id] = idx;
        }
        index.erase(it);
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

struct RunningStats {
    std::uint64_t count = 0;
    long double mean = 0.0L;
    long double m2 = 0.0L;
    long double min = std::numeric_limits<long double>::max();
    long double max = std::numeric_limits<long double>::lowest();

    void add(long double value) {
        ++count;
        long double delta = value - mean;
        mean += delta / static_cast<long double>(count);
        m2 += delta * (value - mean);
        if (value < min) {
            min = value;
        }
        if (value > max) {
            max = value;
        }
    }

    long double stddev() const { return count > 1 ? std::sqrt(m2 / static_cast<long double>(count - 1)) : 0.0L; }
};

struct ReservoirSample {
    explicit ReservoirSample(std::size_t cap) : capacity(cap) { values.reserve(capacity); }

    template <typename RNG> void add(std::int64_t value, RNG& rng) {
        ++seen;
        if (values.size() < capacity) {
            values.push_back(value);
            return;
        }
        std::uniform_int_distribution<std::uint64_t> dist(0, seen - 1);
        std::uint64_t idx = dist(rng);
        if (idx < capacity) {
            values[static_cast<std::size_t>(idx)] = value;
        }
    }

    std::vector<std::int64_t> values;
    std::uint64_t seen = 0;
    std::size_t capacity;
};

struct Quantiles {
    std::int64_t p50 = 0;
    std::int64_t p90 = 0;
    std::int64_t p99 = 0;
};

Quantiles compute_quantiles(std::vector<std::int64_t> sample) {
    Quantiles q{};
    if (sample.empty()) {
        return q;
    }
    std::sort(sample.begin(), sample.end());
    auto pick = [&](double pct) -> std::int64_t {
        double idx = pct * static_cast<double>(sample.size() - 1);
        return sample[static_cast<std::size_t>(idx + 0.5)];
    };
    q.p50 = pick(0.50);
    q.p90 = pick(0.90);
    q.p99 = pick(0.99);
    return q;
}

constexpr std::uint8_t kSideSell = 0;
constexpr std::uint8_t kSideBuy = 1;
constexpr std::uint8_t kUpdateAdd = 0;
constexpr std::uint8_t kUpdateDelete = 1;
constexpr std::uint8_t kUpdateSubtract = 2;
constexpr std::uint8_t kUpdateMatch = 3;
constexpr std::uint8_t kUpdateSet = 4;
constexpr std::uint8_t kUpdateAggressiveTrade = 5;
constexpr std::uint8_t kSourceHistorical = 0;

inline char* append_int(char* dst, std::int64_t value) {
    auto res = std::to_chars(dst, dst + 32, value);
    return res.ptr;
}

inline char* append_uint8(char* dst, std::uint8_t value) {
    auto res = std::to_chars(dst, dst + 8, static_cast<unsigned int>(value));
    return res.ptr;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
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

    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    int rc = ::getaddrinfo(args.host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || res == nullptr) {
        std::fprintf(stderr, "invalid host: %s (%s)\n", args.host.c_str(), gai_strerror(rc));
        return 1;
    }
    std::memcpy(&addr, res->ai_addr, sizeof(sockaddr_in));
    addr.sin_port = htons(static_cast<uint16_t>(args.port));
    ::freeaddrinfo(res);

    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::uniform_int_distribution<int> trader_dist(1, 64);

    std::int64_t next_order_id = 1;
    std::int64_t sent = 0;
    std::int64_t cancel_count = 0;
    std::int64_t match_count = 0;
    std::int64_t subtract_count = 0;
    std::int64_t add_count = 0;

    constexpr std::size_t kSampleSize = 10000;
    ReservoirSample offset_sample(kSampleSize);
    ReservoirSample qty_sample(kSampleSize);
    RunningStats offset_stats;
    RunningStats qty_stats;
    std::int64_t offset_b1 = 0;
    std::int64_t offset_b2_5 = 0;
    std::int64_t offset_b6_10 = 0;
    std::int64_t offset_bgt10 = 0;

    constexpr auto bucket_duration = std::chrono::milliseconds(100);
    const double bucket_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(bucket_duration).count();
    const auto bucket_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(bucket_duration).count();
    std::vector<std::uint32_t> bucket_counts;

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();

    const double rate = args.rate;

    std::string batch;
    batch.reserve(args.max_datagram_bytes);
    std::size_t batch_count = 0;

    auto flush_batch = [&] {
        if (batch.empty()) {
            return;
        }
        const ssize_t n =
            ::sendto(sock, batch.data(), batch.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        (void)n;
        batch.clear();
        batch_count = 0;
    };

    auto emit_line = [&](std::int64_t ts_exchange, std::int64_t ts_received, std::uint8_t side,
                         std::uint8_t update_type, std::int64_t price_ticks, std::int64_t qty_lots,
                         std::int64_t order_id, std::int64_t trader_id, std::int64_t aggressor_id,
                         std::uint8_t update_source, const std::string& symbol_id) {
        char line[256];
        char* ptr = line;
        ptr = append_int(ptr, ts_exchange);
        *ptr++ = ',';
        ptr = append_int(ptr, ts_received);
        *ptr++ = ',';
        ptr = append_uint8(ptr, side);
        *ptr++ = ',';
        ptr = append_uint8(ptr, update_type);
        *ptr++ = ',';
        ptr = append_int(ptr, price_ticks);
        *ptr++ = ',';
        ptr = append_int(ptr, qty_lots);
        *ptr++ = ',';
        ptr = append_int(ptr, order_id);
        *ptr++ = ',';
        ptr = append_int(ptr, trader_id);
        *ptr++ = ',';
        ptr = append_int(ptr, aggressor_id);
        *ptr++ = ',';
        ptr = append_uint8(ptr, update_source);
        *ptr++ = ',';
        std::memcpy(ptr, symbol_id.data(), symbol_id.size());
        ptr += symbol_id.size();
        *ptr++ = '\n';

        const std::size_t len = static_cast<std::size_t>(ptr - line);
        if (batch.size() + len > args.max_datagram_bytes || batch_count >= args.batch_lines) {
            flush_batch();
        }
        batch.append(line, len);
        ++batch_count;
    };

    if (args.header) {
        const std::string header =
            "ts_exchange,ts_received,side,update_type,price_ticks,quantity_lots,order_id,trader_id,"
            "aggressor_id,update_source,symbol_id";
        const ssize_t n =
            ::sendto(sock, header.data(), header.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        (void)n;
    }

    while (true) {
        const auto now = clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        if (elapsed >= args.duration) {
            break;
        }

        const std::int64_t target = rate > 0.0 ? static_cast<std::int64_t>(elapsed * rate) : 0;
        if (sent >= target) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            continue;
        }

        std::int64_t batch_events = std::min<std::int64_t>(target - sent, 4096);
        for (std::int64_t i = 0; i < batch_events; ++i) {
            const auto now_event = clock::now();
            const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now_event - start).count();
            const std::int64_t ts = static_cast<std::int64_t>(elapsed_us);

            bool do_cancel = (uni(rng) < args.cancel_pct) && !live.empty();
            bool do_market = (!do_cancel) && (uni(rng) < args.market_pct) && !live.empty();

            if (do_cancel) {
                OrderInfo& order = live.pick(rng);
                emit_line(ts, ts, order.side, kUpdateDelete, order.price_ticks, 0, order.order_id, order.trader_id, -1,
                          kSourceHistorical, symbols[order.symbol_idx]);
                live.remove(order.order_id);
                ++cancel_count;
            } else if (do_market) {
                OrderInfo& order = live.pick(rng);
                std::int64_t qty = sample_qty(rng, args.qty_mu, args.qty_sigma, args.max_qty);
                qty = std::min<std::int64_t>(qty, order.qty_lots);
                if (qty <= 0) {
                    qty = 1;
                }
                emit_line(ts, ts, order.side, kUpdateMatch, order.price_ticks, qty, order.order_id, order.trader_id, -1,
                          kSourceHistorical, symbols[order.symbol_idx]);
                order.qty_lots -= qty;
                if (order.qty_lots <= 0) {
                    live.remove(order.order_id);
                }
                ++match_count;
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
                std::uint8_t side = uni(rng) < 0.5 ? kSideBuy : kSideSell;
                int qty = sample_qty(rng, args.qty_mu, args.qty_sigma, args.max_qty);
                int trader_id = trader_dist(rng);

                int price = 0;
                int offset = sample_offset(rng, args.mean_offset);
                if (uni(rng) < args.cross_pct) {
                    price = side == kSideBuy ? mid + offset : std::max(1, mid - offset);
                } else {
                    price = side == kSideBuy ? std::max(1, mid - offset) : mid + offset;
                }

                int abs_offset = std::abs(price - mid);
                if (args.enable_stats) {
                    offset_stats.add(static_cast<long double>(abs_offset));
                    offset_sample.add(abs_offset, rng);
                    if (abs_offset <= 1) {
                        ++offset_b1;
                    } else if (abs_offset <= 5) {
                        ++offset_b2_5;
                    } else if (abs_offset <= 10) {
                        ++offset_b6_10;
                    } else {
                        ++offset_bgt10;
                    }
                }

                std::int64_t order_id = next_order_id++;
                emit_line(ts, ts, side, kUpdateAdd, price, qty, order_id, trader_id, -1, kSourceHistorical,
                          symbols[sym_idx]);

                live.add(OrderInfo{order_id, trader_id, price, qty, sym_idx, side});
                if (args.enable_stats) {
                    qty_stats.add(static_cast<long double>(qty));
                    qty_sample.add(qty, rng);
                }
                ++add_count;
            }

            ++sent;
            if (args.enable_stats) {
                auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
                std::size_t bucket = static_cast<std::size_t>(elapsed_ns / bucket_ns);
                if (bucket >= bucket_counts.size()) {
                    bucket_counts.resize(bucket + 1, 0);
                }
                ++bucket_counts[bucket];
            }
        }
        flush_batch();
    }

    flush_batch();

    const auto end = clock::now();
    double duration = std::chrono::duration<double>(end - start).count();
    double actual_rate = duration > 0.0 ? static_cast<double>(sent) / duration : 0.0;
    double rate_error = actual_rate - args.rate;
    double rate_error_pct = args.rate != 0.0 ? rate_error / args.rate : 0.0;

    RunningStats bucket_rate_stats;
    if (args.enable_stats) {
        for (std::uint32_t count : bucket_counts) {
            bucket_rate_stats.add(static_cast<long double>(count) / bucket_seconds);
        }
    }
    double rate_jitter = args.enable_stats ? static_cast<double>(bucket_rate_stats.stddev()) : 0.0;
    double rate_jitter_pct =
        args.enable_stats && bucket_rate_stats.mean != 0.0L
            ? rate_jitter / static_cast<double>(bucket_rate_stats.mean)
            : 0.0;

    Quantiles offset_quantiles = args.enable_stats ? compute_quantiles(offset_sample.values) : Quantiles{};
    Quantiles qty_quantiles = args.enable_stats ? compute_quantiles(qty_sample.values) : Quantiles{};
    double offset_min = args.enable_stats && offset_stats.count > 0 ? static_cast<double>(offset_stats.min) : 0.0;
    double offset_max = args.enable_stats && offset_stats.count > 0 ? static_cast<double>(offset_stats.max) : 0.0;
    double qty_min = args.enable_stats && qty_stats.count > 0 ? static_cast<double>(qty_stats.min) : 0.0;
    double qty_max = args.enable_stats && qty_stats.count > 0 ? static_cast<double>(qty_stats.max) : 0.0;

    auto write_stats = [&](std::ostream& os) {
        os << "{"
           << "\"sent\":" << sent << ","
           << "\"duration_sec\":" << std::setprecision(12) << duration << ","
           << "\"target_rate\":" << args.rate << ","
           << "\"actual_rate\":" << actual_rate << ","
           << "\"rate_error\":" << rate_error << ","
           << "\"rate_error_pct\":" << rate_error_pct << ","
           << "\"rate_bucket_ms\":" << std::chrono::duration_cast<std::chrono::milliseconds>(bucket_duration).count()
           << ","
           << "\"rate_jitter\":" << rate_jitter << ","
           << "\"rate_jitter_pct\":" << rate_jitter_pct << ","
           << "\"symbols\":" << args.symbols << ","
           << "\"cancel_pct\":" << args.cancel_pct << ","
           << "\"market_pct\":" << args.market_pct << ","
           << "\"cross_pct\":" << args.cross_pct << ","
           << "\"cancel_count\":" << cancel_count << ","
           << "\"match_count\":" << match_count << ","
           << "\"subtract_count\":" << subtract_count << ","
           << "\"add_count\":" << add_count << ","
           << "\"offset_mean\":" << static_cast<double>(offset_stats.mean) << ","
           << "\"offset_min\":" << offset_min << ","
           << "\"offset_max\":" << offset_max << ","
           << "\"offset_p50\":" << offset_quantiles.p50 << ","
           << "\"offset_p90\":" << offset_quantiles.p90 << ","
           << "\"offset_p99\":" << offset_quantiles.p99 << ","
           << "\"offset_bucket_1\":" << offset_b1 << ","
           << "\"offset_bucket_2_5\":" << offset_b2_5 << ","
           << "\"offset_bucket_6_10\":" << offset_b6_10 << ","
           << "\"offset_bucket_gt_10\":" << offset_bgt10 << ","
           << "\"qty_mean\":" << static_cast<double>(qty_stats.mean) << ","
           << "\"qty_min\":" << qty_min << ","
           << "\"qty_max\":" << qty_max << ","
           << "\"qty_p50\":" << qty_quantiles.p50 << ","
           << "\"qty_p90\":" << qty_quantiles.p90 << ","
           << "\"qty_p99\":" << qty_quantiles.p99 << ","
           << "\"seed\":" << args.seed << "}" << std::endl;
    };

    write_stats(std::cout);
    if (!args.stats_out.empty()) {
        std::ofstream out(args.stats_out);
        if (out) {
            write_stats(out);
        }
    }

    ::close(sock);
    return 0;
}
