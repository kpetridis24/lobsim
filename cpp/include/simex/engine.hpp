#pragma once

#include "types.hpp"

#include <cstdint>
#include <deque>
#include <iostream>
#include <optional>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <vector>

constexpr std::int64_t UnknownOrderIdSentinel = -1;
constexpr std::int64_t UnknownTraderIdSentinel = -1;
constexpr std::int64_t UnknownAggressorIdSentinel = -1;
constexpr std::int64_t NoAggressorNeededSentinel = -2;

using SidePriceQuantityTicksTriplet = std::tuple<std::int8_t, std::int64_t, std::int64_t>;

class IMatchingEngine
{
public:
    IMatchingEngine() = default;
    virtual ~IMatchingEngine() = default;

    virtual void update(std::int64_t tsExchange, std::int64_t tsReceived, Side side, UpdateType updateType,
                        std::int64_t priceTicks, std::int64_t quantityLots, std::int64_t orderId,
                        std::int64_t traderId = UnknownTraderIdSentinel,
                        std::int64_t aggressorId = NoAggressorNeededSentinel,
                        UpdateSource updateSource = UpdateSource::HISTORICAL) = 0;

    virtual void initFromL2Snapshot(std::vector<Side>& sides, std::vector<std::int64_t>& prices,
                                    std::vector<std::int64_t>& quantities) = 0;

    virtual void initFromL3Snapshot(std::vector<Side>& sides, std::vector<std::int64_t>& prices,
                                    std::vector<std::int64_t>& quantities, std::vector<std::int64_t>& orderIds,
                                    std::vector<std::int64_t>& traderIds) = 0;
};

class PaperTradingSimulatorCore final : public IMatchingEngine
{
public:
    PaperTradingSimulatorCore() : IMatchingEngine() {}
    PaperTradingSimulatorCore(std::vector<Side>& sides, std::vector<std::int64_t>& prices,
                              std::vector<std::int64_t>& quantities)
        : IMatchingEngine()
    {
        initFromL2Snapshot(sides, prices, quantities);
    }
    ~PaperTradingSimulatorCore() = default;

    void update(std::int64_t tsExchange, std::int64_t tsReceived, Side side, UpdateType updateType,
                std::int64_t priceTicks, std::int64_t quantityLots, std::int64_t orderId,
                std::int64_t traderId = UnknownTraderIdSentinel, std::int64_t aggressorId = NoAggressorNeededSentinel,
                UpdateSource updateSource = UpdateSource::HISTORICAL) override;

    void initFromL2Snapshot(std::vector<Side>& sides, std::vector<std::int64_t>& prices,
                            std::vector<std::int64_t>& quantities) override;

    void initFromL3Snapshot(std::vector<Side>& sides, std::vector<std::int64_t>& prices,
                            std::vector<std::int64_t>& quantities, std::vector<std::int64_t>& orderIds,
                            std::vector<std::int64_t>& traderIds) override;

    std::optional<std::int64_t> depthAt(Side side, std::int64_t priceTicks) const;
    std::vector<std::pair<std::int64_t, std::int64_t>> l2TopN(Side side, std::uint32_t n) const;

private:
    using OrderTraderQuantityTriplet = std::tuple<std::int64_t, std::int64_t, std::int64_t>;
    using OrderPriorityQueue = std::deque<OrderTraderQuantityTriplet>;
    using Book = std::unordered_map<std::int64_t, OrderPriorityQueue>;

    // PriceTicks -> FIFO queue of orders sitting on that tick
    Book bids;
    Book asks;
    // Convention is to maintain both as max heaps. Asks must be inserted with the sign reversed
    std::priority_queue<std::int64_t> bidsHeap;
    std::priority_queue<std::int64_t> asksHeap;
};
