#pragma once
#include "simex/engine.hpp"
#include "simex/log_sink.hpp"

class PaperTradingSimulatorCore final : public IMatchingEngine {
public:
    PaperTradingSimulatorCore() : IMatchingEngine() {}
    PaperTradingSimulatorCore(std::vector<Side>& sides, std::vector<std::int64_t>& prices,
                              std::vector<std::int64_t>& quantities, ILogSink* sink = nullptr)
        : IMatchingEngine() {
        this->sink = sink;
        initFromL2Snapshot(sides, prices, quantities);
    }
    ~PaperTradingSimulatorCore() = default;

    void update(std::int64_t tsExchange, std::int64_t tsReceived, Side side, UpdateType updateType,
                std::int64_t priceTicks, std::int64_t quantityLots, std::int64_t orderId,
                std::int64_t traderId = UnknownTraderIdSentinel, std::int64_t aggressorId = NoAggressorNeededSentinel,
                UpdateSource updateSource = UpdateSource::HISTORICAL) override;

    void initFromL2Snapshot(const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
                            const std::vector<std::int64_t>& quantities) override;

    void initFromL3Snapshot(const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
                            const std::vector<std::int64_t>& quantities, const std::vector<std::int64_t>& orderIds,
                            const std::vector<std::int64_t>& traderIds) override;

    std::optional<std::int64_t> depthAt(Side side, std::int64_t priceTicks) const;
    std::vector<std::pair<std::int64_t, std::int64_t>> l2TopN(Side side, std::uint32_t n) const;
    std::int64_t getBestPriceTicks(Side side) const;
    void setLogSink(ILogSink* sink);

private:
    using OrderTraderQuantitySource = std::tuple<std::int64_t, std::int64_t, std::int64_t, UpdateSource>;
    using OrderPriorityQueue = std::list<OrderTraderQuantitySource>;
    using Book = std::unordered_map<std::int64_t, OrderPriorityQueue>;

    void onAdd(std::int64_t tsExchange, std::int64_t tsReceived, Side side, std::int64_t priceTicks,
               std::int64_t quantityLots, std::int64_t orderId, std::int64_t traderId, UpdateSource updateSource);
    void onSubtract(std::int64_t tsExchange, std::int64_t tsReceived, Side side, std::int64_t priceTicks,
                    std::int64_t quantityLots, std::int64_t orderId, std::int64_t traderId, UpdateSource updateSource);
    void onDelete(Side side, std::int64_t priceTicks, std::int64_t quantityLots, std::int64_t orderId,
                  std::int64_t traderId, UpdateSource updateSource);
    void onMatch(std::int64_t tsExchange, std::int64_t tsReceived, Side side, std::int64_t priceTicks,
                 std::int64_t quantityLots, std::int64_t orderId, std::int64_t traderId, UpdateSource updateSource);
    void onSet(Side side, std::int64_t priceTicks, std::int64_t quantityLots, std::int64_t orderId,
               std::int64_t traderId, UpdateSource updateSource);

    void onPartialOrderCancel(std::int64_t tsExchange, std::int64_t tsReceived, Side side, std::int64_t priceTicks,
                              std::int64_t quantityLots, std::int64_t orderId, std::int64_t traderId,
                              UpdateSource updateSource, bool isTradeOnPassiveOrder);

    std::optional<std::int64_t> bestOppositePrice(bool oppositeIsAsk, const Book& oppositeBook,
                                                  std::priority_queue<std::int64_t>& oppositeHeap);

    void clearState();

    std::uint64_t seq = 0;
    // PriceTicks -> FIFO queue of orders sitting on that tick
    Book bids;
    Book asks;
    // Convention is to maintain both as max heaps. Asks must be inserted with the sign reversed
    std::priority_queue<std::int64_t> bidsHeap;
    std::priority_queue<std::int64_t> asksHeap;
    // For O(1) lookup based on orderId (for example for order cancel)
    // For this purpose, we store orderId -> {side, priceTicks, location in queue}
    std::unordered_map<std::int64_t, std::tuple<Side, std::int64_t, OrderPriorityQueue::iterator>> orderInfo;
    // Pointer to sink for fill registering
    ILogSink* sink = nullptr;
};
