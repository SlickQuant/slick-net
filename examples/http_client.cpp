#include <slick/logger.hpp>
#include <slick/net/http.hpp>
#include <slick/net/logging.hpp>
#include <nlohmann/json.hpp>

using namespace slick::net;
using namespace slick::logger;

namespace {
    std::atomic_uint_fast32_t pending_async_requests{4};
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
    logger.add_console_sink(true, true);
    logger.set_level(slick::logger::LogLevel::L_DEBUG);
    logger.init(1024); // use pre-added sinks
    configure_slick_net_logging();

    auto response = Http::get("https://api.coinbase.com/api/v3/brokerage/market/products");
    if (response.is_ok()) {
        LOG_INFO(response.result_text);
    }
    else {
        LOG_ERROR(response.result_text);
    }

    response = Http::get("https://api.coinbase.com/api/v3/brokerage/market/products/BTC-USD/candles?granularity=ONE_MINUTE");
    if (response.is_ok()) {
        LOG_DEBUG(response.result_text);
    }
    else {
        LOG_ERROR(response.result_text);
    }

    Http::async_get([&](Http::Response&& rsp) {
        if (rsp.is_ok()) {
            LOG_INFO(rsp.result_text);
        }
        else {
            LOG_ERROR(rsp.result_text);
        }
        pending_async_requests.fetch_sub(1);
    }, "https://api.coinbase.com/api/v3/brokerage/market/product_book?product_id=BTC-USD");

    Http::async_get([&](Http::Response&& rsp) {
        if (rsp.is_ok()) {
            LOG_DEBUG(rsp.result_text);
        }
        else {
            LOG_ERROR(rsp.result_text);
        }
        pending_async_requests.fetch_sub(1);
    }, "https://api.coinbase.com/api/v3/brokerage/market/products/BTC-USD/ticker?limit=200");

    // Example POST request with JSON body
    nlohmann::json post_data = {
        {"title", "Test Post"},
        {"body", "This is a test"},
        {"userId", 1}
    };
    Http::async_post([&](Http::Response&& rsp) {
        if (rsp.is_ok()) {
            LOG_INFO("POST response: {}", rsp.result_text);
        }
        else {
            LOG_ERROR("POST error: {}", rsp.result_text);
        }
        pending_async_requests.fetch_sub(1);
    }, "https://jsonplaceholder.typicode.com/posts", post_data.dump(), {{"Content-Type", "application/json"}});

    // Example PUT request with JSON body
    nlohmann::json put_data = {
        {"id", 1},
        {"title", "Updated Title"},
        {"body", "Updated body"},
        {"userId", 1}
    };
    Http::async_put([&](Http::Response&& rsp) {
        if (rsp.is_ok()) {
            LOG_INFO("PUT response: {}", rsp.result_text);
        }
        else {
            LOG_ERROR("PUT error: {}", rsp.result_text);
        }
        pending_async_requests.fetch_sub(1);
    }, "https://jsonplaceholder.typicode.com/posts/1", put_data.dump(), {{"Content-Type", "application/json"}});

    LOG_WARN("Waiting For Async Requests...");
    while(pending_async_requests.load(std::memory_order_relaxed));
    shutdown_slick_net_logging();
}
