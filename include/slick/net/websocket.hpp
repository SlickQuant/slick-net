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

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace slick::net
