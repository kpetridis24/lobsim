#pragma once

#include "simex/types.hpp"

#include <cstdint>
#include <iostream>

struct FillRecord {
public:
    std::uint64_t seq;
    std::int64_t tsExchange;
    std::int64_t tsReceived;
    std::int64_t priceTicks;
    std::int64_t qtyLots;
    // Passive order
    Side makerSide;
    std::int64_t makerOrderId;
    std::int64_t makerTraderId;
    UpdateSource makerSource;
    // Aggressive order
    Side takerSide;
    std::int64_t takerOrderId;
    std::int64_t takerTraderId;
    UpdateSource takerSource;
};

struct EventApplyRecord {
    std::uint64_t seq;
    std::int64_t tsExchange;
    std::int64_t tsReceived;

    Side side;
    UpdateType updateType;
    UpdateSource source;

    std::int64_t priceTicks;
    std::int64_t qtyLots;
    std::int64_t orderId;
    std::int64_t traderId;
    std::int64_t aggressorId;
};

class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void onFill(const FillRecord& r) = 0;
    virtual void onEventApply(const EventApplyRecord& r) = 0;
    virtual void reset() {}
};
