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
    std::int64_t tsExchangeUs{};
    int eventType{};
    std::int64_t orderId{};
    std::int64_t size{};
    std::int64_t priceTicks{};
    int direction{};
};

struct LobsterOrderbookRow {
    std::vector<std::int64_t> values;
};

bool parseMessageLine(const std::string& line, LobsterMessageEvent& out) {
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

    const long long orderId = std::strtoll(s, &end, 10);
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

    out.tsExchangeUs = static_cast<std::int64_t>(std::llround(ts * 1'000'000.0));
    out.eventType = static_cast<int>(type);
    out.orderId = static_cast<std::int64_t>(orderId);
    out.size = static_cast<std::int64_t>(size);
    out.priceTicks = static_cast<std::int64_t>(price);
    out.direction = static_cast<int>(direction);
    return true;
}

bool parseOrderbookLine(const std::string& line, std::size_t expectedCols, LobsterOrderbookRow& out) {
    out.values.clear();
    out.values.reserve(expectedCols);

    if (line.empty()) {
        return false;
    }
    const char* s = line.c_str();
    char* end = nullptr;

    for (std::size_t i = 0; i < expectedCols; ++i) {
        const long long v = std::strtoll(s, &end, 10);
        if (end == s) {
            return false;
        }
        out.values.push_back(static_cast<std::int64_t>(v));
        if (i + 1 == expectedCols) {
            return true;
        }
        if (*end != ',') {
            return false;
        }
        s = end + 1;
    }
    return out.values.size() == expectedCols;
}

class LobsterMessageSource {
public:
    explicit LobsterMessageSource(std::string path, std::size_t skipRows = 0) : file(path) {
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open LOBSTER message file: " + path);
        }
        std::string line;
        for (std::size_t i = 0; i < skipRows; ++i) {
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
            if (parseMessageLine(line, out)) {
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
    LobsterOrderbookSource(std::string path, std::size_t levels, std::size_t skipRows = 0)
        : file(path), levels(levels) {
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open LOBSTER orderbook file: " + path);
        }
        std::string line;
        for (std::size_t i = 0; i < skipRows; ++i) {
            if (!std::getline(file, line)) {
                break;
            }
        }
    }

    bool next(LobsterOrderbookRow& out) {
        const std::size_t expectedCols = levels * 4;
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (parseOrderbookLine(line, expectedCols, out)) {
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
        if (!tryNormalize(raw, out)) {
            throw std::runtime_error("LOBSTER hidden execution (type 5) not applied to visible book");
        }
        return out;
    }

    bool tryNormalize(const LobsterMessageEvent& raw, NormalizedLobEvent& out) const {
        if (raw.eventType == 5) {
            return false;
        }

        UpdateType updateType{};
        switch (raw.eventType) {
        case 1:
            updateType = UpdateType::ADD;
            break;
        case 2:
            updateType = UpdateType::SUBTRACT;
            break;
        case 3:
            updateType = UpdateType::DELETE;
            break;
        case 4:
            updateType = UpdateType::MATCH;
            break;
        default:
            throw std::runtime_error("Unsupported LOBSTER event type");
        }

        if (raw.direction != 1 && raw.direction != -1) {
            throw std::runtime_error("Bad LOBSTER direction value");
        }
        const Side side = raw.direction == 1 ? Side::BUY : Side::SELL;

        out.tsExchange = raw.tsExchangeUs;
        out.tsReceived = raw.tsExchangeUs;
        out.side = side;
        out.updateType = updateType;
        // LOBSTER prices are already scaled by 1e-4 dollars, so we use them directly as ticks.
        out.priceTicks = raw.priceTicks;
        out.quantityLots = raw.size;
        out.orderId = raw.orderId;
        out.traderId = UnknownTraderIdSentinel;
        out.aggressorId = UnknownAggressorIdSentinel;
        out.updateSource = UpdateSource::HISTORICAL;
        out.symbolId = symbol;
        return true;
    }

private:
    std::string symbol;
};

void initFromOrderbookRow(PaperTradingSimulator& engine, const LobsterOrderbookRow& row, std::size_t levels) {
    std::vector<Side> sides;
    std::vector<std::int64_t> prices;
    std::vector<std::int64_t> quantities;

    sides.reserve(levels * 2);
    prices.reserve(levels * 2);
    quantities.reserve(levels * 2);

    for (std::size_t level = 0; level < levels; ++level) {
        const auto askPx = row.values[4 * level];
        const auto askSz = row.values[4 * level + 1];
        const auto bidPx = row.values[4 * level + 2];
        const auto bidSz = row.values[4 * level + 3];

        if (askPx > 0 && askSz > 0) {
            sides.push_back(Side::SELL);
            prices.push_back(askPx);
            quantities.push_back(askSz);
        }
        if (bidPx > 0 && bidSz > 0) {
            sides.push_back(Side::BUY);
            prices.push_back(bidPx);
            quantities.push_back(bidSz);
        }
    }

    engine.initFromL2Snapshot(sides, prices, quantities);
}

} // namespace

int main(int argc, char** argv) {
    std::string messagePath = "sample_data/AMZN_2012-06-21_34200000_57600000_message_10.csv";
    std::string orderbookPath = "sample_data/AMZN_2012-06-21_34200000_57600000_orderbook_10.csv";
    if (argc > 1) {
        messagePath = argv[1];
    }
    if (argc > 2) {
        orderbookPath = argv[2];
    }

    constexpr std::size_t levels = 10;

    PaperTradingSimulator engine;
    InMemoryLogSink sink;
    engine.setLogSink(&sink);

    LobsterOrderbookSource bookInit(orderbookPath, levels);
    LobsterOrderbookRow firstRow{};
    if (!bookInit.next(firstRow)) {
        std::cerr << "Failed to read first orderbook row\n";
        return 1;
    }
    initFromOrderbookRow(engine, firstRow, levels);

    // Row i of the orderbook is the state after message row i.
    // We init from row 0 and skip message row 0 to keep alignment.
    LobsterMessageSource messageSource(messagePath, 1);
    LobsterOrderbookSource bookSource(orderbookPath, levels, 1);
    LobsterAdapter adapter("AMZN");
    lobsim::replay::ReplayConfig cfg{};
    cfg.requireMonotonicTsReceived = true;
    cfg.failFast = true;
    lobsim::replay::ReplaySession replay(engine, cfg);

    std::uint64_t processed = 0;
    std::uint64_t applied = 0;
    std::uint64_t skippedHidden = 0;
    std::uint64_t mismatches = 0;

    LobsterMessageEvent raw{};
    LobsterOrderbookRow bookRow{};
    while (messageSource.next(raw) && bookSource.next(bookRow)) {
        ++processed;
        NormalizedLobEvent ev{};
        if (adapter.tryNormalize(raw, ev)) {
            replay.step(ev);
            ++applied;
        } else {
            ++skippedHidden;
        }

        if (processed % 10000 == 0) {
            auto topBid = engine.l2TopN(Side::BUY, 1);
            auto topAsk = engine.l2TopN(Side::SELL, 1);

            const auto bidPx = bookRow.values[2];
            const auto bidSz = bookRow.values[3];
            const auto askPx = bookRow.values[0];
            const auto askSz = bookRow.values[1];

            const bool bidOk =
                (bidPx == 0 && topBid.empty()) || (!topBid.empty() && topBid[0].first == bidPx &&
                                                   topBid[0].second == bidSz);
            const bool askOk =
                (askPx == 0 && topAsk.empty()) || (!topAsk.empty() && topAsk[0].first == askPx &&
                                                   topAsk[0].second == askSz);
            if (!bidOk || !askOk) {
                ++mismatches;
            }

            std::cout << "row=" << processed << " applied=" << applied << " skipped_hidden=" << skippedHidden
                      << " mismatches=" << mismatches << "\n";
        }
    }

    std::cout << "done rows=" << processed << " applied=" << applied << " skipped_hidden=" << skippedHidden
              << " mismatches=" << mismatches << "\n";
    return 0;
}
