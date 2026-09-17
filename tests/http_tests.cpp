#include <gtest/gtest.h>
#include <array>
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
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
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

    // What an HttpStream delivered; events, payloads and errors are only safe to read once no callback can still run
    struct stream_capture {
        std::atomic<bool> connected{false};
        std::atomic<bool> disconnected{false};
        std::atomic<int> connects{0};
        std::atomic<int> disconnects{0};
        std::string events;  // 'C' for each onConnected, 'E' for each onError and 'D' for each onDisconnected, in order
        std::vector<std::string> payloads;
        std::vector<std::string> errors;
    };

    // Opens an HttpStream to url that records what it delivers into capture. A null executor runs the stream on the
    // shared HttpStream service.
    std::shared_ptr<HttpStream> open_captured_stream(std::string url, std::shared_ptr<stream_capture> capture,
                                                     boost::asio::any_io_executor executor = {});

    // Opens a captured HttpStream to a loopback plain HTTP server on port
    std::shared_ptr<HttpStream> open_captured_stream(uint16_t port, std::shared_ptr<stream_capture> capture,
                                                     boost::asio::any_io_executor executor = {}) {
        return open_captured_stream(std::format("http://127.0.0.1:{}/stream", port), std::move(capture), std::move(executor));
    }

    // Streams a scripted_response_server's response, pausing between segments, and waits for the stream to disconnect
    std::shared_ptr<stream_capture> stream_scripted_response(std::vector<std::string> segments,
                                                             std::chrono::milliseconds pause = std::chrono::milliseconds(20),
                                                             boost::asio::any_io_executor executor = {});

    using open_stream_fn = std::function<std::shared_ptr<HttpStream>(uint16_t port, std::shared_ptr<stream_capture> capture)>;

    // Blocks the onData callback of a stream on the shared service, opens a second stream through open_other and
    // checks the second stream delivers its whole response while that callback is still blocked
    void expect_blocked_callback_does_not_stall(const open_stream_fn& open_other);
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
    // Requests whose whole response has been written
    int answered_requests() const noexcept { return answered_requests_.load(std::memory_order_acquire); }

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
        if (auto [ec, n] = co_await boost::asio::async_write(socket, boost::asio::buffer(response), token); !ec) {
            answered_requests_.fetch_add(1, std::memory_order_release);
        }
        boost::system::error_code ec;
        socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    }

    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    uint16_t port_;
    std::atomic<int> slow_requests_{0};
    std::atomic<int> answered_requests_{0};
    std::thread thread_;
};

// An io_context run by a thread of its own, for callbacks and streams that should not run on a shared service
struct executor_thread {
    boost::asio::io_context ioc;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{ioc.get_executor()};
    std::thread thread{[this] { ioc.run(); }};

    ~executor_thread() {
        ioc.stop();
        thread.join();
    }
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

using url_tuple = std::tuple<std::string, std::string, std::string, bool, std::string>;

// Regression: the parser ignored a colon at offset 3 or 4 of the already scheme-stripped authority,
// so "abc:8080" resolved the literal host "abc:8080" on the default port.
TEST_F(HttpTest, ParseUrl_ShortHostWithExplicitPort) {
    EXPECT_EQ(parse_url("http://abc:8080/feed"), (url_tuple{"abc", "/feed", "8080", false, "abc:8080"}));
    EXPECT_EQ(parse_url("https://test:9443"), (url_tuple{"test", "/", "9443", true, "test:9443"}));
    EXPECT_EQ(parse_url("abcd:81/x"), (url_tuple{"abcd", "/x", "81", true, "abcd:81"}));
    EXPECT_EQ(parse_url("http://localhost:8080/p"), (url_tuple{"localhost", "/p", "8080", false, "localhost:8080"}));
}

// Regression: "[::1]:8080" split at the first colon, so std::stoi threw on ":1]:8080".
TEST_F(HttpTest, ParseUrl_Ipv6Literals) {
    EXPECT_EQ(parse_url("http://[::1]:8080/p?q=1"), (url_tuple{"::1", "/p?q=1", "8080", false, "[::1]:8080"}));
    EXPECT_EQ(parse_url("https://[2001:db8::1]"), (url_tuple{"2001:db8::1", "/", "443", true, "[2001:db8::1]"}));
    EXPECT_EQ(parse_url("[fe80::1]:9000/x"), (url_tuple{"fe80::1", "/x", "9000", true, "[fe80::1]:9000"}));
    EXPECT_EQ(parse_url("http://::1/"), (url_tuple{"::1", "/", "80", false, "[::1]"}));
}

TEST_F(HttpTest, ParseUrl_AuthorityDelimiters) {
    EXPECT_EQ(parse_url("http://host:8080?x=1"), (url_tuple{"host", "/?x=1", "8080", false, "host:8080"}));
    EXPECT_EQ(parse_url("https://host:8443#frag"), (url_tuple{"host", "/", "8443", true, "host:8443"}));
    EXPECT_EQ(parse_url("https://host/p#frag"), (url_tuple{"host", "/p", "443", true, "host"}));
    EXPECT_EQ(parse_url("http://host:/p"), (url_tuple{"host", "/p", "80", false, "host"}));
    EXPECT_EQ(parse_url("http://a/b:9000"), (url_tuple{"a", "/b:9000", "80", false, "a"}));
}

// Regression: the Host header was built from the host alone, so a request to a non-default port named the
// wrong origin (e.g. "Host: api.example.com" for http://api.example.com:8080/).
TEST_F(HttpTest, ParseUrl_HostHeaderCarriesOnlyNonDefaultPort) {
    // Default port of the connection, spelled out or not
    EXPECT_EQ(std::get<4>(parse_url("http://h/")), "h");
    EXPECT_EQ(std::get<4>(parse_url("http://h:80/")), "h");
    EXPECT_EQ(std::get<4>(parse_url("https://h:443/")), "h");
    EXPECT_EQ(std::get<4>(parse_url("h:443")), "h");
    EXPECT_EQ(std::get<4>(parse_url("https://[::1]:443/")), "[::1]");
    // The other scheme's default port is not this connection's default
    EXPECT_EQ(std::get<4>(parse_url("http://h:443/")), "h:443");
    EXPECT_EQ(std::get<4>(parse_url("https://h:80/")), "h:80");
    EXPECT_EQ(std::get<4>(parse_url("http://[::1]:65535/")), "[::1]:65535");
}

TEST_F(HttpTest, ParseUrl_RejectsMalformedAuthority) {
    for (const auto* url : {"http://h:abc/", "http://h:70000", "http://h:0", "http://h:-1", "http://h:8080x",
                            "http://[::1", "http://[::1]8080/"}) {
        EXPECT_THROW(parse_url(url), std::invalid_argument) << url;
    }
}

// Regression: any scheme other than "http" was taken as a plaintext connection on port 443, so
// "ftp://h/" or "HTTP://h/" silently spoke plain HTTP to port 443.
TEST_F(HttpTest, ParseUrl_RejectsUnsupportedScheme) {
    for (const auto* url : {"ftp://h/", "ws://h/", "wss://h/", "httpx://h/", "htt://h/", "://h/", "file:///etc/hosts"}) {
        EXPECT_THROW(parse_url(url), std::invalid_argument) << url;
    }
    // Schemes are case-insensitive
    EXPECT_EQ(parse_url("HTTP://h:8080/p"), (url_tuple{"h", "/p", "8080", false, "h:8080"}));
    EXPECT_EQ(parse_url("Https://h/p"), (url_tuple{"h", "/p", "443", true, "h"}));
    // A "://" inside the path or query of a scheme-less URL is not a scheme delimiter
    EXPECT_EQ(parse_url("h/cb?next=ftp://x"), (url_tuple{"h", "/cb?next=ftp://x", "443", true, "h"}));
    EXPECT_EQ(parse_url("h:8443?u=a://b"), (url_tuple{"h", "/?u=a://b", "8443", true, "h:8443"}));
    EXPECT_EQ(parse_url("http://h/r?to=https://x"), (url_tuple{"h", "/r?to=https://x", "80", false, "h"}));

    auto response = Http::get("ftp://127.0.0.1/");
    EXPECT_EQ(response.result_code, 500);
    EXPECT_NE(response.result_text.find("Unsupported URL scheme"), std::string::npos) << response.result_text;

    EXPECT_THROW((void)std::make_shared<HttpStream>("ws://127.0.0.1/", [] {}, [] {}, [](const char*, std::size_t) {},
                                                    [](std::string) {}),
                 std::invalid_argument);
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
    EXPECT_EQ(response.result_text, std::format("GET /host?v=6 host=[::1]:{}", server->port()));
}

// Regression: the session was chosen with `co_await (use_ssl ? ssl_session(std::move(host), ...)
// : plain_session(std::move(host), ...))`. GCC evaluates both arms there, so the plain session got
// moved-from arguments and every http:// request failed with "Host not found" while https:// worked.
// The echoed Host header also checks the port is sent for a non-default port.
TEST_F(HttpTest, PlainHttp_SyncCallbackAndAwaitable_ReachLoopbackServer) {
    local_echo_server server;
    const auto url = std::format("http://127.0.0.1:{}/host", server.port());
    const auto host = std::format("host=127.0.0.1:{}", server.port());

    auto sync_response = Http::get(url);
    EXPECT_EQ(sync_response.result_code, 200) << sync_response.result_text;
    EXPECT_EQ(sync_response.reason, "OK");
    EXPECT_EQ(sync_response.result_text, "GET /host " + host);

    std::atomic<bool> async_done{false};
    Http::Response async_response;
    Http::async_put([&](Http::Response&& r) {
        async_response = std::move(r);
        async_done.store(true, std::memory_order_release);
    }, url, "cb");
    ASSERT_TRUE(wait_for_condition([&] { return async_done.load(std::memory_order_acquire); }, std::chrono::seconds(5)));
    EXPECT_EQ(async_response.result_code, 200) << async_response.reason;
    EXPECT_EQ(async_response.result_text, "PUT /host " + host + " cb");

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
    EXPECT_EQ(awaitable_response.result_text, "POST /host " + host + " coro");
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

// ======================== Async Service Lifetime Tests ========================

// What a callback-based async_*() request delivered. Shared with the callback so a request abandoned by
// shutdown(), and resumed when a later request restarts the service, never writes into dead state.
struct async_capture {
    std::atomic<bool> done{false};
    Http::Response response;  // only safe to read once done is set
};

// Issues a callback-based async GET whose response lands in a capture that outlives this call
std::shared_ptr<async_capture> async_get_captured(const std::string& url) {
    auto capture = std::make_shared<async_capture>();
    Http::async_get([capture](Http::Response&& response) {
        capture->response = std::move(response);
        capture->done.store(true, std::memory_order_release);
    }, url);
    return capture;
}

// Regression: the async service ran on a detached thread that nothing owned and that no caller could stop,
// so a request still in flight at process exit went on using the service io_context and the other statics
// of http.cpp while static destruction was destroying them. shutdown() now stops the service and joins its
// thread, and runs automatically at program exit.
TEST_F(HttpTest, AsyncService_Shutdown_StopsServiceAndLaterRequestRestartsIt) {
    local_echo_server server;

    auto first = async_get_captured(std::format("http://127.0.0.1:{}/first", server.port()));
    ASSERT_TRUE(wait_for_condition([&] { return first->done.load(std::memory_order_acquire); }, std::chrono::seconds(10)));
    EXPECT_EQ(first->response.result_text, "GET /first");
    EXPECT_TRUE(Http::is_running()) << "an async request did not start the service";

    Http::shutdown();
    EXPECT_FALSE(Http::is_running()) << "shutdown() left the service running";

    // The next request starts the service again instead of queueing on a stopped io_context
    auto second = async_get_captured(std::format("http://127.0.0.1:{}/second", server.port()));
    ASSERT_TRUE(wait_for_condition([&] { return second->done.load(std::memory_order_acquire); }, std::chrono::seconds(10)))
        << "request made after shutdown() never ran";
    EXPECT_EQ(second->response.result_text, "GET /second");
    EXPECT_TRUE(Http::is_running());
}

// shutdown() joins the service thread, so it must not wait for the requests that thread is running: it
// abandons them and returns instead of blocking on a slow server, which would stall program exit.
TEST_F(HttpTest, AsyncService_ShutdownWithRequestInFlight_ReturnsWithoutWaitingForIt) {
    local_echo_server server;
    auto capture = async_get_captured(std::format("http://127.0.0.1:{}/slow", server.port()));
    ASSERT_TRUE(wait_for_condition([&] { return server.slow_requests() > 0; }, std::chrono::seconds(5)))
        << "the request never reached the server";

    const auto begin = std::chrono::steady_clock::now();
    Http::shutdown();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();

    EXPECT_FALSE(Http::is_running());
    EXPECT_LT(elapsed_ms, local_echo_server::slow_response_delay.count() / 2)
        << "shutdown() waited for the in-flight request";
}

// shutdown() abandons requests in flight, and a later request restarts the service, which resumes them. Their
// callbacks must still never run: the caller may have destroyed what they use by then, such as the context of
// the executor a callback was to be posted to.
TEST_F(HttpTest, AsyncService_RequestAbandonedByShutdown_CallbackNotCalledAfterRestart) {
    local_echo_server server;
    const auto slow_url = std::format("http://127.0.0.1:{}/slow", server.port());
    auto abandoned_calls = std::make_shared<std::atomic<int>>(0);
    {
        executor_thread executor;
        Http::async_get(executor.ioc.get_executor(), [abandoned_calls](Http::Response&&) {
            abandoned_calls->fetch_add(1, std::memory_order_acq_rel);
        }, slow_url);
        Http::async_get([abandoned_calls](Http::Response&&) {
            abandoned_calls->fetch_add(1, std::memory_order_acq_rel);
        }, slow_url);
        ASSERT_TRUE(wait_for_condition([&] { return server.slow_requests() == 2; }, std::chrono::seconds(5)))
            << "the requests never reached the server";
        Http::shutdown();
    }   // The executor's context is gone from here on

    // Answered after the abandoned requests, so once its callback has run the restarted service has received theirs
    auto restarted = async_get_captured(slow_url);
    ASSERT_TRUE(wait_for_condition([&] { return restarted->done.load(std::memory_order_acquire); }, std::chrono::seconds(10)))
        << "request made after shutdown() never ran";
    EXPECT_EQ(restarted->response.result_text, "GET /slow");
    EXPECT_EQ(server.answered_requests(), 3);
    EXPECT_EQ(abandoned_calls->load(std::memory_order_acquire), 0) << "callbacks of requests shutdown() abandoned still ran";
}

// ======================== Async Callback Dispatch Tests ========================

// Runs the Http service with count callback threads for the scope, restarting it so the count applies
struct callback_threads_scope {
    std::size_t saved = Http::callback_threads();

    explicit callback_threads_scope(std::size_t count) {
        Http::shutdown();
        Http::set_callback_threads(count);
    }

    ~callback_threads_scope() {
        Http::shutdown();
        Http::set_callback_threads(saved);
    }
};

// A callback-based request on the shared service whose callback blocks until release(), or for 10 seconds so a
// stalled check fails its test instead of hanging it
class blocked_async_callback {
public:
    explicit blocked_async_callback(const std::string& url) {
        Http::async_get([state = state_](Http::Response&&) {
            state->in_callback.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!state->release.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            state->in_callback.store(false, std::memory_order_release);
        }, url);
    }

    // Releases the callback and waits for it to return, so it cannot hold up a later request
    ~blocked_async_callback() {
        release();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (in_callback() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool in_callback() const noexcept { return state_->in_callback.load(std::memory_order_acquire); }
    void release() noexcept { state_->release.store(true, std::memory_order_release); }

private:
    struct state {
        std::atomic<bool> in_callback{false};
        std::atomic<bool> release{false};
    };
    // Shared with the callback, so a callback that outlives this object never touches dead state
    std::shared_ptr<state> state_ = std::make_shared<state>();
};

// Regression: the callbacks of callback-based requests ran inline on the service's only I/O thread, so a callback
// that blocked kept every other callback-based request from even connecting. They now run on a callback thread.
TEST_F(HttpTest, AsyncCallback_BlockedCallback_DoesNotStallOtherRequestsIo) {
    const callback_threads_scope threads{1};
    local_echo_server server;
    blocked_async_callback blocked{std::format("http://127.0.0.1:{}/block", server.port())};
    ASSERT_TRUE(wait_for_condition([&] { return blocked.in_callback(); }, std::chrono::seconds(10)))
        << "the blocking callback never ran";

    auto other = async_get_captured(std::format("http://127.0.0.1:{}/other", server.port()));
    const bool answered = wait_for_condition([&] { return server.answered_requests() == 2; }, std::chrono::seconds(5));
    const bool still_blocked = blocked.in_callback();
    blocked.release();

    EXPECT_TRUE(still_blocked) << "the blocking callback returned early, so the second request was not tested against it";
    ASSERT_TRUE(answered) << "the second request's I/O stalled behind the blocked callback";
    // Its callback waited for the only callback thread, which the blocked callback has now released
    ASSERT_TRUE(wait_for_condition([&] { return other->done.load(std::memory_order_acquire); }, std::chrono::seconds(10)));
    EXPECT_EQ(other->response.result_text, "GET /other");
}

// With more callback threads, a blocked callback holds up only its own thread: other callbacks still run.
TEST_F(HttpTest, AsyncCallback_BlockedCallback_DoesNotStallOtherCallbacks_CallbackThreads) {
    const callback_threads_scope threads{2};
    EXPECT_EQ(Http::callback_threads(), 2u);

    local_echo_server server;
    blocked_async_callback blocked{std::format("http://127.0.0.1:{}/block", server.port())};
    ASSERT_TRUE(wait_for_condition([&] { return blocked.in_callback(); }, std::chrono::seconds(10)))
        << "the blocking callback never ran";

    auto other = async_get_captured(std::format("http://127.0.0.1:{}/other", server.port()));
    const bool other_done = wait_for_condition([&] { return other->done.load(std::memory_order_acquire); }, std::chrono::seconds(5));
    const bool still_blocked = blocked.in_callback();
    blocked.release();

    EXPECT_TRUE(still_blocked) << "the blocking callback returned early, so the second request was not tested against it";
    ASSERT_TRUE(other_done) << "the second request's callback stalled behind the blocked callback";
    EXPECT_EQ(other->response.result_text, "GET /other");
}

// A request given an executor posts its callback there: it runs on that executor's thread, and a callback blocking
// the service's only callback thread does not hold it up.
TEST_F(HttpTest, AsyncCallback_CallerExecutor_CallbackRunsThereWhileCallbackThreadIsBlocked) {
    const callback_threads_scope threads{1};
    local_echo_server server;
    executor_thread executor;
    blocked_async_callback blocked{std::format("http://127.0.0.1:{}/block", server.port())};
    ASSERT_TRUE(wait_for_condition([&] { return blocked.in_callback(); }, std::chrono::seconds(10)))
        << "the blocking callback never ran";

    struct executor_capture : async_capture {
        std::thread::id thread;  // only safe to read once done is set
    };
    auto capture = std::make_shared<executor_capture>();
    Http::async_post(executor.ioc.get_executor(), [capture](Http::Response&& response) {
        capture->thread = std::this_thread::get_id();
        capture->response = std::move(response);
        capture->done.store(true, std::memory_order_release);
    }, std::format("http://127.0.0.1:{}/executor", server.port()), "body");
    const bool done = wait_for_condition([&] { return capture->done.load(std::memory_order_acquire); }, std::chrono::seconds(5));
    const bool still_blocked = blocked.in_callback();
    blocked.release();

    EXPECT_TRUE(still_blocked) << "the blocking callback returned early, so the request was not tested against it";
    ASSERT_TRUE(done) << "the callback posted to the caller's executor stalled behind the blocked callback";
    EXPECT_EQ(capture->thread, executor.thread.get_id());
    EXPECT_EQ(capture->response.result_code, 200) << capture->response.reason;
    EXPECT_EQ(capture->response.result_text, "POST /executor body");
}

// Every callback of a request given an executor runs through it - an error response too, and through a strand -
// so a caller running that context on its own thread needs no synchronization with them.
TEST_F(HttpTest, AsyncCallback_CallerExecutor_ResponsesAndErrorsRunOnCallersThread) {
    local_echo_server server;
    boost::asio::io_context ioc;
    auto work = boost::asio::make_work_guard(ioc);
    int calls = 0;
    std::vector<std::thread::id> callback_threads;
    Http::Response ok_response;
    Http::Response error_response;
    const auto finish = [&] {
        callback_threads.push_back(std::this_thread::get_id());
        if (++calls == 2) {
            work.reset();
        }
    };
    Http::async_put(ioc.get_executor(), [&](Http::Response&& response) {
        ok_response = std::move(response);
        finish();
    }, std::format("http://127.0.0.1:{}/run", server.port()), "body");
    Http::async_get(boost::asio::make_strand(ioc), [&](Http::Response&& response) {
        error_response = std::move(response);
        finish();
    }, "http://[::1/");

    // Bounded, so a lost callback fails the test instead of hanging it
    ioc.run_for(std::chrono::seconds(10));

    ASSERT_EQ(calls, 2) << "not every callback ran on the caller's executor";
    EXPECT_EQ(callback_threads, (std::vector<std::thread::id>(2, std::this_thread::get_id())));
    EXPECT_EQ(ok_response.result_code, 200) << ok_response.reason;
    EXPECT_EQ(ok_response.result_text, "PUT /run body");
    EXPECT_EQ(error_response.result_code, 500);
    EXPECT_NE(error_response.reason.find("Invalid IPv6 literal"), std::string::npos) << error_response.reason;
}

// With no callback threads, callbacks run inline on the I/O thread instead of being queued for threads that do
// not exist.
TEST_F(HttpTest, AsyncCallback_ZeroCallbackThreads_CallbacksStillRun) {
    const callback_threads_scope threads{0};
    EXPECT_EQ(Http::callback_threads(), 0u);

    local_echo_server server;
    auto capture = async_get_captured(std::format("http://127.0.0.1:{}/inline", server.port()));
    ASSERT_TRUE(wait_for_condition([&] { return capture->done.load(std::memory_order_acquire); }, std::chrono::seconds(10)))
        << "callback never ran without callback threads";
    EXPECT_EQ(capture->response.result_text, "GET /inline");
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

std::shared_ptr<HttpStream> HttpTest::open_captured_stream(std::string url, std::shared_ptr<stream_capture> capture,
                                                           boost::asio::any_io_executor executor) {
    // Callbacks share ownership so a stream that outlives its test never touches a dead capture. Each records its
    // event before the atomics a test waits on, so the event is visible once the wait ends.
    auto stream = std::make_shared<HttpStream>(
        std::move(executor),
        std::move(url),
        [capture] {
            capture->events += 'C';
            capture->connects.fetch_add(1, std::memory_order_release);
            capture->connected.store(true, std::memory_order_release);
        },
        [capture] {
            capture->events += 'D';
            capture->disconnects.fetch_add(1, std::memory_order_release);
            capture->disconnected.store(true, std::memory_order_release);
        },
        [capture](const char* data, size_t size) { capture->payloads.emplace_back(data, size); },
        [capture](std::string err) {
            capture->events += 'E';
            capture->errors.push_back(std::move(err));
        }
    );
    stream->open();
    return stream;
}

std::shared_ptr<HttpTest::stream_capture> HttpTest::stream_scripted_response(std::vector<std::string> segments,
                                                                             std::chrono::milliseconds pause,
                                                                             boost::asio::any_io_executor executor) {
    scripted_response_server server{std::move(segments), pause};
    auto capture = std::make_shared<stream_capture>();
    auto stream = open_captured_stream(server.port(), capture, std::move(executor));
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

// Regression: the stream request's Host header left out the port, so a stream to a non-default port named the
// wrong origin.
TEST_F(HttpTest, HttpStream_HostHeader_IncludesNonDefaultPort) {
    local_echo_server server;
    auto capture = std::make_shared<stream_capture>();
    auto stream = open_captured_stream(std::format("http://127.0.0.1:{}/host", server.port()), capture);
    ASSERT_TRUE(wait_for_condition([&] { return capture->disconnected.load(std::memory_order_acquire); }, std::chrono::seconds(10)))
        << "stream did not end with the response";

    std::string body;
    for (const auto& payload : capture->payloads) {
        body += payload;
    }
    EXPECT_EQ(body, std::format("GET /host host=127.0.0.1:{}", server.port()));
    EXPECT_TRUE(capture->errors.empty()) << capture->errors.front();
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

// Header of a chunked text/event-stream response
const std::string chunked_sse_header = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n";

void HttpTest::expect_blocked_callback_does_not_stall(const open_stream_fn& open_other) {
    scripted_response_server blocking_server{{chunked_sse_header + http_chunk("data: block\n\n"), "0\r\n\r\n"}, std::chrono::milliseconds(20)};
    scripted_response_server other_server{{chunked_sse_header + http_chunk("data: other\n\n"), "0\r\n\r\n"}, std::chrono::milliseconds(20)};

    struct blocking_state {
        std::atomic<bool> in_callback{false};
        std::atomic<bool> release{false};
        std::atomic<bool> disconnected{false};
    };
    // Shared with the callbacks so a stream that outlives this check never touches dead state
    auto blocking = std::make_shared<blocking_state>();
    auto blocking_stream = std::make_shared<HttpStream>(
        std::format("http://127.0.0.1:{}/stream", blocking_server.port()),
        [] {},
        [blocking] { blocking->disconnected.store(true, std::memory_order_release); },
        [blocking](const char*, size_t) {
            blocking->in_callback.store(true, std::memory_order_release);
            // Bounded so a stalled second stream fails the test instead of hanging it
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!blocking->release.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            blocking->in_callback.store(false, std::memory_order_release);
        },
        [](std::string) {});
    blocking_stream->open();
    ASSERT_TRUE(wait_for_condition([&] { return blocking->in_callback.load(std::memory_order_acquire); }, std::chrono::seconds(10)))
        << "the blocking stream's onData never ran";

    auto capture = std::make_shared<stream_capture>();
    auto other_stream = open_other(other_server.port(), capture);
    const bool other_done = wait_for_condition([&] { return capture->disconnected.load(std::memory_order_acquire); }, std::chrono::seconds(5));
    const bool still_blocked = blocking->in_callback.load(std::memory_order_acquire);
    blocking->release.store(true, std::memory_order_release);

    EXPECT_TRUE(still_blocked) << "the blocking callback returned early, so the second stream was not tested against it";
    EXPECT_TRUE(wait_for_condition([&] { return blocking->disconnected.load(std::memory_order_acquire); }, std::chrono::seconds(10)));
    ASSERT_TRUE(other_done) << "the second stream stalled behind the blocked callback";
    EXPECT_EQ(capture->payloads, (std::vector<std::string>{"other"}));
    EXPECT_TRUE(capture->errors.empty()) << capture->errors.front();
}

// Regression: every HttpStream and all their callbacks ran on one service thread, so a callback that blocked
// stopped every other stream from receiving data. A stream on its own executor is not held up by it.
TEST_F(HttpTest, HttpStream_BlockedCallback_DoesNotStallStreamOnOtherExecutor) {
    executor_thread other_executor;
    expect_blocked_callback_does_not_stall([&](uint16_t port, std::shared_ptr<stream_capture> capture) {
        return open_captured_stream(port, std::move(capture), other_executor.ioc.get_executor());
    });
}

// With more service threads, a callback that blocks holds up only its own stream on the shared service.
TEST_F(HttpTest, HttpStream_BlockedCallback_DoesNotStallOtherStreams_ServiceThreads) {
    struct restore_service_threads {
        std::size_t count = HttpStream::service_threads();
        ~restore_service_threads() { HttpStream::set_service_threads(count); }
    } restore;

    // The thread count applies when the service starts
    HttpStream::shutdown();
    HttpStream::set_service_threads(2);
    EXPECT_EQ(HttpStream::service_threads(), 2u);

    expect_blocked_callback_does_not_stall([&](uint16_t port, std::shared_ptr<stream_capture> capture) {
        return open_captured_stream(port, std::move(capture));
    });
}

// Several streams on the shared service each run their own session: all of them connect, receive their own
// event and end, without one holding up the others.
// This used to open three streams against https://stream.wikimedia.org and wait for all three to connect,
// which made it depend on a live endpoint's tolerance for concurrent connections from one address - it timed
// out on CI while the single-stream tests against the same endpoint passed.
TEST_F(HttpTest, HttpStream_MultipleStreams) {
    constexpr int stream_count = 3;

    // One server per stream: scripted_response_server answers the connections it accepts in turn, so a single
    // shared one would serialize the streams instead of running them at the same time
    std::vector<std::unique_ptr<scripted_response_server>> servers;
    std::vector<std::shared_ptr<stream_capture>> captures;
    std::vector<std::shared_ptr<HttpStream>> streams;

    for (int i = 0; i < stream_count; ++i) {
        servers.push_back(std::make_unique<scripted_response_server>(
            std::vector<std::string>{chunked_sse_header + http_chunk(std::format("data: stream{}\n\n", i)), "0\r\n\r\n"},
            std::chrono::milliseconds(20)));
        auto capture = std::make_shared<stream_capture>();
        streams.push_back(open_captured_stream(servers.back()->port(), capture));
        captures.push_back(std::move(capture));
    }

    for (int i = 0; i < stream_count; ++i) {
        EXPECT_TRUE(wait_for_condition([&] { return captures[i]->connected.load(std::memory_order_acquire); },
                                        std::chrono::seconds(10))) << "stream " << i << " never connected";
    }

    // The final chunk ends each response, so the streams disconnect on their own
    for (int i = 0; i < stream_count; ++i) {
        EXPECT_TRUE(wait_for_condition([&] { return captures[i]->disconnected.load(std::memory_order_acquire); },
                                        std::chrono::seconds(10))) << "stream " << i << " never disconnected";
    }

    // Each stream received its own event and nothing from its neighbours
    for (int i = 0; i < stream_count; ++i) {
        EXPECT_EQ(captures[i]->payloads, (std::vector<std::string>{std::format("stream{}", i)}));
        EXPECT_TRUE(captures[i]->errors.empty()) << captures[i]->errors.front();
    }
}

// A stream on a multi-threaded executor runs its session and close() through one strand: events arrive whole and
// in order, close() still interrupts a pending read, and the shared service is never started for it.
TEST_F(HttpTest, HttpStream_ThreadPoolExecutor_DecodesEventsAndCloses) {
    HttpStream::shutdown();
    boost::asio::thread_pool pool{4};

    auto capture = stream_scripted_response({
        chunked_sse_header,
        http_chunk("data: first\n\n"),
        // An event split across chunks
        http_chunk("data: sec"),
        http_chunk("ond\n\n"),
        "0\r\n\r\n",
    }, std::chrono::milliseconds(20), pool.get_executor());
    ASSERT_TRUE(capture->disconnected.load(std::memory_order_acquire)) << "stream did not end after the final chunk";
    EXPECT_EQ(capture->payloads, (std::vector<std::string>{"first", "second"}));
    EXPECT_TRUE(capture->errors.empty()) << capture->errors.front();

    scripted_response_server idle_server{{chunked_sse_header, "0\r\n\r\n"}, std::chrono::seconds(30)};
    auto idle_capture = std::make_shared<stream_capture>();
    auto stream = open_captured_stream(idle_server.port(), idle_capture, pool.get_executor());
    ASSERT_TRUE(wait_for_condition([&] { return idle_capture->connected.load(std::memory_order_acquire); }, std::chrono::seconds(10)));
    // Let the session start waiting on its body read
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stream->close();
    ASSERT_TRUE(wait_for_condition([&] { return idle_capture->disconnected.load(std::memory_order_acquire); }, std::chrono::seconds(1)))
        << "close() did not interrupt the pending read";
    EXPECT_TRUE(idle_capture->errors.empty()) << idle_capture->errors.front();

    EXPECT_FALSE(HttpStream::is_running()) << "a stream on its own executor started the shared service";
}

// Regression: shutdown() never cleared the flag marking the service thread as started, so a stream opened after
// shutdown() was queued on the stopped io_context and never connected.
TEST_F(HttpTest, HttpStream_OpenAfterShutdown_RestartsService) {
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: 13\r\n\r\ndata: event\n\n";
    ASSERT_TRUE(stream_scripted_response({response})->disconnected.load(std::memory_order_acquire));
    EXPECT_TRUE(HttpStream::is_running());

    HttpStream::shutdown();
    EXPECT_FALSE(HttpStream::is_running());

    auto capture = stream_scripted_response({response});
    EXPECT_TRUE(HttpStream::is_running());
    ASSERT_TRUE(capture->disconnected.load(std::memory_order_acquire)) << "stream opened after shutdown() never ran";
    EXPECT_EQ(capture->payloads, (std::vector<std::string>{"event"}));
    EXPECT_TRUE(capture->errors.empty()) << capture->errors.front();
}

// Loopback server that serves all the connections it accepts at the same time and counts them. Each connection is
// sent response once its request has arrived, then held open until the client closes it; with an empty response
// the server never sends anything, not even its side of a TLS handshake.
class counting_stream_server {
public:
    explicit counting_stream_server(std::string response)
        : response_(std::move(response))
        , acceptor_(ioc_, {boost::asio::ip::address_v4::loopback(), 0})
        , port_(acceptor_.local_endpoint().port()) {
        boost::asio::co_spawn(ioc_, accept_loop(), boost::asio::detached);
        thread_ = std::thread([this] { ioc_.run(); });
    }

    ~counting_stream_server() {
        ioc_.stop();
        thread_.join();
    }

    uint16_t port() const noexcept { return port_; }
    int accepted() const noexcept { return accepted_.load(std::memory_order_acquire); }

private:
    boost::asio::awaitable<void> accept_loop() {
        for (;;) {
            auto [ec, socket] = co_await acceptor_.async_accept(boost::asio::as_tuple(boost::asio::use_awaitable));
            if (ec) {
                co_return;
            }
            accepted_.fetch_add(1, std::memory_order_release);
            boost::asio::co_spawn(ioc_, serve(std::move(socket)), boost::asio::detached);
        }
    }

    boost::asio::awaitable<void> serve(boost::asio::ip::tcp::socket socket) {
        const auto token = boost::asio::as_tuple(boost::asio::use_awaitable);
        if (!response_.empty()) {
            std::string request;
            if (auto [ec, n] = co_await boost::asio::async_read_until(socket, boost::asio::dynamic_buffer(request), "\r\n\r\n", token); ec) {
                co_return;
            }
            if (auto [ec, n] = co_await boost::asio::async_write(socket, boost::asio::buffer(response_), token); ec) {
                co_return;
            }
        }
        std::array<char, 1024> discard;
        for (;;) {
            if (auto [ec, n] = co_await socket.async_read_some(boost::asio::buffer(discard), token); ec) {
                co_return;
            }
        }
    }

    boost::asio::io_context ioc_;
    std::string response_;
    boost::asio::ip::tcp::acceptor acceptor_;
    uint16_t port_;
    std::atomic<int> accepted_{0};
    std::thread thread_;
};

// A complete text/event-stream response carrying the single event "event"
const std::string single_event_response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: 13\r\n\r\ndata: event\n\n";

// Regression: every open() started a session of its own, so open() on an open stream connected a second time beside
// the first, and close() could cancel only the session that had registered its socket last.
TEST_F(HttpTest, HttpStream_OpenWhileOpen_KeepsOneSession) {
    counting_stream_server server{chunked_sse_header};
    auto capture = std::make_shared<stream_capture>();
    auto stream = open_captured_stream(server.port(), capture);
    stream->open();  // While connecting
    ASSERT_TRUE(wait_for_condition([&] { return capture->connected.load(std::memory_order_acquire); }, std::chrono::seconds(10)));
    stream->open();  // While connected

    // Give a second session time to connect
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(server.accepted(), 1);
    EXPECT_EQ(stream->status(), HttpStream::Status::CONNECTED);

    stream->close();
    ASSERT_TRUE(wait_for_condition([&] { return capture->disconnected.load(std::memory_order_acquire); }, std::chrono::seconds(10)));
    // Give a second session time to end as well
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_EQ(capture->connects.load(std::memory_order_acquire), 1);
    ASSERT_EQ(capture->disconnects.load(std::memory_order_acquire), 1);
    EXPECT_EQ(capture->events, "CD");
    EXPECT_TRUE(capture->errors.empty()) << capture->errors.front();
}

// Regression: open() cleared the request of an earlier close(), and close() could cancel only a socket that a session
// had registered after connecting, so every open() connected - even ones closed before their session had started.
TEST_F(HttpTest, HttpStream_ClosedBeforeSessionStarts_NeverConnects) {
    counting_stream_server server{single_event_response};
    // Run only after the calls below, so no session can start in between
    boost::asio::io_context ioc;
    auto capture = std::make_shared<stream_capture>();

    auto stream = open_captured_stream(server.port(), capture, ioc.get_executor());
    stream->close();
    EXPECT_EQ(stream->status(), HttpStream::Status::DISCONNECTED);
    stream->open();
    stream->close();
    stream->open();
    EXPECT_EQ(stream->status(), HttpStream::Status::CONNECTING);

    // Returns once the last session has ended, leaving nothing to run
    ioc.run_for(std::chrono::seconds(10));

    ASSERT_TRUE(capture->disconnected.load(std::memory_order_acquire)) << "the last session did not end";
    // One onDisconnected for every open(), but only the last one, never closed, connects
    EXPECT_EQ(capture->events, "DDCD");
    EXPECT_EQ(capture->payloads, (std::vector<std::string>{"event"}));
    EXPECT_EQ(server.accepted(), 1);
    EXPECT_EQ(stream->status(), HttpStream::Status::DISCONNECTED);
    EXPECT_TRUE(capture->errors.empty()) << capture->errors.front();
}

// Regression: close() followed at once by open() left the closing session running beside the new one. open() cleared
// the close request, so the old session reported its cancelled read as an error, and its onDisconnected could come
// after the new session had connected.
TEST_F(HttpTest, HttpStream_CloseThenOpen_EndsPreviousSessionFirst) {
    counting_stream_server server{chunked_sse_header};
    auto capture = std::make_shared<stream_capture>();
    auto stream = open_captured_stream(server.port(), capture);
    ASSERT_TRUE(wait_for_condition([&] { return capture->connects.load(std::memory_order_acquire) == 1; }, std::chrono::seconds(10)));
    // Let the session start waiting on its body read
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    stream->close();
    stream->open();
    ASSERT_TRUE(wait_for_condition([&] { return capture->connects.load(std::memory_order_acquire) == 2; }, std::chrono::seconds(10)));
    // A late end of the first session would overwrite the second session's status
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(stream->status(), HttpStream::Status::CONNECTED);
    EXPECT_EQ(server.accepted(), 2);

    stream->close();
    ASSERT_TRUE(wait_for_condition([&] { return capture->disconnects.load(std::memory_order_acquire) == 2; }, std::chrono::seconds(10)));
    EXPECT_EQ(capture->events, "CDCD");
    EXPECT_TRUE(capture->errors.empty()) << capture->errors.front();
}

// Regression: a session registered its socket for close() to cancel only once it had connected and finished the TLS
// handshake, so close() could not interrupt the DNS lookup, the connect or the handshake. Against a server that never
// answered the handshake, the closed stream stayed until the 30 s handshake timeout.
TEST_F(HttpTest, HttpStream_Close_InterruptsTlsHandshake) {
    counting_stream_server server{""};
    auto capture = std::make_shared<stream_capture>();
    auto stream = open_captured_stream(std::format("https://127.0.0.1:{}/stream", server.port()), capture);
    ASSERT_TRUE(wait_for_condition([&] { return server.accepted() == 1; }, std::chrono::seconds(10)));
    // Let the session start waiting on the server's side of the handshake
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto begin = std::chrono::steady_clock::now();
    stream->close();
    ASSERT_TRUE(wait_for_condition([&] { return capture->disconnected.load(std::memory_order_acquire); }, std::chrono::seconds(10)))
        << "close() did not interrupt the TLS handshake";
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();

    EXPECT_LT(elapsed_ms, 1000) << "close() waited for the handshake to time out";
    EXPECT_EQ(capture->events, "D");
    EXPECT_TRUE(capture->errors.empty()) << capture->errors.front();
}

// open() from onDisconnected starts the next session once the one that ended has finished
TEST_F(HttpTest, HttpStream_OpenFromOnDisconnected_Reconnects) {
    counting_stream_server server{single_event_response};
    auto capture = std::make_shared<stream_capture>();
    auto weak_stream = std::make_shared<std::weak_ptr<HttpStream>>();
    auto stream = std::make_shared<HttpStream>(
        std::format("http://127.0.0.1:{}/stream", server.port()),
        [capture] { capture->events += 'C'; },
        [capture, weak_stream] {
            capture->events += 'D';
            // Reopen once, from inside the callback
            if (capture->disconnects.fetch_add(1, std::memory_order_acq_rel) == 0) {
                if (auto self = weak_stream->lock()) {
                    self->open();
                }
            }
        },
        [capture](const char* data, size_t size) { capture->payloads.emplace_back(data, size); },
        [capture](std::string err) { capture->errors.push_back(std::move(err)); });
    *weak_stream = stream;
    stream->open();

    ASSERT_TRUE(wait_for_condition([&] { return capture->disconnects.load(std::memory_order_acquire) == 2; }, std::chrono::seconds(10)));
    EXPECT_EQ(capture->events, "CDCD");
    EXPECT_EQ(capture->payloads, (std::vector<std::string>{"event", "event"}));
    EXPECT_EQ(server.accepted(), 2);
    EXPECT_EQ(stream->status(), HttpStream::Status::DISCONNECTED);
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
