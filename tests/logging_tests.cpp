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

TEST(LoggingTest, DisabledMacrosDoNotEvaluateArguments) {
    std::string captured;
    int evaluation_count = 0;
    auto expensive_message = [&]() -> std::string {
        ++evaluation_count;
        return std::format("other format can't avoid {}", "YEAH");
    };

    set_log_handler([&](LogLevel, std::string_view format_text, std::format_args args) {
        // With no args, format_text is the complete message
        captured.append(std::vformat(format_text, args));
    }, []() { return LogLevel::Debug; });

    LOG_DEBUG("Some format {}", expensive_message());
    LOG_TRACE("Some format {}", expensive_message());
    LOG_INFO("Some format {}", expensive_message());

    clear_log_handler();

    EXPECT_EQ(evaluation_count, 2);
    EXPECT_EQ(captured, "Some format other format can't avoid YEAHSome format other format can't avoid YEAH");
}

// --- LogHandlerWithLocation tests ---

TEST(LoggingTest, FileNameFromPathStripsDirectory) {
    EXPECT_STREQ(file_name_from_path("y:/repo/slick-net/src/foo.cpp"), "foo.cpp");
    EXPECT_STREQ(file_name_from_path("src\\foo.cpp"), "foo.cpp");
    EXPECT_STREQ(file_name_from_path("foo.cpp"), "foo.cpp");
    EXPECT_STREQ(file_name_from_path(""), "");
    EXPECT_EQ(file_name_from_path(nullptr), nullptr);
}

TEST(LoggingTest, HandlerWithLocationReceivesExplicitLocation) {
    uint32_t captured_line = 0;
    const char* captured_file = nullptr;
    bool captured_is_static = false;
    std::string captured_message;

    set_log_handler_with_location(
        [&](LogLevel, uint32_t line, const char* file_name, bool is_static,
            const char* fmt, std::format_args args) {
            captured_line = line;
            captured_file = file_name;
            captured_is_static = is_static;
            try { captured_message = std::vformat(fmt, args); } catch (...) {}
        });

    log_message(LogLevel::Info, 42u, "myfile.cpp", false, "hello {}", "world");
    clear_log_handler();

    EXPECT_EQ(captured_line, 42u);
    EXPECT_STREQ(captured_file, "myfile.cpp");
    EXPECT_FALSE(captured_is_static);
    EXPECT_EQ(captured_message, "hello world");
}

TEST(LoggingTest, HandlerWithLocationNullFileNamePassedThrough) {
    uint32_t captured_line = 99u;
    const char* captured_file = reinterpret_cast<const char*>(1);

    set_log_handler_with_location(
        [&](LogLevel, uint32_t line, const char* file_name, bool, const char*, std::format_args) {
            captured_line = line;
            captured_file = file_name;
        });

    log_message(LogLevel::Info, static_cast<uint32_t>(0), static_cast<const char*>(nullptr), true, "msg");
    clear_log_handler();

    EXPECT_EQ(captured_line, 0u);
    EXPECT_EQ(captured_file, nullptr);
}

TEST(LoggingTest, HandlerWithLocationLevelFilteringWorks) {
    std::atomic_uint32_t calls{0};

    set_log_handler_with_location(
        [&](LogLevel, uint32_t, const char*, bool, const char*, std::format_args) {
            calls.fetch_add(1, std::memory_order_relaxed);
        },
        []() { return LogLevel::Warn; });

    LOG_DEBUG("debug");
    LOG_INFO("info");
    LOG_WARN("warn");
    LOG_ERROR("error");
    clear_log_handler();

    EXPECT_EQ(calls.load(std::memory_order_relaxed), 2u);
}

TEST(LoggingTest, ClearHandlerAlsoClearsLocationHandler) {
    std::atomic_uint32_t calls{0};

    set_log_handler_with_location(
        [&](LogLevel, uint32_t, const char*, bool, const char*, std::format_args) {
            calls.fetch_add(1, std::memory_order_relaxed);
        });

    LOG_INFO("before clear");
    clear_log_handler();
    LOG_INFO("after clear");

    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1u);
}

#if SLICK_NET_ENABLE_SOURCE_LOCATION

TEST(LoggingTest, MacroCapturesLineNumber) {
    uint32_t captured_line = 0;

    set_log_handler_with_location(
        [&](LogLevel, uint32_t line, const char*, bool, const char*, std::format_args) {
            captured_line = line;
        });

    const uint32_t expected_line = __LINE__ + 1;
    LOG_INFO("test");
    clear_log_handler();

    EXPECT_EQ(captured_line, expected_line);
}

TEST(LoggingTest, MacroCapturesFileNameBasenameOnly) {
    const char* captured_file = nullptr;

    set_log_handler_with_location(
        [&](LogLevel, uint32_t, const char* file_name, bool, const char*, std::format_args) {
            captured_file = file_name;
        });

    LOG_INFO("test");
    clear_log_handler();

    ASSERT_NE(captured_file, nullptr);
    std::string_view fname(captured_file);
    EXPECT_NE(fname.find("logging_tests.cpp"), std::string_view::npos);
    EXPECT_EQ(fname.find('/'), std::string_view::npos);
    EXPECT_EQ(fname.find('\\'), std::string_view::npos);
}

TEST(LoggingTest, MacroPassesIsStaticTrue) {
    bool captured_is_static = false;

    set_log_handler_with_location(
        [&](LogLevel, uint32_t, const char*, bool is_static, const char*, std::format_args) {
            captured_is_static = is_static;
        });

    LOG_WARN("test");
    clear_log_handler();

    EXPECT_TRUE(captured_is_static);
}

#endif  // SLICK_NET_ENABLE_SOURCE_LOCATION

} // namespace slick::net
