#include <gtest/gtest.h>

#include <slick/net/logging.hpp>

#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace slick::net {

TEST(LoggingTest, DispatchesToConfiguredHandler) {
    std::mutex mutex;
    std::vector<std::string> messages;

    set_log_handler([&](LogLevel level, std::string_view format_text, std::format_args args) {
        std::scoped_lock lock(mutex);
        std::string message;
        try {
            message = std::vformat(format_text, args);
        } catch (...) {
            message = std::string(format_text);
        }
        messages.emplace_back(std::to_string(static_cast<int>(level)) + ":" + message);
    });

    log_message(LogLevel::Info, "test message {}", 42);
    clear_log_handler();

    std::scoped_lock lock(mutex);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages.front(), "2:test message 42");
}

TEST(LoggingTest, ClearLogHandlerDisablesDispatch) {
    std::atomic_uint32_t calls{0};

    set_log_handler([&](LogLevel, std::string_view, std::format_args) {
        calls.fetch_add(1, std::memory_order_relaxed);
    });

    log_message(LogLevel::Debug, "before clear");
    clear_log_handler();
    log_message(LogLevel::Debug, "after clear");

    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1u);
}

TEST(LoggingTest, FormatterFallbackDoesNotThrow) {
    std::atomic_uint32_t calls{0};

    set_log_handler([&](LogLevel, std::string_view format_text, std::format_args args) {
        // Try to format, should fail and fall back to plain message
        std::string message;
        try {
            message = std::vformat(format_text, args);
        } catch (...) {
            message = std::string(format_text);
        }

        if (message == "bad format {") {
            calls.fetch_add(1, std::memory_order_relaxed);
        }
    });

    log_message(LogLevel::Warn, "bad format {", "x");
    clear_log_handler();

    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1u);
}

TEST(LoggingTest, HandlerReceivesUnformattedArgs) {
    std::atomic_uint32_t calls{0};
    std::string captured_format;

    set_log_handler([&](LogLevel, std::string_view format_text, std::format_args args) {
        captured_format = std::string(format_text);
        // Verify we can format
        try {
            auto format = std::vformat(format_text, args);
            calls.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            // Should not throw
        }
    });

    log_message(LogLevel::Info, "value is {}", 42);
    clear_log_handler();

    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(captured_format, "value is {}");
}

TEST(LoggingTest, PlainStringNoArgs) {
    std::string captured;

    set_log_handler([&](LogLevel, std::string_view format_text, std::format_args args) {
        // With no args, format_text is the complete message
        captured = std::vformat(format_text, args);
    });

    log_message(LogLevel::Debug, "plain message");
    clear_log_handler();

    EXPECT_EQ(captured, "plain message");
}

} // namespace slick::net
