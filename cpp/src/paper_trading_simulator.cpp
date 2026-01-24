#include "lobsim/paper_trading_simulator.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>

PaperTradingSimulator::PaperTradingSimulator() : IMatchingEngine(), cfg_(Config{}) {}

PaperTradingSimulator::PaperTradingSimulator(Config cfg) : IMatchingEngine(), cfg_(cfg) {}

#include <iostream>

void PaperTradingSimulator::on_add(const NormalizedLobEvent& event) {
    const bool paper = event.update_source == UpdateSource::STRATEGY;
    const bool isBid = event.side == Side::BUY;
    // std::cout << "on_add: " << (isBid ? "BID" : "ASK") << " " << event.price_ticks << " qty " << event.quantity_lots << std::endl;

    if (order_info.contains(event.order_id) || paper_orders.contains(event.order_id) ||
        paper_order_info.contains(event.order_id)) {
        emit_diagnostic(event, DiagnosticRecordCode::ADD_DUPLICATE_ORDER_ID, DiagnosticRecordSeverity::WARNING);
        return;
    }

    if (event.quantity_lots <= 0) {
        emit_diagnostic(event, DiagnosticRecordCode::ADD_INVOKED_WITH_NEGATIVE_QUANTITY,
                        DiagnosticRecordSeverity::ERROR);
        return;
    }

    std::int64_t remaining = event.quantity_lots;

    if (isBid) {
        // BUY order crossing ASKS
        if (!asks.empty() && event.price_ticks >= asks.begin()->first) {
            if (paper) {
                for (auto itLevel = asks.begin(); itLevel != asks.end() && remaining > 0; ++itLevel) {
                    if (event.price_ticks < itLevel->first) break;
                    auto& levelList = itLevel->second;
                    for (const auto& node : levelList) {
                        if (remaining <= 0) break;
                        auto maker_order_id = std::get<0>(node);
                        const auto maker_trader_id = std::get<1>(node);
                        const auto makerQty = std::get<2>(node);
                        const auto maker_source = std::get<3>(node);
                        if (makerQty <= 0) continue;
                        const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);
                        if (sink) {
                            sink->on_fill(FillRecord{seq, event.ts_exchange, event.ts_received, itLevel->first, take, Side::SELL,
                                                     maker_order_id, maker_trader_id, maker_source, Side::BUY,
                                                     event.order_id, event.trader_id, event.update_source,
                                                     event.symbol_id});
                        }
                        remaining -= take;
                    }
                }
            } else {
                while (remaining > 0 && !asks.empty()) {
                    auto itLevel = asks.begin();
                    const std::int64_t bestPx = itLevel->first;
                    if (event.price_ticks < bestPx) break;
                    auto& levelList = itLevel->second;
                    std::int64_t tradedAtLevel = 0;
                    std::vector<std::pair<std::uint64_t, std::int64_t>> marketDeltas;
                    auto itNode = levelList.begin();
                    while (remaining > 0 && itNode != levelList.end()) {
                        auto& node = *itNode;
                        const auto maker_order_id = std::get<0>(node);
                        const auto maker_trader_id = std::get<1>(node);
                        auto& makerQty = std::get<2>(node);
                        const auto maker_source = std::get<3>(node);
                        if (makerQty <= 0) {
                            order_info.erase(maker_order_id);
                            itNode = levelList.erase(itNode);
                            continue;
                        }
                        const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);
                        if (sink) {
                            sink->on_fill(FillRecord{seq, event.ts_exchange, event.ts_received, bestPx, take, Side::SELL,
                                                     maker_order_id, maker_trader_id, maker_source, Side::BUY,
                                                     event.order_id, event.trader_id, event.update_source,
                                                     event.symbol_id});
                        }
                        marketDeltas.emplace_back(std::get<4>(node), -take);
                        makerQty -= take;
                        remaining -= take;
                        tradedAtLevel += take;
                        if (makerQty == 0) {
                            order_info.erase(maker_order_id);
                            itNode = levelList.erase(itNode);
                        } else {
                            ++itNode;
                        }
                    }
                    if (tradedAtLevel > 0) {
                        apply_paper_trade_at_level(Side::SELL, bestPx, tradedAtLevel, event);
                        if (auto paperLevel = find_paper_level(Side::SELL, bestPx)) {
                            for (const auto& [seq_val, delta] : marketDeltas) {
                                auto idxIt = paperLevel->market_index_by_seq.find(seq_val);
                                if (idxIt != paperLevel->market_index_by_seq.end()) {
                                    paperLevel->market_qty.add(idxIt->second, delta);
                                }
                            }
                        }
                    }
                    if (levelList.empty()) {
                        asks.erase(itLevel);
                    }
                }
            }
        }
        // If historical BUY aggressor still crosses paper orders, trade against them
        if (!paper && remaining > 0) {
            while (remaining > 0) {
                auto paperPx = best_paper_opposite_price(true); // oppIsAsk = true
                if (!paperPx.has_value()) break;
                if (event.price_ticks < *paperPx) break;
                const auto traded = trade_against_paper_level(Side::SELL, *paperPx, remaining, event);
                if (traded <= 0) break;
                remaining -= traded;
            }
        }
    } else {
        // SELL order crossing BIDS
        if (!bids.empty() && event.price_ticks <= bids.begin()->first) {
            if (paper) {
                for (auto itLevel = bids.begin(); itLevel != bids.end() && remaining > 0; ++itLevel) {
                    if (event.price_ticks > itLevel->first) break;
                    auto& levelList = itLevel->second;
                    for (const auto& node : levelList) {
                        if (remaining <= 0) break;
                        auto maker_order_id = std::get<0>(node);
                        const auto maker_trader_id = std::get<1>(node);
                        const auto makerQty = std::get<2>(node);
                        const auto maker_source = std::get<3>(node);
                        if (makerQty <= 0) continue;
                        const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);
                        if (sink) {
                            sink->on_fill(FillRecord{seq, event.ts_exchange, event.ts_received, itLevel->first, take, Side::BUY,
                                                     maker_order_id, maker_trader_id, maker_source, Side::SELL,
                                                     event.order_id, event.trader_id, event.update_source,
                                                     event.symbol_id});
                        }
                        remaining -= take;
                    }
                }
            } else {
                while (remaining > 0 && !bids.empty()) {
                    auto itLevel = bids.begin();
                    const std::int64_t bestPx = itLevel->first;
                    if (event.price_ticks > bestPx) break;
                    auto& levelList = itLevel->second;
                    std::int64_t tradedAtLevel = 0;
                    std::vector<std::pair<std::uint64_t, std::int64_t>> marketDeltas;
                    auto itNode = levelList.begin();
                    while (remaining > 0 && itNode != levelList.end()) {
                        auto& node = *itNode;
                        const auto maker_order_id = std::get<0>(node);
                        const auto maker_trader_id = std::get<1>(node);
                        auto& makerQty = std::get<2>(node);
                        const auto maker_source = std::get<3>(node);
                        if (makerQty <= 0) {
                            order_info.erase(maker_order_id);
                            itNode = levelList.erase(itNode);
                            continue;
                        }
                        const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);
                        if (sink) {
                            sink->on_fill(FillRecord{seq, event.ts_exchange, event.ts_received, bestPx, take, Side::BUY,
                                                     maker_order_id, maker_trader_id, maker_source, Side::SELL,
                                                     event.order_id, event.trader_id, event.update_source,
                                                     event.symbol_id});
                        }
                        marketDeltas.emplace_back(std::get<4>(node), -take);
                        makerQty -= take;
                        remaining -= take;
                        tradedAtLevel += take;
                        if (makerQty == 0) {
                            order_info.erase(maker_order_id);
                            itNode = levelList.erase(itNode);
                        } else {
                            ++itNode;
                        }
                    }
                    if (tradedAtLevel > 0) {
                        apply_paper_trade_at_level(Side::BUY, bestPx, tradedAtLevel, event);
                        if (auto paperLevel = find_paper_level(Side::BUY, bestPx)) {
                            for (const auto& [seq_val, delta] : marketDeltas) {
                                auto idxIt = paperLevel->market_index_by_seq.find(seq_val);
                                if (idxIt != paperLevel->market_index_by_seq.end()) {
                                    paperLevel->market_qty.add(idxIt->second, delta);
                                }
                            }
                        }
                    }
                    if (levelList.empty()) {
                        bids.erase(itLevel);
                    }
                }
            }
        }
        // If historical SELL aggressor still crosses paper orders, trade against them
        if (!paper && remaining > 0) {
            while (remaining > 0) {
                auto paperPx = best_paper_opposite_price(false); // oppIsAsk = false
                if (!paperPx.has_value()) break;
                if (event.price_ticks > *paperPx) break;
                const auto traded = trade_against_paper_level(Side::BUY, *paperPx, remaining, event);
                if (traded <= 0) break;
                remaining -= traded;
            }
        }
    }

    if (remaining > 0) {
        if (paper) {
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
            level.orders.push_back(event.order_id);
            auto itNew = std::prev(level.orders.end());
            paper_order_info.emplace(event.order_id, std::make_tuple(event.side, event.price_ticks, itNew));
            level.queued_lots += remaining;
        } else {
            if (isBid) {
                auto it = bids.find(event.price_ticks);
                if (it == bids.end()) {
                    // std::cout << "Creating new BID level " << event.price_ticks << std::endl;
                    it = bids.emplace(event.price_ticks, OrderPriorityQueue{}).first;
                }
                auto& level = it->second;
                level.emplace_back(event.order_id, event.trader_id, remaining, event.update_source, order_arrival_seq++);
                // std::cout << "Added to BID level. Level size: " << level.size() << std::endl;
                auto itNew = std::prev(level.end());
                order_info.emplace(event.order_id, std::make_tuple(event.side, event.price_ticks, itNew));
                if (auto paperLevel = find_paper_level(event.side, event.price_ticks)) {
                    const auto arrival_seq = std::get<4>(*itNew);
                    const std::size_t idx = paperLevel->market_seqs.size();
                    paperLevel->market_seqs.push_back(arrival_seq);
                    paperLevel->market_index_by_seq.emplace(arrival_seq, idx);
                    paperLevel->market_qty.ensure_size(paperLevel->market_seqs.size());
                    paperLevel->market_qty.add(idx, remaining);
                }
            } else {
                auto it = asks.find(event.price_ticks);
                if (it == asks.end()) {
                    it = asks.emplace(event.price_ticks, OrderPriorityQueue{}).first;
                }
                auto& level = it->second;
                level.emplace_back(event.order_id, event.trader_id, remaining, event.update_source, order_arrival_seq++);
                auto itNew = std::prev(level.end());
                order_info.emplace(event.order_id, std::make_tuple(event.side, event.price_ticks, itNew));
                if (auto paperLevel = find_paper_level(event.side, event.price_ticks)) {
                    const auto arrival_seq = std::get<4>(*itNew);
                    const std::size_t idx = paperLevel->market_seqs.size();
                    paperLevel->market_seqs.push_back(arrival_seq);
                    paperLevel->market_index_by_seq.emplace(arrival_seq, idx);
                    paperLevel->market_qty.ensure_size(paperLevel->market_seqs.size());
                    paperLevel->market_qty.add(idx, remaining);
                }
            }
        }
    }
}

void PaperTradingSimulator::update(const NormalizedLobEvent& event) {
    const std::uint64_t current_seq = seq++;
    current_update_seq = current_seq;
    switch (event.update_type) {
        case UpdateType::ADD:
            on_add(event);
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
        case UpdateType::AGGRESSIVE_TRADE:
            on_aggressive_trade(event);
            break;
        default:
            emit_diagnostic(event, DiagnosticRecordCode::INVALID_UPDATE_TYPE, DiagnosticRecordSeverity::ERROR);
            break;
    }
    if (sink) {
        sink->on_event_apply(EventApplyRecord{current_seq, event.ts_exchange, event.ts_received, event.side, event.update_type,
                                              event.update_source, event.price_ticks, event.quantity_lots,
                                              event.order_id, event.trader_id, event.aggressor_id, event.symbol_id});
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
    std::int64_t remaining = event.quantity_lots;

    if (isBid) {
        for (auto itLevel = asks.begin(); itLevel != asks.end() && remaining > 0; ++itLevel) {
            const auto bestPx = itLevel->first;
            const auto& levelList = itLevel->second;
            for (const auto& node : levelList) {
                if (remaining <= 0) break;
                const auto maker_order_id = std::get<0>(node);
                const auto maker_trader_id = std::get<1>(node);
                const auto makerQty = std::get<2>(node);
                const auto maker_source = std::get<3>(node);
                if (makerQty <= 0) continue;
                const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);
                if (sink) {
                    sink->on_fill(FillRecord{seq, event.ts_exchange, event.ts_received, bestPx, take, Side::SELL,
                                             maker_order_id, maker_trader_id, maker_source, Side::BUY,
                                             event.order_id, event.trader_id, event.update_source,
                                             event.symbol_id});
                }
                remaining -= take;
            }
        }
    } else {
        for (auto itLevel = bids.begin(); itLevel != bids.end() && remaining > 0; ++itLevel) {
            const auto bestPx = itLevel->first;
            const auto& levelList = itLevel->second;
            for (const auto& node : levelList) {
                if (remaining <= 0) break;
                const auto maker_order_id = std::get<0>(node);
                const auto maker_trader_id = std::get<1>(node);
                const auto makerQty = std::get<2>(node);
                const auto maker_source = std::get<3>(node);
                if (makerQty <= 0) continue;
                const std::int64_t take = std::min<std::int64_t>(makerQty, remaining);
                if (sink) {
                    sink->on_fill(FillRecord{seq, event.ts_exchange, event.ts_received, bestPx, take, Side::BUY,
                                             maker_order_id, maker_trader_id, maker_source, Side::SELL,
                                             event.order_id, event.trader_id, event.update_source,
                                             event.symbol_id});
                }
                remaining -= take;
            }
        }
    }

    if (remaining > 0) {
        const bool oppIsAsk = isBid;
        while (remaining > 0) {
            auto paperPx = best_paper_opposite_price(oppIsAsk);
            if (!paperPx) break;
            const auto traded =
                trade_against_paper_level(oppIsAsk ? Side::SELL : Side::BUY, *paperPx, remaining, event);
            if (traded <= 0) break;
            remaining -= traded;
        }
    }
}

std::optional<std::int64_t>
PaperTradingSimulator::best_opposite_price(const BidBook& opposite_book) {
    if (opposite_book.empty()) return std::nullopt;
    return opposite_book.begin()->first;
}

std::optional<std::int64_t>
PaperTradingSimulator::best_opposite_price(const AskBook& opposite_book) {
    if (opposite_book.empty()) return std::nullopt;
    return opposite_book.begin()->first;
}

std::optional<std::int64_t> PaperTradingSimulator::best_paper_opposite_price(bool opposite_is_ask) {
    if (opposite_is_ask) {
        if (paper_asks.empty()) return std::nullopt;
        return paper_asks.begin()->first;
    } else {
        if (paper_bids.empty()) return std::nullopt;
        return paper_bids.begin()->first;
    }
}

PaperTradingSimulator::PaperOrderLevel& PaperTradingSimulator::ensure_paper_level(Side side, std::int64_t price_ticks) {
    if (side == Side::BUY) {
        auto [it, inserted] = paper_bids.emplace(price_ticks, PaperOrderLevel{});
        if (inserted) {
            auto& level = it->second;
            auto bookIt = bids.find(price_ticks);
            if (bookIt != bids.end()) {
                const auto& queue = bookIt->second;
                level.market_seqs.reserve(queue.size());
                level.market_qty.ensure_size(queue.size());
                std::size_t idx = 0;
                for (const auto& node : queue) {
                    const auto seq_val = std::get<4>(node);
                    const auto qty = std::get<2>(node);
                    level.market_seqs.push_back(seq_val);
                    level.market_index_by_seq.emplace(seq_val, idx);
                    level.market_qty.ensure_size(idx + 1);
                    level.market_qty.add(idx, qty);
                    ++idx;
                }
            }
        }
        return it->second;
    } else {
        auto [it, inserted] = paper_asks.emplace(price_ticks, PaperOrderLevel{});
        if (inserted) {
            auto& level = it->second;
            auto bookIt = asks.find(price_ticks);
            if (bookIt != asks.end()) {
                const auto& queue = bookIt->second;
                level.market_seqs.reserve(queue.size());
                level.market_qty.ensure_size(queue.size());
                std::size_t idx = 0;
                for (const auto& node : queue) {
                    const auto seq_val = std::get<4>(node);
                    const auto qty = std::get<2>(node);
                    level.market_seqs.push_back(seq_val);
                    level.market_index_by_seq.emplace(seq_val, idx);
                    level.market_qty.ensure_size(idx + 1);
                    level.market_qty.add(idx, qty);
                    ++idx;
                }
            }
        }
        return it->second;
    }
}

PaperTradingSimulator::PaperOrderLevel* PaperTradingSimulator::find_paper_level(Side side, std::int64_t price_ticks) {
    if (side == Side::BUY) {
        auto it = paper_bids.find(price_ticks);
        if (it == paper_bids.end()) return nullptr;
        return &it->second;
    } else {
        auto it = paper_asks.find(price_ticks);
        if (it == paper_asks.end()) return nullptr;
        return &it->second;
    }
}

void PaperTradingSimulator::apply_paper_trade_at_level(Side passive_side, std::int64_t price_ticks,
                                                       std::int64_t trade_lots, const NormalizedLobEvent& aggressor) {
    if (trade_lots <= 0) return;
    PaperOrderLevel* levelPtr = nullptr;
    if (passive_side == Side::BUY) {
        auto it = paper_bids.find(price_ticks);
        if (it != paper_bids.end()) levelPtr = &it->second;
    } else {
        auto it = paper_asks.find(price_ticks);
        if (it != paper_asks.end()) levelPtr = &it->second;
    }
    if (!levelPtr) return;
    auto& level = *levelPtr;
    auto it = level.orders.begin();
    std::int64_t marketConsumed = 0;
    while (it != level.orders.end() && trade_lots > 0) {
        const auto order_id = *it;
        auto orderIt = paper_orders.find(order_id);
        if (orderIt == paper_orders.end()) {
            it = level.orders.erase(it);
            paper_order_info.erase(order_id);
            continue;
        }
        auto& order = orderIt->second;
        const auto totalMarketAhead = [&]() {
            if (level.market_seqs.empty()) return std::int64_t{0};
            const auto pos = static_cast<std::size_t>(
                std::lower_bound(level.market_seqs.begin(), level.market_seqs.end(), order.placement_seq) -
                level.market_seqs.begin());
            return level.market_qty.sum(pos);
        }();
        const auto aheadMarket = totalMarketAhead > marketConsumed ? (totalMarketAhead - marketConsumed) : std::int64_t{0};
        const auto aheadPaper = level.paper_qty.sum(order.paper_index);
        const auto queueAhead = aheadMarket + aheadPaper;
        if (queueAhead >= trade_lots) break;
        trade_lots -= queueAhead;
        marketConsumed += aheadMarket;
        if (order.remaining_qty <= 0) {
            auto next = std::next(it);
            level.orders.erase(it);
            paper_order_info.erase(order_id);
            paper_orders.erase(orderIt);
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
            sink->on_fill(FillRecord{seq, aggressor.ts_exchange, aggressor.ts_received, price_ticks, fillQty,
                                     passive_side, order_id, order.original_event.trader_id, UpdateSource::STRATEGY,
                                     taker_side, eventIsAggressor ? aggressor.order_id : UnknownOrderIdSentinel,
                                     eventIsAggressor ? aggressor.trader_id : UnknownTraderIdSentinel,
                                     aggressor.update_source, aggressor.symbol_id});
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
            remove_paper_order(level, it, fillQty, PaperOrderStatus::FILLED);
            it = next;
        }
    }
    if (level.orders.empty()) {
        if (passive_side == Side::BUY) paper_bids.erase(price_ticks);
        else paper_asks.erase(price_ticks);
    }
}

std::int64_t PaperTradingSimulator::trade_against_paper_level(Side passive_side, std::int64_t price_ticks,
                                                               std::int64_t trade_lots,
                                                               const NormalizedLobEvent& aggressor) {
    if (trade_lots <= 0) return 0;
    PaperOrderLevel* levelPtr = nullptr;
    if (passive_side == Side::BUY) {
        auto it = paper_bids.find(price_ticks);
        if (it != paper_bids.end()) levelPtr = &it->second;
    } else {
        auto it = paper_asks.find(price_ticks);
        if (it != paper_asks.end()) levelPtr = &it->second;
    }
    if (!levelPtr) return 0;
    auto& level = *levelPtr;
    auto it = level.orders.begin();
    std::int64_t consumed = 0;
    while (trade_lots > 0 && it != level.orders.end()) {
        const auto order_id = *it;
        auto orderIt = paper_orders.find(order_id);
        if (orderIt == paper_orders.end()) {
            it = level.orders.erase(it);
            paper_order_info.erase(order_id);
            continue;
        }
        auto& order = orderIt->second;
        if (order.remaining_qty <= 0 || order.status == PaperOrderStatus::FILLED ||
            order.status == PaperOrderStatus::CANCELLED || order.status == PaperOrderStatus::REJECTED) {
            auto next = std::next(it);
            level.orders.erase(it);
            paper_order_info.erase(order_id);
            paper_orders.erase(orderIt);
            it = next;
            continue;
        }

        const auto fillQty = std::min(order.remaining_qty, trade_lots);
        if (fillQty > 0 && sink) {
            auto taker_side = passive_side == Side::BUY ? Side::SELL : Side::BUY;
            sink->on_fill(FillRecord{seq, aggressor.ts_exchange, aggressor.ts_received, price_ticks, fillQty,
                                     passive_side, order_id, order.original_event.trader_id, UpdateSource::STRATEGY,
                                     taker_side, aggressor.order_id, aggressor.trader_id, aggressor.update_source,
                                     aggressor.symbol_id});
        }
        order.remaining_qty -= fillQty;
        trade_lots -= fillQty;
        consumed += fillQty;
        if (order.remaining_qty == 0) {
            auto next = std::next(it);
            remove_paper_order(level, it, fillQty, PaperOrderStatus::FILLED);
            it = next;
        } else {
            order.status = PaperOrderStatus::PARTIALLY_FILLED;
            level.queued_lots -= fillQty;
            level.paper_qty.add(order.paper_index, -fillQty);
            ++it;
        }
    }
    if (level.orders.empty()) {
        if (passive_side == Side::BUY) paper_bids.erase(price_ticks);
        else paper_asks.erase(price_ticks);
    }
    return consumed;
}

void PaperTradingSimulator::remove_paper_order(PaperOrderLevel& level, PaperOrderQueue::iterator it,
                                               std::int64_t removed_qty, PaperOrderStatus status) {
    const auto order_id = *it;
    auto orderIt = paper_orders.find(order_id);
    if (orderIt == paper_orders.end()) {
        level.orders.erase(it);
        paper_order_info.erase(order_id);
        return;
    }
    auto& order = orderIt->second;
    if (removed_qty <= 0) removed_qty = order.remaining_qty;
    removed_qty = std::min(removed_qty, order.remaining_qty);
    order.remaining_qty -= removed_qty;
    order.status = status;
    level.queued_lots -= removed_qty;
    level.paper_qty.add(order.paper_index, -removed_qty);
    level.orders.erase(it);
    paper_order_info.erase(order_id);
    paper_orders.erase(orderIt);
}

void PaperTradingSimulator::reduce_paper_order(const NormalizedLobEvent& event) {
    if (event.quantity_lots < 0) {
        emit_diagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_PAPER_ORDER_BY_NEGATIVE_QUANTITY, DiagnosticRecordSeverity::ERROR);
        return;
    }
    if (event.quantity_lots == 0) {
        emit_diagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_PAPER_ORDER_BY_ZERO_QUANTITY, DiagnosticRecordSeverity::WARNING);
        return;
    }
    auto infoIt = paper_order_info.find(event.order_id);
    if (infoIt == paper_order_info.end()) return;
    auto [storedSide, storedPriceTicks, queueLocationIt] = infoIt->second;
    auto level = find_paper_level(storedSide, storedPriceTicks);
    if (!level) {
        paper_order_info.erase(infoIt);
        paper_orders.erase(event.order_id);
        return;
    }
    auto orderIt = paper_orders.find(event.order_id);
    if (orderIt == paper_orders.end()) {
        level->orders.erase(queueLocationIt);
        paper_order_info.erase(infoIt);
        return;
    }
    auto& order = orderIt->second;
    auto take = std::min(order.remaining_qty, event.quantity_lots);
    if (take <= 0) return;
    if (take >= order.remaining_qty) {
        remove_paper_order(*level, queueLocationIt, order.remaining_qty, PaperOrderStatus::CANCELLED);
        if (level->orders.empty()) {
            if (storedSide == Side::BUY) paper_bids.erase(storedPriceTicks);
            else paper_asks.erase(storedPriceTicks);
        }
        return;
    }
    order.remaining_qty -= take;
    level->queued_lots -= take;
    level->paper_qty.add(order.paper_index, -take);
}

void PaperTradingSimulator::set_paper_order(std::int64_t order_id, std::int64_t new_qty) {
    if (new_qty < 0) new_qty = 0;
    auto infoIt = paper_order_info.find(order_id);
    if (infoIt == paper_order_info.end()) return;
    auto [storedSide, storedPriceTicks, queueLocationIt] = infoIt->second;
    auto level = find_paper_level(storedSide, storedPriceTicks);
    if (!level) {
        paper_order_info.erase(infoIt);
        paper_orders.erase(order_id);
        return;
    }
    auto orderIt = paper_orders.find(order_id);
    if (orderIt == paper_orders.end()) {
        level->orders.erase(queueLocationIt);
        paper_order_info.erase(infoIt);
        return;
    }
    auto& order = orderIt->second;
    if (new_qty == order.remaining_qty) return;
    if (new_qty == 0) {
        remove_paper_order(*level, queueLocationIt, order.remaining_qty, PaperOrderStatus::CANCELLED);
        if (level->orders.empty()) {
            if (storedSide == Side::BUY) paper_bids.erase(storedPriceTicks);
            else paper_asks.erase(storedPriceTicks);
        }
        return;
    }
    if (new_qty < order.remaining_qty) {
        const auto delta = order.remaining_qty - new_qty;
        order.remaining_qty = new_qty;
        level->queued_lots -= delta;
        level->paper_qty.add(order.paper_index, -delta);
    } else {
        const auto delta = new_qty - order.remaining_qty;
        order.remaining_qty = new_qty;
        level->queued_lots += delta;
        level->paper_qty.add(order.paper_index, delta);
    }
}

void PaperTradingSimulator::on_delete(const NormalizedLobEvent& event) {
    if (event.update_source == UpdateSource::STRATEGY) {
        auto it = paper_order_info.find(event.order_id);
        if (it == paper_order_info.end()) {
            emit_diagnostic(event, DiagnosticRecordCode::DELETE_NON_EXISTING_PAPER_ORDER_ID, DiagnosticRecordSeverity::WARNING);
            return;
        }
        auto [storedSide, storedPriceTicks, queueLocationIt] = it->second;
        auto level = find_paper_level(storedSide, storedPriceTicks);
        if (!level) {
            paper_order_info.erase(it);
            paper_orders.erase(event.order_id);
            return;
        }
        auto orderIt = paper_orders.find(event.order_id);
        if (orderIt == paper_orders.end()) {
            level->orders.erase(queueLocationIt);
            paper_order_info.erase(it);
            return;
        }
        remove_paper_order(*level, queueLocationIt, orderIt->second.remaining_qty, PaperOrderStatus::CANCELLED);
        if (level->orders.empty()) {
            if (storedSide == Side::BUY) paper_bids.erase(storedPriceTicks);
            else paper_asks.erase(storedPriceTicks);
        }
        return;
    }
    auto it = order_info.find(event.order_id);
    if (it == order_info.end()) {
        emit_diagnostic(event, DiagnosticRecordCode::DELETE_NON_EXISTING_HISTORICAL_ORDER_ID, DiagnosticRecordSeverity::WARNING);
        return;
    }
    auto& info = it->second;
    auto storedSide = std::get<0>(info);
    if (storedSide != event.side) emit_diagnostic(event, DiagnosticRecordCode::PROVIDED_SIDE_ON_DELETE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID, DiagnosticRecordSeverity::WARNING);
    auto storedPriceTicks = std::get<1>(info);
    if (storedPriceTicks != event.price_ticks) emit_diagnostic(event, DiagnosticRecordCode::PROVIDED_PRICE_ON_DELETE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID, DiagnosticRecordSeverity::WARNING);
    auto queueLocationIt = std::get<2>(info);
    const bool isBid = storedSide == Side::BUY;
    if (isBid) {
        auto bIt = bids.find(storedPriceTicks);
        if (bIt == bids.end()) {
            emit_diagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK, DiagnosticRecordSeverity::ERROR);
            return;
        }
        OrderPriorityQueue& queue = bIt->second;
        const auto arrivalSeq = std::get<4>(*queueLocationIt);
        const auto removed_qty = std::get<2>(*queueLocationIt);
        if (auto level = find_paper_level(storedSide, storedPriceTicks); level && removed_qty > 0) {
            auto idxIt = level->market_index_by_seq.find(arrivalSeq);
            if (idxIt != level->market_index_by_seq.end()) level->market_qty.add(idxIt->second, -removed_qty);
        }
        queue.erase(queueLocationIt);
        if (queue.empty()) bids.erase(bIt);
    } else {
        auto bIt = asks.find(storedPriceTicks);
        if (bIt == asks.end()) {
            emit_diagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK, DiagnosticRecordSeverity::ERROR);
            return;
        }
        OrderPriorityQueue& queue = bIt->second;
        const auto arrivalSeq = std::get<4>(*queueLocationIt);
        const auto removed_qty = std::get<2>(*queueLocationIt);
        if (auto level = find_paper_level(storedSide, storedPriceTicks); level && removed_qty > 0) {
            auto idxIt = level->market_index_by_seq.find(arrivalSeq);
            if (idxIt != level->market_index_by_seq.end()) level->market_qty.add(idxIt->second, -removed_qty);
        }
        queue.erase(queueLocationIt);
        if (queue.empty()) asks.erase(bIt);
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
    if (event.quantity_lots < 0) emit_diagnostic(event, DiagnosticRecordCode::SET_WITH_NEGATIVE_LIQUIDITY_REQUESTED_WAS_SET_TO_ZERO, DiagnosticRecordSeverity::WARNING);
    auto it = order_info.find(event.order_id);
    if (it == order_info.end()) {
        emit_diagnostic(event, DiagnosticRecordCode::SET_NON_EXISTING_ORDER_ID_IS_REJECTED, DiagnosticRecordSeverity::WARNING);
        return;
    }
    auto& info = it->second;
    auto storedSide = std::get<0>(info);
    auto storedPriceTicks = std::get<1>(info);
    auto queueLocationIt = std::get<2>(info);
    const bool isBid = storedSide == Side::BUY;
    if (storedSide != event.side) emit_diagnostic(event, DiagnosticRecordCode::PROVIDED_SIDE_ON_SET_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID, DiagnosticRecordSeverity::WARNING);
    if (storedPriceTicks != event.price_ticks) emit_diagnostic(event, DiagnosticRecordCode::PROVIDED_PRICE_ON_SET_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID, DiagnosticRecordSeverity::WARNING);
    if (isBid) {
        auto bIt = bids.find(storedPriceTicks);
        if (bIt == bids.end()) {
            emit_diagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK, DiagnosticRecordSeverity::ERROR);
            return;
        }
        auto& queueElement = *queueLocationIt;
        const auto arrivalSeq = std::get<4>(queueElement);
        auto newQuantity = event.quantity_lots < 0 ? 0 : event.quantity_lots;
        const auto oldQuantity = std::get<2>(queueElement);
        const auto delta = newQuantity - oldQuantity;
        if (auto level = find_paper_level(storedSide, storedPriceTicks); level && delta != 0) {
            auto idxIt = level->market_index_by_seq.find(arrivalSeq);
            if (idxIt != level->market_index_by_seq.end()) level->market_qty.add(idxIt->second, delta);
        }
        if (newQuantity == 0) {
            bIt->second.erase(queueLocationIt);
            if (bIt->second.empty()) bids.erase(bIt);
            order_info.erase(it);
        } else {
            std::get<2>(queueElement) = newQuantity;
        }
    } else {
        auto bIt = asks.find(storedPriceTicks);
        if (bIt == asks.end()) {
            emit_diagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK, DiagnosticRecordSeverity::ERROR);
            return;
        }
        auto& queueElement = *queueLocationIt;
        const auto arrivalSeq = std::get<4>(queueElement);
        auto newQuantity = event.quantity_lots < 0 ? 0 : event.quantity_lots;
        const auto oldQuantity = std::get<2>(queueElement);
        const auto delta = newQuantity - oldQuantity;
        if (auto level = find_paper_level(storedSide, storedPriceTicks); level && delta != 0) {
            auto idxIt = level->market_index_by_seq.find(arrivalSeq);
            if (idxIt != level->market_index_by_seq.end()) level->market_qty.add(idxIt->second, delta);
        }
        if (newQuantity == 0) {
            bIt->second.erase(queueLocationIt);
            if (bIt->second.empty()) asks.erase(bIt);
            order_info.erase(it);
        } else {
            std::get<2>(queueElement) = newQuantity;
        }
    }
}

void PaperTradingSimulator::on_partial_order_cancel(const NormalizedLobEvent& event, bool is_trade_on_passive_order) {
    if (event.quantity_lots < 0) {
        emit_diagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_NEGATIVE_QUANTITY, DiagnosticRecordSeverity::ERROR);
        return;
    }
    if (event.quantity_lots == 0) {
        emit_diagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_ORDER_BY_ZERO_QUANTITY, DiagnosticRecordSeverity::WARNING);
        return;
    }
    auto it = order_info.find(event.order_id);
    if (it == order_info.end()) {
        emit_diagnostic(event, DiagnosticRecordCode::REQUESTED_REDUCE_NON_EXISTING_ORDER_ID, DiagnosticRecordSeverity::WARNING);
        return;
    }
    auto& info = it->second;
    auto storedSide = std::get<0>(info);
    if (storedSide != event.side) emit_diagnostic(event, DiagnosticRecordCode::PROVIDED_SIDE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_SIDE_FOR_ORDER_ID, DiagnosticRecordSeverity::WARNING);
    auto storedPriceTicks = std::get<1>(info);
    if (storedPriceTicks != event.price_ticks) emit_diagnostic(event, DiagnosticRecordCode::PROVIDED_PRICE_ON_ORDER_REDUCE_DIFFERS_FROM_ORIGINAL_PRICE_FOR_ORDER_ID, DiagnosticRecordSeverity::WARNING);
    auto queueLocationIt = std::get<2>(info);
    const bool isBid = storedSide == Side::BUY;
    if (isBid) {
        auto bIt = bids.find(storedPriceTicks);
        if (bIt == bids.end()) {
            emit_diagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK, DiagnosticRecordSeverity::ERROR);
            return;
        }
        auto& queueElement = *queueLocationIt;
        // std::cout << "Canceling Bid Order " << event.order_id << " Liquidity: " << std::get<2>(queueElement) << std::endl;
        auto liquidity = std::get<2>(queueElement);
        auto take = std::min<std::int64_t>(liquidity, event.quantity_lots);
        if (event.quantity_lots > liquidity) emit_diagnostic(event, DiagnosticRecordCode::REQUESTED_ORDER_REDUCE_WITH_VOLUME_LARGER_THAN_AVAILABLE_FOR_ORDER_ID, DiagnosticRecordSeverity::WARNING);
        liquidity -= take;
        const auto arrivalSeq = std::get<4>(queueElement);
        if (take > 0) {
            if (is_trade_on_passive_order) apply_paper_trade_at_level(storedSide, storedPriceTicks, take, event);
            if (auto level = find_paper_level(storedSide, storedPriceTicks)) {
                auto idxIt = level->market_index_by_seq.find(arrivalSeq);
                if (idxIt != level->market_index_by_seq.end()) level->market_qty.add(idxIt->second, -take);
            }
        }
        if (is_trade_on_passive_order && sink) {
            sink->on_fill(FillRecord{seq, event.ts_exchange, event.ts_received, storedPriceTicks, take, storedSide,
                                     event.order_id, std::get<1>(queueElement), std::get<3>(queueElement), Side::SELL, UnknownOrderIdSentinel,
                                     UnknownTraderIdSentinel, event.update_source, event.symbol_id});
        }
        if (liquidity == 0) {
            // std::cout << "Erasing Bid Order from list" << std::endl;
            bIt->second.erase(queueLocationIt);
            if (bIt->second.empty()) {
                // std::cout << "Bid Level Empty. Erasing level." << std::endl;
                bids.erase(bIt);
            }
            // std::cout << "Erasing order info" << std::endl;
            order_info.erase(it);
        } else {
            std::get<2>(queueElement) = liquidity;
        }
    } else {
        auto bIt = asks.find(storedPriceTicks);
        if (bIt == asks.end()) {
            emit_diagnostic(event, DiagnosticRecordCode::CORRUPT_BOOK_PRICE_IN_ORDER_INFO_BUT_NOT_IN_BOOK, DiagnosticRecordSeverity::ERROR);
            return;
        }
        auto& queueElement = *queueLocationIt;
        auto liquidity = std::get<2>(queueElement);
        auto take = std::min<std::int64_t>(liquidity, event.quantity_lots);
        if (event.quantity_lots > liquidity) emit_diagnostic(event, DiagnosticRecordCode::REQUESTED_ORDER_REDUCE_WITH_VOLUME_LARGER_THAN_AVAILABLE_FOR_ORDER_ID, DiagnosticRecordSeverity::WARNING);
        liquidity -= take;
        const auto arrivalSeq = std::get<4>(queueElement);
        if (take > 0) {
            if (is_trade_on_passive_order) apply_paper_trade_at_level(storedSide, storedPriceTicks, take, event);
            if (auto level = find_paper_level(storedSide, storedPriceTicks)) {
                auto idxIt = level->market_index_by_seq.find(arrivalSeq);
                if (idxIt != level->market_index_by_seq.end()) level->market_qty.add(idxIt->second, -take);
            }
        }
        if (is_trade_on_passive_order && sink) {
            sink->on_fill(FillRecord{seq, event.ts_exchange, event.ts_received, storedPriceTicks, take, storedSide,
                                     event.order_id, std::get<1>(queueElement), std::get<3>(queueElement), Side::BUY, UnknownOrderIdSentinel,
                                     UnknownTraderIdSentinel, event.update_source, event.symbol_id});
        }
        if (liquidity == 0) {
            bIt->second.erase(queueLocationIt);
            if (bIt->second.empty()) asks.erase(bIt);
            order_info.erase(it);
        } else {
            std::get<2>(queueElement) = liquidity;
        }
    }
    if (is_trade_on_passive_order && (event.update_source == UpdateSource::STRATEGY)) emit_diagnostic(event, DiagnosticRecordCode::PAPER_ORDER_INVOKES_PASSIVE_MATCH_INSTEAD_OF_AGGRESSIVE_TRADE, DiagnosticRecordSeverity::ERROR);
}

void PaperTradingSimulator::init_from_l2_snapshot(std::span<const Side> sides,
                                                  std::span<const std::int64_t> prices,
                                                  std::span<const std::int64_t> quantities) {
    clear_state();
    auto N = sides.size();
    if ((N != prices.size()) || (N != quantities.size())) throw std::runtime_error("All arrays must have the same size.");
    for (int i = 0; i < static_cast<int>(N); ++i) {
        auto side = sides[i];
        auto price = prices[i];
        auto quantity = quantities[i];
        if (side == Side::BUY) {
            if (bids.find(price) != bids.end()) throw std::runtime_error("Duplicate price in L2 snapshot.");
            bids.emplace(price, std::list<OrderTraderQuantitySource>{{UnknownOrderIdSentinel, UnknownTraderIdSentinel, quantity, UpdateSource::HISTORICAL, order_arrival_seq++}});
        } else {
            if (asks.find(price) != asks.end()) throw std::runtime_error("Duplicate price in L2 snapshot.");
            asks.emplace(price, std::list<OrderTraderQuantitySource>{{UnknownOrderIdSentinel, UnknownTraderIdSentinel, quantity, UpdateSource::HISTORICAL, order_arrival_seq++}});
        }
    }
}

void PaperTradingSimulator::init_from_l3_snapshot(std::span<const Side> sides,
                                                  std::span<const std::int64_t> prices,
                                                  std::span<const std::int64_t> quantities,
                                                  std::span<const std::int64_t> order_ids,
                                                  std::span<const std::int64_t> trader_ids) {
    clear_state();
    auto N = sides.size();
    if ((N != prices.size()) || (N != quantities.size()) || (N != order_ids.size()) || (N != trader_ids.size())) throw std::runtime_error("All arrays must have the same size.");
    for (int i = 0; i < static_cast<int>(N); ++i) {
        auto side = sides[i];
        auto price = prices[i];
        auto quantity = quantities[i];
        auto order_id = order_ids[i];
        auto trader_id = trader_ids[i];
        if (order_info.contains(order_id)) throw std::runtime_error("Duplicate order_id in L3 snapshot.");
        if (side == Side::BUY) {
            auto it = bids.find(price);
            if (it == bids.end()) {
                it = bids.emplace(price, OrderPriorityQueue{}).first;
            }
            auto& level = it->second;
            level.emplace_back(order_id, trader_id, quantity, UpdateSource::HISTORICAL, order_arrival_seq++);
            order_info.emplace(order_id, std::make_tuple(side, price, std::prev(level.end())));
        } else {
            auto it = asks.find(price);
            if (it == asks.end()) {
                it = asks.emplace(price, OrderPriorityQueue{}).first;
            }
            auto& level = it->second;
            level.emplace_back(order_id, trader_id, quantity, UpdateSource::HISTORICAL, order_arrival_seq++);
            order_info.emplace(order_id, std::make_tuple(side, price, std::prev(level.end())));
        }
    }
}

void PaperTradingSimulator::clear_state() {
    bids.clear();
    asks.clear();
    order_info.clear();
    paper_orders.clear();
    paper_order_info.clear();
    paper_bids.clear();
    paper_asks.clear();
    order_arrival_seq = 0;
    if (sink) sink->reset();
}

std::optional<std::int64_t> PaperTradingSimulator::depth_at(Side side, std::int64_t price_ticks) const {
    const bool isBid = side == Side::BUY;
    if (isBid) {
        auto it = bids.find(price_ticks);
        if (it == bids.end()) return std::nullopt;
        return std::accumulate(it->second.begin(), it->second.end(), std::int64_t{0}, [](std::int64_t acc, const auto& node) { return acc + std::get<2>(node); });
    } else {
        auto it = asks.find(price_ticks);
        if (it == asks.end()) return std::nullopt;
        return std::accumulate(it->second.begin(), it->second.end(), std::int64_t{0}, [](std::int64_t acc, const auto& node) { return acc + std::get<2>(node); });
    }
}

std::vector<std::pair<std::int64_t, std::int64_t>> PaperTradingSimulator::l2_top_n(Side side, std::uint32_t n) const {
    if (n == 0) return {};
    const bool isBid = side == Side::BUY;
    std::vector<std::pair<std::int64_t, std::int64_t>> top;
    if (isBid) {
        for (auto it = bids.begin(); it != bids.end() && top.size() < n; ++it) {
            std::int64_t total = std::accumulate(it->second.begin(), it->second.end(), std::int64_t{0}, [](std::int64_t acc, const auto& node) { return acc + std::get<2>(node); });
            // std::cout << "Bid Level " << it->first << " Total: " << total << std::endl;
            if (total > 0) top.emplace_back(it->first, total);
        }
    } else {
        for (auto it = asks.begin(); it != asks.end() && top.size() < n; ++it) {
            std::int64_t total = std::accumulate(it->second.begin(), it->second.end(), std::int64_t{0}, [](std::int64_t acc, const auto& node) { return acc + std::get<2>(node); });
            if (total > 0) top.emplace_back(it->first, total);
        }
    }
    return top;
}

std::optional<std::int64_t> PaperTradingSimulator::get_best_price_ticks(Side side) const {
    if (side == Side::BUY) {
        if (bids.empty()) return std::nullopt;
        return bids.begin()->first;
    } else {
        if (asks.empty()) return std::nullopt;
        return asks.begin()->first;
    }
}

void PaperTradingSimulator::set_log_sink(std::shared_ptr<ILogSink> sink) { this->sink = std::move(sink); }

void PaperTradingSimulator::emit_diagnostic(const NormalizedLobEvent& event, DiagnosticRecordCode code, DiagnosticRecordSeverity severity) {
    if (sink) sink->on_diagnostic(DiagnosticRecord{current_update_seq, event.ts_exchange, event.ts_received, code, severity, event.symbol_id});
}
