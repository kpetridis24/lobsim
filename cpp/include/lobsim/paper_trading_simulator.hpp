#pragma once
#include "lobsim/engine.hpp"
#include "lobsim/fast_hash_map.hpp"
#include "lobsim/inline.hpp"
#include "lobsim/log_sink.hpp"

#include <boost/intrusive/list.hpp>

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

struct OrderPoolConfig {
    std::size_t historical_capacity{200000};
    std::size_t paper_capacity{100000};
};

template <typename NodeType> class NodePool {
public:
    NodePool() = default;

    void reserve(std::size_t capacity) {
        pool_.reserve(capacity);
        free_list_.reserve(capacity);
    }

    NodeType* acquire(std::size_t max_capacity) {
        if (!free_list_.empty()) {
            const auto idx = free_list_.back();
            free_list_.pop_back();
            auto& node = pool_[idx];
            node.hook.unlink();
            node.in_use = true;
            return &node;
        }
        if (pool_.size() >= max_capacity) {
            return nullptr;
        }
        const auto idx = pool_.size();
        pool_.emplace_back();
        auto& node = pool_.back();
        node.pool_index = idx;
        node.in_use = true;
        return &node;
    }

    void release(NodeType* node) {
        if (node == nullptr)
            return;
        if (node->hook.is_linked())
            node->hook.unlink();
        if (!node->in_use)
            return;
        node->in_use = false;
        free_list_.push_back(node->pool_index);
    }

    void reset() {
        for (auto& node : pool_) {
            if (node.hook.is_linked())
                node.hook.unlink();
            node.in_use = false;
        }
        free_list_.clear();
        free_list_.reserve(pool_.size());
        for (std::size_t i = 0; i < pool_.size(); ++i) {
            free_list_.push_back(i);
        }
    }

    std::size_t size() const { return pool_.size(); }

private:
    std::vector<NodeType> pool_;
    std::vector<std::size_t> free_list_;
};

class PaperTradingSimulator final : public IMatchingEngine {
public:
    explicit PaperTradingSimulator(OrderPoolConfig pool_config = {});
    PaperTradingSimulator(std::size_t historical_capacity, std::size_t paper_capacity);
    PaperTradingSimulator(std::vector<Side>& sides, std::vector<std::int64_t>& prices,
                          std::vector<std::int64_t>& quantities, ILogSink* sink = nullptr,
                          OrderPoolConfig pool_config = {});
    PaperTradingSimulator(const PaperTradingSimulator&) = delete;
    PaperTradingSimulator& operator=(const PaperTradingSimulator&) = delete;
    PaperTradingSimulator(PaperTradingSimulator&&) = delete;
    PaperTradingSimulator& operator=(PaperTradingSimulator&&) = delete;
    ~PaperTradingSimulator() = default;

    void update(const NormalizedLobEvent& event) override;

    void init_from_l2_snapshot(const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
                               const std::vector<std::int64_t>& quantities) override;

    void init_from_l3_snapshot(const std::vector<Side>& sides, const std::vector<std::int64_t>& prices,
                               const std::vector<std::int64_t>& quantities, const std::vector<std::int64_t>& order_ids,
                               const std::vector<std::int64_t>& trader_ids) override;

    std::optional<std::int64_t> depth_at(Side side, std::int64_t price_ticks) const;
    std::vector<std::pair<std::int64_t, std::int64_t>> l2_top_n(Side side, std::uint32_t n) const;
    std::optional<std::int64_t> get_best_price_ticks(Side side) const;
    void set_log_sink(ILogSink* sink);

private:
    using OrderHook = boost::intrusive::list_member_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;
    using PaperOrderHook =
        boost::intrusive::list_member_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;

    struct OrderNode {
        OrderHook hook{};
        std::int64_t order_id{0};
        std::int64_t trader_id{0};
        std::int64_t qty{0};
        UpdateSource source{UpdateSource::HISTORICAL};
        std::uint64_t arrival_seq{0};
        std::size_t pool_index{0};
        bool in_use{false};
    };

    struct PaperOrderNode {
        PaperOrderHook hook{};
        std::int64_t order_id{0};
        std::size_t pool_index{0};
        bool in_use{false};
    };

    using OrderPriorityQueue =
        boost::intrusive::list<OrderNode, boost::intrusive::member_hook<OrderNode, OrderHook, &OrderNode::hook>,
                               boost::intrusive::constant_time_size<false>>;
    using Book = ska::flat_hash_map<std::int64_t, OrderPriorityQueue>;
    using PaperOrderQueue =
        boost::intrusive::list<PaperOrderNode,
                               boost::intrusive::member_hook<PaperOrderNode, PaperOrderHook, &PaperOrderNode::hook>,
                               boost::intrusive::constant_time_size<false>>;

    struct OrderInfo {
        Side side{Side::BUY};
        std::int64_t price_ticks{0};
        OrderNode* node{nullptr};
    };

    struct PaperOrderInfo {
        Side side{Side::BUY};
        std::int64_t price_ticks{0};
        PaperOrderNode* node{nullptr};
    };

    struct FenwickTree {
        std::vector<std::int64_t> tree{};

        LOBSIM_FORCEINLINE void ensure_size(std::size_t n) {
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

        LOBSIM_FORCEINLINE void add(std::size_t index, std::int64_t delta) {
            for (std::size_t i = index + 1; i < tree.size(); i += i & -i) {
                tree[i] += delta;
            }
        }

        LOBSIM_FORCEINLINE std::int64_t sum(std::size_t count) const {
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
        lobsim::FlatHashMap<std::uint64_t, std::size_t> market_index_by_seq{};
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

    std::optional<std::int64_t> best_opposite_price(bool opposite_is_ask, const Book& opposite_book,
                                                    std::priority_queue<std::int64_t>& opposite_heap);
    std::optional<std::int64_t> best_paper_opposite_price(bool opposite_is_ask);

    PaperOrderLevel& ensure_paper_level(Side side, std::int64_t price_ticks);
    PaperOrderLevel* find_paper_level(Side side, std::int64_t price_ticks);
    void apply_paper_trade_at_level(Side passive_side, std::int64_t price_ticks, std::int64_t trade_lots,
                                    const NormalizedLobEvent& aggressor);
    std::int64_t trade_against_paper_level(Side passive_side, std::int64_t price_ticks, std::int64_t trade_lots,
                                           const NormalizedLobEvent& aggressor);
    void remove_paper_order(PaperOrderLevel& level, PaperOrderNode* node, std::int64_t removed_qty,
                            PaperOrderStatus status);
    void reduce_paper_order(const NormalizedLobEvent& event);
    void set_paper_order(std::int64_t order_id, std::int64_t new_qty);

    void clear_state();
    void init_pools();
    void reset_pools();

    std::uint64_t seq = 0;
    std::uint64_t order_arrival_seq = 0;
    // PriceTicks -> FIFO queue of orders sitting on that tick
    Book bids;
    Book asks;
    // Convention is to maintain both as max heaps. Asks must be inserted with the sign reversed
    mutable std::priority_queue<std::int64_t> bids_heap;
    mutable std::priority_queue<std::int64_t> asks_heap;
    // For O(1) lookup based on order_id (for example for order cancel)
    // For this purpose, we store order_id -> {side, price_ticks, location in queue}
    lobsim::FlatHashMap<std::int64_t, OrderInfo> order_info;
    lobsim::FlatHashMap<std::int64_t, PaperOrder> paper_orders;
    lobsim::FlatHashMap<std::int64_t, PaperOrderInfo> paper_order_info;
    ska::flat_hash_map<std::int64_t, PaperOrderLevel> paper_bids;
    ska::flat_hash_map<std::int64_t, PaperOrderLevel> paper_asks;
    // Pointer to sink for fill registering
    ILogSink* sink = nullptr;

    OrderPoolConfig pool_config_{};
    NodePool<OrderNode> order_pool_{};
    NodePool<PaperOrderNode> paper_pool_{};
};
