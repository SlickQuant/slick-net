#include <slick/net/http_stream.hpp>
#include <slick/net/logging.hpp>
#include <slick/net/tls.hpp>
#include "utils.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/as_tuple.hpp>

#include <array>
#include <thread>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace slick::net {

namespace {
    asio::io_context ioc_;
    std::thread service_thread_;
    std::atomic_bool init_service_thread_{ false };
    std::atomic_bool run_ {false};

    // A Terminator class to ensure HttpStream::shutdown() is called at program exit
    struct HttpStreamTerminater
    {
        HttpStreamTerminater() {
        }

        ~HttpStreamTerminater() {
            HttpStream::shutdown();
        }
    };

    static HttpStreamTerminater s_http_stream_terminater;
}   // end namespace

// Exposes a session's socket to close() while the session sends its request and reads the response.
// Created, destroyed and read only on the service thread, so it needs no synchronization.
struct HttpStream::socket_registration {
    socket_registration(HttpStream& owner, beast::tcp_stream& socket) noexcept
        : owner(owner)
        , socket(socket) {
        owner.registered_socket_ = this;
    }

    ~socket_registration() {
        if (owner.registered_socket_ == this) {
            owner.registered_socket_ = nullptr;
        }
    }

    socket_registration(const socket_registration&) = delete;
    socket_registration& operator=(const socket_registration&) = delete;

    HttpStream& owner;
    beast::tcp_stream& socket;
};

HttpStream::HttpStream(
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
    , on_error_(std::move(onErrorCallback)) {
    auto [host, target, port, use_ssl] = parse_url(url_);
    host_ = std::move(host);
    target_ = std::move(target);
    port_ = std::move(port);
    use_ssl_ = use_ssl;
}

HttpStream::~HttpStream() = default;

bool HttpStream::is_running() noexcept {
    return run_.load(std::memory_order_relaxed);
}

HttpStream::Status HttpStream::status() const noexcept {
    return status_.load(std::memory_order_relaxed);
}

void HttpStream::open()
{
    LOG_INFO("Opening HTTP Stream {}", url_);
    status_.store(Status::CONNECTING, std::memory_order_release);
    should_close_.store(false, std::memory_order_release);

    // Initialize service thread if needed
    bool expected = false;
    if (init_service_thread_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        run_.store(true, std::memory_order_release);
        service_thread_ = std::thread([self = shared_from_this()]() {
            while (run_.load(std::memory_order_acquire)) {
                try {
                    ioc_.run();
                    if (run_.load(std::memory_order_acquire)) {
                        ioc_.restart();
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }
                catch (const std::exception& ex) {
                    self->on_error_(std::format("HttpStream service thread error: {}",ex.what()));
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
                    self->on_error_(std::format("HttpStream session error: {}", ex.what()));
                }
            }
        });
}

void HttpStream::close()
{
    LOG_INFO("Closing HTTP Stream {}", url_);
    should_close_.store(true, std::memory_order_release);

    // A session waiting on a silent server would not see the flag until data arrives, so cancel its pending
    // I/O on the service thread, the only thread that touches the registered socket. The flag is set first,
    // so a session that has not started its next read yet sees it and never waits.
    asio::post(ioc_, [weak_self = weak_from_this()]() {
        if (auto self = weak_self.lock(); self && self->registered_socket_) {
            self->registered_socket_->socket.cancel();
        }
    });
}

void HttpStream::shutdown() {
    bool expected = true;
    if (run_.compare_exchange_strong(expected, false, std::memory_order_acq_rel, std::memory_order_relaxed))
    {
        ioc_.stop();
        if (service_thread_.joinable()) {
            service_thread_.join();
        }
    }
}

asio::awaitable<void> HttpStream::do_stream_session() {
    if (use_ssl_) {
        return do_stream_session_ssl();
    } else {
        return do_stream_session_plain();
    }
}

asio::awaitable<void> HttpStream::do_stream_session_ssl() {
    auto executor = co_await asio::this_coro::executor;
    auto resolver = asio::ip::tcp::resolver{ executor };
    auto stream = ssl::stream<beast::tcp_stream>{ executor, tls_context() };

    try {
        // Set SNI and the host name/IP the server certificate must match
        if (auto ec = detail::set_tls_peer_host(stream.native_handle(), host_)) {
            on_error_("Error setting TLS peer host: " + ec.message());
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

        // Perform the SSL handshake (verifies the certificate chain and peer host)
        if (auto [ec] = co_await stream.async_handshake(ssl::stream_base::client, asio::as_tuple); ec) {
            detail::throw_tls_handshake_error(stream.native_handle(), ec);
        }

        if (co_await stream_response(stream)) {
            // Graceful shutdown
            beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
            auto [ec] = co_await stream.async_shutdown(asio::as_tuple);

            if(ec && ec != asio::ssl::error::stream_truncated) {
                LOG_WARN("SSL shutdown warning: {}", ec.message());
            }
        }
    }
    catch (const std::exception& e) {
        // After close() this is the cancelled request or header read, not a failure
        if (!should_close_.load(std::memory_order_acquire)) {
            LOG_ERROR("HttpStream exception: {}", e.what());
            on_error_(e.what());
        }
    }

    status_.store(Status::DISCONNECTED, std::memory_order_release);
    on_disconnected_();
}

asio::awaitable<void> HttpStream::do_stream_session_plain() {
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

        if (co_await stream_response(stream)) {
            // Graceful shutdown
            stream.expires_after(std::chrono::seconds(5));
            beast::error_code ec;
            stream.socket().shutdown(tcp::socket::shutdown_both, ec);

            if(ec && ec != beast::errc::not_connected) {
                LOG_WARN("Socket shutdown warning: {}", ec.message());
            }
        }
    }
    catch (const std::exception& e) {
        // After close() this is the cancelled request or header read, not a failure
        if (!should_close_.load(std::memory_order_acquire)) {
            LOG_ERROR("HttpStream exception: {}", e.what());
            on_error_(e.what());
        }
    }

    status_.store(Status::DISCONNECTED, std::memory_order_release);
    on_disconnected_();
}

// Sends the streaming GET request over a connected stream and delivers the response body until the
// message ends, close() is requested or the read fails. Returns false if the server rejected the request.
template <typename Stream>
asio::awaitable<bool> HttpStream::stream_response(Stream& stream) {
    auto& socket = beast::get_lowest_layer(stream);

    // Let close() cancel the request and response I/O. Unregistered on return, before the graceful
    // shutdown, so a close() issued from a callback cannot cancel the shutdown.
    socket_registration registration{ *this, socket };

    // Set up an HTTP GET request for streaming
    http::request<http::string_body> req{ http::verb::get, target_, 11 };
    req.set(http::field::host, detail::format_authority(host_));
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::accept, "text/event-stream");
    req.set(http::field::cache_control, "no-cache");

    // Set custom headers
    for (auto &header_pair : headers_) {
        req.set(header_pair.first, header_pair.second);
    }

    // Disable timeout for streaming. A stream may stay silent for any length of time, and a Beast
    // timeout closes the socket, so a read timeout would drop idle streams; close() cancels instead.
    socket.expires_never();

    // Send the HTTP request
    co_await http::async_write(stream, req);

    // Read response header first. The body is read through the same parser so the transfer coding
    // (chunk sizes, chunk extensions, trailers) is decoded; buffer_body has it write the decoded bytes
    // straight into body_buf instead of accumulating them in the message.
    beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(std::numeric_limits<std::uint64_t>::max());

    // Read just the header
    co_await http::async_read_header(stream, buffer, parser);

    auto& res = parser.get();

    if (res.result() != http::status::ok) {
        LOG_ERROR("HTTP Stream failed with status: {}", static_cast<int>(res.result()));
        on_error_(std::format("HTTP error: {}", std::string(res.reason())));
        co_return false;
    }

    // Connection established successfully
    status_.store(Status::CONNECTED, std::memory_order_release);
    on_connected_();

    // Check if this is SSE format
    const bool is_sse = res[http::field::content_type].find("text/event-stream") != std::string::npos;

    // Drop any partial event an earlier response of this stream ended in, so it is not joined to this body
    sse_parser_.reset();

    // Body bytes async_read_header read past the header stay in buffer and are parsed by the first read
    std::array<char, 8192> body_buf;

    // Read body continuously
    while (!parser.is_done() &&
           !should_close_.load(std::memory_order_acquire) &&
           run_.load(std::memory_order_acquire) &&
           status_.load(std::memory_order_acquire) == Status::CONNECTED)
    {
        auto& body = res.body();
        body.data = body_buf.data();
        body.size = body_buf.size();

        auto [ec, bytes_transferred] = co_await http::async_read_some(
            stream, buffer, parser,
            asio::as_tuple(asio::use_awaitable)
        );

        // Deliver what was decoded, even if the read then stopped with an error
        if (const auto decoded = body_buf.size() - body.size; decoded > 0) {
            if (is_sse) {
                sse_parser_.feed(body_buf.data(), decoded, on_data_);
            } else {
                on_data_(body_buf.data(), decoded);
            }
        }

        if (!ec || ec == http::error::need_buffer) {
            // need_buffer: body_buf is full, the rest is delivered by the next read
            continue;
        }

        if (should_close_.load(std::memory_order_acquire)) {
            // close() cancelled the read
            break;
        }

        LOG_ERROR("HTTP Stream read error: {}", ec.message());
        on_error_(ec.message());
        break;
    }

    if (parser.is_done()) {
        // The server finished the response (final chunk, Content-Length reached or EOF-delimited body closed)
        LOG_INFO("HTTP Stream ended");
    }
    co_return true;
}

}   // end namespace slick::net
