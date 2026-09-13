#include <gtest/gtest.h>

#include <atomic>
#include <csignal>
#include <cstddef>
#include <memory>
#include <string>

#include <slick/net/http_stream.hpp>
#include <slick/net/websocket.hpp>

namespace slick::net {

namespace {

using signal_handler_t = void (*)(int);

std::atomic_int app_signal_count{0};

void app_signal_handler(int signal) {
    // MSVC resets the disposition to SIG_DFL before invoking a handler; re-arm it
    std::signal(signal, app_signal_handler);
    app_signal_count.fetch_add(1, std::memory_order_relaxed);
}

// Query the installed handler without changing it
signal_handler_t installed_handler(int signal) {
    auto handler = std::signal(signal, SIG_DFL);
    std::signal(signal, handler);
    return handler;
}

} // namespace

// Regression: HttpStream and Websocket used to install their own SIGINT/SIGTERM
// handlers on first open(). The handlers ran shutdown, logging and thread joins in
// signal context (not async-signal-safe), were never restored, and HttpStream
// overwrote the application's handlers.
class SignalTest : public ::testing::Test {
protected:
    void SetUp() override {
        app_signal_count.store(0, std::memory_order_relaxed);
        previous_sigint_ = std::signal(SIGINT, app_signal_handler);
        previous_sigterm_ = std::signal(SIGTERM, app_signal_handler);
    }

    void TearDown() override {
        std::signal(SIGINT, previous_sigint_);
        std::signal(SIGTERM, previous_sigterm_);
    }

    // The application's handlers must stay installed and receive the signals, and
    // the library must not shut its service down from signal context
    static void expect_signals_left_to_application(bool (*is_running)()) {
        ASSERT_TRUE(is_running());
        EXPECT_EQ(installed_handler(SIGINT), app_signal_handler);
        EXPECT_EQ(installed_handler(SIGTERM), app_signal_handler);

        std::raise(SIGINT);
        std::raise(SIGTERM);

        EXPECT_EQ(app_signal_count.load(std::memory_order_relaxed), 2);
        EXPECT_TRUE(is_running());
    }

private:
    signal_handler_t previous_sigint_ = SIG_DFL;
    signal_handler_t previous_sigterm_ = SIG_DFL;
};

TEST_F(SignalTest, HttpStreamLeavesSignalsToApplication) {
    // Nothing listens on port 1; the connection fails in the background
    auto stream = std::make_shared<HttpStream>(
        "http://127.0.0.1:1/",
        []() {},
        []() {},
        [](const char*, std::size_t) {},
        [](std::string) {});
    stream->open();

    expect_signals_left_to_application(&HttpStream::is_running);

    stream->close();
    HttpStream::shutdown();
    EXPECT_FALSE(HttpStream::is_running());
}

TEST_F(SignalTest, WebsocketLeavesSignalsToApplication) {
    {
        // Nothing listens on port 1; the connection fails in the background
        Websocket<> ws(
            "ws://127.0.0.1:1/",
            []() {},
            []() {},
            [](const char*, std::size_t) {},
            [](std::string&&) {});
        ws.open();

        expect_signals_left_to_application(&Websocket<>::is_running);

        ws.close();
    }
    Websocket<>::shutdown();
    EXPECT_FALSE(Websocket<>::is_running());
}

} // namespace slick::net
