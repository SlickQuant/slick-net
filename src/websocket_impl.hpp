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
        std::function<void()> &&onConnectedCallback,
        std::function<void()> &&onDisconnectedCallback,
        std::function<void(const char*, std::size_t)> &&onDataCallback,
        std::function<void(std::string &&err)> &&onErrorCallback);

    void open();
    bool close();

    void send(const char* buffer, size_t len, bool is_binary = false);
    void send_binary_data(const char* buffer, size_t len);

    Websocket::Status status() const noexcept {
        return status_.load(std::memory_order_relaxed);
    }

    void reset_callbacks() {
        on_connected_ = [](){};
        on_diconnected_ = [](){};
        on_data_ = [](const char*, std::size_t){};
        on_error_ = [](std::string&&){};
    }

private:
    asio::awaitable<void> do_ws_session();
    asio::awaitable<void> do_ws_session_ssl();
    asio::awaitable<void> do_ws_session_plain();
    void do_write();
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void on_close(beast::error_code ec);

private:
    std::unique_ptr<websocket::stream<ssl::stream<beast::tcp_stream>>> wss_;
    std::unique_ptr<websocket::stream<beast::tcp_stream>> ws_;
    beast::flat_buffer r_buffer_;
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
    uint64_t w_cursor_{0};
    std::atomic_bool in_writting_{false};
};

} // namespace slick::net
