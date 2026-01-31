#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

/**
 * A bounded, thread-safe FIFO queue with blocking push/pop operations.
 *
 * @tparam T Type of values stored in the queue.
 */
template <typename T> class BlockingQueue {
  public:
    /**
     * Snapshot of queue statistics for observability.
     */
    struct Stats {
        std::uint64_t pushes = 0;
        std::uint64_t pops = 0;
        std::uint64_t push_dropped = 0;
        std::uint64_t push_block_ns = 0;
        std::uint64_t pop_block_ns = 0;
        std::size_t max_depth = 0;
        std::size_t current_depth = 0;
    };

    /**
     * Construct a queue with a fixed capacity.
     *
     * @param capacity Maximum number of elements allowed in the queue.
     */
    explicit BlockingQueue(std::size_t capacity) : capacity_(capacity) {}

    /**
     * Push a value into the queue, blocking if the queue is full.
     *
     * @tparam U Value category of the argument.
     * @param value Value to enqueue.
     * @return `true` if the value was accepted; `false` if the queue is closed or capacity is zero.
     */
    template <class U> bool push(U&& value) {
        if (capacity_ == 0) {
            push_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::unique_lock lock(mu);
        auto start_wait = std::chrono::steady_clock::time_point{};
        bool waited = false;
        if (!isClosed && queue.size() >= capacity_) {
            waited = true;
            start_wait = std::chrono::steady_clock::now();
        }
        cvNotFull.wait(lock, [&] { return isClosed || queue.size() < capacity_; });
        if (waited) {
            auto waited_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - start_wait)
                                 .count();
            push_block_ns.fetch_add(static_cast<std::uint64_t>(waited_ns),
                                    std::memory_order_relaxed);
        }
        if (isClosed) {
            push_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        queue.push_back(std::forward<U>(value));
        pushes.fetch_add(1, std::memory_order_relaxed);
        update_max_depth(queue.size());
        cvNotEmpty.notify_one();
        return true;
    }

    /**
     * Pop the next value from the queue, blocking until one is available or closed.
     *
     * @return The next value, or `std::nullopt` if the queue is closed and empty.
     */
    std::optional<T> pop() {
        std::unique_lock lock(mu);
        auto start_wait = std::chrono::steady_clock::time_point{};
        bool waited = false;
        if (!isClosed && queue.empty()) {
            waited = true;
            start_wait = std::chrono::steady_clock::now();
        }
        cvNotEmpty.wait(lock, [&] { return isClosed || !queue.empty(); });
        if (waited) {
            auto waited_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - start_wait)
                                 .count();
            pop_block_ns.fetch_add(static_cast<std::uint64_t>(waited_ns),
                                   std::memory_order_relaxed);
        }

        if (queue.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue.front());
        queue.pop_front();
        pops.fetch_add(1, std::memory_order_relaxed);
        cvNotFull.notify_one();
        return value;
    }

    /**
     * Close the queue and wake all waiting producers and consumers.
     */
    void close() {
        {
            std::lock_guard lock(mu);
            isClosed = true;
        }
        cvNotEmpty.notify_all();
        cvNotFull.notify_all();
    }

    /**
     * Capture a snapshot of queue statistics.
     *
     * @return Snapshot of current counters and depth metrics.
     */
    Stats snapshot() const {
        Stats stats{};
        stats.pushes = pushes.load(std::memory_order_relaxed);
        stats.pops = pops.load(std::memory_order_relaxed);
        stats.push_dropped = push_dropped.load(std::memory_order_relaxed);
        stats.push_block_ns = push_block_ns.load(std::memory_order_relaxed);
        stats.pop_block_ns = pop_block_ns.load(std::memory_order_relaxed);
        stats.max_depth = max_depth.load(std::memory_order_relaxed);
        {
            std::lock_guard lock(mu);
            stats.current_depth = queue.size();
        }
        return stats;
    }

  private:
    void update_max_depth(std::size_t depth) {
        std::size_t prev = max_depth.load(std::memory_order_relaxed);
        while (depth > prev &&
               !max_depth.compare_exchange_weak(prev, depth, std::memory_order_relaxed)) {
        }
    }

    mutable std::mutex mu;
    std::condition_variable cvNotEmpty;
    std::condition_variable cvNotFull;
    std::deque<T> queue;
    bool isClosed = false;
    const std::size_t capacity_;
    std::atomic<std::uint64_t> pushes{0};
    std::atomic<std::uint64_t> pops{0};
    std::atomic<std::uint64_t> push_dropped{0};
    std::atomic<std::uint64_t> push_block_ns{0};
    std::atomic<std::uint64_t> pop_block_ns{0};
    std::atomic<std::size_t> max_depth{0};
};
