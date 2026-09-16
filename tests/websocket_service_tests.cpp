#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include <boost/asio/post.hpp>
#include <slick/net/websocket.hpp>
#include <slick/net/detail/websocket_impl.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace slick::net {

namespace {

using namespace std::chrono_literals;

constexpr auto kMeasureWindow = 500ms;
constexpr double kIdleUsage = 0.1;  // fraction of one core
// A loop blocked in run() only turns when stopped; a polling loop turns once per poll()
constexpr std::uint64_t kIdleIterations = 2;
constexpr std::uint64_t kSpinIterations = 1000;

// CPU time consumed by the calling thread
std::chrono::nanoseconds thread_cpu_time() {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user);
    auto to_ns = [](const FILETIME& ft) {
        // FILETIME counts 100ns intervals
        return std::chrono::nanoseconds(
            ((static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) * 100);
    };
    return to_ns(kernel) + to_ns(user);
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
#endif
}

struct service_sample {
    std::chrono::nanoseconds cpu{};
    std::uint64_t loop_iterations = 0;
};

// Samples the service thread on the service thread itself. Process CPU time read from
// another thread is unreliable: macOS getrusage() only sums usage each thread has already
// committed, so a service thread spinning on another core reads as mostly idle.
service_sample sample_service_thread() {
    auto sample = std::make_shared<std::promise<service_sample>>();
    auto result = sample->get_future();
    boost::asio::post(detail::websocket_ioc(), [sample] {
        sample->set_value({thread_cpu_time(), detail::websocket_service_loop_iterations()});
    });
    if (result.wait_for(5s) != std::future_status::ready) {
        ADD_FAILURE() << "service thread did not run the sampling handler";
        return {};
    }
    return result.get();
}

struct service_activity {
    double cpu_usage = 0;  // cores used by the service thread
    std::uint64_t loop_iterations = 0;
};

// Busy polling is detected by loop turns rather than CPU usage: the CPU share an OS accounts to a
// spinning thread varies (the macOS CI runner reports ~0.2 of a core), while a loop blocked in
// run() does not turn at all.
service_activity service_activity_over(std::chrono::milliseconds window) {
    const auto start = sample_service_thread();
    const auto wall_start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(window);
    const auto end = sample_service_thread();
    const auto wall = std::chrono::steady_clock::now() - wall_start;
    return {std::chrono::duration<double>(end.cpu - start.cpu) / std::chrono::duration<double>(wall),
            end.loop_iterations - start.loop_iterations};
}

::testing::AssertionResult service_idle() {
    const auto activity = service_activity_over(kMeasureWindow);
    if (activity.cpu_usage < kIdleUsage && activity.loop_iterations <= kIdleIterations) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "service thread is not idle: cpu usage " << activity.cpu_usage
        << " (limit " << kIdleUsage << "), loop iterations " << activity.loop_iterations
        << " (limit " << kIdleIterations << ")";
}

::testing::AssertionResult service_busy_polling() {
    const auto activity = service_activity_over(kMeasureWindow);
    if (activity.loop_iterations > kSpinIterations) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "service thread is not busy polling: loop iterations "
        << activity.loop_iterations << " (need > " << kSpinIterations << "), cpu usage " << activity.cpu_usage;
}

} // namespace

class WebsocketServiceTest : public ::testing::Test {
protected:
    void TearDown() override {
        Websocket<>::shutdown();
        Websocket<>::set_busy_poll(false);
    }

    // Runs one connection attempt to completion: the service thread is started and then
    // left with no outstanding I/O. Nothing listens on port 1, so the connect fails.
    static bool run_failed_connection() {
        std::atomic_bool disconnected{false};
        Websocket<> ws(
            "ws://127.0.0.1:1/",
            []() {},
            [&]() { disconnected.store(true, std::memory_order_relaxed); },
            [](const char*, std::size_t) {},
            [](std::string&&) {});
        ws.open();

        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!disconnected.load(std::memory_order_relaxed) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        return disconnected.load(std::memory_order_relaxed);
    }
};

// Regression: once the io_context ran out of work, run() returned immediately and the
// service loop restarted it forever, spinning a core with no connection open
TEST_F(WebsocketServiceTest, IdleServiceThreadDoesNotSpin) {
    ASSERT_FALSE(Websocket<>::busy_poll());
    ASSERT_TRUE(run_failed_connection());
    ASSERT_TRUE(Websocket<>::is_running());

    EXPECT_TRUE(service_idle());
}

TEST_F(WebsocketServiceTest, BusyPollSpinsAndDeliversEvents) {
    Websocket<>::set_busy_poll(true);
    EXPECT_TRUE(Websocket<>::busy_poll());

    // The connect completion is dispatched by the polling loop
    ASSERT_TRUE(run_failed_connection());

    EXPECT_TRUE(service_busy_polling());
}

// Enabling busy polling must wake a service thread blocked in run(), and switching
// back must leave the service processing I/O
TEST_F(WebsocketServiceTest, BusyPollSwitchesWhileRunning) {
    ASSERT_TRUE(run_failed_connection());
    EXPECT_TRUE(service_idle());

    Websocket<>::set_busy_poll(true);
    EXPECT_TRUE(service_busy_polling());
    ASSERT_TRUE(run_failed_connection());

    Websocket<>::set_busy_poll(false);
    EXPECT_TRUE(service_idle());
    ASSERT_TRUE(run_failed_connection());
}

TEST_F(WebsocketServiceTest, ShutdownWhileBusyPollingThenRestart) {
    Websocket<>::set_busy_poll(true);
    ASSERT_TRUE(run_failed_connection());

    Websocket<>::shutdown();
    EXPECT_FALSE(Websocket<>::is_running());

    // The next open() restarts the service in busy-poll mode
    ASSERT_TRUE(run_failed_connection());
    EXPECT_TRUE(Websocket<>::is_running());
    EXPECT_TRUE(service_busy_polling());
}

// Regression: callbacks run on the service thread, so shutdown() from one had that thread join itself.
// join() threw resource_deadlock_would_occur and left the static thread object joinable, and because the
// service was already flagged stopped the terminator's shutdown() skipped the join - so the thread
// object's destructor terminated the process during static teardown, and an open() before that
// terminated on the move-assignment onto a joinable thread.
TEST_F(WebsocketServiceTest, ShutdownFromCallbackDoesNotSelfJoin) {
    std::atomic_bool callback_done{false};
    std::atomic_bool shutdown_threw{false};
    std::atomic<std::thread::id> service_thread_id{};
    std::atomic<std::thread::id> callback_thread_id{};
    std::string thrown;  // only safe to read once shutdown_threw is set
    {
        // Nothing listens on port 1, so the connect fails and onError runs on the service thread
        Websocket<> ws(
            "ws://127.0.0.1:1/",
            []() {},
            []() {},
            [](const char*, std::size_t) {},
            [&](std::string&&) {
                callback_thread_id.store(std::this_thread::get_id(), std::memory_order_release);
                try {
                    Websocket<>::shutdown();
                } catch (const std::exception& e) {
                    thrown = e.what();
                    shutdown_threw.store(true, std::memory_order_release);
                }
                callback_done.store(true, std::memory_order_release);
            });

        // The service thread is the only thread that runs the websocket io_context, so a handler queued
        // on it reports the id the callback must share for its shutdown() to have been a self-join.
        // Queued before open() so it is ahead of the session's own handlers however they race.
        boost::asio::post(detail::websocket_ioc(), [&] {
            service_thread_id.store(std::this_thread::get_id(), std::memory_order_release);
        });
        ws.open();

        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!callback_done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
    }

    ASSERT_TRUE(callback_done.load(std::memory_order_acquire)) << "the error callback never ran";
    EXPECT_FALSE(shutdown_threw.load(std::memory_order_acquire)) << "shutdown() threw: " << thrown;
    EXPECT_FALSE(Websocket<>::is_running());

    // Without these the test would still pass if callbacks ever moved off the service thread, where
    // shutdown() is an ordinary join and none of the regression is exercised
    ASSERT_NE(service_thread_id.load(std::memory_order_acquire), std::thread::id{})
        << "the service thread never ran the posted handler";
    EXPECT_EQ(callback_thread_id.load(std::memory_order_acquire), service_thread_id.load(std::memory_order_acquire))
        << "the callback did not run on the service thread, so its shutdown() was not a self-join";

    // Joins the thread the callback's shutdown() could only ask to stop, the way the terminator does at
    // program exit. It also makes the restart below deterministic: the service can only start again
    // once the previous thread has cleared init_service_thread_ on its way out.
    Websocket<>::shutdown();

    // Reusing the thread object move-assigns a new thread onto it
    ASSERT_TRUE(run_failed_connection());
    EXPECT_TRUE(Websocket<>::is_running());
}

} // namespace slick::net
