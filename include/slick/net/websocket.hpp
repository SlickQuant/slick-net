#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace slick::net {

class Websocket {
public:
    explicit Websocket(
        std::string url,
        std::function<void()> &&onConnectedCallback,
        std::function<void()> &&onDiconnectedCallback,
        std::function<void(const char*, std::size_t)> &&onDataCallback,
        std::function<void(std::string &&err)> &&onErrorCallback
    );

    ~Websocket();

    Websocket(const Websocket&) = delete;
    Websocket& operator=(const Websocket&) = delete;
    Websocket(Websocket&&) noexcept;
    Websocket& operator=(Websocket&&) noexcept;

    // Starts a connection. Safe to call on a DISCONNECTED websocket or to
    // reconnect the same object after close() has completed.
    //
    // If called while the previous connection is still DISCONNECTING (e.g.
    // rapid reconnect), the outgoing close is allowed to finish in the
    // background but its onDisconnectedCallback will NOT fire — the callback
    // is suppressed so only the new session's events reach the caller. If you
    // need a guaranteed disconnected notification for every session, wait for
    // status() == DISCONNECTED before calling open() again.
    //
    // Safe to call from within any callback (onConnected, onDisconnected,
    // onData, onError) because the new connection is posted asynchronously
    // and does not block the service thread.
    void open();
    bool close();

    void send(const char* buffer, std::size_t len, bool is_binary = false);
    void send_binary_data(const char* buffer, std::size_t len);
    static void shutdown();

    enum class Status : std::uint8_t {
        CONNECTING,
        CONNECTED,
        DISCONNECTING,
        DISCONNECTED,
    };

    Status status() const noexcept;

    static bool is_running() noexcept;

    void reset_callbacks();

private:
    struct Impl;
    std::string url_;
    std::function<void()> on_connected_;
    std::function<void()> on_diconnected_;
    std::function<void(const char*, std::size_t)> on_data_;
    std::function<void(std::string &&err)> on_error_;
    std::shared_ptr<Impl> impl_;
};

} // namespace slick::net
