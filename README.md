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
- **SSL/TLS Support**: Native support for secure `https://` and `wss://` connections
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

Internal slick-net logs are routed via runtime hooks:

```cpp
#include <slick/net/logging.hpp>

slick::net::set_log_handler([](slick::net::LogLevel level, const char* format_text, std::format_args args) {
    // Route to your logger
});

// Optional cleanup
slick::net::clear_log_handler();
```

## Usage

### Basic WebSocket Client

```cpp
#include <slick/net/websocket.hpp>

using namespace slick::net;

int main() {
    Websocket ws(
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
    while(Websocket::is_running()) {
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
    std::shared_ptr<Websocket> ws;
    ws = std::make_shared<Websocket>(
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
    while(Websocket::is_running()) {
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

**Asynchronous Callback-Based Methods:**
```cpp
void async_get(std::function<void(Response&&)> on_response, std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
void async_post(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
void async_put(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
void async_patch(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
void async_del(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
```

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

**Constructor:**
```cpp
Websocket(
    std::string url,
    std::function<void()> onConnected,
    std::function<void()> onDisconnected,
    std::function<void(const char*, std::size_t)> onData,
    std::function<void(std::string&&)> onError
)
```

**Methods:**
- `void open()` - Start or restart the WebSocket connection (see [Reconnect](#reconnect) below)
- `void close()` - Close the WebSocket connection
- `void send(const char* buffer, size_t len)` - Send data through the WebSocket
- `Status status() const` - Get current connection status
- `static void shutdown()` - Shutdown all WebSocket services

**Status Enum:**
- `CONNECTING` - Connection in progress
- `CONNECTED` - Connected and ready
- `DISCONNECTING` - Disconnection in progress
- `DISCONNECTED` - Disconnected

### Reconnect

The same `Websocket` object can be reused — call `open()` again after the connection closes to reconnect without creating a new instance.

```cpp
ws.close();

// Wait for the previous session to fully close
while (ws.status() != Websocket::Status::DISCONNECTED) {
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

### HttpStream Class

The `HttpStream` class provides support for HTTP streaming, including Server-Sent Events (SSE) and chunked responses.

**Constructor:**
```cpp
HttpStream(
    std::string url,
    std::function<void()> onConnected,
    std::function<void()> onDisconnected,
    std::function<void(const char*, std::size_t)> onData,
    std::function<void(std::string&&)> onError,
    std::vector<std::pair<std::string, std::string>>&& headers = {}
)
```

**Methods:**
- `void open()` - Start the HTTP stream connection
- `void close()` - Close the stream connection
- `Status status() const` - Get current connection status
- `static bool is_running()` - Check if any streams are running
- `static void shutdown()` - Shutdown all HTTP stream services

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
