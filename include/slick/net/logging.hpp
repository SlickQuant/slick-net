#pragma once

#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace slick::net {

enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off,
};

using LogHandler = std::function<void(LogLevel, const char*, std::format_args)>;
using LogLevelGetter = std::function<LogLevel(void)>;
using LogHandlerWithLocation = std::function<void(LogLevel, uint32_t /* line */, const char* /* file_name */, bool /* is_static_file_name */, const char*, std::format_args)>;

void set_log_handler(LogHandler handler, LogLevelGetter get_level = []() { return LogLevel::Info; });
void set_log_handler_with_location(LogHandlerWithLocation handler, LogLevelGetter get_level = []() { return LogLevel::Info; });
void clear_log_handler() noexcept;
void log_message_internal(LogLevel level, uint32_t line, const char* file_name, bool is_static_file_name, const char* format_text, std::format_args args) noexcept;
bool should_log(LogLevel level) noexcept;

template <typename... Args>
inline void log_message(LogLevel level, const char* format_text, Args&&... args) noexcept {
    try {
        log_message_internal(level, 0, nullptr, true, format_text,
            std::make_format_args(args...));
    } catch (...) {
        // Fallback to plain message if args conversion fails
        // Empty format_args created from empty arg list
        log_message_internal(level, 0, nullptr, true, format_text, std::make_format_args());
    }
}

template <typename... Args>
inline void log_message(LogLevel level, uint32_t line, const char* file_name, bool is_static_file_name, const char* format_text, Args&&... args) noexcept {
    try {
        log_message_internal(level, line, file_name, is_static_file_name, format_text,
            std::make_format_args(args...));
    } catch (...) {
        // Fallback to plain message if args conversion fails
        // Empty format_args created from empty arg list
        log_message_internal(level, line, file_name, is_static_file_name, format_text, std::make_format_args());
    }
}

#ifndef __FILE_NAME__
#define __FILE_NAME__ slick::net::file_name_from_path(__FILE__)
#endif

inline constexpr const char* file_name_from_path(const char* path) noexcept {
    if (!path) {
        return nullptr;
    }
    const char* file_name = path;
    for (const char* current = path; *current != '\0'; ++current) {
        if (*current == '/' || *current == '\\') {
            file_name = current + 1;
        }
    }
    return file_name;
}

// Macros for easy logging
#ifndef LOG_DEBUG
#if SLICK_NET_ENABLE_SOURCE_LOCATION
#define SLICK_NET_LOG_AT_CALL_SITE(level, ...) \
    do { \
        static constexpr const char* file_name = __FILE_NAME__; \
        log_message( \
            level, \
            __LINE__, file_name, true, __VA_ARGS__); \
    } while (false)
#else
#define SLICK_NET_LOG_AT_CALL_SITE(level, ...) \
    log_message(level, static_cast<uint32_t>(0), nullptr, true, __VA_ARGS__)
#endif

#define SLICK_NET_LOG_IF_ENABLED(level, ...)        \
    do {                                            \
        if (should_log(level)) {                    \
            SLICK_NET_LOG_AT_CALL_SITE(level, __VA_ARGS__);        \
        }                                           \
    } while (false)

#define LOG_TRACE(...) SLICK_NET_LOG_IF_ENABLED(::slick::net::LogLevel::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) SLICK_NET_LOG_IF_ENABLED(::slick::net::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...) SLICK_NET_LOG_IF_ENABLED(::slick::net::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) SLICK_NET_LOG_IF_ENABLED(::slick::net::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) SLICK_NET_LOG_IF_ENABLED(::slick::net::LogLevel::Error, __VA_ARGS__)
#define LOG_FATAL(...) SLICK_NET_LOG_IF_ENABLED(::slick::net::LogLevel::Fatal, __VA_ARGS__)
#endif

} // namespace slick::net
