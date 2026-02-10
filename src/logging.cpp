#include <slick/net/logging.h>

namespace slick::net {
namespace {

LogHandler g_log_handler;

} // namespace

void set_log_handler(LogHandler handler) {
    g_log_handler = std::move(handler);
}

void clear_log_handler() noexcept {
    g_log_handler = {};
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
