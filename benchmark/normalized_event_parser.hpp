#pragma once

#include "lobsim/inline.hpp"
#include "lobsim/lob_event.hpp"

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace parsing {

LOBSIM_FORCEINLINE std::string_view trim(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t' || value[start] == '\r')) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r')) {
        --end;
    }
    return value.substr(start, end - start);
}

LOBSIM_FORCEINLINE bool next_field(std::string_view line, std::size_t& pos, std::string_view& out) {
    if (pos >= line.size()) {
        return false;
    }
    std::size_t start = pos;
    std::size_t end = line.find(',', start);
    if (end == std::string_view::npos) {
        end = line.size();
        pos = line.size();
    } else {
        pos = end + 1;
    }
    out = trim(line.substr(start, end - start));
    return true;
}

LOBSIM_FORCEINLINE bool parse_int64(std::string_view field, std::int64_t& out) {
    auto first = field.data();
    auto last = field.data() + field.size();
    auto res = std::from_chars(first, last, out);
    return res.ec == std::errc{} && res.ptr == last;
}

LOBSIM_FORCEINLINE bool parse_uint8(std::string_view field, std::uint8_t& out) {
    std::int64_t tmp = 0;
    if (!parse_int64(field, tmp)) {
        return false;
    }
    if (tmp < 0 || tmp > 255) {
        return false;
    }
    out = static_cast<std::uint8_t>(tmp);
    return true;
}

LOBSIM_FORCEINLINE std::optional<NormalizedLobEvent> parse_normalized_event(std::string_view line) {
    std::size_t pos = 0;
    std::string_view field;

    NormalizedLobEvent ev{};

    std::int64_t ts_exchange = 0;
    std::int64_t ts_received = 0;
    std::int64_t price_ticks = 0;
    std::int64_t qty_lots = 0;
    std::int64_t order_id = 0;
    std::int64_t trader_id = 0;
    std::int64_t aggressor_id = 0;
    std::uint8_t side_raw = 0;
    std::uint8_t update_type_raw = 0;
    std::uint8_t update_source_raw = 0;

    if (!next_field(line, pos, field) || !parse_int64(field, ts_exchange)) {
        return std::nullopt;
    }
    if (!next_field(line, pos, field) || !parse_int64(field, ts_received)) {
        return std::nullopt;
    }
    if (!next_field(line, pos, field) || !parse_uint8(field, side_raw)) {
        return std::nullopt;
    }
    if (!next_field(line, pos, field) || !parse_uint8(field, update_type_raw)) {
        return std::nullopt;
    }
    if (!next_field(line, pos, field) || !parse_int64(field, price_ticks)) {
        return std::nullopt;
    }
    if (!next_field(line, pos, field) || !parse_int64(field, qty_lots)) {
        return std::nullopt;
    }
    if (!next_field(line, pos, field) || !parse_int64(field, order_id)) {
        return std::nullopt;
    }
    if (!next_field(line, pos, field) || !parse_int64(field, trader_id)) {
        return std::nullopt;
    }
    if (!next_field(line, pos, field) || !parse_int64(field, aggressor_id)) {
        return std::nullopt;
    }
    if (!next_field(line, pos, field) || !parse_uint8(field, update_source_raw)) {
        return std::nullopt;
    }
    if (!next_field(line, pos, field)) {
        return std::nullopt;
    }
    std::string symbol_id(field);
    if (symbol_id.empty()) {
        return std::nullopt;
    }

    if (side_raw > 1 || update_type_raw > 5 || update_source_raw > 2) {
        return std::nullopt;
    }

    ev.ts_exchange = ts_exchange;
    ev.ts_received = ts_received;
    ev.side = static_cast<Side>(side_raw);
    ev.update_type = static_cast<UpdateType>(update_type_raw);
    ev.price_ticks = price_ticks;
    ev.quantity_lots = qty_lots;
    ev.order_id = order_id;
    ev.trader_id = trader_id;
    ev.aggressor_id = aggressor_id;
    ev.update_source = static_cast<UpdateSource>(update_source_raw);
    ev.symbol_id = std::move(symbol_id);

    return ev;
}

} // namespace parsing
