#include "websocket_impl.hpp"

namespace slick::net {

Websocket::Impl::Impl(
    std::string url,
    std::function<void()> onConnectedCallback,
    std::function<void()> onDisconnectedCallback,
    std::function<void(const char*, std::size_t)> onDataCallback,
    std::function<void(std::string &&err)> onErrorCallback)
    : url_(std::move(url))
    , on_connected_(std::move(onConnectedCallback))
    , on_diconnected_(std::move(onDisconnectedCallback))
    , on_data_(std::move(onDataCallback))
    , on_error_(std::move(onErrorCallback))
    , w_buffer_(1048576) {
    std::string protocol("wss");
    auto pos = url_.find("://");
    if (pos == std::string::npos) {
        pos = url_.find("/");
        if (pos == std::string::npos) {
            host_ = url_;
            path_ = "/";
        }
        else {
            host_ = url_.substr(0, pos);
            path_ = url_.substr(pos);
        }
    }
    else {
        protocol = url_.substr(0, pos);
        auto host_begin = pos + 3;
        auto pos1 = url_.find("/", host_begin);
        if (pos1 == std::string::npos) {
            host_ = url_.substr(host_begin);
            path_ = "/";
        }
        else {
            host_ = url_.substr(host_begin, pos1 - host_begin);
            path_ = url_.substr(pos1);
        }
    }

    pos = host_.find(':');
    if (pos != 3 && pos != 4 && pos != std::string::npos) {
        port_ = std::stoi(host_.substr(pos + 1));
        host_ = host_.substr(0, pos);
    }

    if (port_ == (uint_fast16_t)-1) {
        port_ = (protocol == "ws") ? 80 : 443;
    }

    // Determine if SSL should be used and initialize appropriate stream
    use_ssl_ = (protocol == "wss");
    if (use_ssl_) {
        wss_ = std::make_unique<websocket::stream<ssl::stream<beast::tcp_stream>>>(
            asio::make_strand(detail::ioc_), detail::ctx_);
    } else {
        ws_ = std::make_unique<websocket::stream<beast::tcp_stream>>(
            asio::make_strand(detail::ioc_));
    }
}

void Websocket::Impl::open() {
    Status expected = Status::DISCONNECTED;
    if (!status_.compare_exchange_strong(expected, Status::CONNECTING, std::memory_order_acq_rel)) {
        if (expected == Status::CONNECTED) {
            LOG_DEBUG("open: WebSocket {} already connected", url_);
        }
        else if (expected == Status::CONNECTING) {
            LOG_DEBUG("open: WebSocket {} is connecting", url_);
        }
        else {
            LOG_DEBUG("open: WebSocket {} is disconnecting", url_);
        }
        return;
    }
    LOG_INFO("Opening WebSocket {}", url_);
    asio::co_spawn(detail::ioc_, do_ws_session(),
        [self = shared_from_this()](std::exception_ptr eptr) {
            if (eptr) {
                try {
                    std::rethrow_exception(eptr);
                } catch (const std::exception& e) {
                    self->status_.store(Status::DISCONNECTED, std::memory_order_release);
                    if (detail::run_.load(std::memory_order_relaxed)) {
                        self->on_error_(e.what());
                    }
                }
            }
        });

    auto init_service = detail::init_service_thread_.load(std::memory_order_relaxed);
    if (detail::init_service_thread_.compare_exchange_strong(init_service, true, std::memory_order_acq_rel) && !init_service) {
        detail::install_signal_handlers();
        detail::run_.store(true, std::memory_order_release);
        detail::service_thread_ = std::thread([]() {
            LOG_INFO("Websocket service thread started.");
            while (detail::run_.load(std::memory_order_relaxed)) {
                try {
                    if (detail::ioc_.stopped()) {
                        detail::ioc_.restart();
                    }
                    detail::ioc_.run();
                }
                catch(const std::exception& e) {
                    detail::ioc_.restart();
                    LOG_ERROR("{}", e.what());
                }
            }

            if (!detail::ioc_.stopped()) [[unlikely]] {
                LOG_TRACE("call ioc_.stop at the end of run");
                detail::ioc_.stop();
            }
            LOG_INFO("Websocket service thread exit");
            detail::init_service_thread_.store(false, std::memory_order_release);
        });
    }
}

asio::awaitable<void> Websocket::Impl::do_ws_session() {
    if (use_ssl_) {
        return do_ws_session_ssl();
    }
    else {
        return do_ws_session_plain();
    }
}

asio::awaitable<void> Websocket::Impl::do_ws_session_ssl() {
    // SSL WebSocket (wss://)
    try {
        if (status_.load(std::memory_order_relaxed) != Status::CONNECTING) {
            LOG_DEBUG("Abort connect. WebSocket {} is not CONNECTING", url_);
            co_return;
        }
        tcp::resolver resolver(asio::make_strand(detail::ioc_));

        // Look up the domain name
        auto result = co_await resolver.async_resolve(host_, std::to_string(port_), asio::use_awaitable);

        // Set SNI Hostname (many hosts need this to handshake successfully)
        if (!SSL_set_tlsext_host_name(wss_->next_layer().native_handle(), host_.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()};
            throw beast::system_error{ec};
        }

        // Set a timeout on the operation
        beast::get_lowest_layer(*wss_).expires_after(std::chrono::seconds(30));

        // Make the connection on the IP address we get from DNS
        auto ep = co_await asio::async_connect(wss_->next_layer().lowest_layer(), result, asio::use_awaitable);

        // Build the host header value with port for the WebSocket handshake.
        // Keep host_ unchanged so reconnects don't double-append ports.
        const auto host_header = host_ + ':' + std::to_string(ep.port());

        // Set a timeout on the operation
        beast::get_lowest_layer(*wss_).expires_after(std::chrono::seconds(30));

        // Perform the SSL handshake
        co_await wss_->next_layer().async_handshake(ssl::stream_base::client, asio::use_awaitable);

        // Turn off the timeout on the tcp_stream, because
        // the websocket stream has its own timeout system.
        beast::get_lowest_layer(*wss_).expires_never();

        // Set suggested timeout settings for the websocket
        wss_->set_option(
            websocket::stream_base::timeout::suggested(
                beast::role_type::client));

        // Set a decorator to change the User-Agent of the handshake
        wss_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(http::field::user_agent,
                    std::string(BOOST_BEAST_VERSION_STRING) +
                        " websocket-client-coro");
            }));

        // Perform the WebSocket handshake
        co_await wss_->async_handshake(host_header, path_, asio::use_awaitable);

        if (status_.load(std::memory_order_relaxed) != Status::CONNECTING ||
            !detail::run_.load(std::memory_order_relaxed)) [[unlikely]] {
            // socket close is called
            co_return;
        }

        status_.store(Status::CONNECTED, std::memory_order_release);

        // start read messages
        wss_->async_read(
            r_buffer_,
            beast::bind_front_handler(
                &Websocket::Impl::on_read,
                shared_from_this()));

        on_connected_();
    }
    catch (beast::system_error const& se) {
        if (se.code() != websocket::error::closed) {
            throw;
        }
    }
}

asio::awaitable<void> Websocket::Impl::do_ws_session_plain() {
    // Plain WebSocket (ws://)
    try {
        if (status_.load(std::memory_order_relaxed) != Status::CONNECTING) {
            LOG_DEBUG("Abort connect. WebSocket {} is not CONNECTING", url_);
            co_return;
        }
        tcp::resolver resolver(asio::make_strand(detail::ioc_));

        // Look up the domain name
        auto result = co_await resolver.async_resolve(host_, std::to_string(port_), asio::use_awaitable);

        // Set a timeout on the operation
        beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));

        // Make the connection on the IP address we get from DNS
        auto ep = co_await beast::get_lowest_layer(*ws_).async_connect(result);

        // Build the host header value with port for the WebSocket handshake.
        // Keep host_ unchanged so reconnects don't double-append ports.
        const auto host_header = host_ + ':' + std::to_string(ep.port());

        // Turn off the timeout on the tcp_stream, because
        // the websocket stream has its own timeout system.
        beast::get_lowest_layer(*ws_).expires_never();

        // Set suggested timeout settings for the websocket
        ws_->set_option(
            websocket::stream_base::timeout::suggested(
                beast::role_type::client));

        // Set a decorator to change the User-Agent of the handshake
        ws_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(http::field::user_agent,
                    std::string(BOOST_BEAST_VERSION_STRING) +
                        " websocket-client-coro");
            }));

        // Perform the WebSocket handshake
        co_await ws_->async_handshake(host_header, path_, asio::use_awaitable);

        if (status_.load(std::memory_order_relaxed) != Status::CONNECTING ||
            !detail::run_.load(std::memory_order_relaxed)) [[unlikely]] {
            // socket close is called
            co_return;
        }

        status_.store(Status::CONNECTED, std::memory_order_release);

        // start read messages
        ws_->async_read(
            r_buffer_,
            beast::bind_front_handler(
                &Websocket::Impl::on_read,
                shared_from_this()));

        on_connected_();
    }
    catch (beast::system_error const& se) {
        if (se.code() != websocket::error::closed) {
            throw;
        }
    }
}

} // namespace slick::net
