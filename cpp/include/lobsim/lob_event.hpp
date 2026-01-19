#pragma once

#include "lobsim/types.hpp"

#include <cstring>
#include <iostream>

constexpr std::int64_t UnknownOrderIdSentinel = -1;
constexpr std::int64_t UnknownTraderIdSentinel = -1;
constexpr std::int64_t UnknownAggressorIdSentinel = -1;
constexpr std::int64_t NoAggressorNeededSentinel = -2;

struct NormalizedLobEvent {
    std::int64_t tsExchange;
    std::int64_t tsReceived;
    Side side;
    UpdateType updateType;
    std::int64_t priceTicks;
    std::int64_t quantityLots;
    std::int64_t orderId;
    std::int64_t traderId{UnknownTraderIdSentinel};
    std::int64_t aggressorId{NoAggressorNeededSentinel};
    UpdateSource updateSource{UpdateSource::HISTORICAL};
    std::string symbolId;
};

struct CoinbaseBTCUSDTRawEvent {
    std::int64_t tsExchangeUs{};
    std::int64_t tsReceivedUs{};

    UpdateType updateType{};
    Side side{};

    long double price{};
    long double size{};

    std::string orderId;
};
