#include <slick/logger.hpp>
#include <slick/net/logging.hpp>
#include <slick/net/websocket.hpp>
#include <thread>
#include <nlohmann/json.hpp>
using namespace slick::net;
using namespace slick::logger;

namespace {
auto &logger = Logger::instance();
void configure_slick_net_logging()
{
    set_log_handler([](slick::net::LogLevel level, const char* format_text,
                       std::format_args args) {
        logger.log(static_cast<slick::logger::LogLevel>(level), format_text, args);
    });
}

void shutdown_slick_net_logging()
{
    clear_log_handler();
}
}

int main()
{
    auto &logger = Logger::instance();
    logger.add_console_sink(true, true);
    logger.set_level(slick::logger::LogLevel::L_INFO);
    logger.init(1024, 16777216); // use pre-added sinks
    configure_slick_net_logging();

    std::vector<nlohmann::json> requests {
        R"({
            "type": "subscribe",
            "channel": "level2",
            "product_ids": ["BTC-USD"]
        })"_json,
        R"({
            "type": "subscribe",
            "channel": "market_trades",
            "product_ids": ["BTC-USD"]
        })"_json,
    };

    std::shared_ptr<slick::net::Websocket> ws;
    ws = std::make_shared<slick::net::Websocket>(
        // "wss://ws.postman-echo.com/raw",
        "wss://advanced-trade-ws.coinbase.com",
        [&](){ 
            LOG_INFO("ws connected");
            for (const auto &req : requests) {
                auto str_req = req.dump();
                ws->send(str_req.data(), str_req.size()); 
            }
        },                                                                              // onConnected
        [&](){ 
            LOG_INFO("ws disconnected");
            // reconnect if connection lost
            if (!Websocket::is_running()) {
                ws->open();
            }
        },                                  // onDisconnected
        [&](const char* data, size_t size){ },                                          // onData - do nothing on the service thread
        [&](std::string err){ LOG_ERROR("onError: {}", std::move(err)); ws->close(); }  // onError
    );

    uint64_t cursor = ws->initial_reading_index(); // Initialize cursor to 0 or initial_reading_index() before the first drain_data call.
    
    ws->open();

    // Ctrl-C to exit

    while(Websocket::is_running())
    {
        // Process data on main thread instead of the service thread.
        ws->drain_data(
            cursor,
            [&](const char* data, std::size_t size) {
                LOG_INFO("onData (drained): {}", std::string(data, size));
            }
        );
    }
    shutdown_slick_net_logging();
    return 0;
}
