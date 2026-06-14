#include "websocket_impl.hpp"
#include <slick/stream_buffer.h>

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
        bool suppress_log = msg[1];
        msg += 2; // +2 for is_binary and suppress_log flag
        len -= 2; // -2 for is_binary and suppress_log flag

        if (!suppress_log) {
            LOG_DEBUG("--> {}", std::string_view(msg, len));
        }

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
            if (!is_detached()) {
                on_error_(std::format("Failed to write {}", ec.message()));
            }
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
            if (!is_detached()) {
                on_error_(std::format("Failed to read {}", ec.message()));
            }
            close();
        }
        else if (status_.load(std::memory_order_relaxed) == Status::CONNECTED) {
            // EOF or websocket::error::closed means graceful disconnect.
            status_.store(Status::DISCONNECTED, std::memory_order_release);
            if (!is_detached()) {
                on_diconnected_();
            }
        }
        // The read loop has terminated: no further prepare/commit can reach the shared
        // stream buffer through this impl. Hand ownership to the next session; stale
        // partial bytes are discarded by the next session before its first read.
        release_buffer();
        return;
    }

    if (detail::run_.load(std::memory_order_relaxed) &&
        status_.load(std::memory_order_relaxed) == Status::CONNECTED &&
        !is_detached()) {
        LOG_TRACE("<-- {}", std::string((const char*)r_buffer_.data().data(), bytes_transferred));
        auto record = r_buffer_.consume(bytes_transferred);
        on_data_((const char*)record.data, record.length);

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
    else {
        // Disconnecting, shutting down, or detached: stop reading without publishing
        // the bytes beast already committed and hand the buffer to the next session.
        release_buffer();
    }
}

void Websocket::Impl::on_close(beast::error_code ec) {
    if (ec && detail::run_.load(std::memory_order_relaxed) &&
        ec != beast::websocket::error::closed &&
        ec != asio::error::eof &&
        ec != asio::error::operation_aborted &&
        ec != ssl::error::stream_truncated &&
        !(ec.value() == 995 && ec.category() == boost::system::system_category())) {
        if (!is_detached()) {
            on_error_(ec.message());
        }
    }

    // If we get here then the connection is closed gracefully.
    LOG_INFO("Websocket {} closed", url_);
    status_.store(Status::DISCONNECTED, std::memory_order_release);
    if (!is_detached()) {
        on_diconnected_();
    }
}

bool Websocket::Impl::close() {
    // Claim the DISCONNECTING state atomically so only one caller initiates teardown.
    Status expected = status_.load(std::memory_order_relaxed);
    do {
        if (expected >= Status::DISCONNECTING) {
            return false;
        }
    } while (!status_.compare_exchange_weak(expected, Status::DISCONNECTING,
                                            std::memory_order_acq_rel, std::memory_order_relaxed));
    LOG_INFO("Closing {}", url_);
    // The beast stream is not thread-safe: initiate teardown on the stream's executor
    // so it cannot race operations the service thread is running on the same stream.
    auto executor = use_ssl_ ? wss_->get_executor() : ws_->get_executor();
    asio::post(executor, [self = shared_from_this(), was = expected]() {
        if (was == Status::CONNECTING) {
            // The websocket handshake may not be complete, so a close handshake is not
            // possible. Closing the lowest layer cancels the in-flight connect ops and
            // the session coroutine finishes through its completion handler.
            if (self->use_ssl_) {
                beast::get_lowest_layer(*self->wss_).close();
            } else {
                beast::get_lowest_layer(*self->ws_).close();
            }
            return;
        }
        // Close the WebSocket connection gracefully.
        if (self->use_ssl_) {
            self->wss_->async_close(
                websocket::close_code::normal,
                beast::bind_front_handler(
                    &Websocket::Impl::on_close,
                    self));
        } else {
            self->ws_->async_close(
                websocket::close_code::normal,
                beast::bind_front_handler(
                    &Websocket::Impl::on_close,
                    self));
        }
    });
    return true;
}

void Websocket::Impl::send(const char* buffer, size_t len, bool is_binary, bool suppress_log) {
    if (status_.load(std::memory_order_relaxed) > Status::CONNECTED) {
        LOG_WARN("WebSocket not connected, cannot send data.");
        return;
    }
    auto l = static_cast<uint32_t>(len) + 2;   // +2 for is_binary flag and length prefix
    auto index = w_buffer_.reserve(l);
    *w_buffer_[index] = static_cast<char>(is_binary);
    *w_buffer_[index + 1] = static_cast<char>(suppress_log);
    memcpy(w_buffer_[index + 2], buffer, len);
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

Websocket::Websocket(
    std::string url,
    std::function<void()> &&onConnectedCallback,
    std::function<void()> &&onDiconnectedCallback,
    std::function<void(const char*, std::size_t)> &&onDataCallback,
    std::function<void(std::string &&err)> &&onErrorCallback,
    uint32_t write_buffer_size,
    uint32_t read_buffer_size,
    uint32_t read_record_size,
    std::string read_buffer_shm_name)
    : url_(std::move(url))
    , on_connected_(std::move(onConnectedCallback))
    , on_diconnected_(std::move(onDiconnectedCallback))
    , on_data_(std::move(onDataCallback))
    , on_error_(std::move(onErrorCallback))
    , write_buffer_size_(write_buffer_size)
    , r_stream_buffer_(std::make_shared<slick::SlickStreamBuffer>(read_buffer_size, read_record_size, read_buffer_shm_name.empty() ? nullptr : read_buffer_shm_name.c_str()))
    {}

Websocket::~Websocket() {
    if (impl_) {
        impl_->detach();      // Prevent callbacks from being called during destruction.
        impl_->cancel_open(); // Abort a deferred open if one is still pending.
        if (impl_->session_started()) {
            // Ensure the WebSocket connection is closed gracefully.
            impl_->close();
        }
    }
}

void Websocket::open() {
    if (impl_) {
        if (!impl_->session_started()) {
            // A deferred open is already waiting for the previous session to release
            // the shared read buffer.
            if (!impl_->open_cancelled()) {
                LOG_INFO("WebSocket {} is connecting", url_);
                return;
            }
            // The pending open was cancelled by close(); chain a fresh impl behind it
            // below so buffer ownership still propagates in order.
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
        old->detach(); // Suppress callbacks from the dying session.
        if (old->session_started()) {
            old->close(); // Graceful close in the background; no-op if already closing/closed.
        }
        impl_ = std::make_shared<Impl>(url_, on_connected_, on_diconnected_, on_data_, on_error_, write_buffer_size_, r_stream_buffer_);
        impl_->mark_pending_open();
        // The shared read stream buffer is single-producer: the new session must not
        // issue reads until the old session's read loop can no longer touch the buffer.
        // Runs synchronously if the old session has already released it.
        old->set_on_buffer_released([new_impl = impl_]() { new_impl->open(); });
        return;
    }
    impl_ = std::make_shared<Impl>(url_, on_connected_, on_diconnected_, on_data_, on_error_, write_buffer_size_, r_stream_buffer_);
    impl_->open();
}

bool Websocket::close() {
    if (impl_) {
        if (!impl_->session_started()) {
            // Deferred open that has not started yet: cancel it instead of closing.
            impl_->cancel_open();
            return true;
        }
        return impl_->close();
    }
    return true;
}

void Websocket::send(const char* buffer, std::size_t len, bool is_binary, bool suppress_log) {
    if (impl_) {
        impl_->send(buffer, len, is_binary, suppress_log);
    } else {
        LOG_WARN("WebSocket not initialized, cannot send data.");
    }
}

void Websocket::send_binary_data(const char* buffer, std::size_t len, bool suppress_log) {
    if (impl_) {
        impl_->send(buffer, len, true, suppress_log);
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

void Websocket::detach() {
    if (impl_) {
        impl_->detach();
    }
}

bool Websocket::drain_data(uint64_t &cursor, std::function<void(const char*, std::size_t)> &&on_data, std::size_t max_num_data) {
    std::size_t count = 0;
    while (count < max_num_data) {
        auto record = r_stream_buffer_->read(cursor);
        if (!record.first || record.second == 0) {
            break; // No more data to drain
        }
        on_data((const char*)record.first, record.second);
        ++count;
    }
    return count > 0;
}

uint64_t Websocket::initial_reading_index() const noexcept {
    return r_stream_buffer_->initial_reading_index();
}

} // namespace slick::net
