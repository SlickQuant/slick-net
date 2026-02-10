#include <slick/net/http.hpp>

// #include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
// #include <boost/beast/websocket.hpp>
// #include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ssl.hpp>
// #include <boost/asio/strand.hpp>
#include <boost/asio/co_spawn.hpp>
// #include <boost/asio/signal_set.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/as_tuple.hpp>
#include "utils.hpp"

namespace slick::net {

namespace {
    asio::io_context ioc_;
    std::thread service_thread_;
    std::atomic_bool init_service_thread_{ false };
    std::atomic_bool _run_ {false};

    ssl::context ctx_ = []() {
        ssl::context ctx{ssl::context::tlsv12_client};
        // Verify the remote server's certificate
        ctx.set_verify_mode(ssl::verify_none);
        return ctx;
    }();

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

extern "C" void __http_stream_signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        HttpStream::shutdown();
    }
}

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
HttpStream::HttpStream(HttpStream&&) noexcept = default;
HttpStream& HttpStream::operator=(HttpStream&&) noexcept = default;

void HttpStream::open() {
    impl_->open();
}

void HttpStream::close() {
    impl_->close();
}

bool HttpStream::is_running() noexcept {
    return run_.load(std::memory_order_relaxed);
}

HttpStream::Status HttpStream::status() const noexcept {
    return impl_->status();
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
        std::signal(SIGINT, __http_stream_signal_handler);
        std::signal(SIGTERM, __http_stream_signal_handler);
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
                catch (const std::exception& e) {
                    self->on_error_("HttpStream service thread error: " + ex.what());
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
                    self->on_error_("HttpStream session error: " + ex.what());
                }
            }
        });
}

inline void HttpStream::close()
{
    LOG_INFO("Closing HTTP Stream {}", url_);
    should_close_.store(true, std::memory_order_release);
}

inline void HttpStream::shutdown() {
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
    auto stream = ssl::stream<beast::tcp_stream>{ executor, Http::ctx_ };

    try {
        // Set SNI Hostname
        if(!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str()))
        {
            beast::error_code ec{
                static_cast<int>(::ERR_get_error()),
                asio::error::get_ssl_category()};
            on_error_("Error setting SNI hostname: " + ec.message());
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

void HttpStream::parse_sse_chunk(const char* data, size_t size) {
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

}   // end namespace slick::net