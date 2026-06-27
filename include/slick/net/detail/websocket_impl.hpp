#pragma once

#include <slick/net/websocket.hpp>
#include <slick/net/logging.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <slick/queue.h>

#include <atomic>
#include <csignal>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace beast = boost::beast;
namespace asio = boost::asio;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace slick::net::detail {

asio::io_context& websocket_ioc() noexcept;
ssl::context& websocket_ssl_context() noexcept;
bool websocket_running() noexcept;
void start_websocket_service();
void stop_websocket_service();

struct websocket_url_parts {
    std::string host;
    std::string path;
    uint_fast16_t port;
    bool use_ssl;
};

inline websocket_url_parts parse_websocket_url(const std::string& url) {
    std::string protocol("wss");
    std::string host;
    std::string path;

    auto pos = url.find("://");
    if (pos == std::string::npos) {
        pos = url.find('/');
        if (pos == std::string::npos) {
            host = url;
            path = "/";
        }
        else {
            host = url.substr(0, pos);
            path = url.substr(pos);
        }
    }
    else {
        protocol = url.substr(0, pos);
        auto host_begin = pos + 3;
        auto path_begin = url.find('/', host_begin);
        if (path_begin == std::string::npos) {
            host = url.substr(host_begin);
            path = "/";
        }
        else {
            host = url.substr(host_begin, path_begin - host_begin);
            path = url.substr(path_begin);
        }
    }

    auto port = static_cast<uint_fast16_t>((std::numeric_limits<uint_fast16_t>::max)());
    pos = host.rfind(':');
    if (pos != std::string::npos && host.find(':') == pos) {
        port = static_cast<uint_fast16_t>(std::stoi(host.substr(pos + 1)));
        host = host.substr(0, pos);
    }

    if (port == static_cast<uint_fast16_t>((std::numeric_limits<uint_fast16_t>::max)())) {
        port = (protocol == "ws") ? 80 : 443;
    }

    return {std::move(host), std::move(path), port, protocol == "wss"};
}

} // namespace slick::net::detail

namespace slick::net {

// Structural concept: detects slick::dynamic_buffer<T> by the presence of
// buffer_ptr()/buffer() accessors. No slick headers required in the library.
template<typename T>
concept is_shared_buffer = requires(T& b) { b.buffer_ptr(); b.buffer(); };

template<typename BufferT>
struct Websocket<BufferT>::Impl : public std::enable_shared_from_this<Websocket<BufferT>::Impl> {
    using Status = typename Websocket<BufferT>::Status;

    explicit Impl(
        std::string url,
        std::function<void()> onConnectedCallback,
        std::function<void()> onDisconnectedCallback,
        std::function<void(const char*, std::size_t)> onDataCallback,
        std::function<void(std::string &&err)> onErrorCallback,
        size_t write_buffer_size,
        std::shared_ptr<BufferT> r_buffer);

    void open();
    bool close();
    void send(const char* buffer, size_t len, bool is_binary = false, bool suppress_log = false);

    Status status() const noexcept {
        return status_.load(std::memory_order_relaxed);
    }

    void detach() noexcept {
        detached_.store(true, std::memory_order_release);
    }

    bool is_detached() const noexcept {
        return detached_.load(std::memory_order_relaxed);
    }

    bool session_started() const noexcept {
        return session_started_.load(std::memory_order_acquire);
    }

    bool open_cancelled() const noexcept {
        return open_cancelled_.load(std::memory_order_acquire);
    }

    void cancel_open() noexcept {
        open_cancelled_.store(true, std::memory_order_release);
    }

    void mark_pending_open() noexcept {
        status_.store(Status::CONNECTING, std::memory_order_release);
    }

    // Register a continuation to run once this impl can no longer touch the shared
    // backend (its read loop has terminated). For non-shared buffers (flat_buffer),
    // fires cb() immediately — there is no ordering constraint between sessions.
    void set_on_buffer_released(std::function<void()> cb) {
        if constexpr (is_shared_buffer<BufferT>) {
            on_buffer_released_ = std::move(cb);
            auto expected = ReleaseState::kActive;
            if (!release_state_.compare_exchange_strong(expected, ReleaseState::kCallbackSet,
                                                        std::memory_order_release,
                                                        std::memory_order_acquire)) {
                on_buffer_released_();
            }
        } else {
            cb();
        }
    }

private:
    friend class Websocket<BufferT>;
    asio::awaitable<void> do_ws_session();
    asio::awaitable<void> do_ws_session_ssl();
    asio::awaitable<void> do_ws_session_plain();
    void do_write();
    void maybe_start_queued_write();
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void on_close(beast::error_code ec);

    void clear_read_buffer() {
        if constexpr (requires(BufferT& b) { b.clear(); }) {
            r_buffer_->clear();
        } else {
            r_buffer_->consume(r_buffer_->size());
        }
    }

    // Mark the shared backend free for the next session. No-op for flat_buffer
    // (each session owns its own fresh buffer — no handoff needed).
    void release_buffer() noexcept {
        if constexpr (is_shared_buffer<BufferT>) {
            auto prev = release_state_.exchange(ReleaseState::kReleased, std::memory_order_acq_rel);
            if (prev == ReleaseState::kCallbackSet) {
                on_buffer_released_();
            }
        }
    }

private:
    std::unique_ptr<websocket::stream<ssl::stream<beast::tcp_stream>>> wss_;
    std::unique_ptr<websocket::stream<beast::tcp_stream>> ws_;
    std::string url_;
    std::string host_;
    std::string path_;
    uint_fast16_t port_ = -1;
    bool use_ssl_ = true;
    std::function<void()> on_connected_;
    std::function<void()> on_diconnected_;
    std::function<void(const char*, std::size_t)> on_data_;
    std::function<void(std::string &&err)> on_error_;
    std::atomic<Status> status_{ Status::DISCONNECTED };
    slick::SlickQueue<char> w_buffer_;
    std::shared_ptr<BufferT> r_buffer_;
    uint64_t w_cursor_{0};
    std::atomic_bool in_writting_{false};
    std::atomic_bool detached_{false};

    enum class ReleaseState : uint8_t { kActive, kCallbackSet, kReleased };
    std::atomic<ReleaseState> release_state_{ ReleaseState::kActive };
    std::function<void()> on_buffer_released_;
    std::atomic_bool session_started_{false};
    std::atomic_bool open_cancelled_{false};
    bool read_started_{false};
};

// ─── Impl method bodies (all template — safe to define in a header) ──────────

template<typename BufferT>
Websocket<BufferT>::Impl::Impl(
    std::string url,
    std::function<void()> onConnectedCallback,
    std::function<void()> onDisconnectedCallback,
    std::function<void(const char*, std::size_t)> onDataCallback,
    std::function<void(std::string &&err)> onErrorCallback,
    size_t write_buffer_size,
    std::shared_ptr<BufferT> r_buffer)
    : url_(std::move(url))
    , on_connected_(std::move(onConnectedCallback))
    , on_diconnected_(std::move(onDisconnectedCallback))
    , on_data_(std::move(onDataCallback))
    , on_error_(std::move(onErrorCallback))
    , w_buffer_(write_buffer_size)
    , r_buffer_(std::move(r_buffer))
{
    auto parts = detail::parse_websocket_url(url_);
    host_ = std::move(parts.host);
    path_ = std::move(parts.path);
    port_ = parts.port;
    use_ssl_ = parts.use_ssl;

    if (use_ssl_) {
        wss_ = std::make_unique<websocket::stream<ssl::stream<beast::tcp_stream>>>(
            asio::make_strand(detail::websocket_ioc()), detail::websocket_ssl_context());
    } else {
        ws_ = std::make_unique<websocket::stream<beast::tcp_stream>>(
            asio::make_strand(detail::websocket_ioc()));
    }
}

template<typename BufferT>
void Websocket<BufferT>::Impl::open() {
    if (open_cancelled_.load(std::memory_order_acquire) || is_detached()) {
        // Deferred open that was cancelled or abandoned: forward buffer ownership.
        Status expected = Status::CONNECTING;
        status_.compare_exchange_strong(expected, Status::DISCONNECTED,
                                        std::memory_order_acq_rel, std::memory_order_relaxed);
        release_buffer();
        return;
    }
    Status expected = Status::DISCONNECTED;
    if (!status_.compare_exchange_strong(expected, Status::CONNECTING, std::memory_order_acq_rel)) {
        if (expected != Status::CONNECTING || session_started_.exchange(true, std::memory_order_acq_rel)) {
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
        // mark_pending_open() pre-set CONNECTING and we won the session_started_ exchange.
    }
    else {
        session_started_.store(true, std::memory_order_release);
    }
    LOG_INFO("Opening WebSocket {}", url_);
    asio::co_spawn(detail::websocket_ioc(), do_ws_session(),
        [self = this->shared_from_this()](std::exception_ptr eptr) {
            std::string err;
            bool failed = false;
            if (eptr) {
                try {
                    std::rethrow_exception(eptr);
                } catch (const std::exception& e) {
                    failed = true;
                    err = e.what();
                }
            }
            if (self->read_started_) {
                if (failed) {
                    self->close();
                    self->status_.store(Status::DISCONNECTED, std::memory_order_release);
                    if (detail::websocket_running() && !self->is_detached()) {
                        self->on_error_(std::move(err));
                        self->on_diconnected_();
                    }
                }
                return;
            }
            const auto prev = self->status_.exchange(Status::DISCONNECTED, std::memory_order_acq_rel);
            if (detail::websocket_running() && !self->is_detached()) {
                if (failed && prev != Status::DISCONNECTING) {
                    self->on_error_(std::move(err));
                }
                if (failed || prev == Status::DISCONNECTING) {
                    self->on_diconnected_();
                }
            }
            self->release_buffer();
        });

    detail::start_websocket_service();
}

template<typename BufferT>
asio::awaitable<void> Websocket<BufferT>::Impl::do_ws_session() {
    if (use_ssl_) {
        return do_ws_session_ssl();
    }
    else {
        return do_ws_session_plain();
    }
}

template<typename BufferT>
asio::awaitable<void> Websocket<BufferT>::Impl::do_ws_session_ssl() {
    try {
        if (status_.load(std::memory_order_relaxed) != Status::CONNECTING ||
            open_cancelled_.load(std::memory_order_acquire) || is_detached()) {
            LOG_DEBUG("Abort connect. WebSocket {} is not CONNECTING", url_);
            Status expected = Status::CONNECTING;
            status_.compare_exchange_strong(expected, Status::DISCONNECTED,
                                            std::memory_order_acq_rel, std::memory_order_relaxed);
            co_return;
        }
        tcp::resolver resolver(asio::make_strand(detail::websocket_ioc()));
        auto result = co_await resolver.async_resolve(host_, std::to_string(port_), asio::use_awaitable);

        if (!SSL_set_tlsext_host_name(wss_->next_layer().native_handle(), host_.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()};
            throw beast::system_error{ec};
        }

        beast::get_lowest_layer(*wss_).expires_after(std::chrono::seconds(30));
        auto ep = co_await asio::async_connect(wss_->next_layer().lowest_layer(), result,
                                               asio::use_awaitable);
        // Keep host_ unchanged for reconnects; build the host header with the actual port.
        const auto host_header = host_ + ':' + std::to_string(ep.port());

        beast::get_lowest_layer(*wss_).expires_after(std::chrono::seconds(30));
        co_await wss_->next_layer().async_handshake(ssl::stream_base::client, asio::use_awaitable);

        beast::get_lowest_layer(*wss_).expires_never();
        wss_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        wss_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(http::field::user_agent,
                    std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-coro");
            }));

        co_await wss_->async_handshake(host_header, path_, asio::use_awaitable);

        if (!detail::websocket_running() ||
            open_cancelled_.load(std::memory_order_acquire) || is_detached()) [[unlikely]] {
            Status expected = Status::CONNECTING;
            status_.compare_exchange_strong(expected, Status::DISCONNECTED,
                                            std::memory_order_acq_rel, std::memory_order_relaxed);
            co_return;
        }

        Status expected = Status::CONNECTING;
        if (!status_.compare_exchange_strong(expected, Status::CONNECTED,
                                             std::memory_order_acq_rel, std::memory_order_relaxed)) [[unlikely]] {
            co_return;
        }

        if (!is_detached()) {
            on_connected_();
        }

        if (status_.load(std::memory_order_relaxed) != Status::CONNECTED || is_detached()) {
            co_return;
        }

        // Discard any partial bytes a previous session left committed-but-unpublished.
        clear_read_buffer();
        maybe_start_queued_write();
        read_started_ = true;
        wss_->async_read(*r_buffer_,
            beast::bind_front_handler(&Websocket<BufferT>::Impl::on_read, this->shared_from_this()));
    }
    catch (beast::system_error const& se) {
        if (se.code() != websocket::error::closed) {
            throw;
        }
    }
}

template<typename BufferT>
asio::awaitable<void> Websocket<BufferT>::Impl::do_ws_session_plain() {
    try {
        if (status_.load(std::memory_order_relaxed) != Status::CONNECTING ||
            open_cancelled_.load(std::memory_order_acquire) || is_detached()) {
            LOG_DEBUG("Abort connect. WebSocket {} is not CONNECTING", url_);
            Status expected = Status::CONNECTING;
            status_.compare_exchange_strong(expected, Status::DISCONNECTED,
                                            std::memory_order_acq_rel, std::memory_order_relaxed);
            co_return;
        }
        tcp::resolver resolver(asio::make_strand(detail::websocket_ioc()));
        auto result = co_await resolver.async_resolve(host_, std::to_string(port_), asio::use_awaitable);

        beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));
        auto ep = co_await beast::get_lowest_layer(*ws_).async_connect(result);
        const auto host_header = host_ + ':' + std::to_string(ep.port());

        beast::get_lowest_layer(*ws_).expires_never();
        ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(http::field::user_agent,
                    std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-coro");
            }));

        co_await ws_->async_handshake(host_header, path_, asio::use_awaitable);

        if (!detail::websocket_running() ||
            open_cancelled_.load(std::memory_order_acquire) || is_detached()) [[unlikely]] {
            Status expected = Status::CONNECTING;
            status_.compare_exchange_strong(expected, Status::DISCONNECTED,
                                            std::memory_order_acq_rel, std::memory_order_relaxed);
            co_return;
        }

        Status expected = Status::CONNECTING;
        if (!status_.compare_exchange_strong(expected, Status::CONNECTED,
                                             std::memory_order_acq_rel, std::memory_order_relaxed)) [[unlikely]] {
            co_return;
        }

        if (!is_detached()) {
            on_connected_();
        }

        if (status_.load(std::memory_order_relaxed) != Status::CONNECTED || is_detached()) {
            co_return;
        }

        clear_read_buffer();
        maybe_start_queued_write();
        read_started_ = true;
        ws_->async_read(*r_buffer_,
            beast::bind_front_handler(&Websocket<BufferT>::Impl::on_read, this->shared_from_this()));
    }
    catch (beast::system_error const& se) {
        if (se.code() != websocket::error::closed) {
            throw;
        }
    }
}

template<typename BufferT>
void Websocket<BufferT>::Impl::do_write() {
    if (status_.load(std::memory_order_relaxed) != Status::CONNECTED) [[unlikely]] {
        // Keep in_writting_ set while CONNECTING so queued sends are flushed
        // once the handshake reaches CONNECTED, without spinning the service thread.
        return;
    }
    auto [msg, len] = w_buffer_.read(w_cursor_);
    if (msg && len) {
        bool is_binary = msg[0];
        bool suppress_log = msg[1];
        msg += 2;
        len -= 2;

        if (!suppress_log) {
            LOG_DEBUG("--> {}", std::string_view(msg, len));
        }

        if (use_ssl_) {
            wss_->binary(is_binary);
            wss_->async_write(asio::buffer(msg, len),
                beast::bind_front_handler(&Websocket<BufferT>::Impl::on_write, this->shared_from_this()));
        } else {
            ws_->binary(is_binary);
            ws_->async_write(asio::buffer(msg, len),
                beast::bind_front_handler(&Websocket<BufferT>::Impl::on_write, this->shared_from_this()));
        }
    }
    else {
        in_writting_.store(false, std::memory_order_release);
    }
}

template<typename BufferT>
void Websocket<BufferT>::Impl::maybe_start_queued_write() {
    if (in_writting_.load(std::memory_order_acquire)) {
        auto executor = use_ssl_ ? wss_->get_executor() : ws_->get_executor();
        asio::post(executor, [self = this->shared_from_this()]() {
            if (self->in_writting_.load(std::memory_order_acquire)) {
                self->do_write();
            }
        });
    }
}

template<typename BufferT>
void Websocket<BufferT>::Impl::on_write(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);
    if(ec) {
        if (detail::websocket_running() &&
            status_.load(std::memory_order_relaxed) == Status::CONNECTED &&
            ec != beast::websocket::error::closed &&
            ec != asio::error::eof &&
            ec != asio::error::operation_aborted &&
            ec != ssl::error::stream_truncated &&
            !(ec.value() == 995 && ec.category() == boost::system::system_category())) {
            if (!is_detached()) {
                on_error_(std::format("Failed to write {}", ec.message()));
            }
            close();
        }
        in_writting_.store(false, std::memory_order_release);
        return;
    }
    do_write();
}

template<typename BufferT>
void Websocket<BufferT>::Impl::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    if(ec) {
        if (detail::websocket_running() &&
            status_.load(std::memory_order_relaxed) == Status::CONNECTED &&
            ec != beast::websocket::error::closed &&
            ec != asio::error::eof &&
            ec != asio::error::operation_aborted &&
            ec != ssl::error::stream_truncated &&
            !(ec.value() == 995 && ec.category() == boost::system::system_category())) {
            if (!is_detached()) {
                on_error_(std::format("Failed to read {}", ec.message()));
            }
            close();
        }
        else if (status_.load(std::memory_order_relaxed) == Status::CONNECTED) {
            status_.store(Status::DISCONNECTED, std::memory_order_release);
            if (!is_detached()) {
                on_diconnected_();
            }
        }
        // Read loop terminated — hand the backend to the next session (no-op for flat_buffer).
        release_buffer();
        return;
    }

    if (detail::websocket_running() &&
        status_.load(std::memory_order_relaxed) == Status::CONNECTED &&
        !is_detached()) {
        LOG_TRACE("<-- {}", std::string((const char*)r_buffer_->data().data(), bytes_transferred));
        if constexpr (is_shared_buffer<BufferT>) {
            // slick: consume() publishes a record and returns {data, length}
            auto record = r_buffer_->consume(bytes_transferred);
            on_data_((const char*)record.data, record.length);
        } else {
            // flat_buffer: read data before consume() discards it
            on_data_((const char*)r_buffer_->data().data(), bytes_transferred);
            r_buffer_->consume(bytes_transferred);
        }

        if (use_ssl_) {
            wss_->async_read(*r_buffer_,
                beast::bind_front_handler(&Websocket<BufferT>::Impl::on_read, this->shared_from_this()));
        } else {
            ws_->async_read(*r_buffer_,
                beast::bind_front_handler(&Websocket<BufferT>::Impl::on_read, this->shared_from_this()));
        }
    }
    else {
        release_buffer();
    }
}

template<typename BufferT>
void Websocket<BufferT>::Impl::on_close(beast::error_code ec) {
    if (ec && detail::websocket_running() &&
        ec != beast::websocket::error::closed &&
        ec != asio::error::eof &&
        ec != asio::error::operation_aborted &&
        ec != ssl::error::stream_truncated &&
        !(ec.value() == 995 && ec.category() == boost::system::system_category())) {
        if (!is_detached()) {
            on_error_(ec.message());
        }
    }

    LOG_INFO("Websocket {} closed", url_);
    status_.store(Status::DISCONNECTED, std::memory_order_release);
    if (!is_detached()) {
        on_diconnected_();
    }
}

template<typename BufferT>
bool Websocket<BufferT>::Impl::close() {
    Status expected = status_.load(std::memory_order_relaxed);
    do {
        if (expected >= Status::DISCONNECTING) {
            return false;
        }
    } while (!status_.compare_exchange_weak(expected, Status::DISCONNECTING,
                                            std::memory_order_acq_rel, std::memory_order_relaxed));
    LOG_INFO("Closing {}", url_);
    auto executor = use_ssl_ ? wss_->get_executor() : ws_->get_executor();
    asio::post(executor, [self = this->shared_from_this(), was = expected]() {
        if (was == Status::CONNECTING) {
            if (self->use_ssl_) {
                beast::get_lowest_layer(*self->wss_).close();
            } else {
                beast::get_lowest_layer(*self->ws_).close();
            }
            return;
        }
        if (self->use_ssl_) {
            self->wss_->async_close(websocket::close_code::normal,
                beast::bind_front_handler(&Websocket<BufferT>::Impl::on_close, self));
        } else {
            self->ws_->async_close(websocket::close_code::normal,
                beast::bind_front_handler(&Websocket<BufferT>::Impl::on_close, self));
        }
    });
    return true;
}

template<typename BufferT>
void Websocket<BufferT>::Impl::send(const char* buffer, size_t len, bool is_binary, bool suppress_log) {
    if (status_.load(std::memory_order_relaxed) > Status::CONNECTED) {
        LOG_WARN("WebSocket not connected, cannot send data.");
        return;
    }
    if (len > static_cast<size_t>((std::numeric_limits<uint32_t>::max)()) - 2u) {
        const std::string err = "WebSocket send payload exceeds the maximum queue record size";
        LOG_ERROR("{}", err);
        if (!is_detached()) {
            on_error_(std::string(err));
        }
        return;
    }
    auto l = static_cast<uint32_t>(len) + 2;
    auto index = w_buffer_.reserve(l);
    *w_buffer_[index] = static_cast<char>(is_binary);
    *w_buffer_[index + 1] = static_cast<char>(suppress_log);
    memcpy(w_buffer_[index + 2], buffer, len);
    w_buffer_.publish(index, l);

    auto executor = use_ssl_ ? wss_->get_executor() : ws_->get_executor();
    asio::post(executor, [self = this->shared_from_this()]() {
        bool expected = false;
        if (self->in_writting_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            self->do_write();
        }
    });
}

// ─── Websocket<BufferT> outer class method bodies ────────────────────────────

template<typename BufferT>
Websocket<BufferT>::Websocket(
    std::string url,
    std::function<void()> &&onConnectedCallback,
    std::function<void()> &&onDiconnectedCallback,
    std::function<void(const char*, std::size_t)> &&onDataCallback,
    std::function<void(std::string &&err)> &&onErrorCallback,
    uint32_t write_buffer_size)
requires std::default_initializable<BufferT>
    : url_(std::move(url))
    , on_connected_(std::move(onConnectedCallback))
    , on_diconnected_(std::move(onDiconnectedCallback))
    , on_data_(std::move(onDataCallback))
    , on_error_(std::move(onErrorCallback))
    , write_buffer_size_(write_buffer_size)
    , r_buffer_(std::make_shared<BufferT>())
    {}

template<typename BufferT>
template<typename BackendT>
    requires std::constructible_from<BufferT, std::shared_ptr<BackendT>>
Websocket<BufferT>::Websocket(
    std::string url,
    std::function<void()> &&onConnectedCallback,
    std::function<void()> &&onDiconnectedCallback,
    std::function<void(const char*, std::size_t)> &&onDataCallback,
    std::function<void(std::string &&err)> &&onErrorCallback,
    std::shared_ptr<BackendT> r_backend,
    uint32_t write_buffer_size)
    : url_(std::move(url))
    , on_connected_(std::move(onConnectedCallback))
    , on_diconnected_(std::move(onDiconnectedCallback))
    , on_data_(std::move(onDataCallback))
    , on_error_(std::move(onErrorCallback))
    , write_buffer_size_(write_buffer_size)
    , r_buffer_(std::make_shared<BufferT>(std::move(r_backend)))
    {}

template<typename BufferT>
Websocket<BufferT>::~Websocket() {
    if (impl_) {
        impl_->detach();
        impl_->cancel_open();
        if (impl_->session_started()) {
            impl_->close();
        }
    }
}

template<typename BufferT>
inline void Websocket<BufferT>::open() {
    if (impl_) {
        if (!impl_->session_started()) {
            if (!impl_->open_cancelled()) {
                LOG_INFO("WebSocket {} is connecting", url_);
                return;
            }
        }
        else if (impl_->status() == Status::CONNECTED) {
            LOG_INFO("WebSocket {} already connected", url_);
            return;
        }
        else if (impl_->status() == Status::CONNECTING) {
            LOG_INFO("WebSocket {} is connecting", url_);
            return;
        }
        auto old = std::move(impl_);
        old->detach();
        if (old->session_started()) {
            old->close();
        }
        if constexpr (is_shared_buffer<BufferT>) {
            // Shared backend: the new session reuses the same buffer; it must wait
            // until the old session's read loop releases it.
            impl_ = std::make_shared<Impl>(url_, on_connected_, on_diconnected_, on_data_,
                                           on_error_, write_buffer_size_, r_buffer_);
            impl_->mark_pending_open();
            old->set_on_buffer_released([new_impl = impl_]() { new_impl->open(); });
        } else {
            // Non-shared (flat_buffer): each session gets a fresh buffer immediately.
            impl_ = std::make_shared<Impl>(url_, on_connected_, on_diconnected_, on_data_,
                                           on_error_, write_buffer_size_, std::make_shared<BufferT>());
            impl_->open();
        }
        return;
    }
    impl_ = std::make_shared<Impl>(url_, on_connected_, on_diconnected_, on_data_,
                                   on_error_, write_buffer_size_, r_buffer_);
    impl_->open();
}

template<typename BufferT>
bool Websocket<BufferT>::close() {
    if (impl_) {
        if (!impl_->session_started()) {
            impl_->cancel_open();
            return true;
        }
        return impl_->close();
    }
    return true;
}

template<typename BufferT>
void Websocket<BufferT>::send(const char* buffer, std::size_t len, bool is_binary, bool suppress_log) {
    if (impl_) {
        impl_->send(buffer, len, is_binary, suppress_log);
    } else {
        LOG_WARN("WebSocket not initialized, cannot send data.");
    }
}

template<typename BufferT>
void Websocket<BufferT>::send_binary_data(const char* buffer, std::size_t len, bool suppress_log) {
    if (impl_) {
        impl_->send(buffer, len, true, suppress_log);
    } else {
        LOG_WARN("WebSocket not initialized, cannot send binary data.");
    }
}

template<typename BufferT>
typename Websocket<BufferT>::Status Websocket<BufferT>::status() const noexcept {
    if (impl_) {
        return impl_->status();
    }
    return Status::DISCONNECTED;
}

template<typename BufferT>
bool Websocket<BufferT>::is_running() noexcept {
    return detail::websocket_running();
}

template<typename BufferT>
void Websocket<BufferT>::shutdown() {
    detail::stop_websocket_service();
}

template<typename BufferT>
void Websocket<BufferT>::detach() {
    if (impl_) {
        impl_->detach();
    }
}

} // namespace slick::net
