#include "simex/engine.hpp"
#include "simex/normalized_event.hpp"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

namespace {

std::string_view trim(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
        sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
        sv.remove_suffix(1);
    return sv;
}

bool parse_int(std::string_view sv, std::int64_t& out) {
    sv = trim(sv);
    const char* beg = sv.data();
    const char* end = sv.data() + sv.size();
    auto [ptr, ec] = std::from_chars(beg, end, out);
    return ec == std::errc() && ptr == end;
}

bool parse_double(std::string_view sv, double& out) {
    sv = trim(sv);
    const char* beg = sv.data();
    const char* end = sv.data() + sv.size();
    auto [ptr, ec] = std::from_chars(beg, end, out);
    return ec == std::errc() && ptr == end;
}

bool parse_time_to_us(std::string_view sv, std::int64_t& outUs) {
    sv = trim(sv);
    // Expected format HH:MM:SS.sssssss (times within a day)
    auto c1 = sv.find(':');
    auto c2 = sv.find(':', c1 == std::string_view::npos ? 0 : c1 + 1);
    if (c1 == std::string_view::npos || c2 == std::string_view::npos)
        return false;

    std::int64_t hh{}, mm{};
    if (!parse_int(sv.substr(0, c1), hh))
        return false;
    if (!parse_int(sv.substr(c1 + 1, c2 - c1 - 1), mm))
        return false;

    double secs = 0.0;
    if (!parse_double(sv.substr(c2 + 1), secs))
        return false;

    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || secs < 0.0)
        return false;

    const double totalSeconds = static_cast<double>(hh * 3600 + mm * 60) + secs;
    outUs = static_cast<std::int64_t>(std::llround(totalSeconds * 1'000'000.0));
    return true;
}

bool parse_line(std::string_view line, NormalizedLobEvent& out, const char separator = ';') {
    // CSV with 7 columns:
    // time_exchange,time_coinapi,update_type,is_buy,entry_px,entry_sx,order_id
    std::string_view fields[7];
    std::size_t pos = 0;
    for (int i = 0; i < 6; ++i) {
        auto comma = line.find(separator, pos);
        if (comma == std::string_view::npos) {
            return false;
        }
        fields[i] = line.substr(pos, comma - pos);
        pos = comma + 1;
    }
    fields[6] = line.substr(pos);

    // time_exchange and time_coinapi
    if (!parse_time_to_us(fields[0], out.tsExchange)) {
        return false;
    }
    if (!parse_time_to_us(fields[1], out.tsReceived)) {
        return false;
    }

    // update_type
    auto ut = trim(fields[2]);
    if (ut == "SNAPSHOT") {
        out.updateType = UpdateType::SET;
    } else if (ut == "ADD") {
        out.updateType = UpdateType::ADD;
    } else if (ut == "DELETE") {
        out.updateType = UpdateType::DELETE;
    } else if (ut == "MATCH") {
        out.updateType = UpdateType::MATCH;
    } else if (ut == "SUBTRACT") {
        out.updateType = UpdateType::SUBTRACT;
    } else {
        return false;
    }

    // is_buy
    std::int64_t isBuy{};
    if (!parse_int(fields[3], isBuy) || (isBuy != 0 && isBuy != 1)) {
        return false;
    }
    out.side = isBuy == 1 ? Side::BUY : Side::SELL;

    // entry_px and entry_sx (scale to preserve decimals)
    double px{}, sx{};
    if (!parse_double(fields[4], px)) {
        return false;
    }
    if (!parse_double(fields[5], sx)) {
        return false;
    }
    // Preserve 6 decimal places to integer; adjust if your tick/lot size differs.
    out.priceTicks = static_cast<std::int64_t>(std::llround(px * 1'000'000.0));
    out.quantityLots = static_cast<std::int64_t>(std::llround(sx * 1'000'000.0));

    // order_id: hash the UUID into an int64_t for now
    auto oid = trim(fields[6]);
    if (oid.empty()) {
        return false;
    }
    out.orderId = static_cast<std::int64_t>(std::hash<std::string_view>{}(oid));

    out.traderId = UnknownTraderIdSentinel;
    out.aggressorId = UnknownAggressorIdSentinel;
    out.updateSource = UpdateSource::HISTORICAL;
    out.symbolId.clear(); // not present in this feed
    return true;
}

bool readNextEvent(gzFile file, NormalizedLobEvent& out) {
    static char line[1 << 16]; // 64KB
    while (true) {
        if (!gzgets(file, line, sizeof(line)))
            return false;
        std::string_view sv(line);
        // Drop trailing newline(s)
        while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r'))
            sv.remove_suffix(1);
        if (sv.empty())
            continue;
        if (parse_line(sv, out))
            return true; // parsed successfully
        // corrupted line (including header) -> skip
    }
}

} // namespace

int main() {
    // Adjust path as needed
    gzFile f =
        gzopen("/Users/kpetridis/btcusdt/20250101/IDDI-5950967+SC-COINBASE_SPOT_BTC_USDT+S-BTC__002DUSDT.csv.gz", "rb");
    assert(f);

    NormalizedLobEvent ev;
    while (readNextEvent(f, ev)) {
        // simulator.update(ev);
        ev.tsExchange += 0; // breakpoint target
    }

    gzclose(f);
    return 0;
}
