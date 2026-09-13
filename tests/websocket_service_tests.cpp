#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include <slick/net/websocket.hpp>

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
constexpr double kSpinUsage = 0.5;

// CPU time consumed by all threads of this process
std::chrono::nanoseconds process_cpu_time() {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    auto to_ns = [](const FILETIME& ft) {
        // FILETIME counts 100ns intervals
        return std::chrono::nanoseconds(
            ((static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) * 100);
    };
    return to_ns(kernel) + to_ns(user);
#else
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
#endif
}

// Cores used by the process over the window. The test thread sleeps, so this is
// effectively the service thread.
double cpu_usage_over(std::chrono::milliseconds window) {
    const auto cpu_start = process_cpu_time();
    const auto wall_start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(window);
    const auto cpu = process_cpu_time() - cpu_start;
    const auto wall = std::chrono::steady_clock::now() - wall_start;
    return std::chrono::duration<double>(cpu) / std::chrono::duration<double>(wall);
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

    EXPECT_LT(cpu_usage_over(kMeasureWindow), kIdleUsage);
}

TEST_F(WebsocketServiceTest, BusyPollSpinsAndDeliversEvents) {
    Websocket<>::set_busy_poll(true);
    EXPECT_TRUE(Websocket<>::busy_poll());

    // The connect completion is dispatched by the polling loop
    ASSERT_TRUE(run_failed_connection());

    EXPECT_GT(cpu_usage_over(kMeasureWindow), kSpinUsage);
}

// Enabling busy polling must wake a service thread blocked in run(), and switching
// back must leave the service processing I/O
TEST_F(WebsocketServiceTest, BusyPollSwitchesWhileRunning) {
    ASSERT_TRUE(run_failed_connection());
    EXPECT_LT(cpu_usage_over(kMeasureWindow), kIdleUsage);

    Websocket<>::set_busy_poll(true);
    EXPECT_GT(cpu_usage_over(kMeasureWindow), kSpinUsage);
    ASSERT_TRUE(run_failed_connection());

    Websocket<>::set_busy_poll(false);
    EXPECT_LT(cpu_usage_over(kMeasureWindow), kIdleUsage);
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
    EXPECT_GT(cpu_usage_over(kMeasureWindow), kSpinUsage);
}

} // namespace slick::net
