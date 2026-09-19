# slick-net

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![CI](https://github.com/SlickQuant/slick-net/actions/workflows/ci.yml/badge.svg)](https://github.com/SlickQuant/slick-net/actions/workflows/ci.yml)
[![GitHub release](https://img.shields.io/github/v/release/SlickQuant/slick-net)](https://github.com/SlickQuant/slick-net/releases)

A high-performance C++ HTTP/WebSocket client library built on Boost.Beast with full SSL/TLS support. Designed for asynchronous, non-blocking HTTP/WebSocket communication in modern C++ applications.

## Features

- **HTTP/HTTPS Client**: Full support for GET, POST, PUT, PATCH, and DELETE methods
- **HTTP Streaming**: Support for Server-Sent Events (SSE) and chunked response streaming
- **Asynchronous WebSocket Client**: Built on Boost.Asio coroutines for high-performance async operations
- **SSL/TLS Support**: Native support for secure `https://` and `wss://` connections with certificate and host name verification against the system trust roots
- **Multiple Async APIs**: Synchronous, callback-based, and C++20 coroutine awaitable interfaces
- **Cross-Platform**: Works on Windows, Linux, and macOS
- **Static Library by Default**: Heavy networking implementation compiles once in `slick-net`
- **Callback-Based API**: Clean event-driven interface for connection lifecycle management
- **Thread-Safe**: Proper strand management for concurrent operations
- **Modern C++20**: Leverages coroutines and modern C++ features

## Dependencies

- **Boost** (1.75+): beast, asio, context components
- **OpenSSL**: For SSL/TLS support
- **C++20 Compiler**: Required for coroutine support
  - GCC 14+ (GCC 13 has a known bug with coroutine lambdas in test code)
  - Clang 14+
  - MSVC 2022+

## Installation

### Dependencies via vcpkg

Install [vcpkg](https://github.com/microsoft/vcpkg) and bootstrap it:

```bash
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh   # Linux/macOS
.\bootstrap-vcpkg.bat  # Windows
```

Install the required packages (select the triplet that matches your platform):

| Platform | Default triplet | Static triplet |
|---|---|---|
| Windows x64 | `x64-windows` | `x64-windows-static` |
| Linux x64 | `x64-linux` | *(already static)* |
| Linux arm64 | `arm64-linux` | *(already static)* |
| macOS x64 | `x64-osx` | `x64-osx-static` |
| macOS arm64 | `arm64-osx` | `arm64-osx-static` |

```bash
vcpkg install boost-asio boost-beast boost-context boost-system openssl --triplet <triplet>
```

Then pass the vcpkg toolchain file to CMake:

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet>
```

#### Static linking

Pass `-DLINK_STATICALLY=ON` to CMake — it sets the correct static vcpkg triplet automatically and enables static Boost/OpenSSL linkage:

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DLINK_STATICALLY=ON
```

#### Native CPU optimizations

Release builds are portable by default. Pass `-DSLICK_NET_ENABLE_NATIVE_ARCH=ON` to compile the
library with `-march=native` (GCC/Clang only; ignored on MSVC and when cross-compiling):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSLICK_NET_ENABLE_NATIVE_ARCH=ON
```

Only enable this when the binary runs on the machine that built it — `-march=native` bakes in the
build host's instruction set, and the resulting artifacts crash with an illegal-instruction fault on
any CPU that lacks those extensions.

### CMake Integration

Add slick-net as a subdirectory in your CMake project:

```cmake
add_subdirectory(path/to/slick-net)
target_link_libraries(your_target PRIVATE slick::net)
```

`slick::net` is the default static-library target.

Or use FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(
    slick-net
    GIT_REPOSITORY https://github.com/SlickQuant/slick-net.git
    GIT_TAG main
)
FetchContent_MakeAvailable(slick-net)
target_link_libraries(your_target PRIVATE slick::net)
```

### Runtime Logging Hooks

Internal slick-net logs are routed via runtime hooks. `set_log_handler()` optionally
takes a `LogLevelGetter` so `LOG_*` macros can skip formatting/argument evaluation
entirely when the level is disabled:

```cpp
#include <slick/net/logging.hpp>

slick::net::set_log_handler(
    [](slick::net::LogLevel level, const char* format_text, std::format_args args) {
        // Route to your logger
    },
    []() {
        return slick::net::LogLevel::Info; // minimum level to log
    }
);

// Optional cleanup
slick::net::clear_log_handler();
```

The handler and its level getter are installed as one immutable pair and swapped
atomically, so they can be set, replaced or cleared from any thread while HTTP and
WebSocket worker threads are logging. `clear_log_handler()` stops further dispatch
but does not wait for a handler already running on another thread, so state a
handler captures must stay valid until the last call that can reach it has
finished.

**LogLevel:** `Trace`, `Debug`, `Info`, `Warn`, `Error`, `Fatal`, `Off`

**Macros:** `LOG_TRACE`, `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_FATAL` — each
checks `should_log()` before evaluating its arguments.

### URLs

`Http`, `HttpStream` and `Websocket` accept `[scheme://]host[:port][/path][?query]`. The host
may be a name, an IPv4 address or a bracketed IPv6 literal (`http://[::1]:8080/feed`). The scheme
must be `http`/`https` for `Http` and `HttpStream`, or `ws`/`wss` for `Websocket` (matched
case-insensitively); without a scheme `https`/`wss` is assumed. Without a port, 80 is used for
`http`/`ws` and 443 for `https`/`wss`.
The `Host` header of `Http` and `HttpStream` requests carries the port only when it is not the
connection's default (80, or 443 with TLS), e.g. `Host: api.example.com:8080`.
A `#fragment` is not sent. Any other scheme, a port outside 1-65535 or an unterminated `[` is
rejected with `std::invalid_argument`: `Http` reports it as a `500` response, the `HttpStream`
constructor throws it, and `Websocket::open()` throws it.

### TLS Certificate Verification

`Http`, `HttpStream` and `Websocket` share one TLS client context for `https://` and
`wss://`. Every handshake verifies the server certificate chain and requires the
certificate to match the URL's host name (or IP address); a failure is reported as
`TLS handshake failed (<reason>)`, e.g. `hostname mismatch` or `self-signed certificate`.

Trust roots are loaded on first use:

| Condition | Trust roots |
|---|---|
| `SSL_CERT_FILE` or `SSL_CERT_DIR` set | Exactly those locations |
| Windows | Windows `ROOT` certificate store + OpenSSL default paths |
| Linux / macOS | OpenSSL default paths, or the OS CA bundle (e.g. `/etc/ssl/certs/ca-certificates.crt`, `/etc/ssl/cert.pem`) when OpenSSL's default bundle is missing |

To trust a private CA, configure `slick::net::tls_context()` before opening connections
(it must not be modified while handshakes are in progress):

```cpp
#include <slick/net/tls.hpp>

slick::net::tls_context().load_verify_file("corp-root-ca.pem");
// or from memory:
slick::net::tls_context().add_certificate_authority(boost::asio::buffer(pem));
```

Setting `tls_context().set_verify_mode(boost::asio::ssl::verify_none)` disables both
certificate and host name verification and exposes connections to man-in-the-middle
interception — use it only for local testing.

### Signal Handling

`slick-net` does not install `SIGINT`/`SIGTERM` handlers — signal dispositions belong to
the application. Without a handler the default action terminates the process; the
`Websocket`, `Http` and `HttpStream` services are shut down automatically at normal program exit.

For a graceful Ctrl-C, record the signal in your handler and call `shutdown()` from normal
code. `shutdown()` stops the `io_context` and joins the service thread, so it must never be
called from a signal handler:

```cpp
#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include <slick/net/websocket.hpp>

namespace {
std::atomic_bool stop_requested{false}; // lock-free: safe to store from a signal handler

void on_signal(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}
}

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // ... create and open connections ...

    while (!stop_requested.load(std::memory_order_relaxed) &&
           slick::net::Websocket<>::is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    slick::net::Websocket<>::shutdown();
}
```

## Usage

### Basic WebSocket Client

```cpp
#include <slick/net/websocket.hpp>

using namespace slick::net;

int main() {
    Websocket<> ws(
        "wss://ws.postman-echo.com/raw",           // WebSocket URL
        []() {                                // onConnected
            std::cout << "Connected!\n";
        },
        []() {                                // onDisconnected
            std::cout << "Disconnected!\n";
        },
        [](const char* data, size_t size) {   // onData
            std::cout << "Received: " << std::string(data, size) << "\n";
        },
        [](std::string err) {                 // onError
            std::cerr << "Error: " << err << "\n";
        }
    );
    
    ws.open();
    
    // Send a message
    std::string message = "Hello, WebSocket!";
    ws.send(message.data(), message.size());
    
    // Keep the application running
    while(Websocket<>::is_running()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    return 0;
}
```

### Advanced Usage with JSON

```cpp
#include <slick/net/websocket.hpp>
#include <nlohmann/json.hpp>

using namespace slick::net;
using json = nlohmann::json;

int main() {
    std::shared_ptr<Websocket<>> ws;
    ws = std::make_shared<Websocket<>>(
        "wss://advanced-trade-ws.coinbase.com",
        [&]() { 
            std::cout << "Connected to Coinbase\n";
            // Subscribe to market data
            json subscribe_msg = {
                {"type", "subscribe"},
                {"channel", "level2"},
                {"product_ids", {"BTC-USD"}}
            };
            auto msg_str = subscribe_msg.dump();
            ws->send(msg_str.data(), msg_str.size());
        },
        []() {
            std::cout << "Disconnected from Coinbase\n";
        },
        [](const char* data, size_t size) {
            std::cout << "Market data: " << std::string(data, size) << "\n";
        },
        [](std::string err) {
            std::cerr << "Error: " << err << "\n";
        }
    );
    
    ws->open();
    
    // Ctrl + C to exit
    // Keep running
    while(Websocket<>::is_running()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    return 0;
}
```

## Build Examples

The repository includes working examples. To build them:

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

Run examples:
```bash
./examples/websocket_client_example
./examples/websocket_with_stream_buffer_example
./examples/websocket_with_stream_buffer_multiplexer_example
./examples/websocket_with_custom_buffer_example
./examples/http_client_example
./examples/http_stream_client_example
./examples/http_awaitable_client_example
```

## API Reference

### Http Class

**Synchronous Methods:**
```cpp
Http::Response get(std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
Http::Response post(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
Http::Response put(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
Http::Response patch(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
Http::Response del(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
```

Synchronous methods block the calling thread until the response (or an error) arrives and are safe to call concurrently from any number of threads. Each call runs on an `io_context` of its own, taken from a small lock-free pool of idle contexts that are reused across calls, so concurrent callers share no state, never lock, and the calling threads can exit normally.

**Asynchronous Callback-Based Methods:**
```cpp
void async_get(std::function<void(Response&&)> on_response, std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
void async_post(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
void async_put(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
void async_patch(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
void async_del(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});

// Same, with on_response posted to executor; a null executor selects the callback threads
void async_get(boost::asio::any_io_executor executor, std::function<void(Response&&)> on_response, std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
void async_post(boost::asio::any_io_executor executor, std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
void async_put(boost::asio::any_io_executor executor, std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
void async_patch(boost::asio::any_io_executor executor, std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
void async_del(boost::asio::any_io_executor executor, std::function<void(Response&&)> on_response, std::string_view url, std::string_view data = "", std::vector<std::pair<std::string, std::string>>&& headers = {});
```

Callback-based methods return as soon as the request is queued. The first such call starts a shared service, and every request's I/O runs on that service's I/O thread. Callbacks never run on the I/O thread, so a slow callback cannot hold up another request's I/O. Where a callback runs:

- **Without an executor:** it runs on the service's callback threads, `Http::callback_threads()` of them, 1 by default. With the default single thread, a callback that blocks makes the callbacks queued behind it wait, but responses keep arriving. With more threads, a blocked callback holds up only its own thread, and different requests' callbacks may run at the same time. `set_callback_threads(0)` runs callbacks inline on the I/O thread instead. That saves a thread hop per response but brings back the stall, so use it only for callbacks that never block.
- **With an executor**, such as an `io_context` you run, a strand or a `boost::asio::thread_pool`: the callback is posted there, while the request's I/O still runs on the I/O thread. This is the way to receive responses on your own event-loop thread without any synchronization. The request does not count as work on that executor's context, so keep the context running and alive until the callback has run, or until `shutdown()` abandons the request. For an `io_context`, hold a work guard. A callback that throws propagates out of that context's `run()`.

```cpp
// Run callbacks of requests made without an executor on 4 threads; applies when the service starts
Http::set_callback_threads(4);

// Or receive the response on your own event loop
boost::asio::io_context loop;
auto work = boost::asio::make_work_guard(loop);  // keeps loop.run() waiting for the callback
Http::async_get(loop.get_executor(), [&work](Http::Response&& rsp) {
    // runs on the thread running loop.run()
    work.reset();  // nothing left to wait for, so let run() return
}, "https://api.example.com/data");
loop.run();
```

**Async Service Control:**
```cpp
static bool is_running() noexcept;                         // whether the shared async service is running
static void shutdown();                                    // stop it and join its threads
static void set_callback_threads(std::size_t count) noexcept;  // callback threads (default 1, 0 = inline on the I/O thread); lock-free, applies when the service starts
static std::size_t callback_threads() noexcept;            // configured number of callback threads
```

`shutdown()` stops the shared service and joins its threads, so a hung request cannot stall program exit. It abandons the requests still in flight and any callbacks queued on the callback threads that have not run yet. Their callbacks are never called, even if a later request restarts the service, so an abandoned request never posts to the executor it was given. A callback already posted to a caller's executor still runs there. The next callback-based `async_*()` call starts the service again. `shutdown()` runs automatically at normal program exit, so an in-flight request can never use the service while the statics it runs on are being destroyed. Because it joins the service threads, never call it from a response callback or from a signal handler. The awaitable methods below run on the caller's executor and are unaffected by it.

**Asynchronous Awaitable Methods (C++20 Coroutines):**
```cpp
asio::awaitable<Response> async_get(std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
asio::awaitable<Response> async_post(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
asio::awaitable<Response> async_put(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
asio::awaitable<Response> async_patch(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
asio::awaitable<Response> async_del(std::string_view url, std::string_view data = "", std::vector<std::pair<std::string, std::string>>&& headers = {});
```

**Response Structure:**
```cpp
struct Response {
    uint32_t result_code;     // HTTP status code
    std::string reason;       // Reason phrase of the status line, e.g. "OK"
    std::string result_text;  // Response body or error message
    bool is_ok() const;       // Returns true if status code is 2xx
};
```

**Example Usage - Synchronous:**
```cpp
#include <slick/net/http.hpp>

// Synchronous GET
auto response = Http::get("https://api.example.com/data");
if (response.is_ok()) {
    std::cout << response.result_text << std::endl;
}
```

**Example Usage - Asynchronous Callback-Based:**
```cpp
#include <slick/net/http.hpp>

// Asynchronous POST with JSON
nlohmann::json data = {{"key", "value"}};
Http::async_post([](Http::Response&& rsp) {
    if (rsp.is_ok()) {
        std::cout << "Success: " << rsp.result_text << std::endl;
    }
}, "https://api.example.com/resource", data.dump(), {{"Content-Type", "application/json"}});
```

**Example Usage - Asynchronous Awaitable (C++20 Coroutines):**
```cpp
#include <slick/net/http.hpp>
#include <boost/asio.hpp>

asio::awaitable<void> fetch_data() {
    // Awaitable GET - clean async/await syntax
    auto response = co_await Http::async_get("https://api.example.com/data");
    if (response.is_ok()) {
        std::cout << "Response: " << response.result_text << std::endl;
    }

    // Sequential requests
    nlohmann::json post_data = {{"key", "value"}};
    auto post_response = co_await Http::async_post(
        "https://api.example.com/resource",
        post_data.dump(),
        {{"Content-Type", "application/json"}}
    );

    if (post_response.is_ok()) {
        std::cout << "Created: " << post_response.result_text << std::endl;
    }
}

int main() {
    asio::io_context ioc;

    asio::co_spawn(ioc, fetch_data(), asio::detached);

    ioc.run();
    return 0;
}
```

### Websocket Class

`Websocket` is a class template parameterized on the read buffer type. The default is
`boost::beast::flat_buffer`, which is backward-compatible with existing code. The
library also explicitly instantiates the slick stream-buffer backends below, so users
can include `<slick/net/websocket.hpp>` instead of the implementation header.

**Constructor (1) — default buffer:**
```cpp
template<typename BufferT = boost::beast::flat_buffer>
Websocket<BufferT>(
    std::string url,
    std::function<void()> onConnected,
    std::function<void()> onDisconnected,
    std::function<void(const char*, std::size_t)> onData,
    std::function<void(std::string&&)> onError,
    uint32_t write_buffer_size = 1u << 20   // 1 MiB write buffer
);
```

**Constructor (2) — shared backend (slick dynamic_buffer):**
```cpp
template<typename BufferT>
template<typename BackendT>
    requires std::constructible_from<BufferT, std::shared_ptr<BackendT>>
Websocket<BufferT>(
    std::string url,
    std::function<void()> onConnected,
    std::function<void()> onDisconnected,
    std::function<void(const char*, std::size_t)> onData,
    std::function<void(std::string&&)> onError,
    std::shared_ptr<BackendT> r_backend,
    uint32_t write_buffer_size = 1u << 20
);
```

**Methods:**
- `void open()` - Start or restart the WebSocket connection (see [Reconnect](#reconnect) below)
- `bool close()` - Close the WebSocket connection
- `void send(const char* buffer, size_t len, bool is_binary = false, bool suppress_log = false)` - Send data through the WebSocket
- `void send_binary_data(const char* buffer, size_t len, bool suppress_log = false)` - Send binary data through the WebSocket
- `Status status() const` - Get current connection status
- `void detach()` - Suppress callbacks from this object's session (used internally during teardown/reconnect)
- `static void shutdown()` - Stop the shared service thread and join it; the next `open()` starts it again. Safe to call from a callback: the service thread cannot join itself, so from one it only requests the stop and returns, and the join happens at the next `shutdown()` from another thread, at the next `open()`, or at program exit. Also safe to call concurrently, and concurrently with `open()`: the thread is owned by one caller at a time, so exactly one of them joins it and the rest return once that join is done
- `static void set_busy_poll(bool enable)` - Switch the service thread between blocking and busy polling (see [Busy Polling](#busy-polling) below)
- `static bool busy_poll()` - Whether the service thread busy-polls

**Status Enum:**
- `CONNECTING` - Connection in progress
- `CONNECTED` - Connected and ready
- `DISCONNECTING` - Disconnection in progress
- `DISCONNECTED` - Disconnected

### Sending

`send()` is lock-free and safe to call from any thread, including from inside the WebSocket's own
callbacks. It copies the payload into the write queue and returns; the shared service thread writes
the frames, in the order the queue accepted them.

Only the first send of a burst wakes the service thread. That send starts a *write chain* which
keeps writing until the queue runs dry, so every send landing while the chain runs rides it instead
of posting a wakeup of its own — a burst of a thousand messages costs one wakeup, not a thousand.
Sends made while the connection is still `CONNECTING` queue the same way and flush, in order, once
the handshake completes.

### Busy Polling

All `Websocket` instances share one service thread. By default it blocks in the OS while
no I/O is ready, so an idle service uses no CPU. For latency-sensitive applications,
busy polling keeps the thread spinning on the `io_context` instead, trading a fully used
CPU core for avoiding kernel wake-ups on each message:

```cpp
slick::net::Websocket<>::set_busy_poll(true);   // spin a core
// ...
slick::net::Websocket<>::set_busy_poll(false);  // back to blocking
```

The setting is process-wide, lock-free, and takes effect immediately — before the first
`open()` or while connections are active. It persists across `shutdown()`.

### Reconnect

The same `Websocket` object can be reused — call `open()` again after the connection closes to reconnect without creating a new instance.

```cpp
ws.close();

// Wait for the previous session to fully close
while (ws.status() != Websocket<>::Status::DISCONNECTED) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

ws.open(); // reconnects using the same URL and callbacks
```

`open()` is also safe to call from within any callback (`onConnected`, `onDisconnected`, `onData`, `onError`) because the new connection is posted asynchronously and does not block the service thread.

#### Rapid reconnect and the DISCONNECTING edge case

If `open()` is called while the previous session is still in the `DISCONNECTING` state (i.e. before `close()` has fully completed), the library lets the old close finish in the background but **suppresses `onDisconnected` for that session** — the callback is replaced with a no-op so only the new session's events reach the caller.

```
Normal reconnect (wait for DISCONNECTED):
  open → connected → close → disconnected ← fires
                                           → open → connected → ...

Rapid reconnect (call open() while DISCONNECTING):
  open → connected → close → [disconnected suppressed]
                           → open → connected → ...
```

If you need a guaranteed `onDisconnected` for every session — for example, to flush per-session state — wait for `status() == DISCONNECTED` before calling `open()` again.

#### Deferred reconnect

For shared-backend buffers (e.g. `slick::dynamic_buffer<T>`) the backend is
single-producer, so a same-object `open()` is deferred until the previous session's
read loop has fully released it (normally ~1 round trip, bounded by the close timeout).
`status()` reads `CONNECTING` during this window, and any partial data left over from
the interrupted session is discarded before the new session starts reading.

For `flat_buffer` (the default) each reconnect starts a fresh buffer immediately —
no deferral is needed.

### Custom Read Buffers (slick backends)

Pass a `shared_ptr` to a slick backend to unlock zero-copy, lock-free streaming. Bytes
received by the WebSocket are written directly into the slick ring; consumers read
zero-copy without copying through the callback.

Supported slick websocket buffer types are explicitly instantiated in `slick-net`, so
application code only needs the public websocket header and the buffer type headers.

#### stream_buffer (SPMC)

```cpp
#include <slick/net/websocket.hpp>
#include <slick/dynamic_buffer.hpp>
#include <slick/stream_buffer.hpp>

// Shared backend — survives reconnects
auto sb = std::make_shared<slick::stream_buffer>(1u << 26, 1u << 16); // 64 MiB / 64K records

slick::net::Websocket<slick::dynamic_buffer<slick::stream_buffer>> ws(
    "wss://ws.postman-echo.com/raw",
    [&]() { /* connected */ },
    [&]() { /* disconnected */ },
    [](const char* data, std::size_t len) { /* data delivered via ring record */ },
    [](std::string&&) { /* error */ },
    sb
);

ws.open();

// Consumers read from the same ring zero-copy on any thread:
uint64_t cursor = sb->initial_reading_index();
while (true) {
    auto [ptr, len] = sb->read(cursor);
    if (ptr && len) {
        // process ptr[0..len-1]
    }
}
```

#### Custom buffer types

For custom buffer types, the compiler must see the template method definitions. Keep
normal application files on `<slick/net/websocket.hpp>`, and add one explicit
instantiation translation unit for the custom type:

```cpp
// websocket_my_buffer.cpp
#include <my/buffer.hpp>
#define SLICK_NET_WEBSOCKET_HEADER_ONLY
#include <slick/net/websocket.hpp>

template class slick::net::Websocket<my::buffer>;
```

If the custom websocket uses the shared-backend constructor template, explicitly
instantiate that constructor too:

```cpp
template slick::net::Websocket<my::buffer_adapter>::Websocket(
    std::string,
    std::function<void()> &&,
    std::function<void()> &&,
    std::function<void(const char*, std::size_t)> &&,
    std::function<void(std::string &&)> &&,
    std::shared_ptr<my::backend>,
    uint32_t);
```

Small applications can instead define `SLICK_NET_WEBSOCKET_HEADER_ONLY` before
including `<slick/net/websocket.hpp>` in each translation unit that uses the custom
type, trading compile time for simpler setup.

#### producer_buffer (MPMC fan-in via stream_buffer_multiplexer)

```cpp
#include <slick/net/websocket.hpp>
#include <slick/dynamic_buffer.hpp>
#include <slick/stream_buffer_multiplexer.hpp>

using PBuf = slick::stream_buffer_multiplexer::producer_buffer;

slick::stream_buffer_multiplexer mux(256);
auto pb = mux.add_producer(0, 1u << 26, 1u << 16);

slick::net::Websocket<slick::dynamic_buffer<PBuf>> ws(
    "wss://ws.postman-echo.com/raw",
    [&]() { /* connected */ },
    [&]() { /* disconnected */ },
    [](const char* data, std::size_t len) { /* data from this producer */ },
    [](std::string&&) { /* error */ },
    pb
);

ws.open();

// Multiplexer consumer reads all producers in arrival order:
uint64_t cursor = 0;
while (true) {
    auto rec = mux.read(cursor);
    if (rec) {
        // rec.data, rec.length, rec.producer_id
    }
}
```

### HttpStream Class

The `HttpStream` class provides support for HTTP streaming, including Server-Sent Events (SSE) and chunked responses.

The response body is decoded by Beast's HTTP parser, so `onData` never sees transfer-coding framing (chunk sizes, chunk extensions, trailers). For a `text/event-stream` response it receives the `data` of each complete event, however the events are split across reads (even inside a CRLF line ending), and parsing takes time linear in the body size; a partial event left when a response ends is discarded. For any other content type it receives the decoded body bytes as they arrive, in pieces of at most 8 KiB that need not align with the server's chunks. A chunked or `Content-Length` body that ends early is reported through `onError`. A stream has no idle timeout: it stays open until the server ends the response or `close()` is called.

**Open and close:** a stream runs at most one session at a time. `open()` does nothing while the stream is `CONNECTING` or `CONNECTED`. After `close()`, or once the response has ended, it starts a new session. If the previous session is still ending, the new one starts only after it has ended, so callbacks from the two sessions never interleave. `close()` sets `status()` to `DISCONNECTED` right away and cancels whatever the session is waiting on: the connect, the TLS handshake, or the request or response I/O. A DNS lookup that is already running cannot be interrupted, so its result is thrown away when it returns. After `close()` the session makes no more `onConnected`, `onData` or `onError` calls (a callback already running finishes). Every `open()` that starts a session gets exactly one `onDisconnected`, even if `close()` ends the session before it connects. Both calls are lock-free, and you can call them from any thread, including from inside the stream's own callbacks.

**Threading:** a stream constructed without an executor runs on the shared service: one `io_context` run by `HttpStream::service_threads()` threads, 1 by default. Each stream runs its I/O and callbacks through a strand of its own, so the callbacks of one stream never run concurrently. A callback that blocks, however, occupies a service thread — with the default single thread, every other stream on the service waits for it. To keep a slow callback from stalling other streams, either run the service on more threads (callbacks of different streams may then run concurrently), or give the stream an executor of its own, such as an `io_context` you run or a `boost::asio::thread_pool`. A stream on its own executor never touches the shared service: its `open()` does not start the service and `shutdown()` does not stop it, so keep the executor's context running until the stream disconnects.

```cpp
// Run the shared service on 4 threads; applies at the first open(), or the first open() after shutdown()
HttpStream::set_service_threads(4);

// Or give a stream an executor of its own
boost::asio::thread_pool pool{2};
auto stream = std::make_shared<HttpStream>(
    pool.get_executor(),
    "https://api.example.com/events",
    []() {},
    []() {},
    [](const char* data, size_t size) { /* slow processing only delays this stream */ },
    [](std::string err) {}
);
stream->open();
```

**Constructors:**
```cpp
// Runs on the shared service
HttpStream(
    std::string url,
    std::function<void()> onConnected,
    std::function<void()> onDisconnected,
    std::function<void(const char*, std::size_t)> onData,
    std::function<void(std::string)> onError,
    std::vector<std::pair<std::string, std::string>>&& headers = {}
)

// Runs on executor; a null executor selects the shared service
HttpStream(
    boost::asio::any_io_executor executor,
    std::string url,
    std::function<void()> onConnected,
    std::function<void()> onDisconnected,
    std::function<void(const char*, std::size_t)> onData,
    std::function<void(std::string)> onError,
    std::vector<std::pair<std::string, std::string>>&& headers = {}
)
```

**Methods:**
- `void open()` - Start a session (starting the shared service first if the stream uses it). Does nothing while the stream is `CONNECTING` or `CONNECTED` (see [Open and close](#httpstream-class))
- `void close()` - End the session: `status()` reads `DISCONNECTED` at once, the pending connect, handshake or I/O is cancelled, and `onDisconnected` follows. Does nothing if the stream is not open
- `Status status() const` - Get current connection status
- `static bool is_running()` - Check if the shared stream service is running
- `static void shutdown()` - Stop the shared stream service and join its threads; the next `open()` of a stream on it starts it again. Streams on their own executors are unaffected
- `static void set_service_threads(std::size_t count)` - Number of threads that run the shared service (default 1, minimum 1); lock-free, applies when the service starts
- `static std::size_t service_threads()` - Configured number of shared service threads

**Status Enum:**
- `CONNECTING` - Connection in progress
- `CONNECTED` - Connected and receiving data
- `DISCONNECTED` - Disconnected

**Example Usage - Server-Sent Events (SSE):**
```cpp
#include <slick/net/http.hpp>

auto stream = std::make_shared<HttpStream>(
    "https://api.example.com/events",
    []() {
        std::cout << "Stream connected\n";
    },
    []() {
        std::cout << "Stream disconnected\n";
    },
    [](const char* data, size_t size) {
        std::string event(data, size);
        std::cout << "Event: " << event << "\n";
    },
    [](std::string err) {
        std::cerr << "Error: " << err << "\n";
    }
);

stream->open();

// Stream will receive events via the onData callback
// Close when done
stream->close();
```

**Example Usage - OpenAI Streaming API:**
```cpp
#include <slick/net/http.hpp>
#include <nlohmann/json.hpp>

auto stream = std::make_shared<HttpStream>(
    "https://api.openai.com/v1/chat/completions",
    []() {
        std::cout << "Connected to OpenAI\n";
    },
    []() {
        std::cout << "Stream ended\n";
    },
    [](const char* data, size_t size) {
        // Parse streaming JSON chunks
        std::string chunk(data, size);
        try {
            auto json = nlohmann::json::parse(chunk);
            if (json.contains("choices")) {
                auto delta = json["choices"][0]["delta"];
                if (delta.contains("content")) {
                    std::cout << delta["content"].get<std::string>();
                }
            }
        } catch (...) {}
    },
    [](std::string err) {
        std::cerr << "Error: " << err << "\n";
    },
    {
        {"Authorization", "Bearer YOUR_API_KEY"},
        {"Content-Type", "application/json"}
    }
);

stream->open();
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Author

Part of the [SlickQuant](https://github.com/SlickQuant) ecosystem.

**Made with ⚡ by [SlickQuant](https://github.com/SlickQuant)**
