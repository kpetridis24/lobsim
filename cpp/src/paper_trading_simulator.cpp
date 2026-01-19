#include "lobsim/paper_trading_simulator.hpp"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <numeric>
#include <stdexcept>

/**
 * TODO:
 * - For now, if a strategy wants to execute a market order, it should do it with an ADD call at
 *    the appropriate price level. Maybe make it easier for the client to insert MARKET orders
 *    by providing a new API to be used by the aggressor - AGGRESSIVE_MATCH.
 * - Add checks for out-of-limits values -> reject event completely.
 * - L2 init cannot support exact paper order trading because we reject MODIFYs with orderIds not present.
 * - If a SET with non-existing order ID arrives, it is rejected now. Future work: track invalid events,
 *    and try to reconcile them later. For example if an ADD arrives later with a TS earlier than the SET
 *    that was rejected, it means that the ADD should have happend first (feed delays), so we must reconcile
 *    and maybe adjust the order book
 */

void PaperTradingSimulator::update(const NormalizedLobEvent& event) {
    if (sink) {
        sink->onEventApply(EventApplyRecord{seq, event.tsExchange, event.tsReceived, event.side, event.updateType,
                                            event.updateSource, event.priceTicks, event.quantityLots, event.orderId,
                                            event.traderId, event.aggressorId, event.symbolId});
    }
    switch (event.updateType) {
    case UpdateType::ADD:
        onAdd(event);
        break;
    case UpdateType::AGGRESSIVE_TRADE:
        onAggressiveTrade(event);
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
        emitDiagnostic(event, DiagnosticRecordCode::INVALID_UPDATE_TYPE, DiagnosticRecordSeverity::ERROR);
    }

    ++seq;
}

void PaperTradingSimulator::onAdd(const NormalizedLobEvent& event) {
    if (event.quantityLots < 0) {
        emitDiagnostic(event, DiagnosticRecordCode::ADD_INVOKED_WITH_NEGATIVE_QUANTITY,
                       DiagnosticRecordSeverity::ERROR);
        return;
    }

    const bool paper = event.updateSource == UpdateSource::STRATEGY;
    const bool isBid = event.side == Side::BUY;

    if (orderInfo.contains(event.orderId) || paperOrders.contains(event.orderId) ||
        paperOrderInfo.contains(event.orderId)) {
        emitDiagnostic(event, DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID, DiagnosticRecordSeverity::WARNING);
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
                                                event.traderId, event.updateSource, event.symbolId});
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
                std::int64_t tradedAtLevel = 0;
                std::vector<std::pair<std::uint64_t, std::int64_t>> marketDeltas;
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
                                                event.traderId, event.updateSource, event.symbolId});
                    }

                    marketDeltas.emplace_back(std::get<4>(node), -take);

                    makerQty -= take;
                    remaining -= take;
                    tradedAtLevel += take;

                    if (makerQty == 0) {
                        orderInfo.erase(makerOrderId);
                        itNode = levelList.erase(itNode);
                    } else {
                        ++itNode;
                    }
                }

                if (tradedAtLevel > 0) {
                    auto makerSide = event.side == Side::BUY ? Side::SELL : Side::BUY;
                    applyPaperTradeAtLevel(makerSide, bestPx, tradedAtLevel, event);
                    if (auto paperLevel = findPaperLevel(makerSide, bestPx)) {
                        for (const auto& [seq, delta] : marketDeltas) {
                            auto idxIt = paperLevel->marketIndexBySeq.find(seq);
                            if (idxIt != paperLevel->marketIndexBySeq.end()) {
                                paperLevel->marketQty.add(idxIt->second, delta);
                            }
                        }
                    }
                }

                if (levelList.empty()) {
                    oppBook.erase(bestPx);
                }
            }
        }
    }

    // If historical aggressor still crosses paper orders, trade against them
    if (!paper && remaining > 0) {
        auto bestPaperOpp = [&]() { return bestPaperOppositePrice(oppIsAsk); };
        while (remaining > 0) {
            auto paperPx = bestPaperOpp();
            if (!paperPx.has_value()) {
                break;
            }
            const bool stillCrosses = isBid ? (event.priceTicks >= *paperPx) : (event.priceTicks <= *paperPx);
            if (!stillCrosses) {
                break;
            }
            const auto traded = tradeAgainstPaperLevel(oppIsAsk ? Side::SELL : Side::BUY, *paperPx, remaining, event);
            if (traded <= 0) {
                // Nothing traded; avoid infinite loop
                break;
            }
            remaining -= traded;
        }
    }

    if (remaining > 0) {
        if (paper) {
            auto& level = ensurePaperLevel(event.side, event.priceTicks);
            PaperOrder order{};
            order.originalEvent = event;
            order.status = remaining < event.quantityLots ? PaperOrderStatus::PARTIALLY_FILLED : PaperOrderStatus::OPEN;
            order.remainingQty = remaining;
            order.placementSeq = orderArrivalSeq++;
            const std::size_t paperIndex = level.nextPaperIndex++;
            level.paperQty.ensureSize(level.nextPaperIndex);
            level.paperQty.add(paperIndex, remaining);
            order.paperIndex = paperIndex;
            paperOrders.emplace(event.orderId, order);
            level.orders.push_back(event.orderId);
            auto itNew = std::prev(level.orders.end());
            paperOrderInfo.emplace(event.orderId, std::make_tuple(event.side, event.priceTicks, itNew));
            level.queuedLots += remaining;
        } else {
            auto& level = ownBook[event.priceTicks];
            const bool newLevel = level.empty();
            level.emplace_back(event.orderId, event.traderId, remaining, event.updateSource, orderArrivalSeq++);
            auto itNew = std::prev(level.end());
            if (newLevel) {
                ownHeap.push(isBid ? event.priceTicks : -event.priceTicks);
            }
            orderInfo.emplace(event.orderId, std::make_tuple(event.side, event.priceTicks, itNew));
            if (auto paperLevel = findPaperLevel(event.side, event.priceTicks)) {
                const auto seq = std::get<4>(*itNew);
                const std::size_t idx = paperLevel->marketSeqs.size();
                paperLevel->marketSeqs.push_back(seq);
                paperLevel->marketIndexBySeq.emplace(seq, idx);
                paperLevel->marketQty.ensureSize(paperLevel->marketSeqs.size());
                paperLevel->marketQty.add(idx, remaining);
            }
        }
    }
}

void PaperTradingSimulator::onAggressiveTrade(const NormalizedLobEvent& event) {
    if (event.updateSource != UpdateSource::STRATEGY) {
        emitDiagnostic(event, DiagnosticRecordCode::INVALID_UPDATE_TYPE, DiagnosticRecordSeverity::ERROR);
        return;
    }
    if (event.quantityLots <= 0) {
        emitDiagnostic(event, DiagnosticRecordCode::ADD_INVOKED_WITH_NEGATIVE_QUANTITY,
                       DiagnosticRecordSeverity::ERROR);
        return;
    }

    const bool isBid = event.side == Side::BUY;
    auto& oppBook = isBid ? asks : bids;
    auto oppHeapCopy = isBid ? asksHeap : bidsHeap;
    const bool oppIsAsk = isBid;

    std::int64_t remaining = event.quantityLots;

    auto bestOpp = [&]() -> std::optional<std::int64_t> {
        while (!oppHeapCopy.empty()) {
            const std::int64_t px = oppIsAsk ? -oppHeapCopy.top() : oppHeapCopy.top();
            auto it = oppBook.find(px);
            if (it != oppBook.end() && !it->second.empty()) {
                return px;
            }
            oppHeapCopy.pop();
        }
        return std::nullopt;
    };

    // Consume historical book without mutating it
    while (remaining > 0) {
        auto bestPxOpt = bestOpp();
        if (!bestPxOpt) {
            break;
        }
        const auto bestPx = *bestPxOpt;
        auto itLevel = oppBook.find(bestPx);
        if (itLevel == oppBook.end() || itLevel->second.empty()) {
            continue;
        }
        const auto& levelList = itLevel->second;
        for (const auto& node : levelList) {
            if (remaining <= 0) {
                break;
            }
            const auto makerOrderId = std::get<0>(node);
            const auto makerTraderId = std::get<1>(node);
            const auto makerQty = std::get<2>(node);
            const auto makerSource = std::get<3>(node);
            if (makerQty <= 0) {
                continue;
            }
            const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);
            if (sink) {
                auto makerSide = event.side == Side::BUY ? Side::SELL : Side::BUY;
                sink->onFill(FillRecord{seq, event.tsExchange, event.tsReceived, bestPx, take, makerSide, makerOrderId,
                                        makerTraderId, makerSource, event.side, event.orderId, event.traderId,
                                        event.updateSource, event.symbolId});
            }
            remaining -= take;
        }

        // pop duplicates of this price from the heap copy to progress
        while (!oppHeapCopy.empty()) {
            const auto pxTop = oppIsAsk ? -oppHeapCopy.top() : oppHeapCopy.top();
            if (pxTop != bestPx) {
                break;
            }
            oppHeapCopy.pop();
        }
    }

    // Consume resting paper orders on the opposite side
    if (remaining > 0) {
        auto bestPaperOpp = [&]() { return bestPaperOppositePrice(oppIsAsk); };
        while (remaining > 0) {
            auto paperPx = bestPaperOpp();
            if (!paperPx) {
                break;
            }
            const auto traded = tradeAgainstPaperLevel(oppIsAsk ? Side::SELL : Side::BUY, *paperPx, remaining, event);
            if (traded <= 0) {
                break;
            }
            remaining -= traded;
        }
    }

    ++seq;
}

std::optional<std::int64_t> PaperTradingSimulator::bestOppositePrice(bool oppositeIsAsk, const Book& oppositeBook,
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

std::optional<std::int64_t> PaperTradingSimulator::bestPaperOppositePrice(bool oppositeIsAsk) {
    const auto& levels = oppositeIsAsk ? paperAsks : paperBids;
    std::optional<std::int64_t> best;
    for (const auto& [px, level] : levels) {
        if (level.queuedLots <= 0) {
            continue;
        }
        if (!best) {
            best = px;
            continue;
        }
        if (oppositeIsAsk) {
            if (px < *best) {
                best = px;
            }
        } else {
            if (px > *best) {
                best = px;
            }
        }
    }
    return best;
}

PaperTradingSimulator::PaperOrderLevel& PaperTradingSimulator::ensurePaperLevel(Side side, std::int64_t priceTicks) {
    auto& levels = side == Side::BUY ? paperBids : paperAsks;
    auto [it, inserted] = levels.try_emplace(priceTicks);
    if (inserted) {
        auto& level = it->second;
        const auto& book = side == Side::BUY ? bids : asks;
        auto bookIt = book.find(priceTicks);
        if (bookIt != book.end()) {
            const auto& queue = bookIt->second;
            level.marketSeqs.reserve(queue.size());
            level.marketQty.ensureSize(queue.size());
            std::size_t idx = 0;
            for (const auto& node : queue) {
                const auto seq = std::get<4>(node);
                const auto qty = std::get<2>(node);
                level.marketSeqs.push_back(seq);
                level.marketIndexBySeq.emplace(seq, idx);
                level.marketQty.ensureSize(idx + 1);
                level.marketQty.add(idx, qty);
                ++idx;
            }
        }
    }
    return it->second;
}

PaperTradingSimulator::PaperOrderLevel* PaperTradingSimulator::findPaperLevel(Side side, std::int64_t priceTicks) {
    auto& levels = side == Side::BUY ? paperBids : paperAsks;
    auto it = levels.find(priceTicks);
    if (it == levels.end()) {
        return nullptr;
    }
    return &it->second;
}

void PaperTradingSimulator::applyPaperTradeAtLevel(Side passiveSide, std::int64_t priceTicks, std::int64_t tradeLots,
                                                   const NormalizedLobEvent& aggressor) {
    if (tradeLots <= 0) {
        return;
    }

    auto& levels = passiveSide == Side::BUY ? paperBids : paperAsks;
    auto levelIt = levels.find(priceTicks);
    if (levelIt == levels.end()) {
        return;
    }

    auto& level = levelIt->second;
    auto it = level.orders.begin();
    std::int64_t marketConsumed = 0;
    const auto marketAhead = [&](std::uint64_t placementSeq) {
        if (level.marketSeqs.empty()) {
            return std::int64_t{0};
        }
        const auto pos =
            static_cast<std::size_t>(std::lower_bound(level.marketSeqs.begin(), level.marketSeqs.end(), placementSeq) -
                                     level.marketSeqs.begin());
        return level.marketQty.sum(pos);
    };
    while (it != level.orders.end() && tradeLots > 0) {
        const auto orderId = *it;
        auto orderIt = paperOrders.find(orderId);
        if (orderIt == paperOrders.end()) {
            it = level.orders.erase(it);
            paperOrderInfo.erase(orderId);
            continue;
        }

        auto& order = orderIt->second;
        const auto totalMarketAhead = marketAhead(order.placementSeq);
        const auto aheadMarket =
            totalMarketAhead > marketConsumed ? (totalMarketAhead - marketConsumed) : std::int64_t{0};
        const auto aheadPaper = level.paperQty.sum(order.paperIndex);
        const auto queueAhead = aheadMarket + aheadPaper;
        if (queueAhead >= tradeLots) {
            break;
        }
        tradeLots -= queueAhead;
        marketConsumed += aheadMarket;

        if (order.remainingQty <= 0) {
            auto next = std::next(it);
            level.orders.erase(it);
            paperOrderInfo.erase(orderId);
            paperOrders.erase(orderIt);
            it = next;
            continue;
        }

        const auto fillQty = std::min(order.remainingQty, tradeLots);
        if (fillQty <= 0) {
            ++it;
            continue;
        }

        if (sink) {
            const bool eventIsAggressor = aggressor.updateType == UpdateType::ADD;
            auto takerSide = passiveSide == Side::BUY ? Side::SELL : Side::BUY;
            auto takerOrderId = eventIsAggressor ? aggressor.orderId : UnknownOrderIdSentinel;
            auto takerTraderId = eventIsAggressor ? aggressor.traderId : UnknownTraderIdSentinel;
            auto takerSource = aggressor.updateSource;

            sink->onFill(FillRecord{seq, aggressor.tsExchange, aggressor.tsReceived, priceTicks, fillQty, passiveSide,
                                    orderId, order.originalEvent.traderId, UpdateSource::STRATEGY, takerSide,
                                    takerOrderId, takerTraderId, takerSource, aggressor.symbolId});
        }

        tradeLots -= fillQty;

        if (fillQty < order.remainingQty) {
            order.remainingQty -= fillQty;
            level.queuedLots -= fillQty;
            level.paperQty.add(order.paperIndex, -fillQty);
            order.status = PaperOrderStatus::PARTIALLY_FILLED;
            ++it;
        } else {
            auto next = std::next(it);
            removePaperOrder(level, it, fillQty, PaperOrderStatus::FILLED);
            it = next;
        }
    }

    if (level.orders.empty()) {
        levels.erase(levelIt);
    }
}

std::int64_t PaperTradingSimulator::tradeAgainstPaperLevel(Side passiveSide, std::int64_t priceTicks,
                                                           std::int64_t tradeLots,
                                                           const NormalizedLobEvent& aggressor) {
    if (tradeLots <= 0) {
        return 0;
    }
    auto& levels = passiveSide == Side::BUY ? paperBids : paperAsks;
    auto levelIt = levels.find(priceTicks);
    if (levelIt == levels.end()) {
        return 0;
    }
    auto& level = levelIt->second;
    auto it = level.orders.begin();
    std::int64_t consumed = 0;
    while (tradeLots > 0 && it != level.orders.end()) {
        const auto orderId = *it;
        auto orderIt = paperOrders.find(orderId);
        if (orderIt == paperOrders.end()) {
            auto next = std::next(it);
            level.orders.erase(it);
            paperOrderInfo.erase(orderId);
            it = next;
            continue;
        }
        auto& order = orderIt->second;
        if (order.remainingQty <= 0 || order.status == PaperOrderStatus::FILLED ||
            order.status == PaperOrderStatus::CANCELLED || order.status == PaperOrderStatus::REJECTED) {
            auto next = std::next(it);
            level.orders.erase(it);
            paperOrderInfo.erase(orderId);
            paperOrders.erase(orderIt);
            it = next;
            continue;
        }

        const auto fillQty = std::min(order.remainingQty, tradeLots);
        if (fillQty > 0 && sink) {
            auto makerSide = passiveSide;
            auto takerSide = passiveSide == Side::BUY ? Side::SELL : Side::BUY;
            sink->onFill(FillRecord{seq, aggressor.tsExchange, aggressor.tsReceived, priceTicks, fillQty, makerSide,
                                    orderId, order.originalEvent.traderId, UpdateSource::STRATEGY, takerSide,
                                    aggressor.orderId, aggressor.traderId, aggressor.updateSource, aggressor.symbolId});
        }

        order.remainingQty -= fillQty;
        tradeLots -= fillQty;
        consumed += fillQty;

        if (order.remainingQty == 0) {
            auto next = std::next(it);
            removePaperOrder(level, it, fillQty, PaperOrderStatus::FILLED);
            it = next;
        } else {
            order.status = PaperOrderStatus::PARTIALLY_FILLED;
            level.queuedLots -= fillQty;
            level.paperQty.add(order.paperIndex, -fillQty);
            ++it;
        }
    }

    if (level.orders.empty()) {
        levels.erase(levelIt);
    }
    return consumed;
}

void PaperTradingSimulator::removePaperOrder(PaperOrderLevel& level, PaperOrderQueue::iterator it,
                                             std::int64_t removedQty, PaperOrderStatus status) {
    const auto orderId = *it;
    auto orderIt = paperOrders.find(orderId);
    if (orderIt == paperOrders.end()) {
        level.orders.erase(it);
        paperOrderInfo.erase(orderId);
        return;
    }

    auto& order = orderIt->second;
    if (removedQty <= 0) {
        removedQty = order.remainingQty;
    }
    if (removedQty <= 0) {
        level.orders.erase(it);
        paperOrderInfo.erase(orderId);
        paperOrders.erase(orderIt);
        return;
    }
    removedQty = std::min(removedQty, order.remainingQty);

    order.remainingQty -= removedQty;
    order.status = status;
    level.queuedLots -= removedQty;
    level.paperQty.add(order.paperIndex, -removedQty);

    level.orders.erase(it);
    paperOrderInfo.erase(orderId);
    paperOrders.erase(orderIt);
}

void PaperTradingSimulator::reducePaperOrder(const NormalizedLobEvent& event) {
    if (event.quantityLots < 0) {
        emitDiagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_PAPER_ORDER_BY_NEGATIVE_QUANTITY,
                       DiagnosticRecordSeverity::ERROR);
        return;
    }
    if (event.quantityLots == 0) {
        emitDiagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_PAPER_ORDER_BY_ZERO_QUANTITY,
                       DiagnosticRecordSeverity::WARNING);
        return;
    }

    auto infoIt = paperOrderInfo.find(event.orderId);
    if (infoIt == paperOrderInfo.end()) {
        return;
    }

    auto storedSide = std::get<0>(infoIt->second);
    auto storedPriceTicks = std::get<1>(infoIt->second);
    auto queueLocationIt = std::get<2>(infoIt->second);
    auto level = findPaperLevel(storedSide, storedPriceTicks);
    if (!level) {
        paperOrderInfo.erase(infoIt);
        paperOrders.erase(event.orderId);
        return;
    }

    auto orderIt = paperOrders.find(event.orderId);
    if (orderIt == paperOrders.end()) {
        level->orders.erase(queueLocationIt);
        paperOrderInfo.erase(infoIt);
        return;
    }

    auto& order = orderIt->second;
    auto take = std::min(order.remainingQty, event.quantityLots);
    if (take <= 0) {
        return;
    }

    if (take >= order.remainingQty) {
        removePaperOrder(*level, queueLocationIt, order.remainingQty, PaperOrderStatus::CANCELLED);
        if (level->orders.empty()) {
            auto& levels = storedSide == Side::BUY ? paperBids : paperAsks;
            levels.erase(storedPriceTicks);
        }
        return;
    }

    order.remainingQty -= take;
    level->queuedLots -= take;
    level->paperQty.add(order.paperIndex, -take);
}

void PaperTradingSimulator::setPaperOrder(std::int64_t orderId, std::int64_t newQty) {
    if (newQty < 0) {
        newQty = 0;
    }

    auto infoIt = paperOrderInfo.find(orderId);
    if (infoIt == paperOrderInfo.end()) {
        return;
    }

    auto storedSide = std::get<0>(infoIt->second);
    auto storedPriceTicks = std::get<1>(infoIt->second);
    auto queueLocationIt = std::get<2>(infoIt->second);
    auto level = findPaperLevel(storedSide, storedPriceTicks);
    if (!level) {
        paperOrderInfo.erase(infoIt);
        paperOrders.erase(orderId);
        return;
    }

    auto orderIt = paperOrders.find(orderId);
    if (orderIt == paperOrders.end()) {
        level->orders.erase(queueLocationIt);
        paperOrderInfo.erase(infoIt);
        return;
    }

    auto& order = orderIt->second;
    if (newQty == order.remainingQty) {
        return;
    }

    if (newQty == 0) {
        removePaperOrder(*level, queueLocationIt, order.remainingQty, PaperOrderStatus::CANCELLED);
        if (level->orders.empty()) {
            auto& levels = storedSide == Side::BUY ? paperBids : paperAsks;
            levels.erase(storedPriceTicks);
        }
        return;
    }

    if (newQty < order.remainingQty) {
        const auto delta = order.remainingQty - newQty;
        order.remainingQty = newQty;
        level->queuedLots -= delta;
        level->paperQty.add(order.paperIndex, -delta);
        return;
    }

    const auto delta = newQty - order.remainingQty;
    order.remainingQty = newQty;
    level->queuedLots += delta;
    level->paperQty.add(order.paperIndex, delta);
}

void PaperTradingSimulator::onDelete(const NormalizedLobEvent& event) {
    if (event.updateSource == UpdateSource::STRATEGY) {
        auto it = paperOrderInfo.find(event.orderId);
        if (it == paperOrderInfo.end()) {
            emitDiagnostic(event, DiagnosticRecordCode::DELETE_NON_EXISTING_PAPER_ORDER_ID,
                           DiagnosticRecordSeverity::WARNING);
            return;
        }
        auto storedSide = std::get<0>(it->second);
        auto storedPriceTicks = std::get<1>(it->second);
        auto queueLocationIt = std::get<2>(it->second);
        auto level = findPaperLevel(storedSide, storedPriceTicks);
        if (!level) {
            paperOrderInfo.erase(it);
            paperOrders.erase(event.orderId);
            return;
        }

        auto orderIt = paperOrders.find(event.orderId);
        if (orderIt == paperOrders.end()) {
            level->orders.erase(queueLocationIt);
            paperOrderInfo.erase(it);
            return;
        }

        removePaperOrder(*level, queueLocationIt, orderIt->second.remainingQty, PaperOrderStatus::CANCELLED);
        if (level->orders.empty()) {
            auto& levels = storedSide == Side::BUY ? paperBids : paperAsks;
            levels.erase(storedPriceTicks);
        }
        return;
    }

    auto it = orderInfo.find(event.orderId);
    if (it == orderInfo.end()) {
        emitDiagnostic(event, DiagnosticRecordCode::DELETE_NON_EXISTING_HISTORICAL_ORDER_ID,
                       DiagnosticRecordSeverity::WARNING);
        return;
    }

    auto& info = it->second;

    auto storedSide = std::get<0>(info);
    if (storedSide != event.side) {
        emitDiagnostic(event, DiagnosticRecordCode::PROVIDED_SIDE_ON_DELETE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
                       DiagnosticRecordSeverity::WARNING);
    }

    auto storedPriceTicks = std::get<1>(info);
    if (storedPriceTicks != event.priceTicks) {
        emitDiagnostic(event, DiagnosticRecordCode::PROVIDED_PRICE_ON_DELETE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
                       DiagnosticRecordSeverity::WARNING);
    }

    auto queueLocationIt = std::get<2>(info);

    // Important! Use stored side (truth) to access. If provided side is different, it has been logged as corrupt.
    // The design decision here is to complete the DELETE based only on the provided orderId.
    const bool isBid = storedSide == Side::BUY;
    auto& book = isBid ? bids : asks;

    auto bIt = book.find(storedPriceTicks);
    if (bIt == book.end()) {
        emitDiagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                       DiagnosticRecordSeverity::ERROR);
        return;
    }

    OrderPriorityQueue& queue = bIt->second;
    auto& queueElement = *queueLocationIt;
    const auto arrivalSeq = std::get<4>(queueElement);
    const auto removedQty = std::get<2>(queueElement);

    if (auto level = findPaperLevel(storedSide, storedPriceTicks); level && removedQty > 0) {
        auto idxIt = level->marketIndexBySeq.find(arrivalSeq);
        if (idxIt != level->marketIndexBySeq.end()) {
            level->marketQty.add(idxIt->second, -removedQty);
        }
    }
    queue.erase(queueLocationIt);

    if (queue.empty()) {
        book.erase(bIt);
    }

    orderInfo.erase(it);
}

void PaperTradingSimulator::onSubtract(const NormalizedLobEvent& event) {
    if (event.updateSource == UpdateSource::STRATEGY) {
        reducePaperOrder(event);
        return;
    }
    onPartialOrderCancel(event, false);
}

void PaperTradingSimulator::onMatch(const NormalizedLobEvent& event) {
    onPartialOrderCancel(event, true);
}

void PaperTradingSimulator::onSet(const NormalizedLobEvent& event) {
    if (event.updateSource == UpdateSource::STRATEGY) {
        setPaperOrder(event.orderId, event.quantityLots);
        return;
    }
    if (event.quantityLots < 0) {
        emitDiagnostic(event, DiagnosticRecordCode::SET_WITH_NEGATIVE_LIQUIDITY_REQUESTED_WAS_SET_TO_ZERO,
                       DiagnosticRecordSeverity::WARNING);
    }
    auto it = orderInfo.find(event.orderId);
    if (it == orderInfo.end()) {
        emitDiagnostic(event, DiagnosticRecordCode::SET_NON_EXISTING_ORDER_ID_IS_REJECTED,
                       DiagnosticRecordSeverity::WARNING);
        return;
    }

    auto& info = it->second;

    auto storedSide = std::get<0>(info);
    if (storedSide != event.side) {
        emitDiagnostic(event, DiagnosticRecordCode::PROVIDED_SIDE_ON_SET_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
                       DiagnosticRecordSeverity::WARNING);
    }

    auto storedPriceTicks = std::get<1>(info);
    if (storedPriceTicks != event.priceTicks) {
        emitDiagnostic(event, DiagnosticRecordCode::PROVIDED_PRICE_ON_SET_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
                       DiagnosticRecordSeverity::WARNING);
    }

    auto queueLocationIt = std::get<2>(info);

    const bool isBid = storedSide == Side::BUY;
    auto& book = isBid ? bids : asks;

    auto bIt = book.find(storedPriceTicks);
    if (bIt == book.end()) {
        emitDiagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                       DiagnosticRecordSeverity::ERROR);
        return;
    }

    OrderPriorityQueue& queue = bIt->second;
    OrderTraderQuantitySource& queueElement = *queueLocationIt;
    const auto arrivalSeq = std::get<4>(queueElement);

    auto newQuantity = event.quantityLots < 0 ? 0 : event.quantityLots;
    const auto oldQuantity = std::get<2>(queueElement);
    const auto delta = newQuantity - oldQuantity;
    if (auto level = findPaperLevel(storedSide, storedPriceTicks); level && delta != 0) {
        auto idxIt = level->marketIndexBySeq.find(arrivalSeq);
        if (idxIt != level->marketIndexBySeq.end()) {
            level->marketQty.add(idxIt->second, delta);
        }
    }
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

void PaperTradingSimulator::onPartialOrderCancel(const NormalizedLobEvent& event, bool isTradeOnPassiveOrder) {
    if (event.quantityLots < 0) {
        emitDiagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_NEGATIVE_QUANTITY,
                       DiagnosticRecordSeverity::ERROR);
        return;
    }
    if (event.quantityLots == 0) {
        emitDiagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY,
                       DiagnosticRecordSeverity::WARNING);
        return;
    }

    auto it = orderInfo.find(event.orderId);
    if (it == orderInfo.end()) {
        emitDiagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_NON_EXISTING_ORDER_ID,
                       DiagnosticRecordSeverity::WARNING);
        return;
    }

    auto& info = it->second;

    auto storedSide = std::get<0>(info);
    if (storedSide != event.side) {
        emitDiagnostic(event,
                       DiagnosticRecordCode::PROVIDED_SIDE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
                       DiagnosticRecordSeverity::WARNING);
    }

    auto storedPriceTicks = std::get<1>(info);
    if (storedPriceTicks != event.priceTicks) {
        emitDiagnostic(event,
                       DiagnosticRecordCode::PROVIDED_PRICE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
                       DiagnosticRecordSeverity::WARNING);
    }

    auto queueLocationIt = std::get<2>(info);

    // Important! Use stored side (truth) to access. If provided side is different, it has been logged as corrupt.
    // The design decision here is to complete the partial cancel based only on the provided orderId.
    const bool isBid = storedSide == Side::BUY;
    auto& book = isBid ? bids : asks;

    auto bIt = book.find(storedPriceTicks);
    if (bIt == book.end()) {
        emitDiagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                       DiagnosticRecordSeverity::ERROR);
        return;
    }

    OrderPriorityQueue& queue = bIt->second;
    OrderTraderQuantitySource& queueElement = *queueLocationIt;

    auto liquidity = std::get<2>(queueElement);
    auto take = std::min<std::int64_t>(liquidity, event.quantityLots);

    if (event.quantityLots > liquidity) {
        emitDiagnostic(event,
                       DiagnosticRecordCode::REQUESTED_ORDER_REDUCE_WITH_VOLUME_LARGER_THAN_AVAILABLE_FOR_ORDER_ID,
                       DiagnosticRecordSeverity::WARNING);
    }

    liquidity -= take;

    const auto arrivalSeq = std::get<4>(queueElement);
    if (take > 0) {
        if (isTradeOnPassiveOrder) {
            applyPaperTradeAtLevel(storedSide, storedPriceTicks, take, event);
        }
        if (auto level = findPaperLevel(storedSide, storedPriceTicks)) {
            auto idxIt = level->marketIndexBySeq.find(arrivalSeq);
            if (idxIt != level->marketIndexBySeq.end()) {
                level->marketQty.add(idxIt->second, -take);
            }
        }
    }

    if (isTradeOnPassiveOrder && sink) {
        auto takerSide = storedSide == Side::BUY ? Side::SELL : Side::BUY;
        auto makerSource = std::get<3>(queueElement);
        auto storedTraderId = std::get<1>(queueElement);
        sink->onFill(FillRecord{seq, event.tsExchange, event.tsReceived, storedPriceTicks, take, storedSide,
                                event.orderId, storedTraderId, makerSource, takerSide, UnknownOrderIdSentinel,
                                UnknownTraderIdSentinel, event.updateSource, event.symbolId});
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
        emitDiagnostic(event, DiagnosticRecordCode::PAPER_ORDER_INVOKES_PASSIVE_MATCH_INSTEAD_OF_AGGRESSIVE_TRADE,
                       DiagnosticRecordSeverity::ERROR);
    }
}

void PaperTradingSimulator::initFromL2Snapshot(const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
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
                                      UpdateSource::HISTORICAL, orderArrivalSeq++};

        if (book.find(price) != book.end()) {
            throw std::runtime_error("Duplicate price found. Initializing from L2 snapshot requires volumes to be "
                                     "aggregated per price-level.");
        }

        book.emplace(price, std::list<OrderTraderQuantitySource>{otq});
        heap.push(static_cast<std::int64_t>(sign * price));
    }
}

void PaperTradingSimulator::initFromL3Snapshot(const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
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
        priorityQueue.emplace_back(orderId, traderId, quantity, UpdateSource::HISTORICAL, orderArrivalSeq++);
        auto itNew = std::prev(priorityQueue.end());

        // Only push price into heap the first time the level appears
        if (newLevel) {
            heap.push(isBid ? price : -price); // asks stored as negative
        }

        orderInfo.emplace(orderId, std::make_tuple(side, price, itNew));
    }
}

void PaperTradingSimulator::clearState() {
    bids.clear();
    asks.clear();
    bidsHeap = std::priority_queue<std::int64_t>();
    asksHeap = std::priority_queue<std::int64_t>();
    orderInfo.clear();
    paperOrders.clear();
    paperOrderInfo.clear();
    paperBids.clear();
    paperAsks.clear();
    orderArrivalSeq = 0;
    if (sink) {
        sink->reset();
    }
}

std::optional<std::int64_t> PaperTradingSimulator::depthAt(Side side, std::int64_t priceTicks) const {
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

std::vector<std::pair<std::int64_t, std::int64_t>> PaperTradingSimulator::l2TopN(Side side, std::uint32_t n) const {
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

std::optional<std::int64_t> PaperTradingSimulator::getBestPriceTicks(Side side) const {
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

void PaperTradingSimulator::setLogSink(ILogSink* sink) {
    this->sink = sink;
}

void PaperTradingSimulator::emitDiagnostic(const NormalizedLobEvent& event, DiagnosticRecordCode code,
                                           DiagnosticRecordSeverity severity) {
    if (sink == nullptr) {
        return;
    }
    sink->onDiagnostic(DiagnosticRecord{seq, event.tsExchange, event.tsReceived, code, severity, event.symbolId});
}
