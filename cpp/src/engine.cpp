#include "simex/engine.hpp"

#include <numeric>
#include <stdexcept>

void PaperTradingSimulatorCore::update(std::int64_t tsExchange, std::int64_t tsReceived, Side side,
                                       UpdateType updateType, std::int64_t priceTicks, std::int64_t quantityLots,
                                       std::int64_t orderId, std::int64_t traderId, std::int64_t aggressorId,
                                       UpdateSource updateSource) {
    switch (updateType) {
    case UpdateType::ADD:
        onAdd(side, priceTicks, quantityLots, orderId, traderId, updateSource);
        break;

    case UpdateType::CANCEL:
        /* code */
        break;

    case UpdateType::DELETE:
        /* code */
        break;

    case UpdateType::MATCH:
        /* code */
        break;

    case UpdateType::SET:
        /* code */
        break;

    default:
        break;
    }
}

void PaperTradingSimulatorCore::onAdd(Side side, std::int64_t priceTicks, std::int64_t quantityLots,
                                      std::int64_t orderId, std::int64_t traderId, UpdateSource updateSource) {
    const bool paper = updateSource == UpdateSource::STRATEGY;
    const bool isBid = side == Side::BUY;

    if (orderInfo.contains(orderId)) {
        // TODO: log duplicate orderId. Shouldn't happen on ADD
        return;
    }

    auto& ownBook = isBid ? bids : asks;
    auto& ownHeap = isBid ? bidsHeap : asksHeap;

    auto& oppBook = isBid ? asks : bids;
    auto& oppHeapR = isBid ? asksHeap : bidsHeap;

    const bool oppIsAsk = isBid;

    std::priority_queue<std::int64_t> oppHeapCopy;
    if (paper) {
        oppHeapCopy = oppHeapR;
    }

    auto bestOpp = [&]() -> std::optional<std::int64_t> {
        if (paper) {
            return bestOppositePrice(oppIsAsk, oppBook, oppHeapCopy);
        }
        return bestOppositePrice(oppIsAsk, oppBook, oppHeapR);
    };

    auto bestPxOpt = bestOpp();
    const bool crosses = bestPxOpt && (isBid ? (priceTicks >= *bestPxOpt) : (priceTicks <= *bestPxOpt));

    std::int64_t remaining = quantityLots;

    if (crosses) {
        while (remaining > 0) {
            bestPxOpt = bestOpp();
            if (!bestPxOpt) {
                break;
            }
            const std::int64_t bestPx = *bestPxOpt;
            const bool stillCrosses = isBid ? (priceTicks >= bestPx) : (priceTicks <= bestPx);
            if (!stillCrosses) {
                break;
            }

            auto itLevel = oppBook.find(bestPx);
            if (itLevel == oppBook.end() || itLevel->second.empty()) {
                if (paper) {
                    if (!oppHeapCopy.empty()) {
                        oppHeapCopy.pop();
                    }
                }
                continue;
            }

            auto& levelList = itLevel->second;

            if (paper) {
                for (const auto& node : levelList) {
                    if (remaining <= 0) {
                        break;
                    }
                    const auto makerOrderId = std::get<0>(node);
                    const auto makerTraderId = std::get<1>(node);
                    const auto makerQty = std::get<2>(node);
                    if (makerQty <= 0) {
                        continue;
                    }

                    const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);
                    // TODO: here emit fill (tsExchange, tsReceived, bestPx, take, makerOrderId, makerTraderId, orderId,
                    // traderId, updateSource)
                    remaining -= take;
                }

                if (!oppHeapCopy.empty()) {
                    oppHeapCopy.pop();
                }
            } else {
                auto itNode = levelList.begin();
                while (remaining > 0 && itNode != levelList.end()) {
                    auto& node = *itNode;
                    const auto makerOrderId = std::get<0>(node);
                    const auto makerTraderId = std::get<1>(node);
                    auto& makerQty = std::get<2>(node);

                    if (makerQty <= 0) {
                        orderInfo.erase(makerOrderId);
                        itNode = levelList.erase(itNode);
                        continue;
                    }

                    const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);
                    // TODO: here emit fill (tsExchange, tsReceived, bestPx, take, makerOrderId, makerTraderId, orderId,
                    // traderId, updateSource)

                    makerQty -= take;
                    remaining -= take;

                    if (makerQty == 0) {
                        orderInfo.erase(makerOrderId);
                        itNode = levelList.erase(itNode);
                    } else {
                        ++itNode;
                    }
                }

                if (levelList.empty()) {
                    oppBook.erase(bestPx);
                }
            }
        }
    }

    if (paper) {
        // TODO: currently doesn't support keeping the paper order. In future maybe maintain it for future.
        return;
    }

    if (remaining > 0) {
        auto& level = ownBook[priceTicks];
        const bool newLevel = level.empty();
        level.emplace_back(orderId, traderId, remaining);
        auto itNew = std::prev(level.end());
        if (newLevel) {
            ownHeap.push(isBid ? priceTicks : -priceTicks);
        }
        orderInfo.emplace(orderId, std::make_tuple(side, priceTicks, itNew));
    }
}

std::optional<std::int64_t>
PaperTradingSimulatorCore::bestOppositePrice(bool oppositeIsAsk, const Book& oppositeBook,
                                             std::priority_queue<std::int64_t>& oppositeHeap) {
    while (!oppositeHeap.empty()) {
        const std::int64_t px = oppositeIsAsk ? -oppositeHeap.top() : oppositeHeap.top();
        auto it = oppositeBook.find(px);
        if (it != oppositeBook.end() && !it->second.empty()) {
            return px;
        }
        oppositeHeap.pop();
    }
    return std::nullopt;
}

void PaperTradingSimulatorCore::initFromL2Snapshot(std::vector<Side>& sides, std::vector<std::int64_t>& prices,
                                                   std::vector<std::int64_t>& quantities) {
    auto N = sides.size();
    if ((N != prices.size()) || (N != quantities.size())) {
        throw std::runtime_error("All arrays must have the same size.");
    }

    for (int i = 0; i < static_cast<int>(N); ++i) {
        auto side = sides[i];
        auto price = prices[i];
        auto quantity = quantities[i];

        bool isBid = side == Side::BUY;
        auto& book = isBid ? bids : asks;
        auto& heap = isBid ? bidsHeap : asksHeap;
        int sign = isBid ? 1 : -1;

        OrderTraderQuantityTriplet otq{UnknownOrderIdSentinel, UnknownTraderIdSentinel, quantity};

        if (book.find(price) != book.end()) {
            throw std::runtime_error("Duplicate price found. Initializing from L2 snapshot requires volumes to be "
                                     "aggregated per price-level.");
        }

        book.emplace(price, std::list<OrderTraderQuantityTriplet>{otq});
        heap.push(static_cast<std::int64_t>(sign * price));
    }
}

void PaperTradingSimulatorCore::initFromL3Snapshot(std::vector<Side>& sides, std::vector<std::int64_t>& prices,
                                                   std::vector<std::int64_t>& quantities,
                                                   std::vector<std::int64_t>& orderIds,
                                                   std::vector<std::int64_t>& traderIds) {
    auto N = sides.size();
    if ((N != prices.size()) || (N != quantities.size()) || (N != orderIds.size()) || (N != traderIds.size())) {
        throw std::runtime_error("All arrays must have the same size.");
    }

    for (int i = 0; i < static_cast<int>(N); ++i) {
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

        book.emplace(price, std::list<OrderTraderQuantityTriplet>{otq});
        heap.push(static_cast<std::int64_t>(sign * price));
        orderInfo.emplace(orderId, std::make_tuple(side, price, book.at(price).begin()));
    }
}

std::optional<std::int64_t> PaperTradingSimulatorCore::depthAt(Side side, std::int64_t priceTicks) const {
    bool isBid = side == Side::BUY;
    auto& book = isBid ? bids : asks;
    auto it = book.find(priceTicks);
    if (it == book.end()) {
        return std::nullopt;
    }

    const std::list<OrderTraderQuantityTriplet>& priorityQueue = it->second;
    std::int64_t totalLiquidity = std::accumulate(
        priorityQueue.begin(), priorityQueue.end(), std::int64_t{0},
        [](std::int64_t acc, const OrderTraderQuantityTriplet& triplet) { return acc + std::get<2>(triplet); });
    return totalLiquidity;
}

std::vector<std::pair<std::int64_t, std::int64_t>> PaperTradingSimulatorCore::l2TopN(Side side, std::uint32_t n) const {
    bool isBid = side == Side::BUY;
    auto& book = isBid ? bids : asks;
    if (book.empty()) {
        return {};
    }
    int numItems = static_cast<int>(book.size());
    std::vector<std::pair<std::int64_t, std::int64_t>> pvs;
    pvs.reserve(numItems);

    std::transform(book.begin(), book.end(), std::back_inserter(pvs), [](const auto& p) {
        std::int64_t price = p.first;
        const std::list<OrderTraderQuantityTriplet>& priorityQueue = p.second;
        std::int64_t totalLiquidity = std::accumulate(
            priorityQueue.begin(), priorityQueue.end(), std::int64_t{0},
            [](std::int64_t acc, const OrderTraderQuantityTriplet& triplet) { return acc + std::get<2>(triplet); });
        return std::make_pair(price, totalLiquidity);
    });

    if (side == Side::BUY) {
        std::sort(pvs.begin(), pvs.end(), std::greater<>());
    } else {
        std::sort(pvs.begin(), pvs.end());
    }

    const std::size_t limit = std::min<std::size_t>(n, pvs.size());
    pvs.resize(limit);
    return pvs;
}
