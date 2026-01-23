#include "lobsim/in_memory_sink.hpp"
#include "lobsim/lob_event.hpp"
#include "lobsim/paper_trading_simulator.hpp"
#include "lobsim/replay_session.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct LobsterMessageEvent {
    std::int64_t ts_exchange_us{};
    int event_type{};
    std::int64_t order_id{};
    std::int64_t size{};
    std::int64_t price_ticks{};
    int direction{};
};

struct LobsterOrderbookRow {
    std::vector<std::int64_t> values;
};

bool parse_message_line(const std::string& line, LobsterMessageEvent& out) {
    if (line.empty()) {
        return false;
    }
    const char* s = line.c_str();
    char* end = nullptr;

    const double ts = std::strtod(s, &end);
    if (end == s || *end != ',') {
        return false;
    }
    s = end + 1;

    const long type = std::strtol(s, &end, 10);
    if (end == s || *end != ',') {
        return false;
    }
    s = end + 1;

    const long long order_id = std::strtoll(s, &end, 10);
    if (end == s || *end != ',') {
        return false;
    }
    s = end + 1;

    const long long size = std::strtoll(s, &end, 10);
    if (end == s || *end != ',') {
        return false;
    }
    s = end + 1;

    const long long price = std::strtoll(s, &end, 10);
    if (end == s || *end != ',') {
        return false;
    }
    s = end + 1;

    const long direction = std::strtol(s, &end, 10);
    if (end == s) {
        return false;
    }

    out.ts_exchange_us = static_cast<std::int64_t>(std::llround(ts * 1'000'000.0));
    out.event_type = static_cast<int>(type);
    out.order_id = static_cast<std::int64_t>(order_id);
    out.size = static_cast<std::int64_t>(size);
    out.price_ticks = static_cast<std::int64_t>(price);
    out.direction = static_cast<int>(direction);
    return true;
}

bool parse_orderbook_line(const std::string& line, std::size_t expected_cols, LobsterOrderbookRow& out) {
    out.values.clear();
    out.values.reserve(expected_cols);

    if (line.empty()) {
        return false;
    }
    const char* s = line.c_str();
    char* end = nullptr;

    for (std::size_t i = 0; i < expected_cols; ++i) {
        const long long v = std::strtoll(s, &end, 10);
        if (end == s) {
            return false;
        }
        out.values.push_back(static_cast<std::int64_t>(v));
        if (i + 1 == expected_cols) {
            return true;
        }
        if (*end != ',') {
            return false;
        }
        s = end + 1;
    }
    return out.values.size() == expected_cols;
}

class LobsterMessageSource {
public:
    explicit LobsterMessageSource(std::string path, std::size_t skip_rows = 0) : file(path) {
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open LOBSTER message file: " + path);
        }
        std::string line;
        for (std::size_t i = 0; i < skip_rows; ++i) {
            if (!std::getline(file, line)) {
                break;
            }
        }
    }

    bool next(LobsterMessageEvent& out) {
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (parse_message_line(line, out)) {
                return true;
            }
        }
        return false;
    }

private:
    std::ifstream file;
};

class LobsterOrderbookSource {
public:
    LobsterOrderbookSource(std::string path, std::size_t levels, std::size_t skip_rows = 0)
        : file(path), levels(levels) {
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open LOBSTER orderbook file: " + path);
        }
        std::string line;
        for (std::size_t i = 0; i < skip_rows; ++i) {
            if (!std::getline(file, line)) {
                break;
            }
        }
    }

    bool next(LobsterOrderbookRow& out) {
        const std::size_t expected_cols = levels * 4;
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (parse_orderbook_line(line, expected_cols, out)) {
                return true;
            }
        }
        return false;
    }

private:
    std::ifstream file;
    std::size_t levels;
};

class LobsterAdapter {
public:
    explicit LobsterAdapter(std::string symbol) : symbol(std::move(symbol)) {}

    NormalizedLobEvent normalize(const LobsterMessageEvent& raw) const {
        NormalizedLobEvent out{};
        if (!try_normalize(raw, out)) {
            throw std::runtime_error("LOBSTER hidden execution (type 5) not applied to visible book");
        }
        return out;
    }

    bool try_normalize(const LobsterMessageEvent& raw, NormalizedLobEvent& out) const {
        if (raw.event_type == 5) {
            return false;
        }

        UpdateType update_type{};
        switch (raw.event_type) {
        case 1:
            update_type = UpdateType::ADD;
            break;
        case 2:
            update_type = UpdateType::SUBTRACT;
            break;
        case 3:
            update_type = UpdateType::DELETE;
            break;
        case 4:
            update_type = UpdateType::MATCH;
            break;
        default:
            throw std::runtime_error("Unsupported LOBSTER event type");
        }

        if (raw.direction != 1 && raw.direction != -1) {
            throw std::runtime_error("Bad LOBSTER direction value");
        }
        const Side side = raw.direction == 1 ? Side::BUY : Side::SELL;

        out.ts_exchange = raw.ts_exchange_us;
        out.ts_received = raw.ts_exchange_us;
        out.side = side;
        out.update_type = update_type;
        // LOBSTER prices are already scaled by 1e-4 dollars, so we use them directly as ticks.
        out.price_ticks = raw.price_ticks;
        out.quantity_lots = raw.size;
        out.order_id = raw.order_id;
        out.trader_id = UnknownTraderIdSentinel;
        out.aggressor_id = UnknownAggressorIdSentinel;
        out.update_source = UpdateSource::HISTORICAL;
        out.symbol_id = symbol;
        return true;
    }

private:
    std::string symbol;
};

void init_from_orderbook_row(PaperTradingSimulator& engine, const LobsterOrderbookRow& row, std::size_t levels) {
    std::vector<Side> sides;
    std::vector<std::int64_t> prices;
    std::vector<std::int64_t> quantities;

    sides.reserve(levels * 2);
    prices.reserve(levels * 2);
    quantities.reserve(levels * 2);

    for (std::size_t level = 0; level < levels; ++level) {
        const auto ask_px = row.values[4 * level];
        const auto ask_sz = row.values[4 * level + 1];
        const auto bid_px = row.values[4 * level + 2];
        const auto bid_sz = row.values[4 * level + 3];

        if (ask_px > 0 && ask_sz > 0) {
            sides.push_back(Side::SELL);
            prices.push_back(ask_px);
            quantities.push_back(ask_sz);
        }
        if (bid_px > 0 && bid_sz > 0) {
            sides.push_back(Side::BUY);
            prices.push_back(bid_px);
            quantities.push_back(bid_sz);
        }
    }

    engine.init_from_l2_snapshot(sides, prices, quantities);
}

} // namespace

int main(int argc, char** argv) {
    std::string message_path = "sample_data/AMZN_2012-06-21_34200000_57600000_message_10.csv";
    std::string orderbook_path = "sample_data/AMZN_2012-06-21_34200000_57600000_orderbook_10.csv";
    if (argc > 1) {
        message_path = argv[1];
    }
    if (argc > 2) {
        orderbook_path = argv[2];
    }

    constexpr std::size_t levels = 10;

    PaperTradingSimulator engine;
    InMemoryLogSink sink;
    engine.set_log_sink(&sink);

    LobsterOrderbookSource book_init(orderbook_path, levels);
    LobsterOrderbookRow first_row{};
    if (!book_init.next(first_row)) {
        std::cerr << "Failed to read first orderbook row\n";
        return 1;
    }
    init_from_orderbook_row(engine, first_row, levels);

    // Row i of the orderbook is the state after message row i.
    // We init from row 0 and skip message row 0 to keep alignment.
    LobsterMessageSource message_source(message_path, 1);
    LobsterOrderbookSource book_source(orderbook_path, levels, 1);
    LobsterAdapter adapter("AMZN");
    lobsim::replay::ReplayConfig cfg{};
    cfg.require_monotonic_ts_received = true;
    cfg.fail_fast = true;
    lobsim::replay::ReplaySession replay(engine, cfg);

    std::uint64_t processed = 0;
    std::uint64_t applied = 0;
    std::uint64_t skipped_hidden = 0;
    std::uint64_t mismatches = 0;

    LobsterMessageEvent raw{};
    LobsterOrderbookRow book_row{};
    while (message_source.next(raw) && book_source.next(book_row)) {
        ++processed;
        NormalizedLobEvent ev{};
        if (adapter.try_normalize(raw, ev)) {
            replay.step(ev);
            ++applied;
        } else {
            ++skipped_hidden;
        }

        if (processed % 10000 == 0) {
            auto top_bid = engine.l2_top_n(Side::BUY, 1);
            auto top_ask = engine.l2_top_n(Side::SELL, 1);

            const auto bid_px = book_row.values[2];
            const auto bid_sz = book_row.values[3];
            const auto ask_px = book_row.values[0];
            const auto ask_sz = book_row.values[1];

            const bool bid_ok =
                (bid_px == 0 && top_bid.empty()) || (!top_bid.empty() && top_bid[0].first == bid_px &&
                                                   top_bid[0].second == bid_sz);
            const bool ask_ok =
                (ask_px == 0 && top_ask.empty()) || (!top_ask.empty() && top_ask[0].first == ask_px &&
                                                   top_ask[0].second == ask_sz);
            if (!bid_ok || !ask_ok) {
                ++mismatches;
            }

            std::cout << "row=" << processed << " applied=" << applied << " skipped_hidden=" << skipped_hidden
                      << " mismatches=" << mismatches << "\n";
        }
    }

    std::cout << "done rows=" << processed << " applied=" << applied << " skipped_hidden=" << skipped_hidden
              << " mismatches=" << mismatches << "\n";
    return 0;
}
