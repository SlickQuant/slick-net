// A/B benchmark for the write-chain wakeup coalescing in Websocket<>::send().
//
// Two send paths, run against the same queue, the same io_context and the same consumer:
//
//   post  the path before write_chain_gate - every send posts a wakeup, and the handler CASes a
//         flag so the wakeups that land on a running chain do nothing but cost a post
//   gate  the current path - a seq_cst fence and a load decide whether a chain is already running,
//         and only the send that finds none posts
//
// So the trade under test is one fence per send against one post per send. The consumer side is
// identical in both: the chain drains one record per turn and re-enters through the io_context, the
// way on_write() re-enters do_write(), with --write-ns standing in for the socket write.
//
// What this does NOT measure: a real socket, TLS, the beast write path, or any of the kernel time a
// live connection spends. It isolates the wakeup decision, which is what the gate changes. Numbers
// from it are about that decision, not about end-to-end WebSocket throughput.

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <slick/net/detail/write_chain_gate.hpp>
#include <slick/queue.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using slick::net::detail::write_chain_gate;
using clock_type = std::chrono::steady_clock;

namespace {

enum class send_path { post, gate };

const char* name_of(send_path path) {
    return path == send_path::gate ? "gate" : "post";
}

struct config {
    std::uint32_t producers = 4;
    std::uint32_t records = 50'000;   // per producer
    std::uint32_t payload = 64;       // bytes per record
    std::uint32_t write_ns = 250;     // simulated per-record consumer work
    std::uint32_t gap_ns = 0;         // producer think time between sends
    std::uint32_t reps = 5;
    std::uint32_t timeout_s = 60;
};

struct result {
    double wall_ms = 0.0;
    double producer_ns_per_send = 0.0;
    std::uint64_t posts = 0;
    std::uint64_t wasted_posts = 0;   // post path only: a wakeup that found a chain already running
    std::uint64_t drained = 0;
    bool timed_out = false;
};

// Busy-waits for roughly ns. Used for both the simulated socket write and the producer gap, so the
// two are paced by the same instrument.
void spin_ns(std::uint32_t ns) {
    if (ns == 0) {
        return;
    }
    const auto deadline = clock_type::now() + std::chrono::nanoseconds(ns);
    while (clock_type::now() < deadline) {
    }
}

std::uint32_t round_up_pow2(std::uint64_t value) {
    std::uint32_t size = 1;
    while (size < value && size < (1u << 30)) {
        size <<= 1;
    }
    return size;
}

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto mid = values.size() / 2;
    return values.size() % 2 ? values[mid] : (values[mid - 1] + values[mid]) / 2.0;
}

// One run of one path. Everything the two paths share lives here; only the wakeup decision and the
// chain-end handshake differ, so a difference in the numbers is a difference in those.
class harness {
public:
    harness(send_path path, const config& cfg)
        : path_(path)
        , cfg_(cfg)
        , record_len_(cfg.payload + 2)
        , total_(static_cast<std::uint64_t>(cfg.producers) * cfg.records)
        , queue_(round_up_pow2(total_ * record_len_ + record_len_))
        , payload_(cfg.payload, 'x') {}

    result run() {
        auto work = asio::make_work_guard(ioc_);
        std::thread service([this] { ioc_.run(); });

        std::vector<double> ns_per_send(cfg_.producers, 0.0);
        std::vector<std::thread> producers;
        producers.reserve(cfg_.producers);
        std::atomic<std::uint32_t> ready{0};

        for (std::uint32_t p = 0; p < cfg_.producers; ++p) {
            producers.emplace_back([this, p, &ns_per_send, &ready] {
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (!go_.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                const auto begin = clock_type::now();
                for (std::uint32_t i = 0; i < cfg_.records; ++i) {
                    send();
                    spin_ns(cfg_.gap_ns);
                }
                const auto elapsed = clock_type::now() - begin;
                ns_per_send[p] = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) / cfg_.records;
            });
        }

        // Start the clock with every producer already spawned, so thread creation is not timed
        while (ready.load(std::memory_order_acquire) < cfg_.producers) {
            std::this_thread::yield();
        }
        const auto started = clock_type::now();
        go_.store(true, std::memory_order_release);

        for (auto& producer : producers) {
            producer.join();
        }

        result out;
        // A record left in the queue with no wakeup scheduled - the failure the gate exists to
        // prevent - stalls here instead of silently scoring well
        out.timed_out = done_.get_future().wait_for(std::chrono::seconds(cfg_.timeout_s)) !=
                        std::future_status::ready;
        const auto finished = clock_type::now();

        work.reset();
        ioc_.stop();
        service.join();

        out.wall_ms = std::chrono::duration<double, std::milli>(finished - started).count();
        out.producer_ns_per_send =
            std::accumulate(ns_per_send.begin(), ns_per_send.end(), 0.0) / cfg_.producers;
        out.posts = posts_.load(std::memory_order_relaxed);
        out.wasted_posts = wasted_posts_.load(std::memory_order_relaxed);
        out.drained = drained_.load(std::memory_order_relaxed);
        return out;
    }

private:
    void send() {
        const auto index = queue_.reserve(record_len_);
        *queue_[index] = 0;
        *queue_[index + 1] = 0;
        std::memcpy(queue_[index + 2], payload_.data(), cfg_.payload);
        queue_.publish(index, record_len_);

        if (path_ == send_path::gate) {
            // Current path: the fence and the load decide, and only the claimer posts
            if (gate_.claim_after_publish()) {
                posts_.fetch_add(1, std::memory_order_relaxed);
                asio::post(ioc_, [this] { drain_gate(); });
            }
            return;
        }

        // Former path: post unconditionally and let the handler discard the redundant wakeups
        posts_.fetch_add(1, std::memory_order_relaxed);
        asio::post(ioc_, [this] {
            bool expected = false;
            if (in_writing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                drain_post();
            }
            else {
                wasted_posts_.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Mirrors do_write() -> next_queued_write() -> on_write() -> do_write() on the gate path
    void drain_gate() {
        auto record = queue_.read(cursor_);
        if (!record.first) {
            auto cursor = cursor_;
            if (!gate_.ended_or_reclaimed([&] {
                    record = queue_.read(cursor);
                    return record.first != nullptr;
                })) {
                return;
            }
            cursor_ = cursor;
        }
        consume();
        asio::post(ioc_, [this] { drain_gate(); });
    }

    // Mirrors the former do_write() -> on_write() -> do_write(), which ended the chain with a plain
    // store because every send posted a wakeup of its own
    void drain_post() {
        auto record = queue_.read(cursor_);
        if (!record.first) {
            in_writing_.store(false, std::memory_order_release);
            return;
        }
        consume();
        asio::post(ioc_, [this] { drain_post(); });
    }

    void consume() {
        spin_ns(cfg_.write_ns);
        if (drained_.fetch_add(1, std::memory_order_relaxed) + 1 == total_) {
            done_.set_value();
        }
    }

    send_path path_;
    config cfg_;
    std::uint32_t record_len_;
    std::uint64_t total_;
    slick::queue<char> queue_;
    std::string payload_;

    asio::io_context ioc_;
    write_chain_gate gate_;                 // gate path
    std::atomic_bool in_writing_{false};    // post path
    std::uint64_t cursor_ = 0;              // consumer thread only
    std::atomic_bool go_{false};
    std::atomic<std::uint64_t> posts_{0};
    std::atomic<std::uint64_t> wasted_posts_{0};
    std::atomic<std::uint64_t> drained_{0};
    std::promise<void> done_;
};

struct summary {
    double wall_ms = 0.0;
    double wall_lo = 0.0;
    double wall_hi = 0.0;
    double ns_per_send = 0.0;
    std::uint64_t posts = 0;
    std::uint64_t wasted_posts = 0;
    bool ok = true;
};

summary measure(send_path path, const config& cfg) {
    std::vector<double> wall;
    std::vector<double> ns_per_send;
    summary out;
    for (std::uint32_t rep = 0; rep < cfg.reps; ++rep) {
        harness bench(path, cfg);
        const auto r = bench.run();
        const auto expected = static_cast<std::uint64_t>(cfg.producers) * cfg.records;
        if (r.timed_out || r.drained != expected) {
            std::printf("  !! %s rep %u drained %llu of %llu%s\n", name_of(path), rep,
                        static_cast<unsigned long long>(r.drained),
                        static_cast<unsigned long long>(expected),
                        r.timed_out ? " (timed out - a record was stranded)" : "");
            out.ok = false;
            continue;
        }
        wall.push_back(r.wall_ms);
        ns_per_send.push_back(r.producer_ns_per_send);
        out.posts = r.posts;
        out.wasted_posts = r.wasted_posts;
    }
    if (wall.empty()) {
        out.ok = false;
        return out;
    }
    out.wall_ms = median(wall);
    out.wall_lo = *std::min_element(wall.begin(), wall.end());
    out.wall_hi = *std::max_element(wall.begin(), wall.end());
    out.ns_per_send = median(ns_per_send);
    return out;
}

bool parse_u32(std::string_view text, std::uint32_t& out) {
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

void usage() {
    std::printf(
        "usage: write_chain_bench [options]\n"
        "  --producers N   sending threads (default 4)\n"
        "  --records N     records per producer (default 50000)\n"
        "  --payload N     payload bytes per record (default 64)\n"
        "  --write-ns N    simulated per-record consumer work (default 250)\n"
        "  --gap-ns N      producer think time between sends (default 0)\n"
        "  --reps N        repetitions per path (default 5)\n"
        "  --timeout-s N   per-run stall timeout (default 60)\n");
}

} // namespace

int main(int argc, char** argv) {
    config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        }
        if (i + 1 >= argc) {
            std::printf("missing value for %.*s\n", static_cast<int>(arg.size()), arg.data());
            return 2;
        }
        const std::string_view value(argv[++i]);
        bool ok = true;
        if (arg == "--producers")      ok = parse_u32(value, cfg.producers);
        else if (arg == "--records")   ok = parse_u32(value, cfg.records);
        else if (arg == "--payload")   ok = parse_u32(value, cfg.payload);
        else if (arg == "--write-ns")  ok = parse_u32(value, cfg.write_ns);
        else if (arg == "--gap-ns")    ok = parse_u32(value, cfg.gap_ns);
        else if (arg == "--reps")      ok = parse_u32(value, cfg.reps);
        else if (arg == "--timeout-s") ok = parse_u32(value, cfg.timeout_s);
        else {
            std::printf("unknown option %.*s\n", static_cast<int>(arg.size()), arg.data());
            usage();
            return 2;
        }
        if (!ok) {
            std::printf("bad value for %.*s\n", static_cast<int>(arg.size()), arg.data());
            return 2;
        }
    }
    if (cfg.producers == 0 || cfg.records == 0 || cfg.reps == 0) {
        std::printf("producers, records and reps must be non-zero\n");
        return 2;
    }

    const auto total = static_cast<std::uint64_t>(cfg.producers) * cfg.records;
    std::printf("slick-net write-chain A/B   hardware_concurrency=%u\n",
                std::thread::hardware_concurrency());
    std::printf("producers=%u records=%u/producer (%llu total) payload=%uB write-ns=%u gap-ns=%u reps=%u\n\n",
                cfg.producers, cfg.records, static_cast<unsigned long long>(total),
                cfg.payload, cfg.write_ns, cfg.gap_ns, cfg.reps);

    const auto post = measure(send_path::post, cfg);
    const auto gate = measure(send_path::gate, cfg);

    std::printf("%-6s %14s %16s %12s %12s %12s\n",
                "path", "wall ms", "[min-max]", "records/s", "ns/send", "posts");
    for (const auto& [label, s] : {std::pair{"post", post}, std::pair{"gate", gate}}) {
        if (!s.ok) {
            std::printf("%-6s %14s\n", label, "FAILED");
            continue;
        }
        std::printf("%-6s %14.1f %7.1f-%-8.1f %12.0f %12.1f %12llu",
                    label, s.wall_ms, s.wall_lo, s.wall_hi,
                    static_cast<double>(total) / (s.wall_ms / 1000.0), s.ns_per_send,
                    static_cast<unsigned long long>(s.posts));
        if (s.wasted_posts) {
            std::printf("  (%llu wasted)", static_cast<unsigned long long>(s.wasted_posts));
        }
        std::printf("\n");
    }

    if (post.ok && gate.ok) {
        std::printf("\nwall: gate %.2fx   send cost: gate %.2fx   (>1 means the gate is faster)\n",
                    post.wall_ms / gate.wall_ms, post.ns_per_send / gate.ns_per_send);
        std::printf("posts removed: %llu of %llu\n",
                    static_cast<unsigned long long>(post.posts - gate.posts),
                    static_cast<unsigned long long>(post.posts));
    }
    std::printf("\nMicrobenchmark of the wakeup decision only - no socket, no TLS, no beast write\n"
                "path. Run it on the target hardware, and sweep --gap-ns: sends that never overlap a\n"
                "running chain pay the fence and save no post.\n");
    return (post.ok && gate.ok) ? 0 : 1;
}
