#include <slick/logger.hpp>
#include <slick/net/logging.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
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

    slick::stream_buffer_multiplexer mux(2048);
    auto btc_buffer = mux.add_producer(0, 1 << 24, 1 << 16);
    auto eth_buffer = mux.add_producer(1, 1 << 24, 1 << 16);

    using buffer_t = slick::dynamic_buffer<decltype(btc_buffer)::element_type>;


    std::shared_ptr<Websocket<buffer_t>> ws_btc;
    std::shared_ptr<Websocket<buffer_t>> ws_eth;
    ws_btc = std::make_shared<Websocket<buffer_t>>(
        "wss://advanced-trade-ws.coinbase.com",
        [&ws_btc]() {
            LOG_INFO("BTC ws connected");
            auto str_req = R"({
                "type": "subscribe",
                "channel": "level2",
                "product_ids": ["BTC-USD"]
            })"_json.dump();
            ws_btc->send(str_req.data(), str_req.size()); 
        },                                                                                                  // onConnected
        [&ws_btc](){ 
            LOG_INFO("BTC ws disconnected");
            // reconnect if connection lost
            if (Websocket<>::is_running()) {
                ws_btc->open();
            }
        },                                                                                                  // onDisconnected
        [&ws_btc](const char* data, size_t size){ },                                                        // onData - do nothing on the service thread
        [&ws_btc](std::string err){ LOG_ERROR("BTC ws onError: {}", std::move(err)); ws_btc->close(); },    // onError
        btc_buffer
    );

    ws_eth = std::make_shared<Websocket<buffer_t>>(
        "wss://advanced-trade-ws.coinbase.com",
        [&ws_eth]() {
            LOG_INFO("ETH ws connected");
            auto str_req = R"({
                "type": "subscribe",
                "channel": "level2",
                "product_ids": ["ETH-USD"]
            })"_json.dump();
            ws_eth->send(str_req.data(), str_req.size()); 
        },                                                                                                  // onConnected
        [&ws_eth](){ 
            LOG_INFO("ETH ws disconnected");
            // reconnect if connection lost
            if (Websocket<>::is_running()) {
                ws_eth->open();
            }
        },                                                                                                  // onDisconnected
        [&ws_eth](const char* data, size_t size){ },                                                        // onData - do nothing on the service thread
        [&ws_eth](std::string err){ LOG_ERROR("ETH ws onError: {}", std::move(err)); ws_eth->close(); },    // onError
        eth_buffer
    );

    auto cursor = mux.initial_reading_index();
    ws_btc->open();
    ws_eth->open();

    while (Websocket<>::is_running()) {
        auto record = mux.read(cursor);
        if (record) {
            LOG_INFO("onData [main thread]: {}", std::string((char*)record.data, record.length));
        }
    }
    shutdown_slick_net_logging();
    return 0;
}
