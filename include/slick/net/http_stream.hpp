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

    // Starts a session: looks up the host, connects and streams the response. Does nothing while the stream is
    // CONNECTING or CONNECTED, so a stream never runs two sessions at once. After close(), or once the response
    // has ended, it starts a new session; if the previous session is still ending, the new one starts after it
    // ends, so the two sessions' callbacks never interleave. Every open() that starts a session gets exactly one
    // onDisconnected, even if close() ends the session before it connects. Lock-free; safe to call from any
    // thread and from the stream's callbacks.
    void open();

    // Ends the session: status() reads DISCONNECTED at once, and whatever the session is waiting on (connect,
    // TLS handshake, request or response I/O) is cancelled. A DNS lookup already running cannot be interrupted,
    // so its result is thrown away when it returns. After close() the session runs no more onConnected,
    // onData or onError callbacks (one already running finishes), and onDisconnected follows once the session
    // has ended. Does nothing if the stream is not open. Lock-free; safe to call from any thread and from the
    // stream's callbacks.
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
    struct session_io;  // Defined in http_stream.cpp

    bool is_current(std::uint64_t generation) const noexcept;
    void throw_if_closed(std::uint64_t generation) const;
    bool end_generation(std::uint64_t generation) noexcept;
    void serve();
    boost::asio::awaitable<void> serve_sessions();
    boost::asio::awaitable<void> run_session(std::uint64_t generation);
    boost::asio::awaitable<void> do_stream_session_ssl(std::uint64_t generation);
    boost::asio::awaitable<void> do_stream_session_plain(std::uint64_t generation);
    template <typename Stream>
    boost::asio::awaitable<bool> stream_response(std::uint64_t generation, Stream& stream);

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
    // The generation, the number of open() calls that started a session, above the Status in the lowest two bits.
    // Packed so open(), close() and the session change both in a single compare-and-swap.
    std::atomic<std::uint64_t> state_{ static_cast<std::uint64_t>(Status::DISCONNECTED) };
    detail::sse_parser sse_parser_;          // Parses text/event-stream bodies; executor_ only
    bool serving_ = false;                   // serve_sessions() is running; executor_ only
    std::uint64_t served_generation_ = 0;    // Newest generation serve_sessions() has taken up; executor_ only
    session_io* session_io_ = nullptr;       // I/O of the running session that close() cancels; executor_ only
};

} // namespace slick::net
