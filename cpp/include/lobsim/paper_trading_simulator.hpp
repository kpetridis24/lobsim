#pragma once
#include "lobsim/engine.hpp"
#include "lobsim/log_sink.hpp"
#include "lobsim/flat_map.hpp"
#include <map>
#include <memory>
#include <optional>
#include <vector>


enum class PaperOrderStatus : std::uint8_t {
    OPEN = 0,
    PARTIALLY_FILLED = 1,
    FILLED = 2,
    CANCELLED = 3,
    REJECTED = 4,
};

struct PaperOrder {
public:
    NormalizedLobEvent original_event{};
    PaperOrderStatus status{PaperOrderStatus::OPEN};
    std::int64_t remaining_qty{0};
    std::uint64_t placement_seq{0};
    std::size_t paper_index{0};
};

class PaperTradingSimulator final : public IMatchingEngine {
public:
    struct Config {
        bool some_future_setting{false};
    };

    PaperTradingSimulator();
    PaperTradingSimulator(Config cfg);
    ~PaperTradingSimulator() = default;

    void update(const NormalizedLobEvent& event) override;

    void init_from_l2_snapshot(std::span<const Side> sides, std::span<const std::int64_t> prices,
                               std::span<const std::int64_t> quantities) override;

    void init_from_l3_snapshot(std::span<const Side> sides, std::span<const std::int64_t> prices,
                               std::span<const std::int64_t> quantities, std::span<const std::int64_t> order_ids,
                               std::span<const std::int64_t> trader_ids) override;

    std::optional<std::int64_t> depth_at(Side side, std::int64_t price_ticks) const;
    std::vector<std::pair<std::int64_t, std::int64_t>> l2_top_n(Side side, std::uint32_t n) const;
    std::optional<std::int64_t> get_best_price_ticks(Side side) const;
    void set_log_sink(std::shared_ptr<ILogSink> sink);
    void set_log_sink(ILogSink* sink) {
        set_log_sink(std::shared_ptr<ILogSink>(sink, [](ILogSink*) {}));
    }

private:
    using OrderTraderQuantitySource = std::tuple<std::int64_t, std::int64_t, std::int64_t, UpdateSource, std::uint64_t>;
    using OrderPriorityQueue = std::list<OrderTraderQuantitySource>;
    // using BidBook = lobsim::FlatMap<std::int64_t, OrderPriorityQueue, std::greater<std::int64_t>>;
    // using AskBook = lobsim::FlatMap<std::int64_t, OrderPriorityQueue, std::less<std::int64_t>>;
    using BidBook = std::map<std::int64_t, OrderPriorityQueue, std::greater<std::int64_t>>;
    using AskBook = std::map<std::int64_t, OrderPriorityQueue, std::less<std::int64_t>>;
    using PaperOrderQueue = std::list<std::int64_t>;

    struct FenwickTree {
        std::vector<std::int64_t> tree{};

        void ensure_size(std::size_t n) {
            const std::size_t needed = n + 1;
            if (tree.size() >= needed) {
                return;
            }
            const std::size_t old_size = tree.size();
            tree.resize(needed, 0);
            for (std::size_t i = old_size; i < tree.size(); ++i) {
                if (i == 0) {
                    continue;
                }
                const std::size_t parent = i - (i & -i);
                const std::int64_t prefix = sum(i - 1);
                const std::int64_t prefix_parent = sum(parent);
                tree[i] = prefix - prefix_parent;
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
        std::int64_t queued_lots{0};
        std::vector<std::uint64_t> market_seqs{};
        FenwickTree market_qty{};
        std::unordered_map<std::uint64_t, std::size_t> market_index_by_seq{};
        FenwickTree paper_qty{};
        std::size_t next_paper_index{0};
    };

    void on_add(const NormalizedLobEvent& event);
    void on_aggressive_trade(const NormalizedLobEvent& event);
    void on_subtract(const NormalizedLobEvent& event);
    void on_delete(const NormalizedLobEvent& event);
    void on_match(const NormalizedLobEvent& event);
    void on_set(const NormalizedLobEvent& event);

    void on_partial_order_cancel(const NormalizedLobEvent& event, bool is_trade_on_passive_order);
    void emit_diagnostic(const NormalizedLobEvent& event, DiagnosticRecordCode code, DiagnosticRecordSeverity severity);

    std::optional<std::int64_t> best_opposite_price(const BidBook& opposite_book);
    std::optional<std::int64_t> best_opposite_price(const AskBook& opposite_book);
    std::optional<std::int64_t> best_paper_opposite_price(bool opposite_is_ask);

    PaperOrderLevel& ensure_paper_level(Side side, std::int64_t price_ticks);
    PaperOrderLevel* find_paper_level(Side side, std::int64_t price_ticks);
    void apply_paper_trade_at_level(Side passive_side, std::int64_t price_ticks, std::int64_t trade_lots,
                                    const NormalizedLobEvent& aggressor);
    std::int64_t trade_against_paper_level(Side passive_side, std::int64_t price_ticks, std::int64_t trade_lots,
                                           const NormalizedLobEvent& aggressor);
    void remove_paper_order(PaperOrderLevel& level, PaperOrderQueue::iterator it, std::int64_t removed_qty,
                            PaperOrderStatus status);
    void reduce_paper_order(const NormalizedLobEvent& event);
    void set_paper_order(std::int64_t order_id, std::int64_t new_qty);

    void clear_state();

    std::uint64_t seq = 0;
    std::uint64_t current_update_seq = 0;
    std::uint64_t order_arrival_seq = 0;
    // PriceTicks -> FIFO queue of orders sitting on that tick
    BidBook bids;
    AskBook asks;
    // Convention is to maintain both as max heaps. Asks must be inserted with the sign reversed
    // mutable std::priority_queue<std::int64_t> bids_heap;
    // mutable std::priority_queue<std::int64_t> asks_heap;
    // using PaperBookBid = lobsim::FlatMap<std::int64_t, PaperOrderLevel, std::greater<std::int64_t>>;
    // using PaperBookAsk = lobsim::FlatMap<std::int64_t, PaperOrderLevel, std::less<std::int64_t>>;
    using PaperBookBid = std::map<std::int64_t, PaperOrderLevel, std::greater<std::int64_t>>;
    using PaperBookAsk = std::map<std::int64_t, PaperOrderLevel, std::less<std::int64_t>>;
    // For O(1) lookup based on order_id (for example for order cancel)
    // For this purpose, we store order_id -> {side, price_ticks, location in queue}
    std::unordered_map<std::int64_t, std::tuple<Side, std::int64_t, OrderPriorityQueue::iterator>> order_info;
    std::unordered_map<std::int64_t, PaperOrder> paper_orders;
    std::unordered_map<std::int64_t, std::tuple<Side, std::int64_t, PaperOrderQueue::iterator>> paper_order_info;
    PaperBookBid paper_bids;
    PaperBookAsk paper_asks;
    // Pointer to sink for fill registering
    std::shared_ptr<ILogSink> sink = nullptr;
    Config cfg_;
};
