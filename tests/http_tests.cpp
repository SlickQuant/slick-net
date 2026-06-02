#include <gtest/gtest.h>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <nlohmann/json.hpp>
#include <slick/net/http_stream.hpp>

// Define logging macros for debugging if needed
// #define LOG_DEBUG(fmt, ...) std::cout << std::format("{:%Y-%m-%d %H:%M:%S} ", std::chrono::system_clock::now()) << "[DEBUG] " << std::format(fmt, __VA_ARGS__) << std::endl
// #define LOG_INFO(fmt, ...) std::cout << std::format("{:%Y-%m-%d %H:%M:%S} ", std::chrono::system_clock::now()) << "[INFO] " << std::format(fmt, __VA_ARGS__) << std::endl
// #define LOG_ERROR(fmt, ...) std::cout << std::format("{:%Y-%m-%d %H:%M:%S} ", std::chrono::system_clock::now()) << "[ERROR] " << std::format(fmt, __VA_ARGS__) << std::endl

#include <slick/net/http.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>

namespace slick::net {

// Test fixture for HTTP tests
class HttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code if needed
    }

    void TearDown() override {
        // Cleanup code if needed
    }

    // Helper to wait for async operations
    template<typename Predicate>
    bool wait_for_condition(Predicate pred, std::chrono::milliseconds timeout) {
        auto start = std::chrono::high_resolution_clock::now();
        while (!pred() &&
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::high_resolution_clock::now() - start) < timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return pred();
    }
};

// ======================== Synchronous GET Tests ========================

TEST_F(HttpTest, SyncGet_BasicRequest) {
    auto response = Http::get("https://jsonplaceholder.typicode.com/posts/1");

    EXPECT_TRUE(response.is_ok()) << "Status: " << response.result_code << ", Response: " << response.result_text;
    EXPECT_EQ(response.result_code, 200);
    EXPECT_FALSE(response.result_text.empty());

    // Verify it's valid JSON
    EXPECT_NO_THROW({
        auto json = nlohmann::json::parse(response.result_text);
        EXPECT_TRUE(json.contains("userId"));
        EXPECT_TRUE(json.contains("id"));
        EXPECT_TRUE(json.contains("title"));
        EXPECT_TRUE(json.contains("body"));
    });
}

TEST_F(HttpTest, SyncGet_PlainHttp) {
    // Test plain HTTP (non-SSL) connection
    auto response = Http::get("http://httpbun.com/get");

    EXPECT_TRUE(response.is_ok()) << "Status: " << response.result_code << ", Response: " << response.result_text;
    EXPECT_EQ(response.result_code, 200);
    EXPECT_FALSE(response.result_text.empty());

    // Verify it's valid JSON
    EXPECT_NO_THROW({
        auto json = nlohmann::json::parse(response.result_text);
        EXPECT_TRUE(json.contains("url"));
    });
}

TEST_F(HttpTest, SyncGet_WithCustomHeaders) {
    auto response = Http::get("https://jsonplaceholder.typicode.com/posts/1", {
        {"X-Custom-Header", "test-value"},
        {"Accept", "application/json"}
    });

    EXPECT_TRUE(response.is_ok()) << "Status: " << response.result_code << ", Response: " << response.result_text;
    EXPECT_EQ(response.result_code, 200);

    // Verify response is valid JSON
    auto json = nlohmann::json::parse(response.result_text);
    EXPECT_TRUE(json.contains("id"));
}

TEST_F(HttpTest, SyncGet_404NotFound) {
    auto response = Http::get("https://jsonplaceholder.typicode.com/posts/999999");

    EXPECT_FALSE(response.is_ok()) << "Expected 404, got: " << response.result_code;
    EXPECT_EQ(response.result_code, 404);
}

TEST_F(HttpTest, SyncGet_500ServerError) {
    auto response = Http::get("https://mockhttp.org/status/500");

    EXPECT_FALSE(response.is_ok());
    EXPECT_EQ(response.result_code, 500);
}

// ======================== Synchronous POST Tests ========================

TEST_F(HttpTest, SyncPost_JsonData) {
    nlohmann::json post_data = {
        {"title", "Test Post"},
        {"body", "This is a test post"},
        {"userId", 1}
    };

    auto response = Http::post("https://jsonplaceholder.typicode.com/posts",
                               post_data.dump(),
                               {{"Content-Type", "application/json"}});

    EXPECT_TRUE(response.is_ok()) << "Status: " << response.result_code << ", Response: " << response.result_text;
    EXPECT_EQ(response.result_code, 201); // Created

    // Verify the response
    auto json = nlohmann::json::parse(response.result_text);
    EXPECT_TRUE(json.contains("id"));
    EXPECT_EQ(json["title"], "Test Post");
    EXPECT_EQ(json["body"], "This is a test post");
    EXPECT_EQ(json["userId"], 1);
}

TEST_F(HttpTest, SyncPost_PlainHttp) {
    // Test plain HTTP (non-SSL) POST request
    nlohmann::json post_data = {
        {"test", "plain http post"},
        {"value", 42}
    };

    auto response = Http::post("http://httpbun.com/post",
                               post_data.dump(),
                               {{"Content-Type", "application/json"}});

    EXPECT_TRUE(response.is_ok()) << "Status: " << response.result_code << ", Response: " << response.result_text;
    EXPECT_EQ(response.result_code, 200);

    auto json = nlohmann::json::parse(response.result_text);
    EXPECT_TRUE(json.contains("json"));
    EXPECT_EQ(json["json"]["test"], "plain http post");
    EXPECT_EQ(json["json"]["value"], 42);
}

TEST_F(HttpTest, SyncPost_EmptyBody) {
    nlohmann::json empty_data = nlohmann::json::object();
    auto response = Http::post("https://jsonplaceholder.typicode.com/posts",
                               empty_data.dump(),
                               {{"Content-Type", "application/json"}});

    EXPECT_TRUE(response.is_ok()) << "Status: " << response.result_code;
    EXPECT_EQ(response.result_code, 201);
}

TEST_F(HttpTest, SyncPost_PlainText) {
    std::string text_data = "Hello, this is plain text data";
    auto response = Http::post("https://httpbun.com/post",
                               text_data,
                               {{"Content-Type", "text/plain"}});

    EXPECT_TRUE(response.is_ok()) << "Status: " << response.result_code;
    EXPECT_EQ(response.result_code, 200);

    auto json = nlohmann::json::parse(response.result_text);
    EXPECT_EQ(json["data"], text_data);
}

// ======================== Synchronous PUT Tests ========================

TEST_F(HttpTest, SyncPut_UpdateResource) {
    nlohmann::json put_data = {
        {"id", 1},
        {"title", "Updated Title"},
        {"body", "Updated body content"},
        {"userId", 1}
    };

    auto response = Http::put("https://jsonplaceholder.typicode.com/posts/1",
                              put_data.dump(),
                              {{"Content-Type", "application/json"}});

    EXPECT_TRUE(response.is_ok()) << "Status: " << response.result_code << ", Response: " << response.result_text;
    EXPECT_EQ(response.result_code, 200);

    auto json = nlohmann::json::parse(response.result_text);
    EXPECT_EQ(json["id"], 1);
    EXPECT_EQ(json["title"], "Updated Title");
}

TEST_F(HttpTest, SyncPut_PlainHttp) {
    // Test plain HTTP (non-SSL) PUT request
    nlohmann::json put_data = {
        {"updated", true},
        {"timestamp", 1234567890}
    };

    auto response = Http::put("http://httpbun.com/put",
                              put_data.dump(),
                              {{"Content-Type", "application/json"}});

    EXPECT_TRUE(response.is_ok()) << "Status: " << response.result_code << ", Response: " << response.result_text;
    EXPECT_EQ(response.result_code, 200);

    auto json = nlohmann::json::parse(response.result_text);
    EXPECT_TRUE(json.contains("json"));
    EXPECT_EQ(json["json"]["updated"], true);
}

// ======================== Synchronous PATCH Tests ========================

TEST_F(HttpTest, SyncPatch_PartialUpdate) {
    nlohmann::json patch_data = {
        {"title", "Patched Title"}
    };

    auto response = Http::patch("https://jsonplaceholder.typicode.com/posts/1",
                                patch_data.dump(),
                                {{"Content-Type", "application/json"}});

    EXPECT_TRUE(response.is_ok()) << "Status: " << response.result_code << ", Response: " << response.result_text;
    EXPECT_EQ(response.result_code, 200);

    auto json = nlohmann::json::parse(response.result_text);
    EXPECT_EQ(json["title"], "Patched Title");
    EXPECT_EQ(json["id"], 1);
}

// ======================== Synchronous DELETE Tests ========================

TEST_F(HttpTest, SyncDelete_BasicRequest) {
    auto response = Http::del("https://jsonplaceholder.typicode.com/posts/1", "");

    EXPECT_TRUE(response.is_ok()) << "Status: " << response.result_code << ", Response: " << response.result_text;
    EXPECT_EQ(response.result_code, 200);
}

TEST_F(HttpTest, SyncDelete_WithBody) {
    nlohmann::json delete_data = {
        {"reason", "No longer needed"}
    };

    auto response = Http::del("https://httpbun.com/delete",
                              delete_data.dump(),
                              {{"Content-Type", "application/json"}});

    EXPECT_TRUE(response.is_ok()) << "Status: " << response.result_code << ", Response: " << response.result_text;
    EXPECT_EQ(response.result_code, 200);
    EXPECT_FALSE(response.result_text.empty());

    EXPECT_NO_THROW({
        auto json = nlohmann::json::parse(response.result_text);
        EXPECT_TRUE(json.contains("json") || json.contains("data"));
    });
}

// ======================== Asynchronous GET Tests ========================

TEST_F(HttpTest, AsyncGet_BasicRequest) {
    std::atomic<bool> completed{false};
    Http::Response async_response;

    Http::async_get([&](Http::Response&& response) {
        async_response = std::move(response);
        completed.store(true);
    }, "https://jsonplaceholder.typicode.com/posts/1");

    EXPECT_TRUE(wait_for_condition([&]() { return completed.load(); },
                                    std::chrono::seconds(10)));

    EXPECT_TRUE(async_response.is_ok()) << "Status: " << async_response.result_code << ", Response: " << async_response.result_text;
    EXPECT_EQ(async_response.result_code, 200);
    EXPECT_FALSE(async_response.result_text.empty());

    // Verify it's valid JSON
    EXPECT_NO_THROW({
        auto json = nlohmann::json::parse(async_response.result_text);
        EXPECT_TRUE(json.contains("userId"));
        EXPECT_TRUE(json.contains("id"));
        EXPECT_TRUE(json.contains("title"));
        EXPECT_TRUE(json.contains("body"));
    });
}

TEST_F(HttpTest, AsyncGet_PlainHttp) {
    // Test plain HTTP (non-SSL) async GET request
    std::atomic<bool> completed{false};
    Http::Response async_response;

    Http::async_get([&](Http::Response&& response) {
        async_response = std::move(response);
        completed.store(true);
    }, "http://httpbun.com/get");

    EXPECT_TRUE(wait_for_condition([&]() { return completed.load(); },
                                    std::chrono::seconds(10)));

    EXPECT_TRUE(async_response.is_ok()) << "Status: " << async_response.result_code;
    EXPECT_EQ(async_response.result_code, 200);

    auto json = nlohmann::json::parse(async_response.result_text);
    EXPECT_TRUE(json.contains("url"));
}

TEST_F(HttpTest, AsyncGet_WithHeaders) {
    std::atomic<bool> completed{false};
    Http::Response async_response;

    Http::async_get([&](Http::Response&& response) {
        async_response = std::move(response);
        completed.store(true);
    }, "https://jsonplaceholder.typicode.com/posts/2", {{"Accept", "application/json"}});

    EXPECT_TRUE(wait_for_condition([&]() { return completed.load(); },
                                    std::chrono::seconds(10)));

    EXPECT_TRUE(async_response.is_ok()) << "Status: " << async_response.result_code;
    auto json = nlohmann::json::parse(async_response.result_text);
    EXPECT_EQ(json["id"], 2);
}

// ======================== Asynchronous POST Tests ========================

TEST_F(HttpTest, AsyncPost_JsonData) {
    std::atomic<bool> completed{false};
    Http::Response async_response;

    nlohmann::json post_data = {
        {"title", "Async POST test"},
        {"body", "Testing async POST"},
        {"userId", 1}
    };

    Http::async_post([&](Http::Response&& response) {
        async_response = std::move(response);
        completed.store(true);
    }, "https://jsonplaceholder.typicode.com/posts",
       post_data.dump(),
       {{"Content-Type", "application/json"}});

    EXPECT_TRUE(wait_for_condition([&]() { return completed.load(); },
                                    std::chrono::seconds(10)));

    EXPECT_TRUE(async_response.is_ok()) << "Status: " << async_response.result_code;
    EXPECT_EQ(async_response.result_code, 201);
    auto json = nlohmann::json::parse(async_response.result_text);
    EXPECT_EQ(json["title"], "Async POST test");
}

// ======================== Asynchronous PUT Tests ========================

TEST_F(HttpTest, AsyncPut_UpdateResource) {
    std::atomic<bool> completed{false};
    Http::Response async_response;

    nlohmann::json put_data = {
        {"id", 1},
        {"title", "Async PUT test"},
        {"body", "Testing async PUT"},
        {"userId", 1}
    };

    Http::async_put([&](Http::Response&& response) {
        async_response = std::move(response);
        completed.store(true);
    }, "https://jsonplaceholder.typicode.com/posts/1",
       put_data.dump(),
       {{"Content-Type", "application/json"}});

    EXPECT_TRUE(wait_for_condition([&]() { return completed.load(); },
                                    std::chrono::seconds(10)));

    EXPECT_TRUE(async_response.is_ok()) << "Status: " << async_response.result_code;
    auto json = nlohmann::json::parse(async_response.result_text);
    EXPECT_EQ(json["title"], "Async PUT test");
}

// ======================== Asynchronous PATCH Tests ========================

TEST_F(HttpTest, AsyncPatch_PartialUpdate) {
    std::atomic<bool> completed{false};
    Http::Response async_response;

    nlohmann::json patch_data = {{"title", "Async PATCH test"}};

    Http::async_patch([&](Http::Response&& response) {
        async_response = std::move(response);
        completed.store(true);
    }, "https://jsonplaceholder.typicode.com/posts/1",
       patch_data.dump(),
       {{"Content-Type", "application/json"}});

    EXPECT_TRUE(wait_for_condition([&]() { return completed.load(); },
                                    std::chrono::seconds(10)));

    EXPECT_TRUE(async_response.is_ok()) << "Status: " << async_response.result_code;
    auto json = nlohmann::json::parse(async_response.result_text);
    EXPECT_EQ(json["title"], "Async PATCH test");
}

// ======================== Asynchronous DELETE Tests ========================

TEST_F(HttpTest, AsyncDelete_BasicRequest) {
    std::atomic<bool> completed{false};
    Http::Response async_response;

    nlohmann::json delete_data = {{"force", true}};

    Http::async_del([&](Http::Response&& response) {
        async_response = std::move(response);
        completed.store(true);
    }, "https://jsonplaceholder.typicode.com/posts/1");

    EXPECT_TRUE(wait_for_condition([&]() { return completed.load(); },
                                    std::chrono::seconds(10)));

    EXPECT_TRUE(async_response.is_ok()) << "Status: " << async_response.result_code;
    EXPECT_EQ(async_response.result_code, 200);
}

// ======================== Multiple Concurrent Async Requests ========================

TEST_F(HttpTest, AsyncMultipleRequests) {
    std::atomic<int> completed{0};
    const int num_requests = 5;

    for (int i = 1; i <= num_requests; ++i) {
        std::string url = "https://jsonplaceholder.typicode.com/posts/" + std::to_string(i);
        Http::async_get([&](Http::Response&& response) {
            EXPECT_TRUE(response.is_ok()) << "Status: " << response.result_code;
            completed.fetch_add(1);
        }, url);
    }

    EXPECT_TRUE(wait_for_condition([&]() { return completed.load() == num_requests; },
                                    std::chrono::seconds(30)));

    EXPECT_EQ(completed.load(), num_requests);
}

// ======================== Mixed Async Operations ========================

TEST_F(HttpTest, AsyncMixedOperations) {
    std::atomic<int> completed{0};

    // GET
    Http::async_get([&](Http::Response&& response) {
        EXPECT_TRUE(response.is_ok());
        completed.fetch_add(1);
    }, "https://mockhttp.org/get");

    // POST
    nlohmann::json post_data = {{"test", "post"}};
    Http::async_post([&](Http::Response&& response) {
        EXPECT_TRUE(response.is_ok());
        completed.fetch_add(1);
    }, "https://mockhttp.org/post", post_data.dump(), {{"Content-Type", "application/json"}});

    // PUT
    nlohmann::json put_data = {{"test", "put"}};
    Http::async_put([&](Http::Response&& response) {
        EXPECT_TRUE(response.is_ok());
        completed.fetch_add(1);
    }, "https://mockhttp.org/put", put_data.dump(), {{"Content-Type", "application/json"}});

    // PATCH
    nlohmann::json patch_data = {{"test", "patch"}};
    Http::async_patch([&](Http::Response&& response) {
        EXPECT_TRUE(response.is_ok());
        completed.fetch_add(1);
    }, "https://mockhttp.org/patch", patch_data.dump(), {{"Content-Type", "application/json"}});

    // DELETE
    Http::async_del([&](Http::Response&& response) {
        EXPECT_TRUE(response.is_ok());
        completed.fetch_add(1);
    }, "https://mockhttp.org/delete");

    EXPECT_TRUE(wait_for_condition([&]() { return completed.load() == 5; },
                                    std::chrono::seconds(30)));

    EXPECT_EQ(completed.load(), 5);
}

// ======================== Error Handling Tests ========================

TEST_F(HttpTest, InvalidHostname) {
    // This test may take a while to timeout
    auto response = Http::get("https://invalid-hostname-that-does-not-exist-12345.com");

    EXPECT_FALSE(response.is_ok());
    EXPECT_NE(response.result_code, 200);
}

TEST_F(HttpTest, ResponseStructure_IsOk) {
    Http::Response response_ok{200, "Success"};
    EXPECT_TRUE(response_ok.is_ok());

    Http::Response response_created{201, "Created"};
    EXPECT_TRUE(response_created.is_ok());

    Http::Response response_accepted{202, "Accepted"};
    EXPECT_TRUE(response_accepted.is_ok());

    Http::Response response_no_content{204, ""};
    EXPECT_TRUE(response_no_content.is_ok());

    Http::Response response_moved{301, "Moved"};
    EXPECT_FALSE(response_moved.is_ok());

    Http::Response response_bad_request{400, "Bad Request"};
    EXPECT_FALSE(response_bad_request.is_ok());

    Http::Response response_not_found{404, "Not Found"};
    EXPECT_FALSE(response_not_found.is_ok());

    Http::Response response_server_error{500, "Internal Server Error"};
    EXPECT_FALSE(response_server_error.is_ok());
}

// ======================== HttpStream Tests ========================

TEST_F(HttpTest, HttpStream_BasicConnection) {
    std::atomic<bool> connected{false};
    std::atomic<bool> disconnected{false};
    std::atomic<int> data_received_count{0};
    std::atomic<bool> error_occurred{false};
    std::string last_error;

    auto stream = std::make_shared<HttpStream>(
        "https://stream.wikimedia.org/v2/stream/recentchange",
        [&]() {
            connected.store(true);
        },
        [&]() {
            disconnected.store(true);
        },
        [&](const char* data, size_t size) {
            data_received_count++;
        },
        [&](std::string err) {
            error_occurred.store(true);
            last_error = err;
        }
    );

    stream->open();

    // Wait for connection
    EXPECT_TRUE(wait_for_condition([&]() { return connected.load(); },
                                    std::chrono::seconds(10)));

    // Wait for at least one data event (30s: macOS CI runners are slower to receive the first SSE chunk)
    EXPECT_TRUE(wait_for_condition([&]() { return data_received_count.load() > 0; },
                                    std::chrono::seconds(30)));

    // Close the stream
    stream->close();

    // Wait for disconnection
    EXPECT_TRUE(wait_for_condition([&]() { return disconnected.load(); },
                                    std::chrono::seconds(5)));

    EXPECT_GT(data_received_count.load(), 0);
}

TEST_F(HttpTest, HttpStream_CustomHeaders) {
    std::atomic<bool> connected{false};
    std::atomic<bool> disconnected{false};
    std::atomic<int> data_count{0};

    std::vector<std::pair<std::string, std::string>> headers = {{"X-Custom-Header", "test"}};
    auto stream = std::make_shared<HttpStream>(
        "https://stream.wikimedia.org/v2/stream/recentchange",
        [&]() { connected.store(true); },
        [&]() { disconnected.store(true); },
        [&](const char* data, size_t size) {
            data_count++;
        },
        [](std::string err) {
            // Error handler
        },
        std::move(headers)
    );

    stream->open();

    EXPECT_TRUE(wait_for_condition([&]() { return connected.load(); },
                                    std::chrono::seconds(10)));

    // Let it receive some data
    std::this_thread::sleep_for(std::chrono::seconds(2));

    stream->close();

    EXPECT_TRUE(wait_for_condition([&]() { return disconnected.load(); },
                                    std::chrono::seconds(5)));
}

TEST_F(HttpTest, HttpStream_InvalidUrl) {
    std::atomic<bool> error_occurred{false};
    std::atomic<bool> disconnected{false};
    std::string error_message;

    auto stream = std::make_shared<HttpStream>(
        "https://invalid-host-that-does-not-exist-12345.com/stream",
        []() {},
        [&]() { disconnected.store(true); },
        [](const char*, size_t) {},
        [&](std::string err) {
            error_occurred.store(true);
            error_message = err;
        }
    );

    stream->open();

    // Should get an error
    EXPECT_TRUE(wait_for_condition([&]() { return error_occurred.load(); },
                                    std::chrono::seconds(15)));

    EXPECT_TRUE(error_occurred.load());
    EXPECT_FALSE(error_message.empty());

    stream->close();
}

TEST_F(HttpTest, HttpStream_MultipleStreams) {
    std::atomic<int> connected_count{0};
    std::atomic<int> disconnected_count{0};
    std::atomic<int> data_count{0};

    std::vector<std::shared_ptr<HttpStream>> streams;

    // Create 3 concurrent streams
    for (int i = 0; i < 3; ++i) {
        auto stream = std::make_shared<HttpStream>(
            "https://stream.wikimedia.org/v2/stream/recentchange",
            [&]() { connected_count++; },
            [&]() { disconnected_count++; },
            [&](const char*, size_t) { data_count++; },
            [](std::string) {}
        );
        stream->open();
        streams.push_back(stream);
    }

    // Wait for all to connect
    EXPECT_TRUE(wait_for_condition([&]() { return connected_count.load() == 3; },
                                    std::chrono::seconds(15)));

    // Wait for some data
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Close all streams
    for (auto& stream : streams) {
        stream->close();
    }

    // Wait for all to disconnect
    EXPECT_TRUE(wait_for_condition([&]() { return disconnected_count.load() == 3; },
                                    std::chrono::seconds(10)));

    EXPECT_GT(data_count.load(), 0);
}

TEST_F(HttpTest, HttpStream_StatusCheck) {
    std::atomic<bool> connected{false};
    std::atomic<bool> disconnected{false};

    auto stream = std::make_shared<HttpStream>(
        "https://stream.wikimedia.org/v2/stream/recentchange",
        [&]() { connected.store(true); },
        [&]() { disconnected.store(true); },
        [](const char*, size_t) {},
        [](std::string) {}
    );

    // Initially disconnected
    EXPECT_EQ(stream->status(), HttpStream::Status::DISCONNECTED);

    stream->open();

    // Wait for connection
    EXPECT_TRUE(wait_for_condition([&]() { return connected.load(); },
                                    std::chrono::seconds(10)));

    // Should be connected now
    EXPECT_EQ(stream->status(), HttpStream::Status::CONNECTED);

    stream->close();

    // Wait for disconnection
    EXPECT_TRUE(wait_for_condition([&]() { return disconnected.load(); },
                                    std::chrono::seconds(5)));

    EXPECT_EQ(stream->status(), HttpStream::Status::DISCONNECTED);
}

// ======================== Awaitable GET Tests ========================
// Note: GCC 13 has an internal compiler error with coroutine lambdas
// See: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=103868
#if !defined(__GNUC__) || __GNUC__ >= 14 || defined(__clang__)

TEST_F(HttpTest, AwaitableGet_BasicRequest) {
    boost::asio::io_context ioc;
    Http::Response awaitable_response;
    std::atomic<bool> completed{false};

    auto test_coro = [&]() -> boost::asio::awaitable<void> {
        awaitable_response = co_await Http::async_get("https://jsonplaceholder.typicode.com/posts/1");
        completed.store(true);
    };

    boost::asio::co_spawn(
        ioc,
        test_coro(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    FAIL() << "Exception in coroutine: " << ex.what();
                }
            }
        }
    );

    ioc.run();

    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(awaitable_response.is_ok()) << "Status: " << awaitable_response.result_code << ", Response: " << awaitable_response.result_text;
    EXPECT_EQ(awaitable_response.result_code, 200);
    EXPECT_FALSE(awaitable_response.result_text.empty());

    // Verify it's valid JSON
    EXPECT_NO_THROW({
        auto json = nlohmann::json::parse(awaitable_response.result_text);
        EXPECT_TRUE(json.contains("userId"));
        EXPECT_TRUE(json.contains("id"));
        EXPECT_TRUE(json.contains("title"));
        EXPECT_TRUE(json.contains("body"));
    });
}

TEST_F(HttpTest, AwaitableGet_PlainHttp) {
    boost::asio::io_context ioc;
    Http::Response awaitable_response;
    std::atomic<bool> completed{false};

    auto test_coro = [&]() -> boost::asio::awaitable<void> {
        awaitable_response = co_await Http::async_get("http://httpbun.com/get");
        completed.store(true);
    };

    boost::asio::co_spawn(
        ioc,
        test_coro(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    FAIL() << "Exception in coroutine: " << ex.what();
                }
            }
        }
    );

    ioc.run();

    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(awaitable_response.is_ok()) << "Status: " << awaitable_response.result_code;
    EXPECT_EQ(awaitable_response.result_code, 200);

    auto json = nlohmann::json::parse(awaitable_response.result_text);
    EXPECT_TRUE(json.contains("url"));
}

TEST_F(HttpTest, AwaitableGet_WithCustomHeaders) {
    boost::asio::io_context ioc;
    Http::Response awaitable_response;
    std::atomic<bool> completed{false};

    auto test_coro = [&]() -> boost::asio::awaitable<void> {
        awaitable_response = co_await Http::async_get(
            "https://jsonplaceholder.typicode.com/posts/1",
            {{"X-Custom-Header", "test-value"}, {"Accept", "application/json"}}
        );
        completed.store(true);
    };

    boost::asio::co_spawn(
        ioc,
        test_coro(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    FAIL() << "Exception in coroutine: " << ex.what();
                }
            }
        }
    );

    ioc.run();

    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(awaitable_response.is_ok()) << "Status: " << awaitable_response.result_code;
    EXPECT_EQ(awaitable_response.result_code, 200);

    auto json = nlohmann::json::parse(awaitable_response.result_text);
    EXPECT_TRUE(json.contains("id"));
}

TEST_F(HttpTest, AwaitableGet_404NotFound) {
    boost::asio::io_context ioc;
    Http::Response awaitable_response;
    std::atomic<bool> completed{false};

    auto test_coro = [&]() -> boost::asio::awaitable<void> {
        awaitable_response = co_await Http::async_get("https://jsonplaceholder.typicode.com/posts/999999");
        completed.store(true);
    };

    boost::asio::co_spawn(
        ioc,
        test_coro(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    FAIL() << "Exception in coroutine: " << ex.what();
                }
            }
        }
    );

    ioc.run();

    EXPECT_TRUE(completed.load());
    EXPECT_FALSE(awaitable_response.is_ok()) << "Expected 404, got: " << awaitable_response.result_code;
    EXPECT_EQ(awaitable_response.result_code, 404);
}

// ======================== Awaitable POST Tests ========================

TEST_F(HttpTest, AwaitablePost_JsonData) {
    boost::asio::io_context ioc;
    Http::Response awaitable_response;
    std::atomic<bool> completed{false};

    nlohmann::json post_data = {
        {"title", "Awaitable POST test"},
        {"body", "Testing awaitable POST"},
        {"userId", 1}
    };

    auto test_coro = [&]() -> boost::asio::awaitable<void> {
        awaitable_response = co_await Http::async_post(
            "https://jsonplaceholder.typicode.com/posts",
            post_data.dump(),
            {{"Content-Type", "application/json"}}
        );
        completed.store(true);
    };

    boost::asio::co_spawn(
        ioc,
        test_coro(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    FAIL() << "Exception in coroutine: " << ex.what();
                }
            }
        }
    );

    ioc.run();

    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(awaitable_response.is_ok()) << "Status: " << awaitable_response.result_code;
    EXPECT_EQ(awaitable_response.result_code, 201);

    auto json = nlohmann::json::parse(awaitable_response.result_text);
    EXPECT_TRUE(json.contains("id"));
    EXPECT_EQ(json["title"], "Awaitable POST test");
    EXPECT_EQ(json["body"], "Testing awaitable POST");
}

TEST_F(HttpTest, AwaitablePost_PlainHttp) {
    boost::asio::io_context ioc;
    Http::Response awaitable_response;
    std::atomic<bool> completed{false};

    nlohmann::json post_data = {
        {"test", "awaitable plain http post"},
        {"value", 42}
    };

    auto test_coro = [&]() -> boost::asio::awaitable<void> {
        awaitable_response = co_await Http::async_post(
            "http://httpbun.com/post",
            post_data.dump(),
            {{"Content-Type", "application/json"}}
        );
        completed.store(true);
    };

    boost::asio::co_spawn(
        ioc,
        test_coro(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    FAIL() << "Exception in coroutine: " << ex.what();
                }
            }
        }
    );

    ioc.run();

    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(awaitable_response.is_ok()) << "Status: " << awaitable_response.result_code;
    EXPECT_EQ(awaitable_response.result_code, 200);

    auto json = nlohmann::json::parse(awaitable_response.result_text);
    EXPECT_TRUE(json.contains("json"));
    EXPECT_EQ(json["json"]["test"], "awaitable plain http post");
}

// ======================== Awaitable PUT Tests ========================

TEST_F(HttpTest, AwaitablePut_UpdateResource) {
    boost::asio::io_context ioc;
    Http::Response awaitable_response;
    std::atomic<bool> completed{false};

    nlohmann::json put_data = {
        {"id", 1},
        {"title", "Awaitable PUT test"},
        {"body", "Testing awaitable PUT"},
        {"userId", 1}
    };

    auto test_coro = [&]() -> boost::asio::awaitable<void> {
        awaitable_response = co_await Http::async_put(
            "https://jsonplaceholder.typicode.com/posts/1",
            put_data.dump(),
            {{"Content-Type", "application/json"}}
        );
        completed.store(true);
    };

    boost::asio::co_spawn(
        ioc,
        test_coro(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    FAIL() << "Exception in coroutine: " << ex.what();
                }
            }
        }
    );

    ioc.run();

    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(awaitable_response.is_ok()) << "Status: " << awaitable_response.result_code;
    EXPECT_EQ(awaitable_response.result_code, 200);

    auto json = nlohmann::json::parse(awaitable_response.result_text);
    EXPECT_EQ(json["title"], "Awaitable PUT test");
}

TEST_F(HttpTest, AwaitablePut_PlainHttp) {
    boost::asio::io_context ioc;
    Http::Response awaitable_response;
    std::atomic<bool> completed{false};

    nlohmann::json put_data = {
        {"updated", true},
        {"timestamp", 1234567890}
    };

    auto test_coro = [&]() -> boost::asio::awaitable<void> {
        awaitable_response = co_await Http::async_put(
            "http://httpbun.com/put",
            put_data.dump(),
            {{"Content-Type", "application/json"}}
        );
        completed.store(true);
    };

    boost::asio::co_spawn(
        ioc,
        test_coro(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    FAIL() << "Exception in coroutine: " << ex.what();
                }
            }
        }
    );

    ioc.run();

    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(awaitable_response.is_ok()) << "Status: " << awaitable_response.result_code;
    EXPECT_EQ(awaitable_response.result_code, 200);

    auto json = nlohmann::json::parse(awaitable_response.result_text);
    EXPECT_TRUE(json.contains("json"));
    EXPECT_EQ(json["json"]["updated"], true);
}

// ======================== Awaitable PATCH Tests ========================

TEST_F(HttpTest, AwaitablePatch_PartialUpdate) {
    boost::asio::io_context ioc;
    Http::Response awaitable_response;
    std::atomic<bool> completed{false};

    nlohmann::json patch_data = {
        {"title", "Awaitable PATCH test"}
    };

    auto test_coro = [&]() -> boost::asio::awaitable<void> {
        awaitable_response = co_await Http::async_patch(
            "https://jsonplaceholder.typicode.com/posts/1",
            patch_data.dump(),
            {{"Content-Type", "application/json"}}
        );
        completed.store(true);
    };

    boost::asio::co_spawn(
        ioc,
        test_coro(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    FAIL() << "Exception in coroutine: " << ex.what();
                }
            }
        }
    );

    ioc.run();

    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(awaitable_response.is_ok()) << "Status: " << awaitable_response.result_code;
    EXPECT_EQ(awaitable_response.result_code, 200);

    auto json = nlohmann::json::parse(awaitable_response.result_text);
    EXPECT_EQ(json["title"], "Awaitable PATCH test");
}

// ======================== Awaitable DELETE Tests ========================

TEST_F(HttpTest, AwaitableDelete_BasicRequest) {
    boost::asio::io_context ioc;
    Http::Response awaitable_response;
    std::atomic<bool> completed{false};

    auto test_coro = [&]() -> boost::asio::awaitable<void> {
        awaitable_response = co_await Http::async_del("https://jsonplaceholder.typicode.com/posts/1");
        completed.store(true);
    };

    boost::asio::co_spawn(
        ioc,
        test_coro(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    FAIL() << "Exception in coroutine: " << ex.what();
                }
            }
        }
    );

    ioc.run();

    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(awaitable_response.is_ok()) << "Status: " << awaitable_response.result_code;
    EXPECT_EQ(awaitable_response.result_code, 200);
}

TEST_F(HttpTest, AwaitableDelete_WithBody) {
    boost::asio::io_context ioc;
    Http::Response awaitable_response;
    std::atomic<bool> completed{false};

    nlohmann::json delete_data = {
        {"reason", "Testing awaitable delete"}
    };

    auto test_coro = [&]() -> boost::asio::awaitable<void> {
        awaitable_response = co_await Http::async_del(
            "https://httpbun.com/delete",
            delete_data.dump(),
            {{"Content-Type", "application/json"}}
        );
        completed.store(true);
    };

    boost::asio::co_spawn(
        ioc,
        test_coro(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    FAIL() << "Exception in coroutine: " << ex.what();
                }
            }
        }
    );

    ioc.run();

    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(awaitable_response.is_ok()) << "Status: " << awaitable_response.result_code;
    EXPECT_EQ(awaitable_response.result_code, 200);
}

// ======================== Awaitable Sequential Tests ========================

TEST_F(HttpTest, AwaitableSequential_MultipleRequests) {
    boost::asio::io_context ioc;
    std::atomic<int> request_count{0};
    std::atomic<bool> completed{false};

    auto test_coro = [&]() -> boost::asio::awaitable<void> {
        // Sequential GET requests
        auto resp1 = co_await Http::async_get("https://jsonplaceholder.typicode.com/posts/1");
        EXPECT_TRUE(resp1.is_ok());
        request_count++;

        auto resp2 = co_await Http::async_get("https://jsonplaceholder.typicode.com/posts/2");
        EXPECT_TRUE(resp2.is_ok());
        request_count++;

        auto resp3 = co_await Http::async_get("https://jsonplaceholder.typicode.com/posts/3");
        EXPECT_TRUE(resp3.is_ok());
        request_count++;

        completed.store(true);
    };

    boost::asio::co_spawn(
        ioc,
        test_coro(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    FAIL() << "Exception in coroutine: " << ex.what();
                }
            }
        }
    );

    ioc.run();

    EXPECT_TRUE(completed.load());
    EXPECT_EQ(request_count.load(), 3);
}

// ======================== Awaitable Mixed Operations Test ========================

TEST_F(HttpTest, AwaitableMixed_AllHttpMethods) {
    boost::asio::io_context ioc;
    std::atomic<int> operations_completed{0};
    std::atomic<bool> completed{false};

    auto test_coro = [&]() -> boost::asio::awaitable<void> {
        // GET
        auto get_resp = co_await Http::async_get("https://jsonplaceholder.typicode.com/posts/1");
        EXPECT_TRUE(get_resp.is_ok());
        operations_completed++;

        // POST
        nlohmann::json post_data = {{"title", "test"}, {"body", "test"}, {"userId", 1}};
        auto post_resp = co_await Http::async_post(
            "https://jsonplaceholder.typicode.com/posts",
            post_data.dump(),
            {{"Content-Type", "application/json"}}
        );
        EXPECT_TRUE(post_resp.is_ok());
        operations_completed++;

        // PUT
        nlohmann::json put_data = {{"id", 1}, {"title", "updated"}, {"body", "updated"}, {"userId", 1}};
        auto put_resp = co_await Http::async_put(
            "https://jsonplaceholder.typicode.com/posts/1",
            put_data.dump(),
            {{"Content-Type", "application/json"}}
        );
        EXPECT_TRUE(put_resp.is_ok());
        operations_completed++;

        // PATCH
        nlohmann::json patch_data = {{"title", "patched"}};
        auto patch_resp = co_await Http::async_patch(
            "https://jsonplaceholder.typicode.com/posts/1",
            patch_data.dump(),
            {{"Content-Type", "application/json"}}
        );
        EXPECT_TRUE(patch_resp.is_ok());
        operations_completed++;

        // DELETE
        auto del_resp = co_await Http::async_del("https://jsonplaceholder.typicode.com/posts/1");
        EXPECT_TRUE(del_resp.is_ok());
        operations_completed++;

        completed.store(true);
    };

    boost::asio::co_spawn(
        ioc,
        test_coro(),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    FAIL() << "Exception in coroutine: " << ex.what();
                }
            }
        }
    );

    ioc.run();

    EXPECT_TRUE(completed.load());
    EXPECT_EQ(operations_completed.load(), 5);
}

#endif // GCC version check for awaitable tests

} // namespace slick::net
