#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <slick/net/detail/sse_parser.hpp>

namespace slick::net {

class HttpStream : public std::enable_shared_from_this<HttpStream> {
public:
    // Runs the stream on the shared HttpStream service (see set_service_threads)
    explicit HttpStream(
        std::string url,
        std::function<void()> &&onConnectedCallback,
        std::function<void()> &&onDisconnectedCallback,
        std::function<void(const char*, std::size_t)> &&onDataCallback,
        std::function<void(std::string err)> &&onErrorCallback,
        std::vector<std::pair<std::string, std::string>>&& headers = {}
    );

    // Runs the stream on executor instead of the shared service, e.g. an io_context the caller runs or a
    // boost::asio::thread_pool, so its I/O and callbacks never wait behind streams on other executors.
    // The stream's session, callbacks and close() run through a strand over executor, so they never run
    // concurrently even on a multi-threaded executor. open() does not start the shared service and
    // shutdown() does not stop the stream; keep the executor's context running until the stream
    // disconnects. A null executor selects the shared service.
    explicit HttpStream(
        boost::asio::any_io_executor executor,
        std::string url,
        std::function<void()> &&onConnectedCallback,
        std::function<void()> &&onDisconnectedCallback,
        std::function<void(const char*, std::size_t)> &&onDataCallback,
        std::function<void(std::string err)> &&onErrorCallback,
        std::vector<std::pair<std::string, std::string>>&& headers = {}
    );

    ~HttpStream();

    HttpStream(const HttpStream&) = delete;
    HttpStream& operator=(const HttpStream&) = delete;
    HttpStream(HttpStream&&) noexcept = delete;
    HttpStream& operator=(HttpStream&&) noexcept = delete;

    void open();
    void close();

    // Whether the shared service is running
    static bool is_running() noexcept;

    // Stops the shared service and joins its threads; streams on their own executors are unaffected. The next
    // open() of a stream on the shared service starts it again. Never call it from a callback of such a stream.
    static void shutdown();

    // Number of threads that run the shared service; default 1. Every stream runs through a strand of its own,
    // so with more threads a callback that blocks holds up only its own stream while the other threads serve
    // the rest, and callbacks of different streams may run concurrently. Lock-free; applies when the service
    // starts: at the first open() and at the first open() after shutdown(). A count of 0 is treated as 1.
    static void set_service_threads(std::size_t count) noexcept;
    static std::size_t service_threads() noexcept;

    enum class Status : uint8_t {
        CONNECTING,
        CONNECTED,
        DISCONNECTED,
    };

    Status status() const noexcept;

private:
    struct socket_registration;  // Defined in http_stream.cpp

    boost::asio::awaitable<void> do_stream_session();
    boost::asio::awaitable<void> do_stream_session_ssl();
    boost::asio::awaitable<void> do_stream_session_plain();
    template <typename Stream>
    boost::asio::awaitable<bool> stream_response(Stream& stream);

private:
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
    boost::asio::any_io_executor executor_;  // Strand this stream's sessions and close() run on
    const bool use_service_;                 // executor_ is a strand of the shared service
    std::atomic<Status> status_{ Status::DISCONNECTED };
    std::atomic_bool should_close_{false};
    detail::sse_parser sse_parser_;  // Parses text/event-stream bodies; executor_ only
    socket_registration* registered_socket_ = nullptr;  // Socket close() cancels; executor_ only
};

} // namespace slick::net
