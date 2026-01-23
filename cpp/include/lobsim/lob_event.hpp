#pragma once

#include "lobsim/types.hpp"

#include <cstring>
#include <iostream>

constexpr std::int64_t UnknownOrderIdSentinel = -1;
constexpr std::int64_t UnknownTraderIdSentinel = -1;
constexpr std::int64_t UnknownAggressorIdSentinel = -1;
constexpr std::int64_t NoAggressorNeededSentinel = -2;

struct NormalizedLobEvent {
    std::int64_t ts_exchange;
    std::int64_t ts_received;
    Side side;
    UpdateType update_type;
    std::int64_t price_ticks;
    std::int64_t quantity_lots;
    std::int64_t order_id;
    std::int64_t trader_id{UnknownTraderIdSentinel};
    std::int64_t aggressor_id{NoAggressorNeededSentinel};
    UpdateSource update_source{UpdateSource::HISTORICAL};
    std::string symbol_id;
};

struct CoinbaseBTCUSDTRawEvent {
    std::int64_t ts_exchange_us{};
    std::int64_t ts_received_us{};

    UpdateType update_type{};
    Side side{};

    long double price{};
    long double size{};

    std::string order_id;
};
