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
#include <slick/net/detail/sse_parser.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <format>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "../src/utils.hpp"

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

    // What an HttpStream delivered; payloads and errors are only safe to read once disconnected is set
    struct stream_capture {
        std::atomic<bool> connected{false};
        std::atomic<bool> disconnected{false};
        std::vector<std::string> payloads;
        std::vector<std::string> errors;
    };

    // Opens an HttpStream to a loopback plain HTTP server on port that records what it delivers into capture
    std::shared_ptr<HttpStream> open_captured_stream(uint16_t port, std::shared_ptr<stream_capture> capture);

    // Streams a scripted_response_server's response, pausing between segments, and waits for the stream to disconnect
    std::shared_ptr<stream_capture> stream_scripted_response(std::vector<std::string> segments,
                                                             std::chrono::milliseconds pause = std::chrono::milliseconds(20));
};

// ======================== Concurrent Synchronous Request Tests ========================

// Loopback plain HTTP server that answers every request with "<METHOD> <target>[ host=<Host>][ <body>]",
// letting a caller verify it received the response to its own request. Targets starting with
// "/slow" are answered after slow_response_delay; targets starting with "/host" echo the Host header;
// targets starting with "/header" echo the X-Echo header as "x-echo=<value>".
class local_echo_server {
public:
    static constexpr std::chrono::milliseconds slow_response_delay{2000};

    explicit local_echo_server(const boost::asio::ip::address& address = boost::asio::ip::address_v4::loopback())
        : acceptor_(ioc_, {address, 0})
        , port_(acceptor_.local_endpoint().port()) {
        boost::asio::co_spawn(ioc_, accept_loop(), boost::asio::detached);
        thread_ = std::thread([this] { ioc_.run(); });
    }

    ~local_echo_server() {
        ioc_.stop();
        thread_.join();
    }

    uint16_t port() const noexcept { return port_; }
    int slow_requests() const noexcept { return slow_requests_.load(std::memory_order_acquire); }

private:
    boost::asio::awaitable<void> accept_loop() {
        for (;;) {
            auto [ec, socket] = co_await acceptor_.async_accept(boost::asio::as_tuple(boost::asio::use_awaitable));
            if (ec) {
                co_return;
            }
            boost::asio::co_spawn(ioc_, serve(std::move(socket)), boost::asio::detached);
        }
    }

    boost::asio::awaitable<void> serve(boost::asio::ip::tcp::socket socket) {
        const auto token = boost::asio::as_tuple(boost::asio::use_awaitable);
        std::string request;
        if (auto [ec, n] = co_await boost::asio::async_read_until(socket, boost::asio::dynamic_buffer(request), "\r\n\r\n", token); ec) {
            co_return;
        }

        const auto header_end = request.find("\r\n\r\n") + 4;
        std::size_t content_length = 0;
        if (auto pos = request.find("Content-Length: "); pos < header_end) {
            content_length = std::stoul(request.substr(pos + 16));
        }
        // Drain the body so closing the socket does not reset the connection
        if (request.size() < header_end + content_length) {
            auto remaining = header_end + content_length - request.size();
            if (auto [ec, n] = co_await boost::asio::async_read(socket, boost::asio::dynamic_buffer(request),
                                                                 boost::asio::transfer_exactly(remaining), token); ec) {
                co_return;
            }
        }

        auto echo = request.substr(0, request.find(" HTTP/"));
        if (echo.find(" /slow") != std::string::npos) {
            slow_requests_.fetch_add(1, std::memory_order_release);
            boost::asio::steady_timer timer{socket.get_executor(), slow_response_delay};
            co_await timer.async_wait(token);
        }
        if (echo.find(" /host") != std::string::npos) {
            if (auto pos = request.find("\r\nHost: "); pos < header_end) {
                pos += 8;
                echo += " host=";
                echo.append(request, pos, request.find("\r\n", pos) - pos);
            }
        }
        if (echo.find(" /header") != std::string::npos) {
            if (auto pos = request.find("\r\nX-Echo: "); pos < header_end) {
                pos += 10;
                echo += " x-echo=";
                echo.append(request, pos, request.find("\r\n", pos) - pos);
            }
        }
        if (content_length) {
            echo += ' ';
            echo.append(request, header_end, content_length);
        }
        auto response = std::format("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", echo.size(), echo);
        co_await boost::asio::async_write(socket, boost::asio::buffer(response), token);
        boost::system::error_code ec;
        socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    }

    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    uint16_t port_;
    std::atomic<int> slow_requests_{0};
    std::thread thread_;
};

// Regression: a synchronous call ran the shared io_context until it had no work left at all, so it
// did not return until every other thread's in-flight synchronous request had finished too.
TEST_F(HttpTest, SyncRequest_NotBlockedByOtherThreadsSlowRequest) {
    local_echo_server server;
    const auto base_url = std::format("http://127.0.0.1:{}", server.port());

    Http::Response slow_response;
    std::thread slow_thread([&] { slow_response = Http::get(base_url + "/slow"); });
    const bool slow_in_flight = wait_for_condition([&] { return server.slow_requests() > 0; }, std::chrono::seconds(5));

    const auto begin = std::chrono::steady_clock::now();
    const auto fast_response = Http::get(base_url + "/fast");
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    slow_thread.join();

    ASSERT_TRUE(slow_in_flight) << "slow request never reached the server";
    EXPECT_EQ(fast_response.result_code, 200);
    EXPECT_EQ(fast_response.result_text, "GET /fast");
    EXPECT_LT(elapsed_ms, local_echo_server::slow_response_delay.count() / 2)
        << "fast request waited for the other thread's slow request";
    EXPECT_EQ(slow_response.result_code, 200);
    EXPECT_EQ(slow_response.result_text, "GET /slow");
}

// Regression: synchronous calls ran on a thread_local io_context. On Windows its destructor joins
// Asio's resolver/timer helper thread under the loader lock, so a thread that had made a synchronous
// request never finished exiting and joining it hung forever.
TEST_F(HttpTest, SyncRequest_CallingThreadCanExit) {
    local_echo_server server;
    struct outcome {
        std::atomic<bool> joined{false};
        Http::Response response;
    };
    // Shared with a detached joiner so a regression fails this test instead of hanging it
    auto state = std::make_shared<outcome>();
    auto worker = std::make_shared<std::thread>([state, url = std::format("http://127.0.0.1:{}/exit", server.port())] {
        state->response = Http::get(url);
    });
    std::thread([state, worker] {
        worker->join();
        state->joined.store(true, std::memory_order_release);
    }).detach();

    ASSERT_TRUE(wait_for_condition([&] { return state->joined.load(std::memory_order_acquire); }, std::chrono::seconds(10)))
        << "thread that made a synchronous request did not exit";
    EXPECT_EQ(state->response.result_code, 200) << state->response.result_text;
    EXPECT_EQ(state->response.result_text, "GET /exit");
}

// Regression: all synchronous calls shared one io_context and raced on restart()/run(), so a
// caller could return an empty response (its run() saw the context stopped by another thread)
// or have its response written through a dangling reference after returning.
TEST_F(HttpTest, SyncRequests_ConcurrentThreads_EachGetsOwnResponse) {
    local_echo_server server;
    constexpr int thread_count = 8;
    constexpr int requests_per_thread = 50;

    std::atomic<bool> start{false};
    std::vector<std::vector<std::string>> failures(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < requests_per_thread; ++i) {
                auto target = std::format("/t{}/r{}", t, i);
                auto url = std::format("http://127.0.0.1:{}{}", server.port(), target);
                auto data = std::format("data-{}-{}", t, i);
                Http::Response response;
                std::string expected;
                switch ((t + i) % 5) {
                case 0: response = Http::get(url); expected = "GET " + target; break;
                case 1: response = Http::post(url, data); expected = std::format("POST {} {}", target, data); break;
                case 2: response = Http::put(url, data); expected = std::format("PUT {} {}", target, data); break;
                case 3: response = Http::patch(url, data); expected = std::format("PATCH {} {}", target, data); break;
                default: response = Http::del(url, ""); expected = "DELETE " + target; break;
                }
                if (response.result_code != 200 || response.result_text != expected) {
                    failures[t].push_back(std::format("expected 200 '{}', got {} '{}'",
                                                      expected, response.result_code, response.result_text));
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    for (int t = 0; t < thread_count; ++t) {
        EXPECT_TRUE(failures[t].empty()) << "thread " << t << ": " << failures[t].size()
                                         << " bad responses, first: " << failures[t].front();
    }
}

// ======================== URL Parsing Tests ========================

using url_tuple = std::tuple<std::string, std::string, std::string, bool>;

// Regression: the parser ignored a colon at offset 3 or 4 of the already scheme-stripped authority,
// so "abc:8080" resolved the literal host "abc:8080" on the default port.
TEST_F(HttpTest, ParseUrl_ShortHostWithExplicitPort) {
    EXPECT_EQ(parse_url("http://abc:8080/feed"), (url_tuple{"abc", "/feed", "8080", false}));
    EXPECT_EQ(parse_url("https://test:9443"), (url_tuple{"test", "/", "9443", true}));
    EXPECT_EQ(parse_url("abcd:81/x"), (url_tuple{"abcd", "/x", "81", true}));
    EXPECT_EQ(parse_url("http://localhost:8080/p"), (url_tuple{"localhost", "/p", "8080", false}));
}

// Regression: "[::1]:8080" split at the first colon, so std::stoi threw on ":1]:8080".
TEST_F(HttpTest, ParseUrl_Ipv6Literals) {
    EXPECT_EQ(parse_url("http://[::1]:8080/p?q=1"), (url_tuple{"::1", "/p?q=1", "8080", false}));
    EXPECT_EQ(parse_url("https://[2001:db8::1]"), (url_tuple{"2001:db8::1", "/", "443", true}));
    EXPECT_EQ(parse_url("[fe80::1]:9000/x"), (url_tuple{"fe80::1", "/x", "9000", true}));
    EXPECT_EQ(parse_url("http://::1/"), (url_tuple{"::1", "/", "80", false}));
}

TEST_F(HttpTest, ParseUrl_AuthorityDelimiters) {
    EXPECT_EQ(parse_url("http://host:8080?x=1"), (url_tuple{"host", "/?x=1", "8080", false}));
    EXPECT_EQ(parse_url("https://host:8443#frag"), (url_tuple{"host", "/", "8443", true}));
    EXPECT_EQ(parse_url("https://host/p#frag"), (url_tuple{"host", "/p", "443", true}));
    EXPECT_EQ(parse_url("http://host:/p"), (url_tuple{"host", "/p", "80", false}));
    EXPECT_EQ(parse_url("http://a/b:9000"), (url_tuple{"a", "/b:9000", "80", false}));
}

TEST_F(HttpTest, ParseUrl_RejectsMalformedAuthority) {
    for (const auto* url : {"http://h:abc/", "http://h:70000", "http://h:0", "http://h:-1", "http://h:8080x",
                            "http://[::1", "http://[::1]8080/"}) {
        EXPECT_THROW(parse_url(url), std::invalid_argument) << url;
    }
}

TEST_F(HttpTest, FormatAuthority_BracketsIpv6) {
    EXPECT_EQ(detail::format_authority("example.com"), "example.com");
    EXPECT_EQ(detail::format_authority("::1"), "[::1]");
    EXPECT_EQ(detail::format_authority("::1", 9000), "[::1]:9000");
    EXPECT_EQ(detail::format_authority("abc", 65535), "abc:65535");
}

TEST_F(HttpTest, SyncGet_Ipv6LiteralWithPort) {
    std::unique_ptr<local_echo_server> server;
    try {
        server = std::make_unique<local_echo_server>(boost::asio::ip::address_v6::loopback());
    } catch (const boost::system::system_error& e) {
        GTEST_SKIP() << "IPv6 loopback unavailable: " << e.what();
    }

    auto response = Http::get(std::format("http://[::1]:{}/host?v=6", server->port()));
    EXPECT_EQ(response.result_code, 200) << response.result_text;
    EXPECT_EQ(response.result_text, "GET /host?v=6 host=[::1]");
}

// Regression: the session was chosen with `co_await (use_ssl ? ssl_session(std::move(host), ...)
// : plain_session(std::move(host), ...))`. GCC evaluates both arms there, so the plain session got
// moved-from arguments and every http:// request failed with "Host not found" while https:// worked.
TEST_F(HttpTest, PlainHttp_SyncCallbackAndAwaitable_ReachLoopbackServer) {
    local_echo_server server;
    const auto url = std::format("http://127.0.0.1:{}/host", server.port());

    auto sync_response = Http::get(url);
    EXPECT_EQ(sync_response.result_code, 200) << sync_response.result_text;
    EXPECT_EQ(sync_response.result_text, "GET /host host=127.0.0.1");

    std::atomic<bool> async_done{false};
    Http::Response async_response;
    Http::async_put([&](Http::Response&& r) {
        async_response = std::move(r);
        async_done.store(true, std::memory_order_release);
    }, url, "cb");
    ASSERT_TRUE(wait_for_condition([&] { return async_done.load(std::memory_order_acquire); }, std::chrono::seconds(5)));
    EXPECT_EQ(async_response.result_code, 200) << async_response.reason;
    EXPECT_EQ(async_response.result_text, "PUT /host host=127.0.0.1 cb");

    boost::asio::io_context ioc;
    Http::Response awaitable_response;
    std::string awaitable_error;
    auto request = [&]() -> boost::asio::awaitable<void> {
        awaitable_response = co_await Http::async_post(url, "coro");
    };
    boost::asio::co_spawn(ioc, request(), [&](std::exception_ptr e) {
        if (e) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                awaitable_error = ex.what();
            }
        }
    });
    ioc.run();
    EXPECT_TRUE(awaitable_error.empty()) << awaitable_error;
    EXPECT_EQ(awaitable_response.result_code, 200);
    EXPECT_EQ(awaitable_response.result_text, "POST /host host=127.0.0.1 coro");
}

// Headers and bodies are moved, not copied, through the session coroutines. Every entry point must
// still put them on the wire intact (a moved-from argument would arrive empty), and a body far
// beyond the small-string buffer must round-trip through the request and the response.
TEST_F(HttpTest, PlainHttp_HeadersAndLargeBody_ForwardedIntactByEveryEntryPoint) {
    local_echo_server server;
    const auto url = std::format("http://127.0.0.1:{}/header", server.port());
    const std::string header_value(256, 'h');
    std::string body(256 * 1024, '\0');
    for (std::size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<char>('a' + i % 26);
    }
    const auto headers = [&] { return std::vector<std::pair<std::string, std::string>>{{"X-Echo", header_value}}; };
    const auto expected = [&](std::string_view method) { return std::format("{} /header x-echo={} {}", method, header_value, body); };
    // Compared with EXPECT_TRUE so a mismatch does not dump a 256 KiB string
    const auto check = [&](const Http::Response& response, std::string_view method) {
        EXPECT_EQ(response.result_code, 200) << method << ": " << response.result_text.substr(0, 200);
        EXPECT_TRUE(response.result_text == expected(method))
            << method << ": got " << response.result_text.size() << " bytes: " << response.result_text.substr(0, 200);
    };

    check(Http::post(url, body, headers()), "POST");

    std::atomic<bool> async_done{false};
    Http::Response async_response;
    Http::async_patch([&](Http::Response&& r) {
        async_response = std::move(r);
        async_done.store(true, std::memory_order_release);
    }, url, body, headers());
    ASSERT_TRUE(wait_for_condition([&] { return async_done.load(std::memory_order_acquire); }, std::chrono::seconds(10)));
    check(async_response, "PATCH");

    boost::asio::io_context ioc;
    Http::Response awaitable_response;
    std::string awaitable_error;
    auto request = [&]() -> boost::asio::awaitable<void> {
        awaitable_response = co_await Http::async_put(url, body, headers());
    };
    boost::asio::co_spawn(ioc, request(), [&](std::exception_ptr e) {
        if (e) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                awaitable_error = ex.what();
            }
        }
    });
    ioc.run();
    EXPECT_TRUE(awaitable_error.empty()) << awaitable_error;
    check(awaitable_response, "PUT");
}

// Regression: parse errors threw out of Http::get/async_get instead of producing an error response,
// and async_get had already counted the request, keeping the async service thread alive forever.
TEST_F(HttpTest, MalformedUrl_ReportedAsErrorResponse) {
    auto response = Http::get("http://127.0.0.1:99999/");
    EXPECT_EQ(response.result_code, 500);
    EXPECT_NE(response.result_text.find("Invalid port"), std::string::npos) << response.result_text;

    std::atomic<bool> done{false};
    Http::Response async_response;
    EXPECT_NO_THROW(Http::async_get([&](Http::Response&& r) {
        async_response = std::move(r);
        done.store(true, std::memory_order_release);
    }, "http://[::1/"));
    ASSERT_TRUE(wait_for_condition([&] { return done.load(std::memory_order_acquire); }, std::chrono::seconds(5)));
    EXPECT_EQ(async_response.result_code, 500);
    // The async completion handler reports exceptions as Response{500, what()}, i.e. in `reason`
    EXPECT_NE(async_response.reason.find("Invalid IPv6 literal"), std::string::npos) << async_response.reason;
}

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

    // Skip rather than fail if the external SSE endpoint is unreachable on this runner
    if (!wait_for_condition([&]() { return connected.load(); }, std::chrono::seconds(10))) {
        stream->close();
        GTEST_SKIP() << "Cannot connect to external SSE endpoint (network unreachable on this runner)";
    }

    if (!wait_for_condition([&]() { return data_received_count.load() > 0; }, std::chrono::seconds(10))) {
        stream->close();
        GTEST_SKIP() << "External SSE endpoint connected but sent no data within 10s"
                     << (error_occurred.load() ? ": " + last_error : " (CDN throttling or slow network on this runner)");
    }

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

// Loopback plain HTTP server that answers each connection it accepts, in turn, with the next scripted response,
// writing its raw segments and pausing between them so the client receives each one in a separate read.
class scripted_response_server {
public:
    // Selects the constructor taking one response per connection
    struct per_connection_t {};

    // Answers a single connection
    explicit scripted_response_server(std::vector<std::string> segments,
                                      std::chrono::milliseconds pause = std::chrono::milliseconds(20))
        : scripted_response_server(per_connection_t{}, {std::move(segments)}, pause) {}

    scripted_response_server(per_connection_t, std::vector<std::vector<std::string>> responses,
                             std::chrono::milliseconds pause = std::chrono::milliseconds(20))
        : responses_(std::move(responses))
        , pause_(pause)
        , acceptor_(ioc_, {boost::asio::ip::address_v4::loopback(), 0})
        , port_(acceptor_.local_endpoint().port()) {
        boost::asio::co_spawn(ioc_, serve(), boost::asio::detached);
        thread_ = std::thread([this] { ioc_.run(); });
    }

    ~scripted_response_server() {
        ioc_.stop();
        thread_.join();
    }

    uint16_t port() const noexcept { return port_; }

private:
    boost::asio::awaitable<void> serve() {
        const auto token = boost::asio::as_tuple(boost::asio::use_awaitable);
        for (const auto& segments : responses_) {
            auto [accept_ec, socket] = co_await acceptor_.async_accept(token);
            if (accept_ec) {
                co_return;
            }
            // Keep each segment in its own TCP segment instead of letting Nagle coalesce them
            socket.set_option(boost::asio::ip::tcp::no_delay(true));

            std::string request;
            if (auto [ec, n] = co_await boost::asio::async_read_until(socket, boost::asio::dynamic_buffer(request), "\r\n\r\n", token); ec) {
                co_return;
            }

            boost::asio::steady_timer timer{socket.get_executor()};
            for (std::size_t i = 0; i < segments.size(); ++i) {
                if (i > 0) {
                    timer.expires_after(pause_);
                    co_await timer.async_wait(token);
                }
                if (auto [ec, n] = co_await boost::asio::async_write(socket, boost::asio::buffer(segments[i]), token); ec) {
                    co_return;
                }
            }
            boost::system::error_code ec;
            socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        }
    }

    boost::asio::io_context ioc_;
    std::vector<std::vector<std::string>> responses_;
    std::chrono::milliseconds pause_;
    boost::asio::ip::tcp::acceptor acceptor_;
    uint16_t port_;
    std::thread thread_;
};

std::shared_ptr<HttpStream> HttpTest::open_captured_stream(uint16_t port, std::shared_ptr<stream_capture> capture) {
    // Callbacks share ownership so a stream that outlives its test never touches a dead capture
    auto stream = std::make_shared<HttpStream>(
        std::format("http://127.0.0.1:{}/stream", port),
        [capture] { capture->connected.store(true, std::memory_order_release); },
        [capture] { capture->disconnected.store(true, std::memory_order_release); },
        [capture](const char* data, size_t size) { capture->payloads.emplace_back(data, size); },
        [capture](std::string err) { capture->errors.push_back(std::move(err)); }
    );
    stream->open();
    return stream;
}

std::shared_ptr<HttpTest::stream_capture> HttpTest::stream_scripted_response(std::vector<std::string> segments,
                                                                             std::chrono::milliseconds pause) {
    scripted_response_server server{std::move(segments), pause};
    auto capture = std::make_shared<stream_capture>();
    auto stream = open_captured_stream(server.port(), capture);
    if (!wait_for_condition([&] { return capture->disconnected.load(std::memory_order_acquire); }, std::chrono::seconds(10))) {
        stream->close();
    }
    return capture;
}

// Formats one chunk of a chunked transfer-coded body
std::string http_chunk(std::string_view data, std::string_view extension = {}) {
    return std::format("{:x}{}\r\n{}\r\n", data.size(), extension, data);
}

// Regression: after the header HttpStream read raw socket bytes, so a chunked non-SSE body reached
// onData with its chunk sizes, chunk extensions and trailers still in it.
TEST_F(HttpTest, HttpStream_ChunkedBody_DeliversDecodedBytes) {
    const std::string large(20000, 'x');  // larger than HttpStream's body buffer, so it is delivered in pieces
    auto capture = stream_scripted_response({
        // The first chunk arrives with the header, so it is read along with it
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n" + http_chunk("hello"),
        http_chunk(" world", ";name=value"),
        // A chunk split across reads
        "a\r\n0123",
        "456789\r\n",
        http_chunk(large),
        "0\r\nX-Trailer: done\r\n\r\n",
    });

    ASSERT_TRUE(capture->disconnected.load(std::memory_order_acquire)) << "stream did not end after the final chunk";
    std::string body;
    for (const auto& payload : capture->payloads) {
        body += payload;
    }
    EXPECT_EQ(body, "hello world0123456789" + large);
    EXPECT_TRUE(capture->errors.empty()) << capture->errors.front();
}

// Regression: chunk framing was fed into the SSE parser along with the event text.
TEST_F(HttpTest, HttpStream_ChunkedSse_DecodesEventsAcrossChunks) {
    auto capture = stream_scripted_response({
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n",
        http_chunk("data: first\n\n"),
        // An event split across chunks
        http_chunk("data: sec"),
        http_chunk("ond\n\n", ";x=y"),
        "0\r\n\r\n",
    });

    ASSERT_TRUE(capture->disconnected.load(std::memory_order_acquire)) << "stream did not end after the final chunk";
    EXPECT_EQ(capture->payloads, (std::vector<std::string>{"first", "second"}));
    EXPECT_TRUE(capture->errors.empty()) << capture->errors.front();
}

// A chunked body cut off before its final chunk is reported rather than treated as a normal end.
TEST_F(HttpTest, HttpStream_TruncatedChunkedBody_ReportsError) {
    auto capture = stream_scripted_response({
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n",
        http_chunk("partial"),
    });

    ASSERT_TRUE(capture->disconnected.load(std::memory_order_acquire)) << "stream did not end when the server closed";
    ASSERT_EQ(capture->payloads.size(), 1u);
    EXPECT_EQ(capture->payloads.front(), "partial");
    EXPECT_FALSE(capture->errors.empty());
}

// Regression: each body read had a 2 s timeout so close() was noticed, but Beast closes the socket when a
// timeout fires, so a stream that received nothing for 2 s failed its next read and disconnected.
TEST_F(HttpTest, HttpStream_IdleStream_StaysOpen) {
    auto capture = stream_scripted_response({
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n" + http_chunk("data: before\n\n"),
        http_chunk("data: after\n\n") + "0\r\n\r\n",
    }, std::chrono::milliseconds(3000));

    ASSERT_TRUE(capture->disconnected.load(std::memory_order_acquire)) << "stream did not end after the final chunk";
    EXPECT_EQ(capture->payloads, (std::vector<std::string>{"before", "after"}));
    EXPECT_TRUE(capture->errors.empty()) << capture->errors.front();
}

// close() cancels a read waiting on a silent server instead of leaving it until data or a timeout arrives.
TEST_F(HttpTest, HttpStream_Close_InterruptsIdleRead) {
    scripted_response_server server{{
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n",
        "0\r\n\r\n",
    }, std::chrono::seconds(30)};
    auto capture = std::make_shared<stream_capture>();
    auto stream = open_captured_stream(server.port(), capture);

    ASSERT_TRUE(wait_for_condition([&] { return capture->connected.load(std::memory_order_acquire); }, std::chrono::seconds(10)));
    // Let the session start waiting on its body read
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto begin = std::chrono::steady_clock::now();
    stream->close();
    ASSERT_TRUE(wait_for_condition([&] { return capture->disconnected.load(std::memory_order_acquire); }, std::chrono::seconds(10)));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();

    EXPECT_LT(elapsed_ms, 1000) << "close() waited for the pending read to time out";
    EXPECT_TRUE(capture->errors.empty()) << capture->errors.front();
}

// Regression: the parser kept a partial event across responses, so after a response ended inside an event
// the reopened stream joined that event's data to the first event of the next response.
TEST_F(HttpTest, HttpStream_Reopen_DropsPartialEventOfPreviousResponse) {
    const std::string header = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n";
    // Neither body has a length, so each ends when the server closes the connection
    scripted_response_server server{scripted_response_server::per_connection_t{}, {
        {header + "data: stale"},
        {header + "data: fresh\n\n"},
    }};
    auto capture = std::make_shared<stream_capture>();
    auto stream = open_captured_stream(server.port(), capture);
    ASSERT_TRUE(wait_for_condition([&] { return capture->disconnected.load(std::memory_order_acquire); }, std::chrono::seconds(10)));

    capture->disconnected.store(false, std::memory_order_release);
    stream->open();
    ASSERT_TRUE(wait_for_condition([&] { return capture->disconnected.load(std::memory_order_acquire); }, std::chrono::seconds(10)));

    EXPECT_EQ(capture->payloads, (std::vector<std::string>{"fresh"}));
    EXPECT_TRUE(capture->errors.empty()) << capture->errors.front();
}

// Splits stream into pieces of piece_size bytes, the last one possibly shorter
std::vector<std::string_view> sse_pieces(std::string_view stream, std::size_t piece_size) {
    std::vector<std::string_view> pieces;
    pieces.reserve(stream.size() / piece_size + 1);
    for (std::size_t offset = 0; offset < stream.size(); offset += piece_size) {
        pieces.push_back(stream.substr(offset, piece_size));
    }
    return pieces;
}

// Feeds pieces to a fresh sse_parser in order and returns the events it dispatched
std::vector<std::string> parse_sse_pieces(const std::vector<std::string_view>& pieces) {
    detail::sse_parser parser;
    std::vector<std::string> events;
    for (const auto piece : pieces) {
        parser.feed(piece.data(), piece.size(), [&](const char* data, std::size_t size) { events.emplace_back(data, size); });
    }
    return events;
}

// Regression: a CR ending one piece was turned into LF before the LF opening the next piece arrived, so the
// split CRLF read as a blank line and ended the event early.
TEST_F(HttpTest, SseParser_CrlfSplitAcrossPieces_IsOneLineEnding) {
    EXPECT_EQ(parse_sse_pieces({"data: a\r", "\ndata: b\r", "\n\r", "\n"}), (std::vector<std::string>{"a\nb"}));
    // A CR ending a piece is still a line ending when the next piece does not open with LF
    EXPECT_EQ(parse_sse_pieces({"data: a\r", "data: b\r", "\r"}), (std::vector<std::string>{"a\nb"}));
}

// Splitting a stream anywhere, or feeding it a byte at a time, gives the same events as parsing it whole.
TEST_F(HttpTest, SseParser_AnySplit_MatchesWholeStream) {
    const std::string_view stream =
        ": comment\r\n"
        "event: update\r\n"
        "data: first\r\n"
        "data:second\r\n"
        "id: 1\r\n"
        "\r\n"
        "data: lf\n"
        "\n"
        "data: cr\r"
        "\r"
        "data:\n"
        "data:  two spaces\n"
        "\n"
        "dat: not data\n"
        "data\n"
        "\n"
        "data: last\r\n"
        "\r\n";
    const std::vector<std::string> expected{"first\nsecond", "lf", "cr", " two spaces", "last"};

    ASSERT_EQ(parse_sse_pieces({stream}), expected);
    for (std::size_t split = 1; split < stream.size(); ++split) {
        EXPECT_EQ(parse_sse_pieces({stream.substr(0, split), stream.substr(split)}), expected) << "split at " << split;
    }
    EXPECT_EQ(parse_sse_pieces(sse_pieces(stream, 1)), expected);
}

// Regression: every piece rescanned and erased from the whole retained buffer, so an event arriving in many
// small pieces took time quadratic in its size (hours for this one). Parsing is now linear and takes well
// under a second here; the per-test timeout catches a regression.
TEST_F(HttpTest, SseParser_LargeEventInSmallPieces_ParsesInLinearTime) {
    const std::string value(58, 'x');
    std::string stream;
    std::string expected;
    for (int line = 0; line < 16384; ++line) {  // 1 MiB of 64-byte CRLF-terminated data lines
        stream.append("data: ").append(value).append("\r\n");
        if (line > 0) {
            expected.push_back('\n');
        }
        expected.append(value);
    }
    stream.append("\r\n");

    // Byte-sized pieces also split every CRLF
    const auto events = parse_sse_pieces(sse_pieces(stream, 1));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events.front() == expected) << "event of " << events.front().size() << " bytes differs";
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
