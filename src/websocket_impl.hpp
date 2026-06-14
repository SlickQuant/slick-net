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
#include <slick/dynamic_buffer.h>
#include <slick/stream_buffer.h>

#include <atomic>
#include <csignal>
#include <cstring>
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
using Status = slick::net::Websocket::Status;

namespace slick::net::detail {

extern asio::io_context ioc_;
extern ssl::context ctx_;
extern std::thread service_thread_;
extern std::atomic_bool init_service_thread_;
extern std::atomic_bool run_;

void install_signal_handlers();

} // namespace slick::net::detail

namespace slick::net {

struct Websocket::Impl : public std::enable_shared_from_this<Websocket::Impl> {
    explicit Impl(
        std::string url,
        std::function<void()> onConnectedCallback,
        std::function<void()> onDisconnectedCallback,
        std::function<void(const char*, std::size_t)> onDataCallback,
        std::function<void(std::string &&err)> onErrorCallback,
        size_t write_buffer_size,
        std::shared_ptr<slick::SlickStreamBuffer> read_stream_buffer);

    void open();
    bool close();

    void send(const char* buffer, size_t len, bool is_binary = false, bool suppress_log = false);

    Websocket::Status status() const noexcept {
        return status_.load(std::memory_order_relaxed);
    }

    // Only sets the flag; the std::function members are never reassigned, so the
    // service thread can keep invoking them race-free behind is_detached() guards.
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

    // Cancel a deferred open that has not started yet (user closed or destroyed the
    // Websocket while waiting for the previous session to release the read buffer).
    // Only sets the flag: open() and the session coroutine observe it and abort,
    // forwarding buffer ownership down the handoff chain.
    void cancel_open() noexcept {
        open_cancelled_.store(true, std::memory_order_release);
    }

    // Make status() read CONNECTING while this impl waits for the previous session
    // to release the shared read buffer.
    void mark_pending_open() noexcept {
        status_.store(Websocket::Status::CONNECTING, std::memory_order_release);
    }

    /**
     * Register the continuation to run once this impl can no longer touch the shared
     * read stream buffer (its read loop has terminated). Called at most once, from the
     * user thread. If the buffer was already released, cb runs synchronously.
     * Lock-free: the std::function is written before the release-CAS publishes it;
     * release_buffer()'s acquire-exchange makes the write visible to the invoker.
     */
    void set_on_buffer_released(std::function<void()> cb) {
        on_buffer_released_ = std::move(cb);
        auto expected = ReleaseState::kActive;
        if (!release_state_.compare_exchange_strong(expected, ReleaseState::kCallbackSet,
                                                    std::memory_order_release,
                                                    std::memory_order_acquire)) {
            // Already released; the releaser saw kActive and will not read the callback.
            on_buffer_released_();
        }
    }

private:
    friend class Websocket;
    asio::awaitable<void> do_ws_session();
    asio::awaitable<void> do_ws_session_ssl();
    asio::awaitable<void> do_ws_session_plain();
    void do_write();
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void on_close(beast::error_code ec);

    // Mark the shared read stream buffer free for the next session: called on the
    // service thread at every point after which no further prepare/commit/consume can
    // go through this impl's r_buffer_. Idempotent; runs the registered continuation.
    void release_buffer() noexcept {
        auto prev = release_state_.exchange(ReleaseState::kReleased, std::memory_order_acq_rel);
        if (prev == ReleaseState::kCallbackSet) {
            on_buffer_released_();
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
    std::atomic<Websocket::Status> status_{ Websocket::Status::DISCONNECTED };
    slick::SlickQueue<char> w_buffer_;
    std::shared_ptr<slick::stream_buffer> r_stream_buffer_;
    slick::dynamic_buffer r_buffer_;
    uint64_t w_cursor_{0};
    std::atomic_bool in_writting_{false};
    std::atomic_bool detached_{false};

    // Lock-free ownership handoff of the shared read stream buffer between the
    // disconnecting session and the next one (see release_buffer / set_on_buffer_released).
    enum class ReleaseState : uint8_t { kActive, kCallbackSet, kReleased };
    std::atomic<ReleaseState> release_state_{ ReleaseState::kActive };
    std::function<void()> on_buffer_released_;  // written once before the kCallbackSet CAS
    std::atomic_bool session_started_{false};
    std::atomic_bool open_cancelled_{false};
    bool read_started_{false};                  // service thread only
};

} // namespace slick::net
