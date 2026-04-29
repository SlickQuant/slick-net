#include <slick/logger.hpp>
#include <slick/net/http_stream.hpp>
#include <slick/net/logging.hpp>
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

// Example: Connect to a Server-Sent Events (SSE) endpoint
// This demonstrates HTTP streaming for real-time data

int main()
{
    auto &logger = Logger::instance();
    logger.add_console_sink(true, true);
    logger.set_level(slick::logger::LogLevel::L_INFO);
    logger.init(1024, 16777216); // use pre-added sinks
    configure_slick_net_logging();

    std::atomic<int> event_count{0};

    // Create HTTP stream connection
    // This example uses a test SSE endpoint that sends time updates
    auto stream = std::make_shared<HttpStream>(
        "https://stream.wikimedia.org/v2/stream/recentchange",
        []() {
            LOG_INFO("Stream connected");
        },
        []() {
            LOG_INFO("Stream disconnected");
        },
        [&event_count](const char* data, size_t size) {
            std::string event_data(data, size);
            int count = event_count.fetch_add(1, std::memory_order_relaxed) + 1;
            LOG_INFO("Received event #{}: {}", count, event_data);

            // Try to parse as JSON
            try {
                auto json = nlohmann::json::parse(event_data);
                LOG_DEBUG("Parsed JSON: {}", json.dump(2));
            } catch(...) {
                // Not JSON, just log the raw data
                LOG_DEBUG("Raw data: {}", event_data);
            }
        },
        [](std::string err) {
            LOG_ERROR("Stream error: {}", err);
        }
    );

    // Open the stream
    stream->open();

    LOG_INFO("HTTP Stream started. Listening for 30 seconds...");

    // Let it run for 30 seconds to receive some events
    for (int i = 0; i < 30 && HttpStream::is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Close the stream
    stream->close();

    // Wait a bit for graceful shutdown
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    LOG_INFO("Total events received: {}", event_count.load());

    // Shutdown the service
    HttpStream::shutdown();
    shutdown_slick_net_logging();

    return 0;
}
