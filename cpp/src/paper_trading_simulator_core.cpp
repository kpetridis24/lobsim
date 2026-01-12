#include "simex/paper_trading_simulator_core.hpp"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <numeric>
#include <stdexcept>

/**
 * TODO:
 * 1. Design and implement logging/emitting of events.
 * 2. Perhaps use an internal order/event flag (monotonic seq) for reproducibility/determinism.
 * 3. For now, if a strategy wants to execute a market order, it should do it with an ADD call at
 *    the appropriate price level. Maybe make it easier for the client to insert MARKET orders
 *    by providing a new API to be used by the aggressor - AGGRESSIVE_MATCH.
 * 4. Add checks for out-of-limits values -> reject event completely.
 * 5. Implement l2TopN efficiently.
 */

void PaperTradingSimulatorCore::update(const NormalizedLobEvent& event) {
    if (sink) {
        sink->onEventApply(EventApplyRecord{seq, event.tsExchange, event.tsReceived, event.side, event.updateType,
                                            event.updateSource, event.priceTicks, event.quantityLots, event.orderId,
                                            event.traderId, event.aggressorId});
    }
    switch (event.updateType) {
    case UpdateType::ADD:
        onAdd(event);
        break;

    case UpdateType::DELETE:
        onDelete(event);
        break;

    case UpdateType::SUBTRACT:
        onSubtract(event);
        break;

    case UpdateType::MATCH:
        onMatch(event);
        break;

    case UpdateType::SET:
        onSet(event);
        break;

    default:
        throw std::runtime_error("Unknown updateType found.");
    }

    ++seq;
}

void PaperTradingSimulatorCore::onAdd(const NormalizedLobEvent& event) {
    if (event.quantityLots < 0) {
        throw std::runtime_error("Negative ADD quantity found.");
    }

    const bool paper = event.updateSource == UpdateSource::STRATEGY;
    const bool isBid = event.side == Side::BUY;

    if (orderInfo.contains(event.orderId)) {
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
    const bool crosses = bestPxOpt && (isBid ? (event.priceTicks >= *bestPxOpt) : (event.priceTicks <= *bestPxOpt));

    std::int64_t remaining = event.quantityLots;

    if (crosses) {
        while (remaining > 0) {
            bestPxOpt = bestOpp();
            if (!bestPxOpt) {
                break;
            }
            const std::int64_t bestPx = *bestPxOpt;
            const bool stillCrosses = isBid ? (event.priceTicks >= bestPx) : (event.priceTicks <= bestPx);
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
                    auto makerOrderId = std::get<0>(node);
                    const auto makerTraderId = std::get<1>(node);
                    const auto makerQty = std::get<2>(node);
                    const auto makerSource = std::get<3>(node);

                    if (makerQty <= 0) {
                        continue;
                    }

                    const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);
                    if (sink) {
                        auto makerSide = event.side == Side::BUY ? Side::SELL : Side::BUY;
                        sink->onFill(FillRecord{seq, event.tsExchange, event.tsReceived, bestPx, take, makerSide,
                                                makerOrderId, makerTraderId, makerSource, event.side, event.orderId,
                                                event.traderId, event.updateSource});
                    }
                    remaining -= take;
                }

                if (!oppHeapCopy.empty()) {
                    // Pop *all* duplicates of this same bestPx from the heap copy
                    const auto decode = [&](std::int64_t top) { return oppIsAsk ? -top : top; };
                    while (!oppHeapCopy.empty() && decode(oppHeapCopy.top()) == bestPx) {
                        oppHeapCopy.pop();
                    }
                }
            } else {
                auto itNode = levelList.begin();
                while (remaining > 0 && itNode != levelList.end()) {
                    auto& node = *itNode;
                    const auto makerOrderId = std::get<0>(node);
                    const auto makerTraderId = std::get<1>(node);
                    auto& makerQty = std::get<2>(node);
                    auto& makerSource = std::get<3>(node);

                    if (makerQty <= 0) {
                        orderInfo.erase(makerOrderId);
                        itNode = levelList.erase(itNode);
                        continue;
                    }

                    const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);

                    if (sink) {
                        auto makerSide = event.side == Side::BUY ? Side::SELL : Side::BUY;
                        sink->onFill(FillRecord{seq, event.tsExchange, event.tsReceived, bestPx, take, makerSide,
                                                makerOrderId, makerTraderId, makerSource, event.side, event.orderId,
                                                event.traderId, event.updateSource});
                    }

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
        auto& level = ownBook[event.priceTicks];
        const bool newLevel = level.empty();
        level.emplace_back(event.orderId, event.traderId, remaining, event.updateSource);
        auto itNew = std::prev(level.end());
        if (newLevel) {
            ownHeap.push(isBid ? event.priceTicks : -event.priceTicks);
        }
        orderInfo.emplace(event.orderId, std::make_tuple(event.side, event.priceTicks, itNew));
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

void PaperTradingSimulatorCore::onDelete(const NormalizedLobEvent& event) {
    auto it = orderInfo.find(event.orderId);
    if (it == orderInfo.end()) {
        // TODO: log DELETE on non-existing orderId.
        // TODO: figure out how to handle paper orders in the future (no they aren't stored).
        return;
    }

    auto& info = it->second;

    auto storedSide = std::get<0>(info);
    if (storedSide != event.side) {
        // TODO: log that given side doesn't match with what is stored for this orderId. Warn for corrupted data.
    }

    auto storedPriceTicks = std::get<1>(info);
    if (storedPriceTicks != event.priceTicks) {
        // TODO: log that given price doesn't match with what is stored for this orderId. Warn for corrupted data.
    }

    auto queueLocationIt = std::get<2>(info);

    // Important! Use stored side (truth) to access. If provided side is different, it has been logged as corrupt.
    // The design decision here is to complete the DELETE based only on the provided orderId.
    const bool isBid = storedSide == Side::BUY;
    auto& book = isBid ? bids : asks;

    auto bIt = book.find(storedPriceTicks);
    if (bIt == book.end()) {
        throw std::runtime_error("Corrupt book. Price found in orderInfo but not present in book.");
    }

    OrderPriorityQueue& queue = bIt->second;
    queue.erase(queueLocationIt);

    if (queue.empty()) {
        book.erase(bIt);
        // TODO: check if heap behaves properly after this, with stale entry.
    }

    orderInfo.erase(it);
}

void PaperTradingSimulatorCore::onSubtract(const NormalizedLobEvent& event) {
    onPartialOrderCancel(event, false);
}

void PaperTradingSimulatorCore::onMatch(const NormalizedLobEvent& event) {
    onPartialOrderCancel(event, true);
}

void PaperTradingSimulatorCore::onSet(const NormalizedLobEvent& event) {
    if (event.quantityLots < 0) {
        // TODO: log that set with negative liquidity was requested. Set to 0 for non-blocking behaviour.
    }
    auto it = orderInfo.find(event.orderId);
    if (it == orderInfo.end()) {
        // TODO: log SET on non-existing orderId.
        // TODO: figure out how to handle paper orders in the future (no they aren't stored).
        return;
    }

    auto& info = it->second;

    auto storedSide = std::get<0>(info);
    if (storedSide != event.side) {
        // TODO: log that given side doesn't match with what is stored for this orderId. Warn for corrupted data.
    }

    auto storedPriceTicks = std::get<1>(info);
    if (storedPriceTicks != event.priceTicks) {
        // TODO: log that given price doesn't match with what is stored for this orderId. Warn for corrupted data.
    }

    auto queueLocationIt = std::get<2>(info);

    const bool isBid = storedSide == Side::BUY;
    auto& book = isBid ? bids : asks;

    auto bIt = book.find(storedPriceTicks);
    if (bIt == book.end()) {
        throw std::runtime_error("Corrupt book. Price found in orderInfo but not present in book.");
    }

    OrderPriorityQueue& queue = bIt->second;
    OrderTraderQuantitySource& queueElement = *queueLocationIt;

    auto newQuantity = event.quantityLots < 0 ? 0 : event.quantityLots;
    if (newQuantity == 0) {
        queue.erase(queueLocationIt);
        if (queue.empty()) {
            book.erase(bIt);
        }
        orderInfo.erase(it);
    } else {
        std::get<2>(queueElement) = newQuantity;
    }
}

void PaperTradingSimulatorCore::onPartialOrderCancel(const NormalizedLobEvent& event, bool isTradeOnPassiveOrder) {
    if (event.quantityLots < 0) {
        throw std::runtime_error("Negative quantityLots found.");
    }
    if (event.quantityLots == 0) {
        // TODO: log that a order cancel with 0 quantity was requested.
        return;
    }

    auto it = orderInfo.find(event.orderId);
    if (it == orderInfo.end()) {
        // TODO: log SUBTRACT on non-existing orderId.
        // TODO: figure out how to handle paper orders in the future (no they aren't stored).
        return;
    }

    auto& info = it->second;

    auto storedSide = std::get<0>(info);
    if (storedSide != event.side) {
        // TODO: log that given side doesn't match with what is stored for this orderId. Warn for corrupted data.
    }

    auto storedPriceTicks = std::get<1>(info);
    if (storedPriceTicks != event.priceTicks) {
        // TODO: log that given price doesn't match with what is stored for this orderId. Warn for corrupted data.
    }

    auto queueLocationIt = std::get<2>(info);

    // Important! Use stored side (truth) to access. If provided side is different, it has been logged as corrupt.
    // The design decision here is to complete the partial cancel based only on the provided orderId.
    const bool isBid = storedSide == Side::BUY;
    auto& book = isBid ? bids : asks;

    auto bIt = book.find(storedPriceTicks);
    if (bIt == book.end()) {
        throw std::runtime_error("Corrupt book. Price found in orderInfo but not present in book.");
    }

    OrderPriorityQueue& queue = bIt->second;
    OrderTraderQuantitySource& queueElement = *queueLocationIt;

    auto liquidity = std::get<2>(queueElement);
    auto take = std::min<std::int64_t>(liquidity, event.quantityLots);

    if (event.quantityLots > liquidity) {
        // TODO: log that requested SUBTRACT with Q > existing, warning for corrupt data.
    }

    liquidity -= take;

    if (isTradeOnPassiveOrder && sink) {
        auto takerSide = storedSide == Side::BUY ? Side::SELL : Side::BUY;
        auto makerSource = std::get<3>(queueElement);
        auto storedTraderId = std::get<1>(queueElement);
        sink->onFill(FillRecord{seq, event.tsExchange, event.tsReceived, storedPriceTicks, take, storedSide,
                                event.orderId, storedTraderId, makerSource, takerSide, UnknownOrderIdSentinel,
                                UnknownTraderIdSentinel, event.updateSource});
    }

    if (liquidity == 0) {
        queue.erase(queueLocationIt);
        if (queue.empty()) {
            book.erase(bIt);
        }
        orderInfo.erase(it);
    } else {
        std::get<2>(queueElement) = liquidity;
    }

    if (isTradeOnPassiveOrder && (event.updateSource == UpdateSource::STRATEGY)) {
        // TODO: Log warning. If someone wants to do a Market Order via their strategy, they must use an ADD that
        // crosses. In future we'll implement an aggressor API. MATCH is for the passive order ONLY.
    }
}

void PaperTradingSimulatorCore::initFromL2Snapshot(const std::vector<Side>& sides,
                                                   const std::vector<std::int64_t>& prices,
                                                   const std::vector<std::int64_t>& quantities) {
    clearState();
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

        OrderTraderQuantitySource otq{UnknownOrderIdSentinel, UnknownTraderIdSentinel, quantity,
                                      UpdateSource::HISTORICAL};

        if (book.find(price) != book.end()) {
            throw std::runtime_error("Duplicate price found. Initializing from L2 snapshot requires volumes to be "
                                     "aggregated per price-level.");
        }

        book.emplace(price, std::list<OrderTraderQuantitySource>{otq});
        heap.push(static_cast<std::int64_t>(sign * price));
    }
}

void PaperTradingSimulatorCore::initFromL3Snapshot(const std::vector<Side>& sides,
                                                   const std::vector<std::int64_t>& prices,
                                                   const std::vector<std::int64_t>& quantities,
                                                   const std::vector<std::int64_t>& orderIds,
                                                   const std::vector<std::int64_t>& traderIds) {
    clearState();
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

        // Enforce unique live order ids
        if (orderInfo.contains(orderId)) {
            throw std::runtime_error("Duplicate orderId found in L3 snapshot.");
        }

        bool isBid = side == Side::BUY;
        auto& book = isBid ? bids : asks;
        auto& heap = isBid ? bidsHeap : asksHeap;

        // Get/create the price level in the real book
        auto& priorityQueue = book[price];
        const bool newLevel = priorityQueue.empty();

        // Append order at end (FIFO)
        priorityQueue.emplace_back(orderId, traderId, quantity, UpdateSource::HISTORICAL);
        auto itNew = std::prev(priorityQueue.end());

        // Only push price into heap the first time the level appears
        if (newLevel) {
            heap.push(isBid ? price : -price); // asks stored as negative
        }

        orderInfo.emplace(orderId, std::make_tuple(side, price, itNew));
    }
}

void PaperTradingSimulatorCore::clearState() {
    bids.clear();
    asks.clear();
    bidsHeap = std::priority_queue<std::int64_t>();
    asksHeap = std::priority_queue<std::int64_t>();
    orderInfo.clear();
    if (sink) {
        sink->reset();
    }
}

std::optional<std::int64_t> PaperTradingSimulatorCore::depthAt(Side side, std::int64_t priceTicks) const {
    bool isBid = side == Side::BUY;
    auto& book = isBid ? bids : asks;
    auto it = book.find(priceTicks);
    if (it == book.end()) {
        return std::nullopt;
    }

    const std::list<OrderTraderQuantitySource>& priorityQueue = it->second;
    std::int64_t totalLiquidity = std::accumulate(
        priorityQueue.begin(), priorityQueue.end(), std::int64_t{0},
        [](std::int64_t acc, const OrderTraderQuantitySource& triplet) { return acc + std::get<2>(triplet); });
    return totalLiquidity;
}

std::vector<std::pair<std::int64_t, std::int64_t>> PaperTradingSimulatorCore::l2TopN(Side side, std::uint32_t n) const {
    const bool isBid = side == Side::BUY;
    const auto& book = isBid ? bids : asks;
    if (book.empty() || n == 0) {
        return {};
    }

    using Level = std::pair<std::int64_t, std::int64_t>; // price, total liquidity
    auto cmp = [isBid](const Level& a, const Level& b) {
        // Heap keeps the "worst" at top: lowest price for bids, highest price for asks.
        return isBid ? (a.first > b.first) : (a.first < b.first);
    };
    std::priority_queue<Level, std::vector<Level>, decltype(cmp)> heap(cmp);

    for (const auto& [price, queue] : book) {
        const std::int64_t totalLiquidity = std::accumulate(
            queue.begin(), queue.end(), std::int64_t{0},
            [](std::int64_t acc, const OrderTraderQuantitySource& triplet) { return acc + std::get<2>(triplet); });
        if (totalLiquidity <= 0) {
            continue;
        }

        Level lvl{price, totalLiquidity};
        if (heap.size() < n) {
            heap.push(lvl);
        } else {
            if ((isBid && lvl.first > heap.top().first) || (!isBid && lvl.first < heap.top().first)) {
                heap.pop();
                heap.push(lvl);
            }
        }
    }

    std::vector<Level> top;
    top.reserve(heap.size());
    while (!heap.empty()) {
        top.push_back(heap.top());
        heap.pop();
    }

    if (isBid) {
        std::sort(top.begin(), top.end(), [](const Level& a, const Level& b) { return a.first > b.first; });
    } else {
        std::sort(top.begin(), top.end(), [](const Level& a, const Level& b) { return a.first < b.first; });
    }

    return top;
}

std::optional<std::int64_t> PaperTradingSimulatorCore::getBestPriceTicks(Side side) const {
    const bool isBid = side == Side::BUY;
    const auto& book = isBid ? bids : asks;
    auto& heap = isBid ? bidsHeap : asksHeap;

    while (!heap.empty()) {
        const std::int64_t px = isBid ? heap.top() : -heap.top();
        auto it = book.find(px);
        if (it != book.end() && !it->second.empty()) {
            return px;
        }
        heap.pop();
    }

    return std::nullopt;
}

void PaperTradingSimulatorCore::setLogSink(ILogSink* sink) {
    this->sink = sink;
}
