#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace slick {
    class SlickStreamBuffer;
}

namespace slick::net {

class Websocket {
public:
    explicit Websocket(
        std::string url,
        std::function<void()> &&onConnectedCallback,
        std::function<void()> &&onDiconnectedCallback,
        std::function<void(const char*, std::size_t)> &&onDataCallback,
        std::function<void(std::string &&err)> &&onErrorCallback,
        uint32_t write_buffer_size = 1u << 20,    // 1 MB write buffer
        uint32_t read_buffer_size = 1u << 26,     // 64 MB reading buffer
        uint32_t read_record_size = 1u << 16,     // 64K message records
        std::string read_buffer_shm_name = ""
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
    // The internal read buffer is single-producer, so the new session is
    // deferred until the previous session's read loop has fully terminated
    // and released the buffer (normally ~1 round trip; bounded by the close
    // timeout). status() reads CONNECTING during this window.
    //
    // Safe to call from within any callback (onConnected, onDisconnected,
    // onData, onError) because the new connection is posted asynchronously
    // and does not block the service thread.
    void open();
    bool close();

    void send(const char* buffer, std::size_t len, bool is_binary = false, bool suppress_log = false);
    void send_binary_data(const char* buffer, std::size_t len, bool suppress_log = false);
    static void shutdown();

    /**
     * @brief The callbacks passed in constructor are invoked on the websocket service thread. This function allows the caller to drain incoming messages onto their own thread and process them there.
     * @param cursor A reference to a cursor that tracks the position in the data queue. The caller should initialize it to 0 or initial_reading_index().
     * @param on_data The callback to invoke for each message.
     * @param max_num_data Maximum number of messages to drain in one iteration. This is to prevent starvation of the calling thread if the on_data callback is slow.
     * @return true if any data was drained, false if no data is available to drain.
     * @note This function is thread-safe and can be called concurrently with the websocket callbacks. The caller should know that using onDataCallback and drain_data will result in messages being processed twice.
     */
    bool drain_data(uint64_t &cursor, std::function<void(const char*, std::size_t)> &&on_data, std::size_t max_num_data = 100);

    enum class Status : std::uint8_t {
        CONNECTING,
        CONNECTED,
        DISCONNECTING,
        DISCONNECTED,
    };

    Status status() const noexcept;

    static bool is_running() noexcept;

    void detach();

    uint64_t initial_reading_index() const noexcept;

private:
    struct Impl;
    std::string url_;
    std::function<void()> on_connected_;
    std::function<void()> on_diconnected_;
    std::function<void(const char*, std::size_t)> on_data_;
    std::function<void(std::string &&err)> on_error_;
    std::shared_ptr<Impl> impl_;
    size_t write_buffer_size_;
    std::shared_ptr<slick::SlickStreamBuffer> r_stream_buffer_;
};

} // namespace slick::net
