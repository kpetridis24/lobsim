#pragma once
#include "lobsim/engine.hpp"
#include "lobsim/log_sink.hpp"

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

    void update(const NormalizedLobEvent& event) override;

    void initFromL2Snapshot(const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
                            const std::vector<std::int64_t>& quantities) override;

    void initFromL3Snapshot(const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
                            const std::vector<std::int64_t>& quantities, const std::vector<std::int64_t>& orderIds,
                            const std::vector<std::int64_t>& traderIds) override;

    std::optional<std::int64_t> depthAt(Side side, std::int64_t priceTicks) const;
    std::vector<std::pair<std::int64_t, std::int64_t>> l2TopN(Side side, std::uint32_t n) const;
    std::optional<std::int64_t> getBestPriceTicks(Side side) const;
    void setLogSink(ILogSink* sink);

private:
    using OrderTraderQuantitySource = std::tuple<std::int64_t, std::int64_t, std::int64_t, UpdateSource>;
    using OrderPriorityQueue = std::list<OrderTraderQuantitySource>;
    using Book = std::unordered_map<std::int64_t, OrderPriorityQueue>;

    void onAdd(const NormalizedLobEvent& event);
    void onSubtract(const NormalizedLobEvent& event);
    void onDelete(const NormalizedLobEvent& event);
    void onMatch(const NormalizedLobEvent& event);
    void onSet(const NormalizedLobEvent& event);

    void onPartialOrderCancel(const NormalizedLobEvent& event, bool isTradeOnPassiveOrder);

    std::optional<std::int64_t> bestOppositePrice(bool oppositeIsAsk, const Book& oppositeBook,
                                                  std::priority_queue<std::int64_t>& oppositeHeap);

    void clearState();

    std::uint64_t seq = 0;
    // PriceTicks -> FIFO queue of orders sitting on that tick
    Book bids;
    Book asks;
    // Convention is to maintain both as max heaps. Asks must be inserted with the sign reversed
    mutable std::priority_queue<std::int64_t> bidsHeap;
    mutable std::priority_queue<std::int64_t> asksHeap;
    // For O(1) lookup based on orderId (for example for order cancel)
    // For this purpose, we store orderId -> {side, priceTicks, location in queue}
    std::unordered_map<std::int64_t, std::tuple<Side, std::int64_t, OrderPriorityQueue::iterator>> orderInfo;
    // Pointer to sink for fill registering
    ILogSink* sink = nullptr;
};
