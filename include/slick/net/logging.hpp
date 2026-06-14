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

void set_log_handler(LogHandler handler, LogLevelGetter get_level = []() { return LogLevel::Info; });
void clear_log_handler() noexcept;
void log_message_internal(LogLevel level, const char* format_text, std::format_args args) noexcept;
bool should_log(LogLevel level) noexcept;

template <typename... Args>
// inline void log_message(LogLevel level, std::string_view format_text, Args&&... args) noexcept {
inline void log_message(LogLevel level, const char* format_text, Args&&... args) noexcept {
    try {
        log_message_internal(level, format_text,
            std::make_format_args(args...));
    } catch (...) {
        // Fallback to plain message if args conversion fails
        // Empty format_args created from empty arg list
        log_message_internal(level, format_text, std::make_format_args());
    }
}

// Macros for easy logging
#ifndef LOG_DEBUG
#define SLICK_NET_LOG_IF_ENABLED(level, ...)        \
    do {                                            \
        if (should_log(level)) {                    \
            log_message(level, __VA_ARGS__);        \
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
