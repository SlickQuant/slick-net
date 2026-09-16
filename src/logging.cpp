#include <slick/net/logging.hpp>

#include <atomic>

namespace slick::net {
namespace {

// The handler and its level getter are published together as one immutable node.
// Writing the two std::functions in place raced with worker threads reading and
// invoking them: a reader could observe a half-assigned std::function (its target
// pointer already swung at a target not yet constructed, or at one already
// destroyed) or a new handler paired with the previous getter. A node is fully
// built before it is published, and never mutated afterwards, so a reader only
// ever sees a consistent pair.
//
// Readers take a single acquire load - no reference count, no lock - which keeps
// the common "no handler installed" path a plain pointer test.
struct log_hooks {
    LogHandlerWithLocation handler;
    LogLevelGetter get_level;
    log_hooks* retired_next{ nullptr };  // links into g_retired, see retire()
};

std::atomic<log_hooks*> g_hooks{ nullptr };

// A reader can still be inside handler() when the node is replaced, and there is
// no lock-free way to prove it has left, so a replaced node is pushed here and
// kept for the life of the process rather than deleted. Nothing ever pops this
// stack, so the push needs no ABA protection. Installing a handler is a
// setup-time operation, so this holds a handful of small nodes at most.
std::atomic<log_hooks*> g_retired{ nullptr };

void retire(log_hooks* node) noexcept {
    if (!node) {
        return;
    }
    node->retired_next = g_retired.load(std::memory_order_relaxed);
    while (!g_retired.compare_exchange_weak(node->retired_next, node,
                                            std::memory_order_release, std::memory_order_relaxed)) {
    }
}

void publish(log_hooks* node) noexcept {
    retire(g_hooks.exchange(node, std::memory_order_acq_rel));
}

} // namespace

void set_log_handler(LogHandler handler, LogLevelGetter get_level) {
    set_log_handler_with_location(
        [handler = std::move(handler)](LogLevel level, uint32_t /* line */, const char* /* file_name */, bool /* is_static_file_name */, const char* format_text, std::format_args args) {
            handler(level, format_text, args);
        },
        std::move(get_level));
}

void set_log_handler_with_location(LogHandlerWithLocation handler, LogLevelGetter get_level) {
    publish(new log_hooks{ std::move(handler), std::move(get_level) });
}

bool should_log(LogLevel level) noexcept {
    const auto* hooks = g_hooks.load(std::memory_order_acquire);
    if (!hooks || !hooks->handler || !hooks->get_level) {
        return false;
    }

    try {
        return level >= hooks->get_level();
    } catch (...) {
        // Logging must never throw back into library code.
        return false;
    }
}

void clear_log_handler() noexcept {
    publish(nullptr);
}

void log_message_internal(LogLevel level, uint32_t line, const char* file_name, bool is_static_file_name, const char* format_text, std::format_args args) noexcept {
    const auto* hooks = g_hooks.load(std::memory_order_acquire);
    if (!hooks || !hooks->handler) {
        return;
    }

    try {
        hooks->handler(level, line, file_name, is_static_file_name, format_text, args);
    } catch (...) {
        // Logging must never throw back into library code.
    }
}

} // namespace slick::net
