#include <slick/logger.hpp>
#include <slick/net/logging.h>
#include <slick/net/websocket.hpp>
#include <thread>
#include <nlohmann/json.hpp>
using namespace slick::net;
using namespace slick::logger;

namespace {
void configure_slick_net_logging()
{
    set_log_handler([](slick::net::LogLevel level, const char* format_text,
                       std::format_args args) {
        std::string message;
        try {
            message = std::vformat(format_text, args);
        } catch (...) {
            message = std::string(format_text);
        }

        switch (level) {
            case slick::net::LogLevel::Trace:
                LOG_TRACE("slick-net: {}", message);
                break;
            case slick::net::LogLevel::Debug:
                LOG_DEBUG("slick-net: {}", message);
                break;
            case slick::net::LogLevel::Info:
                LOG_INFO("slick-net: {}", message);
                break;
            case slick::net::LogLevel::Warn:
                LOG_WARN("slick-net: {}", message);
                break;
            case slick::net::LogLevel::Error:
                LOG_ERROR("slick-net: {}", message);
                break;
        }
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
        },                                                                                          // onConnected
        [](){ LOG_INFO("ws disconnected"); },                                                       // onDisconnected
        [&](const char* data, size_t size){ LOG_INFO("onData: {}", std::string(data, size)); },   // onData
        [&](std::string err){ LOG_ERROR("onError: {}", std::move(err)); ws->close(); }            // onError
    );
    ws->open();

    // Ctrl-C to exit

    while(Websocket::is_running())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    shutdown_slick_net_logging();
    return 0;
}
