#pragma once

#include "simex/types.hpp"

#include <cstring>
#include <iostream>

struct NormalizedLobEvent {
    std::int64_t tsExchange;
    std::int64_t tsReceived;
    Side side;
    UpdateType updateType;
    std::int64_t priceTicks;
    std::int64_t quantityLots;
    std::int64_t orderId;
    std::int64_t traderId;
    std::int64_t aggressorId;
    UpdateSource updateSource;
    std::string symbolId;
};
