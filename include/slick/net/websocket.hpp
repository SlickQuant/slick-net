#pragma once

#include <boost/beast/core/flat_buffer.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>

namespace slick::net {

/**
 * @brief WebSocket client templated on the read buffer type.
 *
 * @tparam BufferT  Supported DynamicBuffer_v1-compatible type. Defaults to
 *                  `boost::beast::flat_buffer` for zero-dependency, backward-compatible
 *                  behavior.
 *
 * The library explicitly instantiates these buffer types, so users only need
 * to include `<slick/net/websocket.hpp>` plus the buffer type's own header:
 *  - `boost::beast::flat_buffer` (default)
 *  - `slick::dynamic_buffer<slick::stream_buffer>`
 *  - `slick::dynamic_buffer<slick::stream_buffer_multiplexer::producer_buffer>`
 *
 * Additional buffer types need their template definitions visible in one
 * translation unit. Define `SLICK_NET_WEBSOCKET_HEADER_ONLY` before including
 * this header in that translation unit to opt into those definitions.
 *
 * For slick backends, use the `shared_ptr<BackendT>` constructor overload:
 * @code
 *   auto sb = std::make_shared<slick::stream_buffer>(1u<<26, 1u<<16);
 *   Websocket<slick::dynamic_buffer<slick::stream_buffer>> ws(url, callbacks..., sb);
 * @endcode
 */
template<typename BufferT = boost::beast::flat_buffer>
class Websocket {
public:
    /**
     * @brief Default constructor — only available when BufferT is default-constructible.
     *
     * Creates its own `shared_ptr<BufferT>` internally. Suitable for the default
     * `boost::beast::flat_buffer` and any other default-constructible DynamicBuffer.
     */
    explicit Websocket(
        std::string url,
        std::function<void()> &&onConnectedCallback,
        std::function<void()> &&onDiconnectedCallback,
        std::function<void(const char*, std::size_t)> &&onDataCallback,
        std::function<void(std::string &&err)> &&onErrorCallback,
        uint32_t write_buffer_size = 1u << 20
    ) requires std::default_initializable<BufferT>;

    /**
     * @brief Backend constructor — accepts a `shared_ptr<BackendT>` and constructs
     *        `BufferT` from it in-place.
     *
     * Participates in overload resolution only when `BufferT` is constructible from
     * `std::shared_ptr<BackendT>`.  Enables e.g.:
     * @code
     *   // stream_buffer
     *   auto sb = std::make_shared<slick::stream_buffer>(1u<<26, 1u<<16);
     *   Websocket<slick::dynamic_buffer<slick::stream_buffer>> ws(url, cbs..., sb);
     *
     *   // producer_buffer
     *   auto pb = mux.add_producer(0, 1u<<26, 1u<<16);
     *   Websocket<slick::dynamic_buffer<stream_buffer_multiplexer::producer_buffer>> ws(url, cbs..., pb);
     * @endcode
     */
    template<typename BackendT>
        requires std::constructible_from<BufferT, std::shared_ptr<BackendT>>
    explicit Websocket(
        std::string url,
        std::function<void()> &&onConnectedCallback,
        std::function<void()> &&onDiconnectedCallback,
        std::function<void(const char*, std::size_t)> &&onDataCallback,
        std::function<void(std::string &&err)> &&onErrorCallback,
        std::shared_ptr<BackendT> r_backend,
        uint32_t write_buffer_size = 1u << 20
    );

    ~Websocket();

    Websocket(const Websocket&) = delete;
    Websocket& operator=(const Websocket&) = delete;
    Websocket(Websocket&&) noexcept = default;
    Websocket& operator=(Websocket&&) noexcept = default;

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
    // For shared-backend buffers (e.g. slick::dynamic_buffer<T>) the new session
    // is deferred until the previous session's read loop has fully terminated
    // and released the shared backend. status() reads CONNECTING during this window.
    //
    // For flat_buffer and other non-shared buffers each reconnect starts a fresh
    // buffer immediately — no deferral is needed.
    //
    // Safe to call from within any callback (onConnected, onDisconnected,
    // onData, onError) because the new connection is posted asynchronously
    // and does not block the service thread.
    void open();
    bool close();

    void send(const char* buffer, std::size_t len, bool is_binary = false, bool suppress_log = false);
    void send_binary_data(const char* buffer, std::size_t len, bool suppress_log = false);
    static void shutdown();

    enum class Status : std::uint8_t {
        CONNECTING,
        CONNECTED,
        DISCONNECTING,
        DISCONNECTED,
    };

    Status status() const noexcept;

    static bool is_running() noexcept;

    void detach();

private:
    struct Impl;
    std::string url_;
    std::function<void()> on_connected_;
    std::function<void()> on_diconnected_;
    std::function<void(const char*, std::size_t)> on_data_;
    std::function<void(std::string &&err)> on_error_;
    std::shared_ptr<Impl> impl_;
    size_t write_buffer_size_;
    std::shared_ptr<BufferT> r_buffer_;
};

// Suppress implicit instantiation in user TUs — the library provides the definition.
extern template class Websocket<boost::beast::flat_buffer>;

} // namespace slick::net

#ifdef SLICK_NET_WEBSOCKET_HEADER_ONLY
#include <slick/net/detail/websocket_impl.hpp>
#endif
