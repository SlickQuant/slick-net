#include <slick/net/websocket.hpp>
#include <slick/net/logging.h>

#include <memory>
#include <utility>

#define Websocket SlickNetLegacyWebsocket
#define LOG_DEBUG(...) ::slick::net::log_message(::slick::net::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...) ::slick::net::log_message(::slick::net::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) ::slick::net::log_message(::slick::net::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::slick::net::log_message(::slick::net::LogLevel::Error, __VA_ARGS__)
#define LOG_TRACE(...) ::slick::net::log_message(::slick::net::LogLevel::Trace, __VA_ARGS__)
#include "legacy/websocket_legacy_impl.h"
#undef LOG_TRACE
#undef LOG_ERROR
#undef LOG_WARN
#undef LOG_INFO
#undef LOG_DEBUG
#undef Websocket

namespace slick::net {
namespace {

using LegacyWebsocket = SlickNetLegacyWebsocket;

Websocket::Status to_status(LegacyWebsocket::Status status) {
    switch (status) {
        case LegacyWebsocket::Status::CONNECTING:
            return Websocket::Status::CONNECTING;
        case LegacyWebsocket::Status::CONNECTED:
            return Websocket::Status::CONNECTED;
        case LegacyWebsocket::Status::DISCONNECTING:
            return Websocket::Status::DISCONNECTING;
        case LegacyWebsocket::Status::DISCONNECTED:
        default:
            return Websocket::Status::DISCONNECTED;
    }
}

} // namespace

struct Websocket::Impl {
    explicit Impl(
        std::string url,
        std::function<void()> &&onConnectedCallback,
        std::function<void()> &&onDisconnectedCallback,
        std::function<void(const char*, std::size_t)> &&onDataCallback,
        std::function<void(std::string err)> &&onErrorCallback)
        : legacy_(std::make_shared<LegacyWebsocket>(
            std::move(url),
            std::move(onConnectedCallback),
            std::move(onDisconnectedCallback),
            std::move(onDataCallback),
            std::move(onErrorCallback))) {}

    std::shared_ptr<LegacyWebsocket> legacy_;
};

Websocket::Websocket(
    std::string url,
    std::function<void()> &&onConnectedCallback,
    std::function<void()> &&onDiconnectedCallback,
    std::function<void(const char*, std::size_t)> &&onDataCallback,
    std::function<void(std::string err)> &&onErrorCallback)
    : impl_(std::make_unique<Impl>(
        std::move(url),
        std::move(onConnectedCallback),
        std::move(onDiconnectedCallback),
        std::move(onDataCallback),
        std::move(onErrorCallback))) {}

Websocket::~Websocket() = default;
Websocket::Websocket(Websocket&&) noexcept = default;
Websocket& Websocket::operator=(Websocket&&) noexcept = default;

void Websocket::open() {
    impl_->legacy_->open();
}

bool Websocket::close() {
    return impl_->legacy_->close();
}

void Websocket::send(const char* buffer, std::size_t len, bool is_binary) {
    impl_->legacy_->send(buffer, len, is_binary);
}

void Websocket::send_binary_data(const char* buffer, std::size_t len) {
    impl_->legacy_->send_binary_data(buffer, len);
}

void Websocket::shutdown() {
    LegacyWebsocket::shutdown();
}

Websocket::Status Websocket::status() const noexcept {
    return to_status(impl_->legacy_->status());
}

bool Websocket::is_running() noexcept {
    return LegacyWebsocket::is_running();
}

} // namespace slick::net
