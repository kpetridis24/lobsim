#pragma once
#include "lobsim/engine.hpp"
#include "lobsim/log_sink.hpp"

enum class PaperOrderStatus : std::uint8_t {
    OPEN = 0,
    PARTIALLY_FILLED = 1,
    FILLED = 2,
    CANCELLED = 3,
    REJECTED = 4,
};

struct PaperOrder {
public:
    NormalizedLobEvent originalEvent{};
    PaperOrderStatus status{PaperOrderStatus::OPEN};
    std::int64_t remainingQty{0};
    std::uint64_t placementSeq{0};
    std::size_t paperIndex{0};
};

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
    using OrderTraderQuantitySource = std::tuple<std::int64_t, std::int64_t, std::int64_t, UpdateSource, std::uint64_t>;
    using OrderPriorityQueue = std::list<OrderTraderQuantitySource>;
    using Book = std::unordered_map<std::int64_t, OrderPriorityQueue>;
    using PaperOrderQueue = std::list<std::int64_t>;

    struct FenwickTree {
        std::vector<std::int64_t> tree{};

        void ensureSize(std::size_t n) {
            if (tree.size() < n + 1) {
                tree.resize(n + 1, 0);
            }
        }

        void add(std::size_t index, std::int64_t delta) {
            for (std::size_t i = index + 1; i < tree.size(); i += i & -i) {
                tree[i] += delta;
            }
        }

        std::int64_t sum(std::size_t count) const {
            std::int64_t res = 0;
            for (std::size_t i = count; i > 0; i -= i & -i) {
                res += tree[i];
            }
            return res;
        }
    };

    struct PaperOrderLevel {
        PaperOrderQueue orders{};
        std::int64_t queuedLots{0};
        std::vector<std::uint64_t> marketSeqs{};
        FenwickTree marketQty{};
        std::unordered_map<std::uint64_t, std::size_t> marketIndexBySeq{};
        FenwickTree paperQty{};
        std::size_t nextPaperIndex{0};
    };

    void onAdd(const NormalizedLobEvent& event);
    void onSubtract(const NormalizedLobEvent& event);
    void onDelete(const NormalizedLobEvent& event);
    void onMatch(const NormalizedLobEvent& event);
    void onSet(const NormalizedLobEvent& event);

    void onPartialOrderCancel(const NormalizedLobEvent& event, bool isTradeOnPassiveOrder);

    std::optional<std::int64_t> bestOppositePrice(bool oppositeIsAsk, const Book& oppositeBook,
                                                  std::priority_queue<std::int64_t>& oppositeHeap);

    PaperOrderLevel& ensurePaperLevel(Side side, std::int64_t priceTicks);
    PaperOrderLevel* findPaperLevel(Side side, std::int64_t priceTicks);
    void applyPaperTradeAtLevel(Side passiveSide, std::int64_t priceTicks, std::int64_t tradeLots,
                                const NormalizedLobEvent& aggressor);
    void removePaperOrder(PaperOrderLevel& level, PaperOrderQueue::iterator it, std::int64_t removedQty,
                          PaperOrderStatus status);
    void reducePaperOrder(std::int64_t orderId, std::int64_t reduceQty);
    void setPaperOrder(std::int64_t orderId, std::int64_t newQty);

    void clearState();

    std::uint64_t seq = 0;
    std::uint64_t orderArrivalSeq = 0;
    // PriceTicks -> FIFO queue of orders sitting on that tick
    Book bids;
    Book asks;
    // Convention is to maintain both as max heaps. Asks must be inserted with the sign reversed
    mutable std::priority_queue<std::int64_t> bidsHeap;
    mutable std::priority_queue<std::int64_t> asksHeap;
    // For O(1) lookup based on orderId (for example for order cancel)
    // For this purpose, we store orderId -> {side, priceTicks, location in queue}
    std::unordered_map<std::int64_t, std::tuple<Side, std::int64_t, OrderPriorityQueue::iterator>> orderInfo;
    std::unordered_map<std::int64_t, PaperOrder> paperOrders;
    std::unordered_map<std::int64_t, std::tuple<Side, std::int64_t, PaperOrderQueue::iterator>> paperOrderInfo;
    std::unordered_map<std::int64_t, PaperOrderLevel> paperBids;
    std::unordered_map<std::int64_t, PaperOrderLevel> paperAsks;
    // Pointer to sink for fill registering
    ILogSink* sink = nullptr;
};
