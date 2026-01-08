#include "simex/engine.hpp"

#include <numeric>
#include <stdexcept>

void PaperTradingSimulatorCore::update(std::int64_t tsExchange, std::int64_t tsReceived, Side side,
                                       UpdateType updateType, std::int64_t priceTicks, std::int64_t quantityLots,
                                       std::int64_t orderId, std::int64_t traderId, std::int64_t aggressorId,
                                       UpdateSource updateSource)
{
}

void PaperTradingSimulatorCore::initFromL2Snapshot(std::vector<Side>& sides, std::vector<std::int64_t>& prices,
                                                   std::vector<std::int64_t>& quantities)
{
    auto N = sides.size();
    if ((N != prices.size()) || (N != quantities.size()))
    {
        throw std::runtime_error("All arrays must have the same size.");
    }

    for (int i = 0; i < static_cast<int>(N); ++i)
    {
        auto side = sides[i];
        auto price = prices[i];
        auto quantity = quantities[i];

        bool isBid = side == Side::BUY;
        auto& book = isBid ? bids : asks;
        auto& heap = isBid ? bidsHeap : asksHeap;
        int sign = isBid ? 1 : -1;

        OrderTraderQuantityTriplet otq{UnknownOrderIdSentinel, UnknownTraderIdSentinel, quantity};

        if (book.find(price) != book.end())
        {
            throw std::runtime_error("Duplicate price found. Initializing from L2 snapshot requires volumes to be "
                                     "aggregated per price-level.");
        }

        book.emplace(price, std::deque<OrderTraderQuantityTriplet>{otq});
        heap.push(static_cast<std::int64_t>(sign * price));
    }
}

void PaperTradingSimulatorCore::initFromL3Snapshot(std::vector<Side>& sides, std::vector<std::int64_t>& prices,
                                                   std::vector<std::int64_t>& quantities,
                                                   std::vector<std::int64_t>& orderIds,
                                                   std::vector<std::int64_t>& traderIds)
{
    auto N = sides.size();
    if ((N != prices.size()) || (N != quantities.size()) || (N != orderIds.size()) || (N != traderIds.size()))
    {
        throw std::runtime_error("All arrays must have the same size.");
    }

    for (int i = 0; i < static_cast<int>(N); ++i)
    {
        auto side = sides[i];
        auto price = prices[i];
        auto quantity = quantities[i];
        auto orderId = orderIds[i];
        auto traderId = traderIds[i];

        bool isBid = side == Side::BUY;
        auto& book = isBid ? bids : asks;
        auto& heap = isBid ? bidsHeap : asksHeap;
        int sign = isBid ? 1 : -1;

        OrderTraderQuantityTriplet otq{orderId, traderId, quantity};

        book.emplace(price, std::deque<OrderTraderQuantityTriplet>{otq});
        heap.push(static_cast<std::int64_t>(sign * price));
    }
}

std::optional<std::int64_t> PaperTradingSimulatorCore::depthAt(Side side, std::int64_t priceTicks) const
{
    bool isBid = side == Side::BUY;
    auto& book = isBid ? bids : asks;
    auto it = book.find(priceTicks);
    if (it == book.end())
    {
        return std::nullopt;
    }

    const std::deque<OrderTraderQuantityTriplet> priorityQueue = it->second;
    std::int64_t totalLiquidity =
        std::accumulate(priorityQueue.begin(), priorityQueue.end(), 0,
                        [&](int acc, const OrderTraderQuantityTriplet& triplet) { return acc + std::get<2>(triplet); });
    return totalLiquidity;
}
