#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

#include "blocking_queue.hpp"
#include "diagnostic_queue_sink.hpp"
#include "lobsim/paper_trading_simulator.hpp"
#include "normalized_event_parser.hpp"

constexpr uint16_t kUdpPort = 1234;
constexpr std::size_t kMaxDatagramSize = 65507;
constexpr std::size_t kInboundQueueCapacity = 8192;
constexpr std::size_t kDiagnosticsQueueCapacity = 1024;

namespace {

struct RunningStats {
    std::uint64_t count = 0;
    long double mean = 0.0L;
    long double m2 = 0.0L;
    std::int64_t min = std::numeric_limits<std::int64_t>::max();
    std::int64_t max = std::numeric_limits<std::int64_t>::min();

    void add(std::int64_t value) {
        ++count;
        long double delta = static_cast<long double>(value) - mean;
        mean += delta / static_cast<long double>(count);
        m2 += delta * (static_cast<long double>(value) - mean);
        if (value < min) {
            min = value;
        }
        if (value > max) {
            max = value;
        }
    }

    std::int64_t mean_ns() const { return count ? static_cast<std::int64_t>(mean + 0.5L) : 0; }

    std::int64_t stddev_ns() const {
        return count > 1 ? static_cast<std::int64_t>(std::sqrt(m2 / (count - 1)) + 0.5L) : 0;
    }
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
        auto idx = dist(rng);
        if (idx < capacity) {
            values[static_cast<std::size_t>(idx)] = value;
        }
    }

    std::vector<std::int64_t> values;
    std::uint64_t seen = 0;
    std::size_t capacity;
};

struct Percentiles {
    std::int64_t p50 = 0;
    std::int64_t p90 = 0;
    std::int64_t p99 = 0;
};

Percentiles compute_percentiles(std::vector<std::int64_t> sample) {
    Percentiles result{};
    if (sample.empty()) {
        return result;
    }
    std::sort(sample.begin(), sample.end());
    auto pick = [&](double pct) -> std::int64_t {
        double idx = pct * static_cast<double>(sample.size() - 1);
        return sample[static_cast<std::size_t>(idx + 0.5)];
    };
    result.p50 = pick(0.50);
    result.p90 = pick(0.90);
    result.p99 = pick(0.99);
    return result;
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

int main() {
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create UDP socket: " << std::strerror(errno) << "\n";
        return 1;
    }

    const char* metrics_env = std::getenv("METRICS");
    const bool metrics_enabled = metrics_env != nullptr && metrics_env[0] != '\0' && metrics_env[0] != '0';

    int reuse = 1;
    if (::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::cerr << "Warning: setsockopt(SO_REUSEADDR) failed: " << std::strerror(errno) << "\n";
    }

    if (const char* rcvbuf_env = std::getenv("UDP_RCVBUF")) {
        char* end = nullptr;
        const long rcvbuf = std::strtol(rcvbuf_env, &end, 10);
        if (end != rcvbuf_env && rcvbuf > 0) {
            const int buf = static_cast<int>(rcvbuf);
            if (::setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf)) < 0) {
                std::cerr << "Failed to set UDP receive buffer: " << std::strerror(errno) << "\n";
            }
        }
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kUdpPort);

    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Failed to bind UDP port " << kUdpPort << ": " << std::strerror(errno) << "\n";
        ::close(sock);
        return 1;
    }

    std::optional<std::chrono::milliseconds> shutdown_after;
    if (const char* duration_env = std::getenv("ENGINE_DURATION_SEC")) {
        char* end = nullptr;
        const long seconds = std::strtol(duration_env, &end, 10);
        if (end != duration_env && seconds > 0) {
            shutdown_after = std::chrono::milliseconds(seconds * 1000L);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 200000;
            if (::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
                std::cerr << "Failed to set UDP receive timeout: " << std::strerror(errno) << "\n";
            }
        }
    }

    BlockingQueue<std::string> inbound(kInboundQueueCapacity);
    BlockingQueue<DiagnosticRecord> diagnostics(kDiagnosticsQueueCapacity);

    DiagnosticQueueSink sink(&diagnostics);
    PaperTradingSimulator engine;
    engine.set_log_sink(&sink);

    std::atomic<bool> shutdown_requested{false};

    auto request_shutdown = [&] {
        bool expected = false;
        if (!shutdown_requested.compare_exchange_strong(expected, true)) {
            return;
        }
        inbound.close();
        diagnostics.close();
        ::shutdown(sock, SHUT_RDWR);
    };

    std::thread shutdown_timer;
    if (shutdown_after) {
        const auto delay = *shutdown_after;
        shutdown_timer = std::thread([&, delay] {
            using Clock = std::chrono::steady_clock;
            auto deadline = Clock::now() + delay;
            while (!shutdown_requested.load(std::memory_order_relaxed)) {
                auto now = Clock::now();
                if (now >= deadline) {
                    break;
                }
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                if (remaining > std::chrono::milliseconds(100)) {
                    remaining = std::chrono::milliseconds(100);
                }
                std::this_thread::sleep_for(remaining);
            }
            if (!shutdown_requested.load(std::memory_order_relaxed)) {
                request_shutdown();
            }
        });
    }

    ThreadUsage input_usage{};
    ThreadUsage processing_usage{};
    ThreadUsage diagnostics_usage{};
    std::int64_t processing_elapsed_ns = 0;

    RunningStats processing_stats;
    ReservoirSample processing_sample(10000);
    std::uint64_t parse_failures = 0;

    auto input_thread_job = [&] {
        std::string buf;
        buf.resize(kMaxDatagramSize);

        while (true) {
            if (shutdown_requested.load(std::memory_order_relaxed)) {
                break;
            }
            const ssize_t n = ::recvfrom(sock, buf.data(), buf.size(), 0, nullptr, nullptr);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (shutdown_requested.load(std::memory_order_relaxed)) {
                        break;
                    }
                    continue;
                }
                std::cerr << "recvfrom failed: " << std::strerror(errno) << "\n";
                break;
            }
            if (n == 0) {
                if (shutdown_requested.load(std::memory_order_relaxed)) {
                    break;
                }
                continue;
            }

            std::string_view payload(buf.data(), static_cast<std::size_t>(n));
            std::size_t start = 0;
            while (start < payload.size()) {
                std::size_t end = payload.find('\n', start);
                if (end == std::string_view::npos) {
                    end = payload.size();
                }
                std::string_view line = parsing::trim(payload.substr(start, end - start));
                if (!line.empty()) {
                    inbound.push(std::string(line));
                }
                start = end + 1;
            }
        }

        inbound.close();
        input_usage = read_thread_usage();
    };

    auto processing_thread_job = [&] {
        std::mt19937_64 rng(0xBADC0FFEEULL);
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
            auto start = clock::time_point{};
            if (metrics_enabled) {
                start = clock::now();
            }
            auto event = parsing::parse_normalized_event(*raw_event);
            if (event) {
                engine.update(*event);
            } else if (metrics_enabled) {
                ++parse_failures;
            }
            if (metrics_enabled) {
                auto end = clock::now();
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                processing_stats.add(ns);
                processing_sample.add(ns, rng);
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
    if (shutdown_timer.joinable()) {
        shutdown_timer.join();
    }
    ::close(sock);

    if (metrics_enabled) {
        Percentiles proc_percentiles = compute_percentiles(processing_sample.values);
        std::int64_t proc_min = processing_stats.count > 0 ? processing_stats.min : 0;
        std::int64_t proc_max = processing_stats.count > 0 ? processing_stats.max : 0;
        const double proc_elapsed_sec =
            processing_elapsed_ns > 0 ? static_cast<double>(processing_elapsed_ns) / 1e9 : 0.0;
        const double proc_rate =
            proc_elapsed_sec > 0 ? static_cast<double>(processing_stats.count) / proc_elapsed_sec : 0.0;
        auto in_stats = inbound.snapshot();
        auto diag_stats = diagnostics.snapshot();
        std::cerr << "METRICS "
                  << "in_pushes=" << in_stats.pushes << " "
                  << "in_pops=" << in_stats.pops << " "
                  << "in_dropped=" << in_stats.push_dropped << " "
                  << "in_max_depth=" << in_stats.max_depth << " "
                  << "in_cur_depth=" << in_stats.current_depth << " "
                  << "in_push_block_ns=" << in_stats.push_block_ns << " "
                  << "in_pop_block_ns=" << in_stats.pop_block_ns << " "
                  << "diag_pushes=" << diag_stats.pushes << " "
                  << "diag_pops=" << diag_stats.pops << " "
                  << "proc_count=" << processing_stats.count << " proc_mean_ns=" << processing_stats.mean_ns()
                  << " proc_stddev_ns=" << processing_stats.stddev_ns() << " proc_min_ns=" << proc_min
                  << " proc_max_ns=" << proc_max << " proc_p50_ns=" << proc_percentiles.p50
                  << " proc_p90_ns=" << proc_percentiles.p90 << " proc_p99_ns=" << proc_percentiles.p99
                  << " proc_parse_failures=" << parse_failures
                  << " proc_elapsed_sec=" << proc_elapsed_sec << " proc_rate=" << proc_rate;
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
