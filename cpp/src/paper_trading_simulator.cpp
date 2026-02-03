#include "lobsim/paper_trading_simulator.hpp"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <numeric>
#include <stdexcept>

/**
 * TODO:
 * - Add checks for out-of-limits values -> reject event completely.
 * - L2 init cannot support exact paper order trading because we reject MODIFYs with order_ids not present.
 * - If a SET with non-existing order ID arrives, it is rejected now. Future work: track invalid events,
 *    and try to reconcile them later. For example if an ADD arrives later with a TS earlier than the SET
 *    that was rejected, it means that the ADD should have happend first (feed delays), so we must reconcile
 *    and maybe adjust the order book
 * - Implement logic for 'orphan' orders where for example a SUB for unknown order_id arrives and the ADD arrives later.
 *   These orders must be reconciled with a delay. Also devise the reconciliation mechanism -> AI agent with context
 *   might make this simpler.
 */

PaperTradingSimulator::PaperTradingSimulator(OrderPoolConfig pool_config)
    : IMatchingEngine(), pool_config_(pool_config) {
    init_pools();
}

PaperTradingSimulator::PaperTradingSimulator(std::size_t historical_capacity, std::size_t paper_capacity)
    : PaperTradingSimulator(OrderPoolConfig{historical_capacity, paper_capacity}) {}

PaperTradingSimulator::PaperTradingSimulator(std::vector<Side>& sides, std::vector<std::int64_t>& prices,
                                             std::vector<std::int64_t>& quantities, ILogSink* sink,
                                             OrderPoolConfig pool_config)
    : PaperTradingSimulator(pool_config) {
    this->sink = sink;
    init_from_l2_snapshot(sides, prices, quantities);
}

void PaperTradingSimulator::init_pools() {
    order_pool_.reserve(pool_config_.historical_capacity);
    paper_pool_.reserve(pool_config_.paper_capacity);
}

void PaperTradingSimulator::reset_pools() {
    order_pool_.reset();
    paper_pool_.reset();
}

void PaperTradingSimulator::update(const NormalizedLobEvent& event) {
    if (sink) {
        sink->on_event_apply(EventApplyRecord{seq, event.ts_exchange, event.ts_received, event.side, event.update_type,
                                              event.update_source, event.price_ticks, event.quantity_lots,
                                              event.order_id, event.trader_id, event.aggressor_id, event.symbol_id});
    }
    switch (event.update_type) {
    case UpdateType::ADD:
        on_add(event);
        break;
    case UpdateType::AGGRESSIVE_TRADE:
        on_aggressive_trade(event);
        break;

    case UpdateType::DELETE:
        on_delete(event);
        break;

    case UpdateType::SUBTRACT:
        on_subtract(event);
        break;

    case UpdateType::MATCH:
        on_match(event);
        break;

    case UpdateType::SET:
        on_set(event);
        break;

    default:
        emit_diagnostic(event, DiagnosticRecordCode::INVALID_UPDATE_TYPE, DiagnosticRecordSeverity::ERROR);
    }

    ++seq;
}

void PaperTradingSimulator::on_add(const NormalizedLobEvent& event) {
    if (event.quantity_lots < 0) {
        emit_diagnostic(event, DiagnosticRecordCode::ADD_INVOKED_WITH_NEGATIVE_QUANTITY,
                        DiagnosticRecordSeverity::ERROR);
        return;
    }

    const bool paper = event.update_source == UpdateSource::STRATEGY;
    const bool isBid = event.side == Side::BUY;

    if (order_info.contains(event.order_id) || paper_orders.contains(event.order_id) ||
        paper_order_info.contains(event.order_id)) {
        emit_diagnostic(event, DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID, DiagnosticRecordSeverity::WARNING);
        return;
    }

    auto& ownBook = isBid ? bids : asks;
    auto& ownHeap = isBid ? bids_heap : asks_heap;

    auto& oppBook = isBid ? asks : bids;
    auto& oppHeapR = isBid ? asks_heap : bids_heap;

    const bool oppIsAsk = isBid;

    std::priority_queue<std::int64_t> oppHeapCopy;
    if (paper) {
        oppHeapCopy = oppHeapR;
    }

    auto bestOpp = [&]() -> std::optional<std::int64_t> {
        if (paper) {
            return best_opposite_price(oppIsAsk, oppBook, oppHeapCopy);
        }
        return best_opposite_price(oppIsAsk, oppBook, oppHeapR);
    };

    auto bestPxOpt = bestOpp();
    const bool crosses = bestPxOpt && (isBid ? (event.price_ticks >= *bestPxOpt) : (event.price_ticks <= *bestPxOpt));

    std::int64_t remaining = event.quantity_lots;

    if (crosses) {
        while (remaining > 0) {
            bestPxOpt = bestOpp();
            if (!bestPxOpt) {
                break;
            }
            const std::int64_t bestPx = *bestPxOpt;
            const bool stillCrosses = isBid ? (event.price_ticks >= bestPx) : (event.price_ticks <= bestPx);
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
                    const auto maker_order_id = node.order_id;
                    const auto maker_trader_id = node.trader_id;
                    const auto makerQty = node.qty;
                    const auto maker_source = node.source;

                    if (makerQty <= 0) {
                        continue;
                    }

                    const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);
                    if (sink) {
                        auto maker_side = event.side == Side::BUY ? Side::SELL : Side::BUY;
                        sink->on_fill(FillRecord{seq, event.ts_exchange, event.ts_received, bestPx, take, maker_side,
                                                 maker_order_id, maker_trader_id, maker_source, event.side,
                                                 event.order_id, event.trader_id, event.update_source,
                                                 event.symbol_id});
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
                    auto* node_ptr = &node;
                    const auto maker_order_id = node.order_id;
                    const auto maker_trader_id = node.trader_id;
                    auto& makerQty = node.qty;
                    auto& maker_source = node.source;

                    if (makerQty <= 0) {
                        order_info.erase(maker_order_id);
                        itNode = levelList.erase(itNode);
                        order_pool_.release(node_ptr);
                        continue;
                    }

                    const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);

                    if (sink) {
                        auto maker_side = event.side == Side::BUY ? Side::SELL : Side::BUY;
                        sink->on_fill(FillRecord{seq, event.ts_exchange, event.ts_received, bestPx, take, maker_side,
                                                 maker_order_id, maker_trader_id, maker_source, event.side,
                                                 event.order_id, event.trader_id, event.update_source,
                                                 event.symbol_id});
                    }

                    marketDeltas.emplace_back(node.arrival_seq, -take);

                    makerQty -= take;
                    remaining -= take;
                    tradedAtLevel += take;

                    if (makerQty == 0) {
                        order_info.erase(maker_order_id);
                        itNode = levelList.erase(itNode);
                        order_pool_.release(node_ptr);
                    } else {
                        ++itNode;
                    }
                }

                if (tradedAtLevel > 0) {
                    auto maker_side = event.side == Side::BUY ? Side::SELL : Side::BUY;
                    apply_paper_trade_at_level(maker_side, bestPx, tradedAtLevel, event);
                    if (auto paperLevel = find_paper_level(maker_side, bestPx)) {
                        for (const auto& [seq, delta] : marketDeltas) {
                            auto idxIt = paperLevel->market_index_by_seq.find(seq);
                            if (idxIt != paperLevel->market_index_by_seq.end()) {
                                paperLevel->market_qty.add(idxIt->second, delta);
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
        auto bestPaperOpp = [&]() { return best_paper_opposite_price(oppIsAsk); };
        while (remaining > 0) {
            auto paperPx = bestPaperOpp();
            if (!paperPx.has_value()) {
                break;
            }
            const bool stillCrosses = isBid ? (event.price_ticks >= *paperPx) : (event.price_ticks <= *paperPx);
            if (!stillCrosses) {
                break;
            }
            const auto traded =
                trade_against_paper_level(oppIsAsk ? Side::SELL : Side::BUY, *paperPx, remaining, event);
            if (traded <= 0) {
                // Nothing traded; avoid infinite loop
                break;
            }
            remaining -= traded;
        }
    }

    if (remaining > 0) {
        if (paper) {
            auto* paper_node = paper_pool_.acquire(pool_config_.paper_capacity);
            if (!paper_node) {
                emit_diagnostic(event, DiagnosticRecordCode::PAPER_ORDER_POOL_EXHAUSTED,
                                DiagnosticRecordSeverity::ERROR);
                return;
            }
            auto& level = ensure_paper_level(event.side, event.price_ticks);
            PaperOrder order{};
            order.original_event = event;
            order.status =
                remaining < event.quantity_lots ? PaperOrderStatus::PARTIALLY_FILLED : PaperOrderStatus::OPEN;
            order.remaining_qty = remaining;
            order.placement_seq = order_arrival_seq++;
            const std::size_t paper_index = level.next_paper_index++;
            level.paper_qty.ensure_size(level.next_paper_index);
            level.paper_qty.add(paper_index, remaining);
            order.paper_index = paper_index;
            paper_orders.emplace(event.order_id, order);
            paper_node->order_id = event.order_id;
            level.orders.push_back(*paper_node);
            paper_order_info.emplace(event.order_id, PaperOrderInfo{event.side, event.price_ticks, paper_node});
            level.queued_lots += remaining;
        } else {
            auto* node = order_pool_.acquire(pool_config_.historical_capacity);
            if (!node) {
                emit_diagnostic(event, DiagnosticRecordCode::HISTORICAL_ORDER_POOL_EXHAUSTED,
                                DiagnosticRecordSeverity::ERROR);
                return;
            }
            node->order_id = event.order_id;
            node->trader_id = event.trader_id;
            node->qty = remaining;
            node->source = event.update_source;
            node->arrival_seq = order_arrival_seq++;

            auto& level = ownBook[event.price_ticks];
            const bool newLevel = level.empty();
            level.push_back(*node);
            if (newLevel) {
                ownHeap.push(isBid ? event.price_ticks : -event.price_ticks);
            }
            order_info.emplace(event.order_id, OrderInfo{event.side, event.price_ticks, node});
            if (auto paperLevel = find_paper_level(event.side, event.price_ticks)) {
                const auto seq = node->arrival_seq;
                const std::size_t idx = paperLevel->market_seqs.size();
                paperLevel->market_seqs.push_back(seq);
                paperLevel->market_index_by_seq.emplace(seq, idx);
                paperLevel->market_qty.ensure_size(paperLevel->market_seqs.size());
                paperLevel->market_qty.add(idx, remaining);
            }
        }
    }
}

void PaperTradingSimulator::on_aggressive_trade(const NormalizedLobEvent& event) {
    if (event.update_source != UpdateSource::STRATEGY) {
        emit_diagnostic(event, DiagnosticRecordCode::INVALID_UPDATE_TYPE, DiagnosticRecordSeverity::ERROR);
        return;
    }
    if (event.quantity_lots <= 0) {
        emit_diagnostic(event, DiagnosticRecordCode::ADD_INVOKED_WITH_NEGATIVE_QUANTITY,
                        DiagnosticRecordSeverity::ERROR);
        return;
    }

    const bool isBid = event.side == Side::BUY;
    auto& oppBook = isBid ? asks : bids;
    auto oppHeapCopy = isBid ? asks_heap : bids_heap;
    const bool oppIsAsk = isBid;

    std::int64_t remaining = event.quantity_lots;

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
            const auto maker_order_id = node.order_id;
            const auto maker_trader_id = node.trader_id;
            const auto makerQty = node.qty;
            const auto maker_source = node.source;
            if (makerQty <= 0) {
                continue;
            }
            const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);
            if (sink) {
                auto maker_side = event.side == Side::BUY ? Side::SELL : Side::BUY;
                sink->on_fill(FillRecord{seq, event.ts_exchange, event.ts_received, bestPx, take, maker_side,
                                         maker_order_id, maker_trader_id, maker_source, event.side, event.order_id,
                                         event.trader_id, event.update_source, event.symbol_id});
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
        auto bestPaperOpp = [&]() { return best_paper_opposite_price(oppIsAsk); };
        while (remaining > 0) {
            auto paperPx = bestPaperOpp();
            if (!paperPx) {
                break;
            }
            const auto traded =
                trade_against_paper_level(oppIsAsk ? Side::SELL : Side::BUY, *paperPx, remaining, event);
            if (traded <= 0) {
                break;
            }
            remaining -= traded;
        }
    }

    ++seq;
}

std::optional<std::int64_t>
PaperTradingSimulator::best_opposite_price(bool opposite_is_ask, const Book& opposite_book,
                                           std::priority_queue<std::int64_t>& opposite_heap) {
    while (!opposite_heap.empty()) {
        const std::int64_t px = opposite_is_ask ? -opposite_heap.top() : opposite_heap.top();
        auto it = opposite_book.find(px);
        if (it != opposite_book.end() && !it->second.empty()) {
            return px;
        }
        opposite_heap.pop();
    }
    return std::nullopt;
}

std::optional<std::int64_t> PaperTradingSimulator::best_paper_opposite_price(bool opposite_is_ask) {
    const auto& levels = opposite_is_ask ? paper_asks : paper_bids;
    std::optional<std::int64_t> best;
    for (const auto& [px, level] : levels) {
        if (level.queued_lots <= 0) {
            continue;
        }
        if (!best) {
            best = px;
            continue;
        }
        if (opposite_is_ask) {
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

PaperTradingSimulator::PaperOrderLevel& PaperTradingSimulator::ensure_paper_level(Side side, std::int64_t price_ticks) {
    auto& levels = side == Side::BUY ? paper_bids : paper_asks;
    auto [it, inserted] = levels.try_emplace(price_ticks);
    if (inserted) {
        auto& level = it->second;
        const auto& book = side == Side::BUY ? bids : asks;
        auto bookIt = book.find(price_ticks);
        if (bookIt != book.end()) {
            const auto& queue = bookIt->second;
            level.market_seqs.reserve(queue.size());
            level.market_qty.ensure_size(queue.size());
            std::size_t idx = 0;
            for (const auto& node : queue) {
                const auto seq = node.arrival_seq;
                const auto qty = node.qty;
                level.market_seqs.push_back(seq);
                level.market_index_by_seq.emplace(seq, idx);
                level.market_qty.ensure_size(idx + 1);
                level.market_qty.add(idx, qty);
                ++idx;
            }
        }
    }
    return it->second;
}

PaperTradingSimulator::PaperOrderLevel* PaperTradingSimulator::find_paper_level(Side side, std::int64_t price_ticks) {
    auto& levels = side == Side::BUY ? paper_bids : paper_asks;
    auto it = levels.find(price_ticks);
    if (it == levels.end()) {
        return nullptr;
    }
    return &it->second;
}

void PaperTradingSimulator::apply_paper_trade_at_level(Side passive_side, std::int64_t price_ticks,
                                                       std::int64_t trade_lots, const NormalizedLobEvent& aggressor) {
    if (trade_lots <= 0) {
        return;
    }

    auto& levels = passive_side == Side::BUY ? paper_bids : paper_asks;
    auto levelIt = levels.find(price_ticks);
    if (levelIt == levels.end()) {
        return;
    }

    auto& level = levelIt->second;
    auto it = level.orders.begin();
    std::int64_t marketConsumed = 0;
    const auto marketAhead = [&](std::uint64_t placement_seq) {
        if (level.market_seqs.empty()) {
            return std::int64_t{0};
        }
        const auto pos = static_cast<std::size_t>(
            std::lower_bound(level.market_seqs.begin(), level.market_seqs.end(), placement_seq) -
            level.market_seqs.begin());
        return level.market_qty.sum(pos);
    };
    while (it != level.orders.end() && trade_lots > 0) {
        auto* node = &*it;
        const auto order_id = node->order_id;
        auto orderIt = paper_orders.find(order_id);
        if (orderIt == paper_orders.end()) {
            it = level.orders.erase(it);
            paper_order_info.erase(order_id);
            paper_pool_.release(node);
            continue;
        }

        auto& order = orderIt->second;
        const auto totalMarketAhead = marketAhead(order.placement_seq);
        const auto aheadMarket =
            totalMarketAhead > marketConsumed ? (totalMarketAhead - marketConsumed) : std::int64_t{0};
        const auto aheadPaper = level.paper_qty.sum(order.paper_index);
        const auto queueAhead = aheadMarket + aheadPaper;
        if (queueAhead >= trade_lots) {
            break;
        }
        trade_lots -= queueAhead;
        marketConsumed += aheadMarket;

        if (order.remaining_qty <= 0) {
            auto next = std::next(it);
            level.orders.erase(it);
            paper_order_info.erase(order_id);
            paper_orders.erase(orderIt);
            paper_pool_.release(node);
            it = next;
            continue;
        }

        const auto fillQty = std::min(order.remaining_qty, trade_lots);
        if (fillQty <= 0) {
            ++it;
            continue;
        }

        if (sink) {
            const bool eventIsAggressor = aggressor.update_type == UpdateType::ADD;
            auto taker_side = passive_side == Side::BUY ? Side::SELL : Side::BUY;
            auto taker_order_id = eventIsAggressor ? aggressor.order_id : UnknownOrderIdSentinel;
            auto taker_trader_id = eventIsAggressor ? aggressor.trader_id : UnknownTraderIdSentinel;
            auto taker_source = aggressor.update_source;

            sink->on_fill(FillRecord{seq, aggressor.ts_exchange, aggressor.ts_received, price_ticks, fillQty,
                                     passive_side, order_id, order.original_event.trader_id, UpdateSource::STRATEGY,
                                     taker_side, taker_order_id, taker_trader_id, taker_source, aggressor.symbol_id});
        }

        trade_lots -= fillQty;

        if (fillQty < order.remaining_qty) {
            order.remaining_qty -= fillQty;
            level.queued_lots -= fillQty;
            level.paper_qty.add(order.paper_index, -fillQty);
            order.status = PaperOrderStatus::PARTIALLY_FILLED;
            ++it;
        } else {
            auto next = std::next(it);
            remove_paper_order(level, node, fillQty, PaperOrderStatus::FILLED);
            it = next;
        }
    }

    if (level.orders.empty()) {
        levels.erase(levelIt);
    }
}

std::int64_t PaperTradingSimulator::trade_against_paper_level(Side passive_side, std::int64_t price_ticks,
                                                              std::int64_t trade_lots,
                                                              const NormalizedLobEvent& aggressor) {
    if (trade_lots <= 0) {
        return 0;
    }
    auto& levels = passive_side == Side::BUY ? paper_bids : paper_asks;
    auto levelIt = levels.find(price_ticks);
    if (levelIt == levels.end()) {
        return 0;
    }
    auto& level = levelIt->second;
    auto it = level.orders.begin();
    std::int64_t consumed = 0;
    while (trade_lots > 0 && it != level.orders.end()) {
        auto* node = &*it;
        const auto order_id = node->order_id;
        auto orderIt = paper_orders.find(order_id);
        if (orderIt == paper_orders.end()) {
            auto next = std::next(it);
            level.orders.erase(it);
            paper_order_info.erase(order_id);
            paper_pool_.release(node);
            it = next;
            continue;
        }
        auto& order = orderIt->second;
        if (order.remaining_qty <= 0 || order.status == PaperOrderStatus::FILLED ||
            order.status == PaperOrderStatus::CANCELLED || order.status == PaperOrderStatus::REJECTED) {
            auto next = std::next(it);
            level.orders.erase(it);
            paper_order_info.erase(order_id);
            paper_orders.erase(orderIt);
            paper_pool_.release(node);
            it = next;
            continue;
        }

        const auto fillQty = std::min(order.remaining_qty, trade_lots);
        if (fillQty > 0 && sink) {
            auto maker_side = passive_side;
            auto taker_side = passive_side == Side::BUY ? Side::SELL : Side::BUY;
            sink->on_fill(FillRecord{seq, aggressor.ts_exchange, aggressor.ts_received, price_ticks, fillQty,
                                     maker_side, order_id, order.original_event.trader_id, UpdateSource::STRATEGY,
                                     taker_side, aggressor.order_id, aggressor.trader_id, aggressor.update_source,
                                     aggressor.symbol_id});
        }

        order.remaining_qty -= fillQty;
        trade_lots -= fillQty;
        consumed += fillQty;

        if (order.remaining_qty == 0) {
            auto next = std::next(it);
            remove_paper_order(level, node, fillQty, PaperOrderStatus::FILLED);
            it = next;
        } else {
            order.status = PaperOrderStatus::PARTIALLY_FILLED;
            level.queued_lots -= fillQty;
            level.paper_qty.add(order.paper_index, -fillQty);
            ++it;
        }
    }

    if (level.orders.empty()) {
        levels.erase(levelIt);
    }
    return consumed;
}

void PaperTradingSimulator::remove_paper_order(PaperOrderLevel& level, PaperOrderNode* node, std::int64_t removed_qty,
                                               PaperOrderStatus status) {
    if (node == nullptr) {
        return;
    }
    const auto order_id = node->order_id;
    auto orderIt = paper_orders.find(order_id);
    if (orderIt == paper_orders.end()) {
        if (node->hook.is_linked()) {
            level.orders.erase(level.orders.iterator_to(*node));
        }
        paper_order_info.erase(order_id);
        paper_pool_.release(node);
        return;
    }

    auto& order = orderIt->second;
    if (removed_qty <= 0) {
        removed_qty = order.remaining_qty;
    }
    if (removed_qty <= 0) {
        if (node->hook.is_linked()) {
            level.orders.erase(level.orders.iterator_to(*node));
        }
        paper_order_info.erase(order_id);
        paper_orders.erase(orderIt);
        paper_pool_.release(node);
        return;
    }
    removed_qty = std::min(removed_qty, order.remaining_qty);

    order.remaining_qty -= removed_qty;
    order.status = status;
    level.queued_lots -= removed_qty;
    level.paper_qty.add(order.paper_index, -removed_qty);

    if (node->hook.is_linked()) {
        level.orders.erase(level.orders.iterator_to(*node));
    }
    paper_order_info.erase(order_id);
    paper_orders.erase(orderIt);
    paper_pool_.release(node);
}

void PaperTradingSimulator::reduce_paper_order(const NormalizedLobEvent& event) {
    if (event.quantity_lots < 0) {
        emit_diagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_PAPER_ORDER_BY_NEGATIVE_QUANTITY,
                        DiagnosticRecordSeverity::ERROR);
        return;
    }
    if (event.quantity_lots == 0) {
        emit_diagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_PAPER_ORDER_BY_ZERO_QUANTITY,
                        DiagnosticRecordSeverity::WARNING);
        return;
    }

    auto infoIt = paper_order_info.find(event.order_id);
    if (infoIt == paper_order_info.end()) {
        return;
    }

    auto storedSide = infoIt->second.side;
    auto storedPriceTicks = infoIt->second.price_ticks;
    auto* node = infoIt->second.node;
    auto level = find_paper_level(storedSide, storedPriceTicks);
    if (!level) {
        paper_pool_.release(node);
        paper_order_info.erase(infoIt);
        paper_orders.erase(event.order_id);
        return;
    }

    auto orderIt = paper_orders.find(event.order_id);
    if (orderIt == paper_orders.end()) {
        if (node && node->hook.is_linked()) {
            level->orders.erase(level->orders.iterator_to(*node));
        }
        paper_pool_.release(node);
        paper_order_info.erase(infoIt);
        return;
    }

    auto& order = orderIt->second;
    auto take = std::min(order.remaining_qty, event.quantity_lots);
    if (take <= 0) {
        return;
    }

    if (take >= order.remaining_qty) {
        remove_paper_order(*level, node, order.remaining_qty, PaperOrderStatus::CANCELLED);
        if (level->orders.empty()) {
            auto& levels = storedSide == Side::BUY ? paper_bids : paper_asks;
            levels.erase(storedPriceTicks);
        }
        return;
    }

    order.remaining_qty -= take;
    level->queued_lots -= take;
    level->paper_qty.add(order.paper_index, -take);
}

void PaperTradingSimulator::set_paper_order(std::int64_t order_id, std::int64_t new_qty) {
    if (new_qty < 0) {
        new_qty = 0;
    }

    auto infoIt = paper_order_info.find(order_id);
    if (infoIt == paper_order_info.end()) {
        return;
    }

    auto storedSide = infoIt->second.side;
    auto storedPriceTicks = infoIt->second.price_ticks;
    auto* node = infoIt->second.node;
    auto level = find_paper_level(storedSide, storedPriceTicks);
    if (!level) {
        paper_pool_.release(node);
        paper_order_info.erase(infoIt);
        paper_orders.erase(order_id);
        return;
    }

    auto orderIt = paper_orders.find(order_id);
    if (orderIt == paper_orders.end()) {
        if (node && node->hook.is_linked()) {
            level->orders.erase(level->orders.iterator_to(*node));
        }
        paper_pool_.release(node);
        paper_order_info.erase(infoIt);
        return;
    }

    auto& order = orderIt->second;
    if (new_qty == order.remaining_qty) {
        return;
    }

    if (new_qty == 0) {
        remove_paper_order(*level, node, order.remaining_qty, PaperOrderStatus::CANCELLED);
        if (level->orders.empty()) {
            auto& levels = storedSide == Side::BUY ? paper_bids : paper_asks;
            levels.erase(storedPriceTicks);
        }
        return;
    }

    if (new_qty < order.remaining_qty) {
        const auto delta = order.remaining_qty - new_qty;
        order.remaining_qty = new_qty;
        level->queued_lots -= delta;
        level->paper_qty.add(order.paper_index, -delta);
        return;
    }

    const auto delta = new_qty - order.remaining_qty;
    order.remaining_qty = new_qty;
    level->queued_lots += delta;
    level->paper_qty.add(order.paper_index, delta);
}

void PaperTradingSimulator::on_delete(const NormalizedLobEvent& event) {
    if (event.update_source == UpdateSource::STRATEGY) {
        auto it = paper_order_info.find(event.order_id);
        if (it == paper_order_info.end()) {
            emit_diagnostic(event, DiagnosticRecordCode::DELETE_NON_EXISTING_PAPER_ORDER_ID,
                            DiagnosticRecordSeverity::WARNING);
            return;
        }
        auto storedSide = it->second.side;
        auto storedPriceTicks = it->second.price_ticks;
        auto* node = it->second.node;
        auto level = find_paper_level(storedSide, storedPriceTicks);
        if (!level) {
            paper_pool_.release(node);
            paper_order_info.erase(it);
            paper_orders.erase(event.order_id);
            return;
        }

        auto orderIt = paper_orders.find(event.order_id);
        if (orderIt == paper_orders.end()) {
            if (node && node->hook.is_linked()) {
                level->orders.erase(level->orders.iterator_to(*node));
            }
            paper_pool_.release(node);
            paper_order_info.erase(it);
            return;
        }

        remove_paper_order(*level, node, orderIt->second.remaining_qty, PaperOrderStatus::CANCELLED);
        if (level->orders.empty()) {
            auto& levels = storedSide == Side::BUY ? paper_bids : paper_asks;
            levels.erase(storedPriceTicks);
        }
        return;
    }

    auto it = order_info.find(event.order_id);
    if (it == order_info.end()) {
        emit_diagnostic(event, DiagnosticRecordCode::DELETE_NON_EXISTING_HISTORICAL_ORDER_ID,
                        DiagnosticRecordSeverity::WARNING);
        return;
    }

    auto& info = it->second;

    auto storedSide = info.side;
    if (storedSide != event.side) {
        emit_diagnostic(event, DiagnosticRecordCode::PROVIDED_SIDE_ON_DELETE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
                        DiagnosticRecordSeverity::WARNING);
    }

    auto storedPriceTicks = info.price_ticks;
    if (storedPriceTicks != event.price_ticks) {
        emit_diagnostic(event, DiagnosticRecordCode::PROVIDED_PRICE_ON_DELETE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
                        DiagnosticRecordSeverity::WARNING);
    }

    auto* node = info.node;

    // Important! Use stored side (truth) to access. If provided side is different, it has been logged as corrupt.
    // The design decision here is to complete the DELETE based only on the provided order_id.
    const bool isBid = storedSide == Side::BUY;
    auto& book = isBid ? bids : asks;

    auto bIt = book.find(storedPriceTicks);
    if (bIt == book.end()) {
        emit_diagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                        DiagnosticRecordSeverity::ERROR);
        return;
    }

    OrderPriorityQueue& queue = bIt->second;
    if (node == nullptr) {
        emit_diagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                        DiagnosticRecordSeverity::ERROR);
        return;
    }
    const auto arrivalSeq = node->arrival_seq;
    const auto removed_qty = node->qty;

    if (auto level = find_paper_level(storedSide, storedPriceTicks); level && removed_qty > 0) {
        auto idxIt = level->market_index_by_seq.find(arrivalSeq);
        if (idxIt != level->market_index_by_seq.end()) {
            level->market_qty.add(idxIt->second, -removed_qty);
        }
    }
    if (node->hook.is_linked()) {
        queue.erase(queue.iterator_to(*node));
    }
    order_pool_.release(node);

    if (queue.empty()) {
        book.erase(bIt);
    }

    order_info.erase(it);
}

void PaperTradingSimulator::on_subtract(const NormalizedLobEvent& event) {
    if (event.update_source == UpdateSource::STRATEGY) {
        reduce_paper_order(event);
        return;
    }
    on_partial_order_cancel(event, false);
}

void PaperTradingSimulator::on_match(const NormalizedLobEvent& event) {
    on_partial_order_cancel(event, true);
}

void PaperTradingSimulator::on_set(const NormalizedLobEvent& event) {
    if (event.update_source == UpdateSource::STRATEGY) {
        set_paper_order(event.order_id, event.quantity_lots);
        return;
    }
    if (event.quantity_lots < 0) {
        emit_diagnostic(event, DiagnosticRecordCode::SET_WITH_NEGATIVE_LIQUIDITY_REQUESTED_WAS_SET_TO_ZERO,
                        DiagnosticRecordSeverity::WARNING);
    }
    auto it = order_info.find(event.order_id);
    if (it == order_info.end()) {
        emit_diagnostic(event, DiagnosticRecordCode::SET_NON_EXISTING_ORDER_ID_IS_REJECTED,
                        DiagnosticRecordSeverity::WARNING);
        return;
    }

    auto& info = it->second;

    auto storedSide = info.side;
    if (storedSide != event.side) {
        emit_diagnostic(event, DiagnosticRecordCode::PROVIDED_SIDE_ON_SET_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
                        DiagnosticRecordSeverity::WARNING);
    }

    auto storedPriceTicks = info.price_ticks;
    if (storedPriceTicks != event.price_ticks) {
        emit_diagnostic(event, DiagnosticRecordCode::PROVIDED_PRICE_ON_SET_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
                        DiagnosticRecordSeverity::WARNING);
    }

    auto* node = info.node;

    const bool isBid = storedSide == Side::BUY;
    auto& book = isBid ? bids : asks;

    auto bIt = book.find(storedPriceTicks);
    if (bIt == book.end()) {
        emit_diagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                        DiagnosticRecordSeverity::ERROR);
        return;
    }

    OrderPriorityQueue& queue = bIt->second;
    if (node == nullptr) {
        emit_diagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                        DiagnosticRecordSeverity::ERROR);
        return;
    }
    const auto arrivalSeq = node->arrival_seq;

    auto newQuantity = event.quantity_lots < 0 ? 0 : event.quantity_lots;
    const auto oldQuantity = node->qty;
    const auto delta = newQuantity - oldQuantity;
    if (auto level = find_paper_level(storedSide, storedPriceTicks); level && delta != 0) {
        auto idxIt = level->market_index_by_seq.find(arrivalSeq);
        if (idxIt != level->market_index_by_seq.end()) {
            level->market_qty.add(idxIt->second, delta);
        }
    }
    if (newQuantity == 0) {
        if (node->hook.is_linked()) {
            queue.erase(queue.iterator_to(*node));
        }
        order_pool_.release(node);
        if (queue.empty()) {
            book.erase(bIt);
        }
        order_info.erase(it);
    } else {
        node->qty = newQuantity;
    }
}

void PaperTradingSimulator::on_partial_order_cancel(const NormalizedLobEvent& event, bool is_trade_on_passive_order) {
    if (event.quantity_lots < 0) {
        emit_diagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_NEGATIVE_QUANTITY,
                        DiagnosticRecordSeverity::ERROR);
        return;
    }
    if (event.quantity_lots == 0) {
        emit_diagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY,
                        DiagnosticRecordSeverity::WARNING);
        return;
    }

    auto it = order_info.find(event.order_id);
    if (it == order_info.end()) {
        emit_diagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_NON_EXISTING_ORDER_ID,
                        DiagnosticRecordSeverity::WARNING);
        return;
    }

    auto& info = it->second;

    auto storedSide = info.side;
    if (storedSide != event.side) {
        emit_diagnostic(event,
                        DiagnosticRecordCode::PROVIDED_SIDE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID,
                        DiagnosticRecordSeverity::WARNING);
    }

    auto storedPriceTicks = info.price_ticks;
    if (storedPriceTicks != event.price_ticks) {
        emit_diagnostic(event,
                        DiagnosticRecordCode::PROVIDED_PRICE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID,
                        DiagnosticRecordSeverity::WARNING);
    }

    auto* node = info.node;

    // Important! Use stored side (truth) to access. If provided side is different, it has been logged as corrupt.
    // The design decision here is to complete the partial cancel based only on the provided order_id.
    const bool isBid = storedSide == Side::BUY;
    auto& book = isBid ? bids : asks;

    auto bIt = book.find(storedPriceTicks);
    if (bIt == book.end()) {
        emit_diagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                        DiagnosticRecordSeverity::ERROR);
        return;
    }

    OrderPriorityQueue& queue = bIt->second;
    if (node == nullptr) {
        emit_diagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK,
                        DiagnosticRecordSeverity::ERROR);
        return;
    }

    auto liquidity = node->qty;
    auto take = std::min<std::int64_t>(liquidity, event.quantity_lots);

    if (event.quantity_lots > liquidity) {
        emit_diagnostic(event,
                        DiagnosticRecordCode::REQUESTED_ORDER_REDUCE_WITH_VOLUME_LARGER_THAN_AVAILABLE_FOR_ORDER_ID,
                        DiagnosticRecordSeverity::WARNING);
    }

    liquidity -= take;

    const auto arrivalSeq = node->arrival_seq;
    if (take > 0) {
        if (is_trade_on_passive_order) {
            apply_paper_trade_at_level(storedSide, storedPriceTicks, take, event);
        }
        if (auto level = find_paper_level(storedSide, storedPriceTicks)) {
            auto idxIt = level->market_index_by_seq.find(arrivalSeq);
            if (idxIt != level->market_index_by_seq.end()) {
                level->market_qty.add(idxIt->second, -take);
            }
        }
    }

    if (is_trade_on_passive_order && sink) {
        auto taker_side = storedSide == Side::BUY ? Side::SELL : Side::BUY;
        auto maker_source = node->source;
        auto storedTraderId = node->trader_id;
        sink->on_fill(FillRecord{seq, event.ts_exchange, event.ts_received, storedPriceTicks, take, storedSide,
                                 event.order_id, storedTraderId, maker_source, taker_side, UnknownOrderIdSentinel,
                                 UnknownTraderIdSentinel, event.update_source, event.symbol_id});
    }

    if (liquidity == 0) {
        if (node->hook.is_linked()) {
            queue.erase(queue.iterator_to(*node));
        }
        order_pool_.release(node);
        if (queue.empty()) {
            book.erase(bIt);
        }
        order_info.erase(it);
    } else {
        node->qty = liquidity;
    }

    if (is_trade_on_passive_order && (event.update_source == UpdateSource::STRATEGY)) {
        emit_diagnostic(event, DiagnosticRecordCode::PAPER_ORDER_INVOKES_PASSIVE_MATCH_INSTEAD_OF_AGGRESSIVE_TRADE,
                        DiagnosticRecordSeverity::ERROR);
    }
}

void PaperTradingSimulator::init_from_l2_snapshot(const std::vector<Side>& sides,
                                                  const std::vector<std::int64_t>& prices,
                                                  const std::vector<std::int64_t>& quantities) {
    clear_state();
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
        auto& heap = isBid ? bids_heap : asks_heap;
        int sign = isBid ? 1 : -1;

        if (book.find(price) != book.end()) {
            throw std::runtime_error("Duplicate price found. Initializing from L2 snapshot requires volumes to be "
                                     "aggregated per price-level.");
        }
        auto* node = order_pool_.acquire(pool_config_.historical_capacity);
        if (!node) {
            NormalizedLobEvent diag_event{};
            diag_event.side = side;
            diag_event.update_type = UpdateType::ADD;
            diag_event.update_source = UpdateSource::HISTORICAL;
            diag_event.price_ticks = price;
            diag_event.quantity_lots = quantity;
            diag_event.order_id = UnknownOrderIdSentinel;
            emit_diagnostic(diag_event, DiagnosticRecordCode::HISTORICAL_ORDER_POOL_EXHAUSTED,
                            DiagnosticRecordSeverity::ERROR);
            continue;
        }
        node->order_id = UnknownOrderIdSentinel;
        node->trader_id = UnknownTraderIdSentinel;
        node->qty = quantity;
        node->source = UpdateSource::HISTORICAL;
        node->arrival_seq = order_arrival_seq++;

        auto& level = book[price];
        level.push_back(*node);
        heap.push(static_cast<std::int64_t>(sign * price));
    }
}

void PaperTradingSimulator::init_from_l3_snapshot(const std::vector<Side>& sides,
                                                  const std::vector<std::int64_t>& prices,
                                                  const std::vector<std::int64_t>& quantities,
                                                  const std::vector<std::int64_t>& order_ids,
                                                  const std::vector<std::int64_t>& trader_ids) {
    clear_state();
    auto N = sides.size();
    if ((N != prices.size()) || (N != quantities.size()) || (N != order_ids.size()) || (N != trader_ids.size())) {
        throw std::runtime_error("All arrays must have the same size.");
    }

    for (int i = 0; i < static_cast<int>(N); ++i) {
        auto side = sides[i];
        auto price = prices[i];
        auto quantity = quantities[i];
        auto order_id = order_ids[i];
        auto trader_id = trader_ids[i];

        // Enforce unique live order ids
        if (order_info.contains(order_id)) {
            throw std::runtime_error("Duplicate order_id found in L3 snapshot.");
        }

        bool isBid = side == Side::BUY;
        auto& book = isBid ? bids : asks;
        auto& heap = isBid ? bids_heap : asks_heap;

        // Get/create the price level in the real book
        auto& priorityQueue = book[price];
        const bool newLevel = priorityQueue.empty();

        auto* node = order_pool_.acquire(pool_config_.historical_capacity);
        if (!node) {
            NormalizedLobEvent diag_event{};
            diag_event.side = side;
            diag_event.update_type = UpdateType::ADD;
            diag_event.update_source = UpdateSource::HISTORICAL;
            diag_event.price_ticks = price;
            diag_event.quantity_lots = quantity;
            diag_event.order_id = order_id;
            diag_event.trader_id = trader_id;
            emit_diagnostic(diag_event, DiagnosticRecordCode::HISTORICAL_ORDER_POOL_EXHAUSTED,
                            DiagnosticRecordSeverity::ERROR);
            continue;
        }
        node->order_id = order_id;
        node->trader_id = trader_id;
        node->qty = quantity;
        node->source = UpdateSource::HISTORICAL;
        node->arrival_seq = order_arrival_seq++;

        // Append order at end (FIFO)
        priorityQueue.push_back(*node);

        // Only push price into heap the first time the level appears
        if (newLevel) {
            heap.push(isBid ? price : -price); // asks stored as negative
        }

        order_info.emplace(order_id, OrderInfo{side, price, node});
    }
}

void PaperTradingSimulator::clear_state() {
    for (auto& [price, queue] : bids) {
        queue.clear();
    }
    for (auto& [price, queue] : asks) {
        queue.clear();
    }
    for (auto& [price, level] : paper_bids) {
        level.orders.clear();
    }
    for (auto& [price, level] : paper_asks) {
        level.orders.clear();
    }
    bids.clear();
    asks.clear();
    bids_heap = std::priority_queue<std::int64_t>();
    asks_heap = std::priority_queue<std::int64_t>();
    order_info.clear();
    paper_orders.clear();
    paper_order_info.clear();
    paper_bids.clear();
    paper_asks.clear();
    order_arrival_seq = 0;
    reset_pools();
    if (sink) {
        sink->reset();
    }
}

std::optional<std::int64_t> PaperTradingSimulator::depth_at(Side side, std::int64_t price_ticks) const {
    bool isBid = side == Side::BUY;
    auto& book = isBid ? bids : asks;
    auto it = book.find(price_ticks);
    if (it == book.end()) {
        return std::nullopt;
    }

    const auto& priorityQueue = it->second;
    std::int64_t totalLiquidity =
        std::accumulate(priorityQueue.begin(), priorityQueue.end(), std::int64_t{0},
                        [](std::int64_t acc, const OrderNode& node) { return acc + node.qty; });
    return totalLiquidity;
}

std::vector<std::pair<std::int64_t, std::int64_t>> PaperTradingSimulator::l2_top_n(Side side, std::uint32_t n) const {
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
        const std::int64_t totalLiquidity =
            std::accumulate(queue.begin(), queue.end(), std::int64_t{0},
                            [](std::int64_t acc, const OrderNode& node) { return acc + node.qty; });
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

std::optional<std::int64_t> PaperTradingSimulator::get_best_price_ticks(Side side) const {
    const bool isBid = side == Side::BUY;
    const auto& book = isBid ? bids : asks;
    auto& heap = isBid ? bids_heap : asks_heap;

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

void PaperTradingSimulator::set_log_sink(ILogSink* sink) {
    this->sink = sink;
}

void PaperTradingSimulator::emit_diagnostic(const NormalizedLobEvent& event, DiagnosticRecordCode code,
                                            DiagnosticRecordSeverity severity) {
    if (sink == nullptr) {
        return;
    }
    sink->on_diagnostic(DiagnosticRecord{seq, event.ts_exchange, event.ts_received, code, severity, event.symbol_id});
}
