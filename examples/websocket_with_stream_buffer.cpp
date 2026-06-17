#include <slick/logger.hpp>
#include <slick/net/logging.hpp>
#include <slick/stream_buffer.hpp>
#include <slick/dynamic_buffer.hpp>
#include <slick/net/websocket.hpp>
#include <thread>
#include <nlohmann/json.hpp>
using namespace slick::net;
using namespace slick::logger;

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
}

int main()
{
    gLogger.add_console_sink(true, true);
    gLogger.set_level(slick::logger::LogLevel::L_INFO);
    gLogger.init(1024, 16777216);
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

    using WsType = Websocket<slick::dynamic_buffer<slick::stream_buffer>>;
    auto sb = std::make_shared<slick::stream_buffer>(1 << 24, 1 << 16);
    std::shared_ptr<WsType> ws;
    ws = std::make_shared<WsType>(
        "wss://advanced-trade-ws.coinbase.com",
        [&]() {
            LOG_INFO("ws connected");
            for (const auto &req : requests) {
                auto str_req = req.dump();
                ws->send(str_req.data(), str_req.size());
            }
        },                                                                                      // onConnected
        [&](){
            LOG_INFO("ws disconnected");
            // reconnect if connection lost
            if (WsType::is_running()) {
                ws->open();
            }
        },                                                                                      // onDisconnected
        [](const char* data, size_t len){ LOG_INFO("onData: {}", std::string(data, len)); },    // onData
        [&](std::string err){ LOG_ERROR("onError: {}", std::move(err)); ws->close(); },         // onError
        sb
    );

    auto cursor = sb->initial_reading_index();
    ws->open();

    while (WsType::is_running()) {
        auto [ptr, len] = sb->read(cursor);
        if (ptr && len) {
            LOG_INFO("onData [main thread]: {}", std::string((char*)ptr, len));
        }
    }
    shutdown_slick_net_logging();
    return 0;
}
