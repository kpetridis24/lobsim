#pragma once

#include "types.hpp"

#include <cstdint>
#include <iostream>
#include <list>
#include <optional>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <vector>

constexpr std::int64_t UnknownOrderIdSentinel = -1;
constexpr std::int64_t UnknownTraderIdSentinel = -1;
constexpr std::int64_t UnknownAggressorIdSentinel = -1;
constexpr std::int64_t NoAggressorNeededSentinel = -2;

class IMatchingEngine {
public:
    IMatchingEngine() = default;
    virtual ~IMatchingEngine() = default;

    virtual void update(std::int64_t tsExchange, std::int64_t tsReceived, Side side, UpdateType updateType,
                        std::int64_t priceTicks, std::int64_t quantityLots, std::int64_t orderId,
                        std::int64_t traderId = UnknownTraderIdSentinel,
                        std::int64_t aggressorId = NoAggressorNeededSentinel,
                        UpdateSource updateSource = UpdateSource::HISTORICAL) = 0;

    virtual void initFromL2Snapshot(const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
                                    const std::vector<std::int64_t>& quantities) = 0;

    virtual void initFromL3Snapshot(const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
                                    const std::vector<std::int64_t>& quantities,
                                    const std::vector<std::int64_t>& orderIds,
                                    const std::vector<std::int64_t>& traderIds) = 0;
};
