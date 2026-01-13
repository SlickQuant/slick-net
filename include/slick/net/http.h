// The MIT License (MIT)
// Copyright (c) 2025-2026 SlickQuant
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <atomic>
#include <cstdlib>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <csignal>
#include <thread>

#define WIN32_LEAN_AND_MEAN

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/as_tuple.hpp>
#include <slick/queue.h>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace asio = boost::asio;           // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl;       // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

// Logging function placeholders
// User can assign their own log functions by defining these macros before including this file
// The log functions must support fmt-style formatting e.g. LOG_INFO("Hello {}", "world");

#ifndef LOG_DEBUG
#define LOG_DEBUG(...) do {} while(0)
#endif
#ifndef LOG_INFO  
#define LOG_INFO(...) do {} while(0)
#endif
#ifndef LOG_WARN
#define LOG_WARN(...) do {} while(0)
#endif
#ifndef LOG_ERROR
#define LOG_ERROR(...) do {} while(0)
#endif
#ifndef LOG_TRACE
#define LOG_TRACE(...) do {} while(0)
#endif

namespace slick::net {

class Http
{   
public:
    struct Response
    {
        uint32_t result_code = 0;
        std::string result_text;

        bool is_ok() const noexcept {
            return result_code >= 200 && result_code < 300;
        }
    };
    static Response get(std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static Response post(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static Response put(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static Response patch(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static Response del(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});

    static void async_get(std::function<void(Response&&)> on_response, std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_post(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_put(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_patch(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_del(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data = "", std::vector<std::pair<std::string, std::string>>&& headers = {});

    static asio::awaitable<Response> async_get(std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static asio::awaitable<Response> async_post(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static asio::awaitable<Response> async_put(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static asio::awaitable<Response> async_patch(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static asio::awaitable<Response> async_del(std::string_view url, std::string_view data = "", std::vector<std::pair<std::string, std::string>>&& headers = {});

private:
    static std::tuple<std::string, std::string, std::string, bool> parse_url(std::string_view url);
    static asio::awaitable<void> do_session(
        std::string url,
        http::verb method,
        std::function<void(Response&&)> on_response,
        std::vector<std::pair<std::string, std::string>> headers = {},
        std::string body = "",
        int version = 11
    );
    static asio::awaitable<void> do_session_ssl(
        std::string host,
        std::string target,
        std::string port,
        http::verb method,
        std::function<void(Response&&)> on_response,
        std::vector<std::pair<std::string, std::string>> headers,
        std::string body,
        int version
    );
    static asio::awaitable<void> do_session_plain(
        std::string host,
        std::string target,
        std::string port,
        http::verb method,
        std::function<void(Response&&)> on_response,
        std::vector<std::pair<std::string, std::string>> headers,
        std::string body,
        int version
    );
    static asio::awaitable<Response> do_session_awaitable(
        std::string url,
        http::verb method,
        std::vector<std::pair<std::string, std::string>> headers = {},
        std::string body = "",
        int version = 11
    );
    static asio::awaitable<Response> do_session_ssl_awaitable(
        std::string host,
        std::string target,
        std::string port,
        http::verb method,
        std::vector<std::pair<std::string, std::string>> headers,
        std::string body,
        int version
    );
    static asio::awaitable<Response> do_session_plain_awaitable(
        std::string host,
        std::string target,
        std::string port,
        http::verb method,
        std::vector<std::pair<std::string, std::string>> headers,
        std::string body,
        int version
    );

private:
    static ssl::context init();
    static void ensure_service_thread();

private:
    friend class session;
    friend class HttpStream;
    static asio::io_context ioc_;
    static asio::io_context async_ioc_;

    struct service_info
    {
        uint32_t async_requests_ = 0;
        bool service_running_ = false;
    };
    static std::atomic<service_info> async_service_;
    static ssl::context ctx_;
};

// HTTP Stream class for Server-Sent Events (SSE) and chunked response streaming
class HttpStream : public std::enable_shared_from_this<HttpStream>
{
public:
    explicit HttpStream(
        std::string url,
        std::function<void()> &&onConnectedCallback,
        std::function<void()> &&onDisconnectedCallback,
        std::function<void(const char*, std::size_t)> &&onDataCallback,
        std::function<void(std::string err)> &&onErrorCallback,
        std::vector<std::pair<std::string, std::string>>&& headers = {}
    );

    ~HttpStream() = default;

    // Start the streaming connection
    void open();

    // Close the connection
    void close();

    // Check if running
    static bool is_running() noexcept
    {
        return run_.load(std::memory_order_relaxed);
    }

    // Shutdown all streams
    static void shutdown();

    enum class Status : uint8_t
    {
        CONNECTING,
        CONNECTED,
        DISCONNECTED,
    };

    Status status() const noexcept
    {
        return status_.load(std::memory_order_relaxed);
    }

private:
    asio::awaitable<void> do_stream_session();
    asio::awaitable<void> do_stream_session_ssl();
    asio::awaitable<void> do_stream_session_plain();
    void parse_sse_chunk(const char* data, size_t size);

private:
    static asio::io_context ioc_;
    static std::thread service_thread_;
    static std::atomic_bool init_service_thread_;
    static std::atomic_bool run_;

    std::string url_;
    std::string host_;
    std::string target_;
    std::string port_;
    bool use_ssl_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;
    std::function<void(const char*, std::size_t)> on_data_;
    std::function<void(std::string err)> on_error_;
    std::atomic<Status> status_{ Status::DISCONNECTED };
    std::atomic_bool should_close_{false};
    std::string sse_buffer_;  // For incomplete SSE events
};


// ---------------------------------------------------- Implementation ----------------------------------------------------

inline asio::io_context Http::ioc_;
inline asio::io_context Http::async_ioc_;
inline std::atomic<Http::service_info> Http::async_service_;
inline ssl::context Http::ctx_ = Http::init();

inline ssl::context Http::init() {
    ssl::context ctx{ssl::context::tlsv12_client};
    // Verify the remote server's certificate
    ctx.set_verify_mode(ssl::verify_none);
    return ctx;
}

inline asio::awaitable<void> Http::do_session(
    std::string url,
    http::verb method,
    std::function<void(Response&&)> on_response,
    std::vector<std::pair<std::string, std::string>> headers,
    std::string body,
    int version)
{
    auto [host, target, port, use_ssl] = parse_url(url);

    if (use_ssl) {
        return do_session_ssl(host, target, port, method, on_response, headers, body, version);
    } else {
        return do_session_plain(host, target, port, method, on_response, headers, body, version);
    }
}

inline asio::awaitable<void> Http::do_session_ssl(
    std::string host,
    std::string target,
    std::string port,
    http::verb method,
    std::function<void(Response&&)> on_response,
    std::vector<std::pair<std::string, std::string>> headers,
    std::string body,
    int version)
{
    auto response = co_await do_session_ssl_awaitable(host, target, port, method, headers, body, version);
    on_response(std::move(response));
    co_return;
}

inline asio::awaitable<void> Http::do_session_plain(
    std::string host,
    std::string target,
    std::string port,
    http::verb method,
    std::function<void(Response&&)> on_response,
    std::vector<std::pair<std::string, std::string>> headers,
    std::string body,
    int version)
{
    auto response = co_await do_session_plain_awaitable(host, target, port, method, headers, body, version);
    on_response(std::move(response));
    co_return;
}

inline asio::awaitable<Http::Response> Http::do_session_plain_awaitable(
    std::string host,
    std::string target,
    std::string port,
    http::verb method,
    std::vector<std::pair<std::string, std::string>> headers,
    std::string body,
    int version)
{
    auto executor = co_await asio::this_coro::executor;
    auto resolver = asio::ip::tcp::resolver{ executor };
    auto stream   = beast::tcp_stream{ executor };

    // Look up the domain name
    auto const results = co_await resolver.async_resolve(host, port);

    // Set the timeout.
    stream.expires_after(std::chrono::seconds(30));

    // Make the connection on the IP address we get from a lookup
    co_await stream.async_connect(results);

    // Set up an HTTP request message
    http::request<http::string_body> req{ method, target, version };
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    // Set headers
    for (auto &header_pair : headers) {
        req.set(header_pair.first, header_pair.second);
    }

    // Set request body if provided
    if (!body.empty()) {
        req.body() = body;
        req.prepare_payload();
    }

    // Set the timeout.
    stream.expires_after(std::chrono::seconds(30));

    // Send the HTTP request to the remote host
    co_await http::async_write(stream, req);

    // This buffer is used for reading and must be persisted
    beast::flat_buffer buffer;

    // Declare a container to hold the response
    http::response<http::dynamic_body> res;

    // Receive the HTTP response
    co_await http::async_read(stream, buffer, res);

    Response response;
    if (res.result() == http::status::ok ||
        (res.result_int() >= 200 && res.result_int() < 300)) {
        response.result_code = static_cast<uint32_t>(res.result_int());
        response.result_text = beast::buffers_to_string(res.body().data());
    }
    else {
        response.result_code = static_cast<uint32_t>(res.result_int());
        response.result_text = std::string(res.reason());
    }

    // Set the timeout.
    stream.expires_after(std::chrono::seconds(30));

    // Gracefully close the socket
    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    // not_connected happens sometimes, so don't bother reporting it.
    if(ec && ec != beast::errc::not_connected) {
        LOG_ERROR("Socket shutdown error: {}", ec.message());
    }

    co_return response;
}

inline asio::awaitable<Http::Response> Http::do_session_ssl_awaitable(
    std::string host,
    std::string target,
    std::string port,
    http::verb method,
    std::vector<std::pair<std::string, std::string>> headers,
    std::string body,
    int version)
{
    auto executor = co_await asio::this_coro::executor;
    auto resolver = asio::ip::tcp::resolver{ executor };
    auto stream   = ssl::stream<beast::tcp_stream>{ executor, Http::ctx_ };

    // Set SNI Hostname (many hosts need this to handshake successfully)
    if(!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
    {
        beast::error_code ec{
            static_cast<int>(::ERR_get_error()),
            asio::error::get_ssl_category()};
        LOG_ERROR("Error setting SNI hostname: {}", ec.message());
        co_return Response{5000, std::format("Error setting SNI hostname: {}", ec.message())};
    }

    // Look up the domain name
    auto const results = co_await resolver.async_resolve(host, port);

    // Set the timeout.
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));

    // Make the connection on the IP address we get from a lookup
    co_await beast::get_lowest_layer(stream).async_connect(results);

    // Set the timeout.
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));

    // Perform the SSL handshake
    co_await stream.async_handshake(ssl::stream_base::client);

    // Set up an HTTP request message
    http::request<http::string_body> req{ method, target, version };
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    // Set headers
    for (auto &header_pair : headers) {
        req.set(header_pair.first, header_pair.second);
    }

    // Set request body if provided
    if (!body.empty()) {
        req.body() = body;
        req.prepare_payload();
    }

    // Set the timeout.
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));

    // Send the HTTP request to the remote host
    co_await http::async_write(stream, req);

    // This buffer is used for reading and must be persisted
    beast::flat_buffer buffer;

    // Declare a container to hold the response
    http::response<http::dynamic_body> res;

    // Receive the HTTP response
    co_await http::async_read(stream, buffer, res);

    Response response;
    if (res.result() == http::status::ok ||
        (res.result_int() >= 200 && res.result_int() < 300)) {
        response.result_code = static_cast<uint32_t>(res.result_int());
        response.result_text = beast::buffers_to_string(res.body().data());
    }
    else {
        response.result_code = static_cast<uint32_t>(res.result_int());
        response.result_text = std::string(res.reason());
    }

    // Set the timeout.
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));

    // Gracefully close the stream - do not threat every error as an exception!
    auto [ec] = co_await stream.async_shutdown(asio::as_tuple);

    // ssl::error::stream_truncated, also known as an SSL "short read",
    // indicates the peer closed the connection without performing the
    // required closing handshake (for example, Google does this to
    // improve performance). Generally this can be a security issue,
    // but if your communication protocol is self-terminated (as
    // it is with both HTTP and WebSocket) then you may simply
    // ignore the lack of close_notify.
    //
    // https://github.com/boostorg/beast/issues/38
    //
    // https://security.stackexchange.com/questions/91435/how-to-handle-a-malicious-ssl-tls-shutdown
    //
    // When a short read would cut off the end of an HTTP message,
    // Beast returns the error beast::http::error::partial_message.
    // Therefore, if we see a short read here, it has occurred
    // after the message has been completed, so it is safe to ignore it.

    if(ec && ec != asio::ssl::error::stream_truncated) {
        LOG_ERROR("SSL shutdown error: {}", ec.message());
    }

    co_return response;
}

inline asio::awaitable<Http::Response> Http::do_session_awaitable(
    std::string url,
    http::verb method,
    std::vector<std::pair<std::string, std::string>> headers,
    std::string body,
    int version)
{
    auto [host, target, port, use_ssl] = parse_url(url);

    if (use_ssl) {
        return do_session_ssl_awaitable(host, target, port, method, headers, body, version);
    } else {
        return do_session_plain_awaitable(host, target, port, method, headers, body, version);
    }
}

inline std::tuple<std::string, std::string, std::string, bool> Http::parse_url(std::string_view url)
{
    std::string host;
    std::string target;
    uint_fast16_t port = (uint_fast16_t)-1;
    bool use_ssl = true;  // Default to SSL

    std::string protoco("https");
    auto pos = url.find("://");
    if (pos == std::string::npos)
    {
        pos = url.find("/");
        if (pos == std::string::npos)
        {
            host = std::string(url);
            target = "/";
        }
        else
        {
            host = std::string(url.substr(0, pos));
            target = std::string(url.substr(pos));
        }
    }
    else
    {
        protoco = std::string(url.substr(0, pos));
        auto host_begin = pos + 3;
        pos = url.find("/", host_begin);
        if (pos == std::string::npos)
        {
            host = std::string(url.substr(host_begin));
            target = "/";
        }
        else
        {
            host = std::string(url.substr(host_begin, pos - host_begin));
            target = std::string(url.substr(pos));
        }
    }

    pos = host.find(':');
    if (pos != 3 && pos != 4 && pos != std::string::npos)
    {
        port = std::stoi(std::string(host.substr(pos + 1)));
        host = std::string(host.substr(0, pos));
    }

    if (port == (uint_fast16_t)-1)
    {
        port = (protoco == "http") ? 80 : 443;
    }

    // Determine if SSL should be used
    use_ssl = (protoco == "https");

    return {host, target, std::to_string(port), use_ssl};
}

inline Http::Response Http::get(std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers)
{
    Response res;
    ioc_.restart();
    asio::co_spawn(
        ioc_,
        do_session(std::string(url), http::verb::get, [&res](Response&& response) {
            res = std::move(response);
        }, std::move(headers)),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    LOG_ERROR("Http get error: {}", e.what());
                }
            }
        });
    ioc_.run();
    return res;
}

inline Http::Response Http::post(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers)
{
    Response res;
    ioc_.restart();
    asio::co_spawn(
        ioc_,
        do_session(std::string(url), http::verb::post, [&res](Response&& response) {
            res = std::move(response);
        }, std::move(headers), std::string(data)),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    LOG_ERROR("Http post error: {}", e.what());
                }
            }
        });
    ioc_.run();
    return res;
}

inline Http::Response Http::put(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers)
{
    Response res;
    ioc_.restart();
    asio::co_spawn(
        ioc_,
        do_session(std::string(url), http::verb::put, [&res](Response&& response) {
            res = std::move(response);
        }, std::move(headers), std::string(data)),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    LOG_ERROR("Http put error: {}", e.what());
                }
            }
        });
    ioc_.run();
    return res;
}

inline Http::Response Http::patch(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers)
{
    Response res;
    ioc_.restart();
    asio::co_spawn(
        ioc_,
        do_session(std::string(url), http::verb::patch, [&res](Response&& response) {
            res = std::move(response);
        }, std::move(headers), std::string(data)),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    LOG_ERROR("Http patch error: {}", e.what());
                }
            }
        });
    ioc_.run();
    return res;
}

inline Http::Response Http::del(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers)
{
    Response res;
    ioc_.restart();
    asio::co_spawn(
        ioc_,
        do_session(std::string(url), http::verb::delete_, [&res](Response&& response) {
            res = std::move(response);
        }, std::move(headers), std::string(data)),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    LOG_ERROR("Http delete error: {}", e.what());
                }
            }
        });
    ioc_.run();
    return res;
}

inline void Http::async_get(std::function<void(Response&&)> on_response, std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers)
{
    ensure_service_thread();
    asio::co_spawn(
        async_ioc_,
        do_session(std::string(url), http::verb::get,
            [on_response = std::move(on_response)](Response&& response) mutable {
                on_response(std::move(response));
                auto svc_info = async_service_.load(std::memory_order_relaxed);
                service_info update;
                do {
                    assert(svc_info.async_requests_ > 0);
                    update = svc_info;
                    --update.async_requests_;
                } while (!async_service_.compare_exchange_weak(svc_info, update, std::memory_order_acq_rel, std::memory_order_relaxed));
            },
            std::move(headers)),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    LOG_ERROR("Http async_get error: {}", e.what());
                }
            }
    });
}

inline void Http::async_post(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers)
{
    ensure_service_thread();
    asio::co_spawn(
        async_ioc_,
        do_session(std::string(url), http::verb::post,
            [on_response = std::move(on_response)](Response&& response) mutable {
                on_response(std::move(response));
                auto svc_info = async_service_.load(std::memory_order_relaxed);
                service_info update;
                do {
                    assert(svc_info.async_requests_ > 0);
                    update = svc_info;
                    --update.async_requests_;
                } while (!async_service_.compare_exchange_weak(svc_info, update, std::memory_order_acq_rel, std::memory_order_relaxed));
            },
            std::move(headers), std::string(data)),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    LOG_ERROR("Http async_post error: {}", e.what());
                }
            }
    });
}

inline void Http::async_put(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers)
{
    ensure_service_thread();
    asio::co_spawn(
        async_ioc_,
        do_session(std::string(url), http::verb::put,
            [on_response = std::move(on_response)](Response&& response) mutable {
                on_response(std::move(response));
                auto svc_info = async_service_.load(std::memory_order_relaxed);
                service_info update;
                do {
                    assert(svc_info.async_requests_ > 0);
                    update = svc_info;
                    --update.async_requests_;
                } while (!async_service_.compare_exchange_weak(svc_info, update, std::memory_order_acq_rel, std::memory_order_relaxed));
            },
            std::move(headers), std::string(data)),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    LOG_ERROR("Http async_put error: {}", e.what());
                }
            }
    });
}

inline void Http::async_patch(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers)
{
    ensure_service_thread();
    asio::co_spawn(
        async_ioc_,
        do_session(std::string(url), http::verb::patch,
            [on_response = std::move(on_response)](Response&& response) mutable {
                on_response(std::move(response));
                auto svc_info = async_service_.load(std::memory_order_relaxed);
                service_info update;
                do {
                    assert(svc_info.async_requests_ > 0);
                    update = svc_info;
                    --update.async_requests_;
                } while (!async_service_.compare_exchange_weak(svc_info, update, std::memory_order_acq_rel, std::memory_order_relaxed));
            },
            std::move(headers), std::string(data)),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    LOG_ERROR("Http async_patch error: {}", e.what());
                }
            }
    });
}

inline void Http::async_del(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers)
{
    ensure_service_thread();
    asio::co_spawn(
        async_ioc_,
        do_session(std::string(url), http::verb::delete_,
            [on_response = std::move(on_response)](Response&& response) mutable {
                on_response(std::move(response));
                auto svc_info = async_service_.load(std::memory_order_relaxed);
                service_info update;
                do {
                    assert(svc_info.async_requests_ > 0);
                    update = svc_info;
                    --update.async_requests_;
                } while (!async_service_.compare_exchange_weak(svc_info, update, std::memory_order_acq_rel, std::memory_order_relaxed));
            },
            std::move(headers), std::string(data)),
        [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    LOG_ERROR("Http async_del error: {}", e.what());
                }
            }
    });
}

inline asio::awaitable<Http::Response> Http::async_get(std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers)
{
    return do_session_awaitable(std::string(url), http::verb::get, std::move(headers), "", 11);
}

inline asio::awaitable<Http::Response> Http::async_post(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers)
{
    return do_session_awaitable(std::string(url), http::verb::post, std::move(headers), std::string(data), 11);
}

inline asio::awaitable<Http::Response> Http::async_put(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers)
{
    return do_session_awaitable(std::string(url), http::verb::put, std::move(headers), std::string(data), 11);
}

inline asio::awaitable<Http::Response> Http::async_patch(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers)
{
    return do_session_awaitable(std::string(url), http::verb::patch, std::move(headers), std::string(data), 11);
}

inline asio::awaitable<Http::Response> Http::async_del(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers)
{
    return do_session_awaitable(std::string(url), http::verb::delete_, std::move(headers), std::string(data), 11);
}

inline void Http::ensure_service_thread() {
    auto svc_info = async_service_.load(std::memory_order_relaxed);
    service_info update;
    do {
        update = svc_info;
        ++update.async_requests_;
        update.service_running_ = true;
    } while (!async_service_.compare_exchange_weak(svc_info, update, std::memory_order_acq_rel, std::memory_order_relaxed));

    if (!svc_info.service_running_)
    {
        std::thread([](){
            bool run = true;
            while (run) {
                try {
                    if (async_ioc_.stopped()) {
                        auto svc_info = async_service_.load(std::memory_order_relaxed);
                        service_info update;
                        do {
                            if (svc_info.async_requests_) {
                                break;
                            }

                            update = svc_info;
                            update.service_running_ = false; 
                        }
                        while (!async_service_.compare_exchange_weak(svc_info, update, std::memory_order_acq_rel, std::memory_order_relaxed));
                        run = svc_info.async_requests_;
                        async_ioc_.restart();
                        continue;
                    }
                    async_ioc_.run();
                }
                catch(const std::exception& e) {
                    async_ioc_.restart();
                    LOG_ERROR("Http service thread error: {}", e.what());
                }
            }
        }).detach();
    }
}

// ---------------------------------------------------- HttpStream Implementation ----------------------------------------------------

// Static member definitions
inline asio::io_context HttpStream::ioc_;
inline std::thread HttpStream::service_thread_;
inline std::atomic_bool HttpStream::init_service_thread_{ false };
inline std::atomic_bool HttpStream::run_;

inline HttpStream::HttpStream(
    std::string url,
    std::function<void()> &&onConnectedCallback,
    std::function<void()> &&onDisconnectedCallback,
    std::function<void(const char*, std::size_t)> &&onDataCallback,
    std::function<void(std::string err)> &&onErrorCallback,
    std::vector<std::pair<std::string, std::string>>&& headers)
    : url_(std::move(url))
    , headers_(std::move(headers))
    , on_connected_(std::move(onConnectedCallback))
    , on_disconnected_(std::move(onDisconnectedCallback))
    , on_data_(std::move(onDataCallback))
    , on_error_(std::move(onErrorCallback))
{
    auto [host, target, port, use_ssl] = Http::parse_url(url_);
    host_ = std::move(host);
    target_ = std::move(target);
    port_ = std::move(port);
    use_ssl_ = use_ssl;
}

extern "C" inline void __http_stream_signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        HttpStream::shutdown();
    }
}

inline void HttpStream::open()
{
    LOG_INFO("Opening HTTP Stream {}", url_);
    status_.store(Status::CONNECTING, std::memory_order_release);
    should_close_.store(false, std::memory_order_release);

    // Initialize service thread if needed
    bool expected = false;
    if (init_service_thread_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        std::signal(SIGINT, __http_stream_signal_handler);
        std::signal(SIGTERM, __http_stream_signal_handler);
        run_.store(true, std::memory_order_release);
        service_thread_ = std::thread([]() {
            while (run_.load(std::memory_order_acquire)) {
                try {
                    ioc_.run();
                    if (run_.load(std::memory_order_acquire)) {
                        ioc_.restart();
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }
                catch (const std::exception& e) {
                    LOG_ERROR("HttpStream service thread error: {}", e.what());
                    ioc_.restart();
                }
            }
        });
    }

    // Start the session - keep object alive with shared_from_this
    asio::co_spawn(
        ioc_,
        do_stream_session(),
        [self = shared_from_this()](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    LOG_ERROR("HttpStream session error: {}", ex.what());
                    self->on_error_(ex.what());
                }
            }
        });
}

inline void HttpStream::close()
{
    LOG_INFO("Closing HTTP Stream {}", url_);
    should_close_.store(true, std::memory_order_release);
}

inline void HttpStream::shutdown()
{
    bool expected = true;
    if (run_.compare_exchange_strong(expected, false, std::memory_order_acq_rel, std::memory_order_relaxed))
    {
        ioc_.stop();
        if (service_thread_.joinable()) {
            service_thread_.join();
        }
    }
}

inline asio::awaitable<void> HttpStream::do_stream_session()
{
    if (use_ssl_) {
        return do_stream_session_ssl();
    } else {
        return do_stream_session_plain();
    }
}

inline asio::awaitable<void> HttpStream::do_stream_session_ssl()
{
    auto executor = co_await asio::this_coro::executor;
    auto resolver = asio::ip::tcp::resolver{ executor };
    auto stream = ssl::stream<beast::tcp_stream>{ executor, Http::ctx_ };

    try {
        // Set SNI Hostname
        if(!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str()))
        {
            beast::error_code ec{
                static_cast<int>(::ERR_get_error()),
                asio::error::get_ssl_category()};
            LOG_ERROR("Error setting SNI hostname: {}", ec.message());
            on_error_(std::format("Error setting SNI hostname: {}", ec.message()));
            status_.store(Status::DISCONNECTED, std::memory_order_release);
            on_disconnected_();
            co_return;
        }

        // Look up the domain name
        auto const results = co_await resolver.async_resolve(host_, port_);

        // Set the timeout
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));

        // Make the connection
        co_await beast::get_lowest_layer(stream).async_connect(results);

        // Set the timeout
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));

        // Perform the SSL handshake
        co_await stream.async_handshake(ssl::stream_base::client);

        // Set up an HTTP GET request for streaming
        http::request<http::string_body> req{ http::verb::get, target_, 11 };
        req.set(http::field::host, host_);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::accept, "text/event-stream");
        req.set(http::field::cache_control, "no-cache");

        // Set custom headers
        for (auto &header_pair : headers_) {
            req.set(header_pair.first, header_pair.second);
        }

        // Disable timeout for streaming
        beast::get_lowest_layer(stream).expires_never();

        // Send the HTTP request
        co_await http::async_write(stream, req);

        // Read response header first
        beast::flat_buffer buffer;
        http::response_parser<http::dynamic_body> parser;
        parser.body_limit(std::numeric_limits<std::uint64_t>::max());

        // Read just the header
        co_await http::async_read_header(stream, buffer, parser);

        auto& res = parser.get();

        if (res.result() != http::status::ok) {
            LOG_ERROR("HTTP Stream failed with status: {}", static_cast<int>(res.result()));
            on_error_(std::format("HTTP error: {}", std::string(res.reason())));
            status_.store(Status::DISCONNECTED, std::memory_order_release);
            on_disconnected_();
            co_return;
        }

        // Connection established successfully
        status_.store(Status::CONNECTED, std::memory_order_release);
        on_connected_();

        // Check if this is SSE format
        bool is_sse = false;
        auto content_type = res[http::field::content_type];
        if (content_type.find("text/event-stream") != std::string::npos) {
            is_sse = true;
        }

        // Read body chunks continuously
        while (!should_close_.load(std::memory_order_acquire) &&
               run_.load(std::memory_order_acquire) &&
               status_.load(std::memory_order_acquire) == Status::CONNECTED)
        {
            // Read some data from the stream
            auto [ec, bytes_transferred] = co_await stream.async_read_some(
                buffer.prepare(8192),
                asio::as_tuple(asio::use_awaitable)
            );

            if (ec == http::error::end_of_stream || ec == asio::error::eof) {
                // Stream ended gracefully
                LOG_INFO("HTTP Stream ended");
                break;
            }
            else if (ec) {
                LOG_ERROR("HTTP Stream read error: {}", ec.message());
                on_error_(ec.message());
                break;
            }

            // Commit the received data to the buffer
            buffer.commit(bytes_transferred);

            // Convert buffer to string
            auto data_view = beast::buffers_to_string(buffer.data());

            if (!data_view.empty()) {
                if (is_sse) {
                    parse_sse_chunk(data_view.data(), data_view.size());
                } else {
                    // Raw chunked data
                    on_data_(data_view.data(), data_view.size());
                }

                // Clear the consumed data
                buffer.consume(buffer.size());
            }
        }

        // Graceful shutdown
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        auto [ec] = co_await stream.async_shutdown(asio::as_tuple);

        if(ec && ec != asio::ssl::error::stream_truncated) {
            LOG_WARN("SSL shutdown warning: {}", ec.message());
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("HttpStream exception: {}", e.what());
        on_error_(e.what());
    }

    status_.store(Status::DISCONNECTED, std::memory_order_release);
    on_disconnected_();
}

inline asio::awaitable<void> HttpStream::do_stream_session_plain()
{
    auto executor = co_await asio::this_coro::executor;
    auto resolver = asio::ip::tcp::resolver{ executor };
    auto stream = beast::tcp_stream{ executor };

    try {
        // Look up the domain name
        auto const results = co_await resolver.async_resolve(host_, port_);

        // Set the timeout
        stream.expires_after(std::chrono::seconds(30));

        // Make the connection
        co_await stream.async_connect(results);

        // Set up an HTTP GET request for streaming
        http::request<http::string_body> req{ http::verb::get, target_, 11 };
        req.set(http::field::host, host_);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::accept, "text/event-stream");
        req.set(http::field::cache_control, "no-cache");

        // Set custom headers
        for (auto &header_pair : headers_) {
            req.set(header_pair.first, header_pair.second);
        }

        // Disable timeout for streaming
        stream.expires_never();

        // Send the HTTP request
        co_await http::async_write(stream, req);

        // Read response header first
        beast::flat_buffer buffer;
        http::response_parser<http::dynamic_body> parser;
        parser.body_limit(std::numeric_limits<std::uint64_t>::max());

        // Read just the header
        co_await http::async_read_header(stream, buffer, parser);

        auto& res = parser.get();

        if (res.result() != http::status::ok) {
            LOG_ERROR("HTTP Stream failed with status: {}", static_cast<int>(res.result()));
            on_error_(std::format("HTTP error: {}", std::string(res.reason())));
            status_.store(Status::DISCONNECTED, std::memory_order_release);
            on_disconnected_();
            co_return;
        }

        // Connection established successfully
        status_.store(Status::CONNECTED, std::memory_order_release);
        on_connected_();

        // Check if this is SSE format
        bool is_sse = false;
        auto content_type = res[http::field::content_type];
        if (content_type.find("text/event-stream") != std::string::npos) {
            is_sse = true;
        }

        // Read body chunks continuously
        while (!should_close_.load(std::memory_order_acquire) &&
               run_.load(std::memory_order_acquire) &&
               status_.load(std::memory_order_acquire) == Status::CONNECTED)
        {
            // Read some data from the stream
            auto [ec, bytes_transferred] = co_await stream.async_read_some(
                buffer.prepare(8192),
                asio::as_tuple(asio::use_awaitable)
            );

            if (ec == http::error::end_of_stream || ec == asio::error::eof) {
                // Stream ended gracefully
                LOG_INFO("HTTP Stream ended");
                break;
            }
            else if (ec) {
                LOG_ERROR("HTTP Stream read error: {}", ec.message());
                on_error_(ec.message());
                break;
            }

            // Commit the received data to the buffer
            buffer.commit(bytes_transferred);

            // Convert buffer to string
            auto data_view = beast::buffers_to_string(buffer.data());

            if (!data_view.empty()) {
                if (is_sse) {
                    parse_sse_chunk(data_view.data(), data_view.size());
                } else {
                    // Raw chunked data
                    on_data_(data_view.data(), data_view.size());
                }

                // Clear the consumed data
                buffer.consume(buffer.size());
            }
        }

        // Graceful shutdown
        stream.expires_after(std::chrono::seconds(5));
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        if(ec && ec != beast::errc::not_connected) {
            LOG_WARN("Socket shutdown warning: {}", ec.message());
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("HttpStream exception: {}", e.what());
        on_error_(e.what());
    }

    status_.store(Status::DISCONNECTED, std::memory_order_release);
    on_disconnected_();
}

inline void HttpStream::parse_sse_chunk(const char* data, size_t size)
{
    // Append new data to buffer
    sse_buffer_.append(data, size);

    // Process complete events (separated by double newline)
    size_t pos = 0;
    while ((pos = sse_buffer_.find("\n\n")) != std::string::npos) {
        std::string event = sse_buffer_.substr(0, pos);
        sse_buffer_.erase(0, pos + 2);

        // Parse SSE event fields
        std::string event_data;
        std::istringstream iss(event);
        std::string line;

        while (std::getline(iss, line)) {
            if (line.empty() || line[0] == ':') {
                continue; // Skip empty lines and comments
            }

            if (line.starts_with("data:")) {
                std::string data_line = line.substr(5);
                if (!data_line.empty() && data_line[0] == ' ') {
                    data_line = data_line.substr(1);
                }
                if (!event_data.empty()) {
                    event_data += '\n';
                }
                event_data += data_line;
            }
            // We could also parse event:, id:, retry: fields if needed
        }

        // Deliver the parsed event data
        if (!event_data.empty()) {
            on_data_(event_data.data(), event_data.size());
        }
    }
}

// A Terminator class to ensure HttpStream::shutdown() is called at program exit
struct HttpStreamTerminater
{
    HttpStreamTerminater() {
    }
    ~HttpStreamTerminater() {
        HttpStream::shutdown();
    }
};

inline static HttpStreamTerminater s_http_stream_terminater;

}   // namespace slick::net