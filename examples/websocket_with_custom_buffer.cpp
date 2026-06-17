#include <slick/logger.hpp>
#include <slick/net/logging.hpp>

#include <boost/beast/core/flat_buffer.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>

namespace example {

class metered_flat_buffer : public boost::beast::flat_buffer {
public:
    using boost::beast::flat_buffer::flat_buffer;

    void consume(std::size_t n)
    {
        bytes_consumed_ += n;
        boost::beast::flat_buffer::consume(n);
    }

    std::size_t bytes_consumed() const noexcept { return bytes_consumed_; }

private:
    std::size_t bytes_consumed_{0};
};

} // namespace example

// Custom buffer types need the template method definitions in one translation unit.
// Define this before the public websocket header instead of including detail headers.
#define SLICK_NET_WEBSOCKET_HEADER_ONLY
#include <slick/net/websocket.hpp>

using namespace slick::logger;
using namespace slick::net;

namespace {
auto &gLogger = Logger::instance();

void configure_slick_net_logging()
{
    set_log_handler([](slick::net::LogLevel level, const char* format_text,
                       std::format_args args) {
        gLogger.log(static_cast<slick::logger::LogLevel>(level), format_text, args);
    });
}

void shutdown_slick_net_logging() { clear_log_handler(); }
} // namespace

int main()
{
    gLogger.add_console_sink(true, true);
    gLogger.set_level(slick::logger::LogLevel::L_INFO);
    gLogger.init(1024, 16777216);
    configure_slick_net_logging();

    using WsType = Websocket<example::metered_flat_buffer>;
    std::atomic_bool done{false};
    std::shared_ptr<WsType> ws;

    ws = std::make_shared<WsType>(
        "wss://ws.postman-echo.com/raw",
        [&]() {
            LOG_INFO("ws connected");
            const std::string message = "hello from a custom websocket buffer";
            ws->send(message.data(), message.size());
        },
        [&]() {
            LOG_INFO("ws disconnected");
            done.store(true, std::memory_order_release);
        },
        [&](const char* data, std::size_t size) {
            LOG_INFO("onData: {}", std::string(data, size));
            ws->close();
        },
        [&](std::string err) {
            LOG_ERROR("onError: {}", std::move(err));
            done.store(true, std::memory_order_release);
            ws->close();
        });

    ws->open();

    while (WsType::is_running() && !done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    WsType::shutdown();
    shutdown_slick_net_logging();
    return 0;
}
