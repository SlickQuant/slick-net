#pragma once

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

#include <boost/asio/awaitable.hpp>

#include <slick/net/detail/sse_parser.hpp>

namespace slick::net {

class HttpStream : public std::enable_shared_from_this<HttpStream> {
public:
    explicit HttpStream(
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

    static bool is_running() noexcept;
    static void shutdown();

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
    std::atomic<Status> status_{ Status::DISCONNECTED };
    std::atomic_bool should_close_{false};
    detail::sse_parser sse_parser_;  // Parses text/event-stream bodies; service thread only
    socket_registration* registered_socket_ = nullptr;  // Socket close() cancels; service thread only
};

} // namespace slick::net
