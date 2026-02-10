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
};

// using LogHandler = std::function<void(LogLevel, std::string_view, std::format_args)>;
using LogHandler = std::function<void(LogLevel, const char*, std::format_args)>;

void set_log_handler(LogHandler handler);
void clear_log_handler() noexcept;
// void log_message_internal(LogLevel level, std::string_view format_text, std::format_args args) noexcept;
void log_message_internal(LogLevel level, const char* format_text, std::format_args args) noexcept;

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

#ifndef LOG_DEBUG
#define LOG_DEBUG(...) ::slick::net::log_message(::slick::net::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...) ::slick::net::log_message(::slick::net::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) ::slick::net::log_message(::slick::net::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::slick::net::log_message(::slick::net::LogLevel::Error, __VA_ARGS__)
#define LOG_TRACE(...) ::slick::net::log_message(::slick::net::LogLevel::Trace, __VA_ARGS__)
#endif

} // namespace slick::net
