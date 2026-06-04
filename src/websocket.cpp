#include "websocket_impl.hpp"

namespace slick::net::detail {

asio::io_context ioc_;
ssl::context ctx_{ssl::context::tlsv12_client};
std::thread service_thread_;
std::atomic_bool init_service_thread_{ false };
std::atomic_bool run_;

extern "C" inline void __signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        Websocket::shutdown();
    }
}

void install_signal_handlers() {
    std::signal(SIGINT, __signal_handler);
    std::signal(SIGTERM, __signal_handler);
}

// Ensure Websocket::shutdown() is called at process exit.
struct WebsocketServiceTerminater {
    ~WebsocketServiceTerminater() {
        Websocket::shutdown();
    }
};

WebsocketServiceTerminater s_websocket_service_terminater;

} // namespace slick::net::detail

namespace slick::net {

void Websocket::Impl::do_write() {
    if (status_.load(std::memory_order_relaxed) != Status::CONNECTED) [[unlikely]] {
        if (status_.load(std::memory_order_relaxed) == Status::CONNECTING) {
            // Still connecting, repost do_write to try later.
            auto executor = use_ssl_ ? wss_->get_executor() : ws_->get_executor();
            asio::post(executor, [self = shared_from_this()]() {
                self->do_write();
            });
        }
        // Else: socket close is called.
        return;
    }
    // Read is already within the executor strand, safe to access w_cursor_.
    auto [msg, len] = w_buffer_.read(w_cursor_);
    if (msg && len) {
        bool is_binary = msg[0];
        ++msg;
        --len;

        LOG_DEBUG("--> {}", std::string_view(msg, len));
        // Only one async_write at a time - this is guaranteed by in_writting_ flag.
        if (use_ssl_) {
            wss_->binary(is_binary);
            wss_->async_write(
                asio::buffer(msg, len),
                beast::bind_front_handler(
                    &Websocket::Impl::on_write,
                    shared_from_this()));
        } else {
            ws_->binary(is_binary);
            ws_->async_write(
                asio::buffer(msg, len),
                beast::bind_front_handler(
                    &Websocket::Impl::on_write,
                    shared_from_this()));
        }
    }
    else {
        // No more data to write, release the write lock.
        in_writting_.store(false, std::memory_order_release);
    }
}

void Websocket::Impl::on_write(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);
    if(ec) {
        if (detail::run_.load(std::memory_order_relaxed) &&
            status_.load(std::memory_order_relaxed) == Status::CONNECTED &&
            ec != beast::websocket::error::closed &&
            ec != asio::error::eof &&
            ec != asio::error::operation_aborted &&
            ec != ssl::error::stream_truncated &&
            !(ec.value() == 995 && ec.category() == boost::system::system_category())) {
            on_error_(std::format("Failed to write {}", ec.message()));
            close();
        }
        in_writting_.store(false, std::memory_order_release);
        return;
    }

    // Continue writing next message if available.
    // This is safe because we're already in the strand and in_writting_ is still true.
    do_write();
}

void Websocket::Impl::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    if(ec) {
        if (detail::run_.load(std::memory_order_relaxed) &&
            status_.load(std::memory_order_relaxed) == Status::CONNECTED &&
            ec != beast::websocket::error::closed &&
            ec != asio::error::eof &&
            ec != asio::error::operation_aborted &&
            ec != ssl::error::stream_truncated &&
            !(ec.value() == 995 && ec.category() == boost::system::system_category())) {
            on_error_(std::format("Failed to read {}", ec.message()));
            close();
        }
        else if (status_.load(std::memory_order_relaxed) == Status::CONNECTED) {
            // EOF or websocket::error::closed means graceful disconnect.
            status_.store(Status::DISCONNECTED, std::memory_order_release);
            on_diconnected_();
        }
        return;
    }

    if (detail::run_.load(std::memory_order_relaxed) &&
        status_.load(std::memory_order_relaxed) == Status::CONNECTED) {
        LOG_TRACE("<-- {}", std::string((const char*)r_buffer_.data().data(), bytes_transferred));
        on_data_((const char*)r_buffer_.data().data(), bytes_transferred);
        r_buffer_.consume(bytes_transferred);

        // Read next message.
        if (use_ssl_) {
            wss_->async_read(
                r_buffer_,
                beast::bind_front_handler(
                    &Websocket::Impl::on_read,
                    shared_from_this()));
        } else {
            ws_->async_read(
                r_buffer_,
                beast::bind_front_handler(
                    &Websocket::Impl::on_read,
                    shared_from_this()));
        }
    }
}

void Websocket::Impl::on_close(beast::error_code ec) {
    if (ec && detail::run_.load(std::memory_order_relaxed) &&
        ec != beast::websocket::error::closed &&
        ec != asio::error::eof &&
        ec != asio::error::operation_aborted &&
        ec != ssl::error::stream_truncated &&
        !(ec.value() == 995 && ec.category() == boost::system::system_category())) {
        on_error_(ec.message());
    }

    // If we get here then the connection is closed gracefully.
    LOG_INFO("Websocket {} closed", url_);
    status_.store(Status::DISCONNECTED, std::memory_order_release);
    on_diconnected_();
}

bool Websocket::Impl::close() {
    if (status_.load(std::memory_order_relaxed) < Status::DISCONNECTING) {
        LOG_INFO("Closing {}", url_);
        status_.store(Status::DISCONNECTING, std::memory_order_release);
        // Close the WebSocket connection.
        if (use_ssl_) {
            wss_->async_close(
                websocket::close_code::normal,
                beast::bind_front_handler(
                    &Websocket::Impl::on_close,
                    shared_from_this()));
        } else {
            ws_->async_close(
                websocket::close_code::normal,
                beast::bind_front_handler(
                    &Websocket::Impl::on_close,
                    shared_from_this()));
        }
        return true;
    }
    return false;
}

void Websocket::Impl::send(const char* buffer, size_t len, bool is_binary) {
    if (status_.load(std::memory_order_relaxed) > Status::CONNECTED) {
        LOG_WARN("WebSocket not connected, cannot send data.");
        return;
    }
    auto l = static_cast<uint32_t>(len) + 1;   // +1 for is_bool flag
    auto index = w_buffer_.reserve(l);
    *w_buffer_[index] = static_cast<char>(is_binary);
    memcpy(w_buffer_[index + 1], buffer, len);
    w_buffer_.publish(index, l);

    // Always post to the executor to ensure thread-safe write initiation.
    auto executor = use_ssl_ ? wss_->get_executor() : ws_->get_executor();
    asio::post(executor, [self = shared_from_this()]() {
        // Check and set in_writting_ atomically within the executor context.
        bool expected = false;
        if (self->in_writting_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            self->do_write();
        }
    });
}

void Websocket::Impl::send_binary_data(const char* buffer, size_t len) {
    send(buffer, len, true);
}

Websocket::Websocket(
    std::string url,
    std::function<void()> &&onConnectedCallback,
    std::function<void()> &&onDiconnectedCallback,
    std::function<void(const char*, std::size_t)> &&onDataCallback,
    std::function<void(std::string &&err)> &&onErrorCallback)
    : url_(std::move(url)),
      on_connected_(std::move(onConnectedCallback)),
      on_diconnected_(std::move(onDiconnectedCallback)),
      on_data_(std::move(onDataCallback)),
      on_error_(std::move(onErrorCallback))
    {}

Websocket::~Websocket() {
    if (impl_) {
        // Ensure the WebSocket connection is closed gracefully.
        impl_->reset_callbacks(); // Prevent callbacks from being called during destruction.
        impl_->close();
    }
}
Websocket::Websocket(Websocket&&) noexcept = default;
Websocket& Websocket::operator=(Websocket&&) noexcept = default;

void Websocket::open() {
    if (impl_) {
        if (impl_->status() == Status::CONNECTED) {
            LOG_INFO("WebSocket {} already connected", url_);
            return;
        }
        else if (impl_->status() == Status::CONNECTING) {
            LOG_INFO("WebSocket {} is connecting", url_);
            return;
        }
        impl_->reset_callbacks();
        impl_->close();
    }
    impl_ = std::make_shared<Impl>(url_, on_connected_, on_diconnected_, on_data_, on_error_);
    impl_->open();
}

bool Websocket::close() {
    if (impl_) {
        return impl_->close();
    }
    return true;
}

void Websocket::send(const char* buffer, std::size_t len, bool is_binary) {
    if (impl_) {
        impl_->send(buffer, len, is_binary);
    } else {
        LOG_WARN("WebSocket not initialized, cannot send data.");
    }
}

void Websocket::send_binary_data(const char* buffer, std::size_t len) {
    if (impl_) {
        impl_->send_binary_data(buffer, len);
    } else {
        LOG_WARN("WebSocket not initialized, cannot send binary data.");
    }
}

Websocket::Status Websocket::status() const noexcept {
    if (impl_) {
        return impl_->status();
    }
    return Status::DISCONNECTED;
}

bool Websocket::is_running() noexcept {
    return detail::run_.load(std::memory_order_relaxed);
}

void Websocket::shutdown() {
    if (detail::run_.load(std::memory_order_relaxed)) {
        LOG_DEBUG("Shutting down WebSocket service thread.");
        detail::run_.store(false, std::memory_order_release);
        detail::ioc_.stop();
        if (detail::service_thread_.joinable()) {
            detail::service_thread_.join();
        }
    }
}

void Websocket::reset_callbacks() {
    if (impl_) {
        impl_->reset_callbacks();
    }
}

} // namespace slick::net
