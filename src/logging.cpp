#include <slick/net/logging.hpp>

namespace slick::net {
namespace {

LogHandlerWithLocation g_log_handler_with_location;
LogLevelGetter g_log_level_getter;

} // namespace

void set_log_handler(LogHandler handler, LogLevelGetter get_level) {
    g_log_handler_with_location = [handler = std::move(handler)](LogLevel level, uint32_t /* line */, const char* /* file_name */, bool /* is_static_file_name */, const char* format_text, std::format_args args) {
        handler(level, format_text, args);
    };
    g_log_level_getter = std::move(get_level);
}

void set_log_handler_with_location(LogHandlerWithLocation handler, LogLevelGetter get_level) {
    g_log_handler_with_location = std::move(handler);
    g_log_level_getter = std::move(get_level);
}

bool should_log(LogLevel level) noexcept {
    if (g_log_handler_with_location && g_log_level_getter) {
        return level >= g_log_level_getter();
    }
    return false;
}

void clear_log_handler() noexcept {
    g_log_handler_with_location= {};
    g_log_level_getter = {};
}

void log_message_internal(LogLevel level, uint32_t line, const char* file_name, bool is_static_file_name, const char* format_text, std::format_args args) noexcept {
    if (!g_log_handler_with_location) {
        return;
    }

    try {
        g_log_handler_with_location(level, line, file_name, is_static_file_name, format_text, args);
    } catch (...) {
        // Logging must never throw back into library code.
    }
}

} // namespace slick::net
