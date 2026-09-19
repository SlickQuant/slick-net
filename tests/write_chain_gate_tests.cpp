#include <gtest/gtest.h>

#include <slick/net/detail/write_chain_gate.hpp>
#include <slick/queue.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace slick::net::detail {

namespace {

// The queue/gate pairing of Websocket::Impl with the record reduced to one byte: publish() is
// Impl::send()'s tail and next() is Impl::next_queued_write(). Keeping the two in the same shape is
// the point of the harness - a copy that drifted would stop testing the protocol in use.
struct chain_harness {
    explicit chain_harness(uint32_t capacity) : queue(capacity) {}

    // Producer side, split so a test can line a publish up with the chain's last look at the queue:
    // reserve() puts everything but the record's visibility in place, publish_reserved() completes it.
    [[nodiscard]] uint64_t reserve(char value) {
        auto index = queue.reserve(1);
        *queue[index] = value;
        return index;
    }

    // Returns true when the caller owns a new chain and would post the wakeup
    [[nodiscard]] bool publish_reserved(uint64_t index) {
        queue.publish(index, 1);
        return gate.claim_after_publish();
    }

    [[nodiscard]] bool publish(char value) { return publish_reserved(reserve(value)); }

    // Chain owner's side. Returns the next record, or nullptr once the chain has ended.
    [[nodiscard]] std::pair<char*, uint32_t> next() {
        auto record = queue.read(cursor);
        if (record.first) {
            return record;
        }
        auto probe_cursor = cursor;
        if (!gate.ended_or_reclaimed([&]() {
                record = queue.read(probe_cursor);
                return record.first != nullptr;
            })) {
            return {nullptr, 0};
        }
        cursor = probe_cursor;
        return record;
    }

    // Runs a chain to its end, as a posted wakeup would. Returns the records it wrote.
    [[nodiscard]] std::size_t drain() {
        std::size_t drained = 0;
        while (next().first) {
            ++drained;
        }
        return drained;
    }

    // Whether a record is queued that no chain is scheduled to write - the stranding this protocol
    // exists to prevent. Only meaningful with every producer and the chain idle.
    [[nodiscard]] bool stranded() {
        if (gate.running()) {
            return false;
        }
        auto probe_cursor = cursor;
        return queue.read(probe_cursor).first != nullptr;
    }

    slick::queue<char> queue;
    write_chain_gate gate;
    uint64_t cursor{0};  // chain owner only
};

// The gate on its own, with the queue reduced to the one bit the protocol asks it for: is a record
// visible? That is all `probe` reports, so a flag tests the same protocol - and it tests it where
// slick::queue cannot. queue::publish() ends in a CAS on its last-published index, and a CAS is a
// locked instruction, so on x86 it drains the publishing core's store buffer and hides the
// producer's half of the race behind an ordering the protocol never asked for. A plain release
// store leaves both halves exposed.
struct litmus_harness {
    // The chain's side: end the chain, then look once more for a record
    [[nodiscard]] bool chain_drained() {
        return gate.ended_or_reclaimed([this]() { return pending.load(std::memory_order_acquire); });
    }

    // The producer's side: make a record visible, then look for a chain to leave it to
    [[nodiscard]] bool publish() {
        pending.store(true, std::memory_order_release);
        return gate.claim_after_publish();
    }

    void consume() noexcept { pending.store(false, std::memory_order_release); }

    // A record no chain is scheduled to write
    [[nodiscard]] bool stranded() const noexcept {
        return pending.load(std::memory_order_acquire) && !gate.running();
    }

    // On its own line, as the record is in the Websocket - it lives in the write queue, not beside
    // the gate, and a gate keeps a line to itself
    alignas(64) std::atomic_bool pending{false};
    write_chain_gate gate;
};

// Spin-synchronised rounds. Threads waiting here leave together within a few hundred cycles, which
// is what lines a producer's publish up with the chain's last read of the queue.
class spin_rounds {
public:
    explicit spin_rounds(int participants) : participants_(participants) {}

    // Main thread: opens a round and blocks until every participant has finished it
    void run_round(uint64_t round) {
        finished_.store(0, std::memory_order_relaxed);
        round_.store(round, std::memory_order_release);
        while (finished_.load(std::memory_order_acquire) != participants_) {
            std::this_thread::yield();
        }
    }

    void stop() noexcept { stopped_.store(true, std::memory_order_release); }

    // Participant: spins until `round` opens. Returns false once the harness is stopping.
    [[nodiscard]] bool await_round(uint64_t round) const noexcept {
        while (round_.load(std::memory_order_acquire) != round) {
            if (stopped_.load(std::memory_order_acquire)) {
                return false;
            }
        }
        return true;
    }

    void finish_round() noexcept { finished_.fetch_add(1, std::memory_order_acq_rel); }

private:
    const int participants_;
    std::atomic<uint64_t> round_{0};
    std::atomic<int> finished_{0};
    std::atomic_bool stopped_{false};
};

} // namespace

// ─── protocol, single threaded ───────────────────────────────────────────────

TEST(WriteChainGateTest, FirstPublisherStartsTheChainAndLaterOnesLeaveItToRun) {
    write_chain_gate gate;
    EXPECT_FALSE(gate.running());

    EXPECT_TRUE(gate.claim_after_publish());
    EXPECT_TRUE(gate.running());

    // Records published behind a running chain post no wakeup of their own
    EXPECT_FALSE(gate.claim_after_publish());
    EXPECT_FALSE(gate.claim_after_publish());
    EXPECT_TRUE(gate.running());
}

TEST(WriteChainGateTest, DrainedChainEndsWhenTheQueueStaysEmpty) {
    write_chain_gate gate;
    ASSERT_TRUE(gate.claim_after_publish());

    EXPECT_FALSE(gate.ended_or_reclaimed([]() { return false; }));
    EXPECT_FALSE(gate.running());

    // The gate is free again, so the next record starts a chain
    EXPECT_TRUE(gate.claim_after_publish());
}

TEST(WriteChainGateTest, DrainedChainKeepsRunningWhenItsProbeFindsARecord) {
    write_chain_gate gate;
    ASSERT_TRUE(gate.claim_after_publish());

    EXPECT_TRUE(gate.ended_or_reclaimed([]() { return true; }));
    EXPECT_TRUE(gate.running());
    // Still the same chain, so a record published now waits for it
    EXPECT_FALSE(gate.claim_after_publish());
}

// The half of the race the probe alone cannot settle: a producer claims the gate in the window
// between the chain releasing it and the chain's probe. The chain must lose and end, leaving the
// record to the wakeup that producer posts - two chains writing one stream would interleave frames.
// Calling the producer's side from inside the probe puts it exactly in that window.
TEST(WriteChainGateTest, ChainEndsWhenAProducerClaimsTheGateDuringItsProbe) {
    write_chain_gate gate;
    ASSERT_TRUE(gate.claim_after_publish());

    bool producer_starts_chain = false;
    EXPECT_FALSE(gate.ended_or_reclaimed([&]() {
        producer_starts_chain = gate.claim_after_publish();
        return true;  // the producer's record is visible to the probe
    }));
    EXPECT_TRUE(producer_starts_chain);
    EXPECT_TRUE(gate.running());
}

TEST(WriteChainGateTest, AbandonEndsTheChainWithoutDraining) {
    chain_harness harness(1024);
    ASSERT_TRUE(harness.publish('a'));

    harness.gate.abandon();
    EXPECT_FALSE(harness.gate.running());

    // The undrained record is not lost: the next producer finds no chain and starts one
    EXPECT_TRUE(harness.publish('b'));
    EXPECT_EQ(harness.drain(), 2u);
}

// ─── protocol against the queue ──────────────────────────────────────────────

TEST(WriteChainGateTest, OneChainDrainsEveryRecordPublishedBehindIt) {
    chain_harness harness(1024);
    ASSERT_TRUE(harness.publish('a'));
    EXPECT_FALSE(harness.publish('b'));
    EXPECT_FALSE(harness.publish('c'));

    EXPECT_EQ(harness.drain(), 3u);
    EXPECT_FALSE(harness.gate.running());
    EXPECT_FALSE(harness.stranded());
}

// A producer whose record is published but hidden behind an earlier producer's unpublished slot is
// still the one that has to start the chain, because the chain cannot read past the hole.
TEST(WriteChainGateTest, RecordBehindAnUnpublishedSlotIsDrainedByTheEarlierPublisher) {
    chain_harness harness(1024);
    const auto first = harness.reserve('a');
    const auto second = harness.reserve('b');

    // The later record publishes first and starts a chain, but the chain cannot reach it yet
    EXPECT_TRUE(harness.publish_reserved(second));
    EXPECT_EQ(harness.drain(), 0u);
    EXPECT_FALSE(harness.gate.running());

    // Filling the hole is what makes both readable, so that publisher starts the next chain
    EXPECT_TRUE(harness.publish_reserved(first));
    EXPECT_EQ(harness.drain(), 2u);
    EXPECT_FALSE(harness.stranded());
}

// ─── the race the protocol has to survive ────────────────────────────────────

// The interleaving the protocol exists for, forced rather than waited for.
//
// Each round lands a producer's publish on the chain's last look at the queue. Both sides then run
// the same shape - store, then load the other side's store - and the question is whether either
// sees the other. Release/acquire alone does not answer it: the two stores are to different
// objects, so nothing stops each core from reading the other's pre-store value out of memory while
// its own store still sits in its store buffer. When that happens the producer leaves its record to
// a chain that has already ended, the chain ends having never seen the record, and the record is
// stranded with nothing scheduled to write it.
//
// Running the same rounds against the gate with its seq_cst fences removed is what shows this test
// works: without them x86 strands a record within a few thousand rounds.
TEST(WriteChainGateTest, ProducerAndChainRacingOnTheGateNeverBothMiss) {
    constexpr uint64_t kRounds = 2000000;
    constexpr auto kTimeBudget = std::chrono::seconds(20);

    litmus_harness harness;
    spin_rounds rounds(2);
    std::atomic<uint64_t> wakeups{0};

    // Each round starts with the chain owning the gate and nothing queued, so the chain's first act
    // is to end - the window the producer aims at.
    ASSERT_TRUE(harness.gate.claim_after_publish());

    std::thread chain([&]() {
        for (uint64_t round = 1; rounds.await_round(round); ++round) {
            while (harness.chain_drained()) {
                harness.consume();
            }
            rounds.finish_round();
        }
    });

    std::thread producer([&]() {
        for (uint64_t round = 1; rounds.await_round(round); ++round) {
            if (harness.publish()) {
                wakeups.fetch_add(1, std::memory_order_relaxed);
            }
            rounds.finish_round();
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + kTimeBudget;
    uint64_t strand_round = 0;
    uint64_t completed_rounds = 0;
    for (uint64_t round = 1; round <= kRounds; ++round) {
        rounds.run_round(round);

        // Both threads are idle here, so the record is either consumed or owned by a chain that
        // will consume it. Anything else is a record no wakeup will ever write.
        if (harness.stranded()) {
            strand_round = round;
            break;
        }
        completed_rounds = round;
        // Run the wakeup the producer posted, then hand the chain the gate for the next round
        if (harness.gate.running()) {
            while (harness.chain_drained()) {
                harness.consume();
            }
        }
        if (!harness.gate.claim_after_publish()) {
            strand_round = round;  // nothing else owns the gate here
            break;
        }
        if ((round & 0xFFF) == 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }

    rounds.stop();
    chain.join();
    producer.join();

    EXPECT_EQ(strand_round, 0u)
        << "the producer left its record to a chain that had already ended, round " << strand_round;
    EXPECT_GT(completed_rounds, 0u);
    // A round the producer wins outright teaches nothing, so check the rounds really did collide
    EXPECT_LT(wakeups.load(std::memory_order_relaxed), completed_rounds);
}

// The same rounds as the litmus test above, but against the real queue, so they also cover what a
// bare flag leaves out: the cursor handed between a chain that ends and the chain a producer starts
// in its place, and records that a wrapping ring buffer makes visible out of order.
//
// It cannot force the ordering failure the litmus test forces - queue::publish()'s trailing CAS
// orders the producer's half for it - so read a pass here as "the handoff drains everything", not
// as "the protocol is ordered".
TEST(WriteChainGateTest, ProducersRacingTheChainEndLeaveNoRecordUndrained) {
    constexpr int kProducers = 2;
    constexpr uint64_t kRounds = 200000;
    constexpr auto kTimeBudget = std::chrono::seconds(30);

    chain_harness harness(1024);
    spin_rounds rounds(kProducers + 1);
    std::atomic<std::size_t> published{0};
    std::atomic<std::size_t> drained{0};

    // The chain starts each round owning the gate with the queue already drained, so the first
    // thing it does is end - the window the producers aim at.
    ASSERT_TRUE(harness.gate.claim_after_publish());

    std::thread chain([&]() {
        for (uint64_t round = 1; rounds.await_round(round); ++round) {
            drained.fetch_add(harness.drain(), std::memory_order_relaxed);
            rounds.finish_round();
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            const char value = static_cast<char>('a' + p);
            for (uint64_t round = 1;; ++round) {
                const auto index = harness.reserve(value);
                const bool running = rounds.await_round(round);
                // Publish either way, so a stopping producer leaves no hole in the queue
                (void)harness.publish_reserved(index);
                published.fetch_add(1, std::memory_order_relaxed);
                if (!running) {
                    return;
                }
                rounds.finish_round();
            }
        });
    }

    const auto deadline = std::chrono::steady_clock::now() + kTimeBudget;
    uint64_t strand_round = 0;
    uint64_t completed_rounds = 0;
    for (uint64_t round = 1; round <= kRounds; ++round) {
        rounds.run_round(round);

        // Every thread is idle here, so the queue must be either drained or owned by a chain that
        // will drain it. Anything else is a record no wakeup will ever write.
        if (harness.stranded()) {
            strand_round = round;
            break;
        }
        completed_rounds = round;
        // Run the wakeup a producer posted, then hand the chain the gate for the next round
        if (harness.gate.running()) {
            drained.fetch_add(harness.drain(), std::memory_order_relaxed);
        }
        if (!harness.gate.claim_after_publish()) {
            strand_round = round;  // nothing else owns the gate here
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }

    rounds.stop();
    chain.join();
    for (auto& producer : producers) {
        producer.join();
    }

    EXPECT_EQ(strand_round, 0u)
        << "a record published as the chain ended was left with no chain to write it, round "
        << strand_round;
    EXPECT_GT(completed_rounds, 0u);

    // Nothing the threads left behind is stranded, so the totals have to meet
    if (!harness.gate.running()) {
        (void)harness.gate.claim_after_publish();
    }
    drained.fetch_add(harness.drain(), std::memory_order_relaxed);
    EXPECT_EQ(drained.load(std::memory_order_relaxed), published.load(std::memory_order_relaxed));
}

// The same queue and chain without the round structure: producers running flat out against a chain
// that ends whenever it catches up. This covers the interleavings the rounds rule out - several
// producers inside the window at once, and a chain that ends between two records of one producer.
TEST(WriteChainGateTest, FreeRunningProducersNeverStrandARecord) {
    constexpr int kProducers = 4;
    constexpr std::size_t kRecordsPerProducer = 50000;
    constexpr auto kTimeBudget = std::chrono::seconds(30);
    // One byte per record, sized past every record the test publishes so a chain that falls behind
    // is never overrun by a wrapping producer - that would lose records for reasons of its own.
    constexpr uint32_t kCapacity = 1u << 20;
    static_assert(kCapacity > kProducers * kRecordsPerProducer);

    chain_harness harness(kCapacity);
    std::atomic<std::size_t> published{0};
    std::atomic<std::size_t> drained{0};
    std::atomic<std::size_t> wakeups{0};
    std::atomic_bool chain_pending{false};
    std::atomic_bool producers_done{false};

    // Stands in for the service thread: runs one chain per posted wakeup, never two at once
    std::thread consumer([&]() {
        for (;;) {
            if (!chain_pending.exchange(false, std::memory_order_acq_rel)) {
                if (producers_done.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::yield();
                continue;
            }
            drained.fetch_add(harness.drain(), std::memory_order_relaxed);
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + kTimeBudget;
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            const char value = static_cast<char>('a' + p);
            for (std::size_t i = 0; i < kRecordsPerProducer; ++i) {
                if (harness.publish(value)) {
                    wakeups.fetch_add(1, std::memory_order_relaxed);
                    chain_pending.store(true, std::memory_order_release);
                }
                published.fetch_add(1, std::memory_order_relaxed);
                if ((i & 0x3F) == 0x3F && std::chrono::steady_clock::now() >= deadline) {
                    return;
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    producers_done.store(true, std::memory_order_release);
    consumer.join();

    // A strand shows up here as records the chain never wrote: every producer has finished, so
    // nothing is left to restart a chain that ended while a record was still queued.
    drained.fetch_add(harness.drain(), std::memory_order_relaxed);
    EXPECT_FALSE(harness.stranded());
    EXPECT_EQ(drained.load(std::memory_order_relaxed), published.load(std::memory_order_relaxed));

    // The coalescing earns its fence only if most records ride a chain that is already running
    const auto posted = wakeups.load(std::memory_order_relaxed);
    EXPECT_GE(posted, 1u);
    EXPECT_LT(posted, published.load(std::memory_order_relaxed));
}

} // namespace slick::net::detail
