#pragma once

#include <atomic>
#include <cstddef>

namespace slick::net::detail {

/**
 * @brief Wakeup coalescing for a single drain chain fed by many producers.
 *
 * Producers publish records into a lock-free queue; one chain on the consumer thread drains it and
 * ends when the queue runs dry. Without coalescing every producer has to post a wakeup, which costs
 * far more than the record it carries. This gate lets only the producer that finds no chain running
 * post one - every producer behind it relies on the running chain to drain what it published.
 *
 * Making that reliance safe needs the two sides to agree on a single question: did the chain end
 * before or after this record was published? Each side answers it by storing, then loading:
 *
 *   producer: publish(record)          consumer: running_ = false
 *             load running_                      probe the queue
 *
 * This is the store-buffer shape, and release/acquire is not enough for it. A release store and an
 * acquire load only pair on the *same* object; across two objects both loads may read the
 * pre-store value, so the producer sees a chain that has already ended while that chain's probe
 * has not yet seen the record - and the record sits in the queue with nothing scheduled to write
 * it, stranded until some later producer happens to restart the chain. Real hardware does this:
 * on x86 each store sits in the storing core's store buffer while the load after it reads memory.
 *
 * The seq_cst fence between each side's store and load is what rules that out. The two fences take
 * part in one total order, so at least one side observes the other: either the probe sees the
 * record and the chain keeps running, or the producer sees the ended chain and starts a new one.
 * Both may happen - the CAS then decides which, and the loser leaves the chain to the winner.
 *
 * Cost is one fence per published record (an `mfence` on x86) in place of the wakeup it replaces,
 * which is an allocation, a handler queued on the executor, and often a syscall to wake the
 * consumer thread. The fence is not on the consumer's per-record path: it runs once per chain,
 * when the queue has already run dry.
 */
class write_chain_gate {
public:
    write_chain_gate() = default;
    write_chain_gate(const write_chain_gate&) = delete;
    write_chain_gate& operator=(const write_chain_gate&) = delete;

    /**
     * @brief Producer side, called immediately after publishing a record.
     * @return true when the caller now owns the chain and must start it (post the wakeup);
     *         false when a chain is already running and will drain the record.
     *
     * Must be called after the publish, on the same thread: it is the publish this fence orders.
     */
    [[nodiscard]] bool claim_after_publish() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        // Load before the CAS so records published during a busy chain - the common case - leave
        // the flag's cache line shared instead of taking it exclusive on every send.
        if (running_.load(std::memory_order_acquire)) {
            return false;
        }
        bool expected = false;
        return running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }

    /**
     * @brief Chain owner's side, called when a drain has found the queue empty.
     * @param probe Re-reads the queue, returning true if a record is available. It must not commit
     *              the read - probe off a copy of the cursor - because a producer can claim the
     *              gate first, and then its wakeup starts the chain from the committed cursor.
     * @return true when the chain continues and owns the record the probe found;
     *         false when the chain has ended, either with nothing to drain or because a producer
     *         claimed the gate and will start a chain of its own.
     */
    template<typename ProbeFn>
    [[nodiscard]] bool ended_or_reclaimed(ProbeFn&& probe) noexcept {
        running_.store(false, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!probe()) {
            return false;
        }
        bool expected = false;
        return running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }

    /**
     * @brief End the chain without draining the queue, for a consumer that can no longer write.
     *
     * Records left behind are not lost: the next producer to publish finds no chain and starts one.
     */
    void abandon() noexcept {
        running_.store(false, std::memory_order_release);
    }

    /// Whether a chain is running - a wakeup posted, a record being written, or a chain parked.
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

private:
    // Every producer reads this flag, and the consumer writes it only when a chain starts or ends.
    // Left to share a cache line with what the consumer writes per record - a read cursor, say - a
    // read that should hit in L1 would miss on every record the chain wrote since.
    static constexpr std::size_t cacheline_size = 64;
    alignas(cacheline_size) std::atomic_bool running_{false};
    char padding_[cacheline_size - sizeof(std::atomic_bool)]{};
};

} // namespace slick::net::detail
