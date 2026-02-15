#include <slick/net/http.hpp>
#include <slick/net/logging.hpp>
#include <nlohmann/json.hpp>
#include <slick/logger.hpp>
#include <boost/asio.hpp>

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

// Example coroutine that demonstrates awaitable HTTP GET request
boost::asio::awaitable<void> example_get()
{
    LOG_INFO("Starting awaitable GET request...");

    auto response = co_await Http::async_get("https://api.coinbase.com/api/v3/brokerage/market/products");
    if (response.is_ok()) {
        LOG_INFO("GET response received: {} bytes", response.result_text.size());
        // Parse and display first few products
        try {
            auto json = nlohmann::json::parse(response.result_text);
            if (json.contains("products") && json["products"].is_array() && json["products"].size() > 0) {
                LOG_INFO("First product: {}", json["products"][0].dump());
            }
        } catch (const std::exception& e) {
            LOG_ERROR("JSON parse error: {}", e.what());
        }
    }
    else {
        LOG_ERROR("GET failed with status {}: {}", response.result_code, response.result_text);
    }
}

// Example coroutine that demonstrates awaitable HTTP GET with custom headers
boost::asio::awaitable<void> example_get_with_headers()
{
    LOG_INFO("Starting awaitable GET with custom headers...");

    auto response = co_await Http::async_get(
        "https://jsonplaceholder.typicode.com/posts/1",
        {{"Accept", "application/json"}, {"X-Custom-Header", "awaitable-test"}}
    );

    if (response.is_ok()) {
        LOG_INFO("GET with headers response: {}", response.result_text);
    }
    else {
        LOG_ERROR("GET failed with status {}: {}", response.result_code, response.result_text);
    }
}

// Example coroutine that demonstrates awaitable HTTP POST request
boost::asio::awaitable<void> example_post()
{
    LOG_INFO("Starting awaitable POST request...");

    nlohmann::json post_data = {
        {"title", "Awaitable POST Test"},
        {"body", "This is a test using async/await pattern"},
        {"userId", 1}
    };

    auto response = co_await Http::async_post(
        "https://jsonplaceholder.typicode.com/posts",
        post_data.dump(),
        {{"Content-Type", "application/json"}}
    );

    if (response.is_ok()) {
        LOG_INFO("POST response (status {}): {}", response.result_code, response.result_text);
        try {
            auto json = nlohmann::json::parse(response.result_text);
            LOG_INFO("Created post with ID: {}", json.value("id", 0));
        } catch (const std::exception& e) {
            LOG_ERROR("JSON parse error: {}", e.what());
        }
    }
    else {
        LOG_ERROR("POST failed with status {}: {}", response.result_code, response.result_text);
    }
}

// Example coroutine that demonstrates awaitable HTTP PUT request
boost::asio::awaitable<void> example_put()
{
    LOG_INFO("Starting awaitable PUT request...");

    nlohmann::json put_data = {
        {"id", 1},
        {"title", "Updated via Awaitable PUT"},
        {"body", "Updated content using async/await"},
        {"userId", 1}
    };

    auto response = co_await Http::async_put(
        "https://jsonplaceholder.typicode.com/posts/1",
        put_data.dump(),
        {{"Content-Type", "application/json"}}
    );

    if (response.is_ok()) {
        LOG_INFO("PUT response: {}", response.result_text);
    }
    else {
        LOG_ERROR("PUT failed with status {}: {}", response.result_code, response.result_text);
    }
}

// Example coroutine that demonstrates awaitable HTTP PATCH request
boost::asio::awaitable<void> example_patch()
{
    LOG_INFO("Starting awaitable PATCH request...");

    nlohmann::json patch_data = {
        {"title", "Patched Title via Awaitable"}
    };

    auto response = co_await Http::async_patch(
        "https://jsonplaceholder.typicode.com/posts/1",
        patch_data.dump(),
        {{"Content-Type", "application/json"}}
    );

    if (response.is_ok()) {
        LOG_INFO("PATCH response: {}", response.result_text);
    }
    else {
        LOG_ERROR("PATCH failed with status {}: {}", response.result_code, response.result_text);
    }
}

// Example coroutine that demonstrates awaitable HTTP DELETE request
boost::asio::awaitable<void> example_delete()
{
    LOG_INFO("Starting awaitable DELETE request...");

    auto response = co_await Http::async_del("https://jsonplaceholder.typicode.com/posts/1");

    if (response.is_ok()) {
        LOG_INFO("DELETE succeeded with status {}", response.result_code);
    }
    else {
        LOG_ERROR("DELETE failed with status {}: {}", response.result_code, response.result_text);
    }
}

// Example coroutine that demonstrates multiple sequential awaitable requests
boost::asio::awaitable<void> example_sequential_requests()
{
    LOG_INFO("Starting sequential awaitable requests...");

    // GET user 1
    auto user_response = co_await Http::async_get("https://jsonplaceholder.typicode.com/users/1");
    if (!user_response.is_ok()) {
        LOG_ERROR("Failed to get user");
        co_return;
    }

    LOG_INFO("Got user: {}", user_response.result_text);

    // GET posts for user 1
    auto posts_response = co_await Http::async_get("https://jsonplaceholder.typicode.com/posts?userId=1");
    if (!posts_response.is_ok()) {
        LOG_ERROR("Failed to get posts");
        co_return;
    }

    try {
        auto posts = nlohmann::json::parse(posts_response.result_text);
        LOG_INFO("User has {} posts", posts.size());
    } catch (const std::exception& e) {
        LOG_ERROR("JSON parse error: {}", e.what());
    }
}

#if 0 // Requires boost::asio::experimental features - not available in standard Boost.Asio
// Example coroutine that demonstrates parallel awaitable requests
boost::asio::awaitable<void> example_parallel_requests()
{
    LOG_INFO("Starting parallel awaitable requests...");

    auto executor = co_await boost::asio::this_coro::executor;

    // Launch multiple requests in parallel
    auto fut1 = boost::asio::co_spawn(executor,
        Http::async_get("https://jsonplaceholder.typicode.com/posts/1"),
        boost::asio::use_awaitable);

    auto fut2 = boost::asio::co_spawn(executor,
        Http::async_get("https://jsonplaceholder.typicode.com/posts/2"),
        boost::asio::use_awaitable);

    auto fut3 = boost::asio::co_spawn(executor,
        Http::async_get("https://jsonplaceholder.typicode.com/posts/3"),
        boost::asio::use_awaitable);

    // Wait for all to complete
    auto [resp1, resp2, resp3] = co_await boost::asio::experimental::make_parallel_group(
        std::move(fut1), std::move(fut2), std::move(fut3)
    ).async_wait(
        boost::asio::experimental::wait_for_all(),
        boost::asio::use_awaitable
    );

    LOG_INFO("Parallel request 1: status {}", std::get<0>(resp1).result_code);
    LOG_INFO("Parallel request 2: status {}", std::get<0>(resp2).result_code);
    LOG_INFO("Parallel request 3: status {}", std::get<0>(resp3).result_code);
}
#endif

// Example coroutine that demonstrates error handling
boost::asio::awaitable<void> example_error_handling()
{
    LOG_INFO("Starting error handling example...");

    try {
        // This should fail with 404
        auto response = co_await Http::async_get("https://jsonplaceholder.typicode.com/posts/999999");

        if (!response.is_ok()) {
            LOG_WARN("Expected error received: status {}", response.result_code);
        }

        // Check specific status codes
        if (response.result_code == 404) {
            LOG_INFO("Resource not found (404)");
        }

    } catch (const std::exception& e) {
        LOG_ERROR("Exception caught: {}", e.what());
    }
}

// Main coordinator coroutine
boost::asio::awaitable<void> run_all_examples()
{
    try {
        co_await example_get();
        co_await example_get_with_headers();
        co_await example_post();
        co_await example_put();
        co_await example_patch();
        co_await example_delete();
        co_await example_sequential_requests();
        co_await example_error_handling();

        // Note: parallel requests require asio::experimental
        // Uncomment if you have experimental features enabled
        // co_await example_parallel_requests();

        LOG_INFO("All examples completed!");
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal error: {}", e.what());
    }
}

int main()
{
    // Initialize logger
    logger.add_console_sink(true, true);
    logger.set_level(slick::logger::LogLevel::L_DEBUG);
    logger.init(1024);
    configure_slick_net_logging();

    LOG_INFO("HTTP Awaitable Client Example");
    LOG_INFO("================================");

    // Create io_context and run the examples
    boost::asio::io_context ioc;

    boost::asio::co_spawn(
        ioc,
        run_all_examples(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    LOG_ERROR("Coroutine exception: {}", ex.what());
                }
            }
        }
    );

    LOG_INFO("Running io_context...");
    ioc.run();
    LOG_INFO("io_context finished");
    shutdown_slick_net_logging();

    return 0;
}
