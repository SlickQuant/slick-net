#include <slick/net/logging.hpp>

namespace slick::net {
namespace {

LogHandler g_log_handler;
LogLevelGetter g_log_level_getter;

} // namespace

void set_log_handler(LogHandler handler, LogLevelGetter get_level) {
    g_log_handler = std::move(handler);
    g_log_level_getter = std::move(get_level);
}

bool should_log(LogLevel level) noexcept {
    if (g_log_handler && g_log_level_getter) {
        return level >= g_log_level_getter();
    }
    return false;
}

void clear_log_handler() noexcept {
    g_log_handler = {};
    g_log_level_getter = {};
}

// void log_message_internal(LogLevel level, std::string_view format_text, std::format_args args) noexcept {
void log_message_internal(LogLevel level, const char* format_text, std::format_args args) noexcept {
    if (!g_log_handler) {
        return;
    }

    try {
        g_log_handler(level, format_text, args);
    } catch (...) {
        // Logging must never throw back into library code.
    }
}

} // namespace slick::net
