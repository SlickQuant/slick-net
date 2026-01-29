# [unreleased] 

## Fixed
- WebSocket open now rejects DISCONNECTING state to avoid overlapping reconnect/close races.
- WebSocket handshake no longer mutates the stored host with an appended port, preventing host corruption on reconnect.
- WebSocket write errors are now surfaced while connected instead of being ignored.
- Async HTTP request accounting now decrements even when the coroutine fails, allowing the service thread to stop.

## Changed
- Updated slick-net-config.cmake.in file to remove cmake config warning.

# [v1.2.2] - 2026-01-13
- Upgraded to slick-queue v1.2.2
- Renamed repository from slick_net to slick-net (hyphenated naming follows recommended convention)
- Changed export name from slick_net, slick::slick_net to slick::net
- Refactored repository name in CMake configuration files
- Updated documentation and build references to use new repository name
- Added release GitHub Workflow
- Updated license copyright years
- Improved Websocket unittest

# [v1.2.1] - 2025-01-25

## New Features
- **vcpkg Package Manager Support**: Full integration with vcpkg for easy installation and dependency management
  - Created CMake config files for proper package discovery
  - Ready for submission to official vcpkg registry

## Improvements
- **WebSocket Message Queueing**: Enhanced `send()` method to properly queue messages sent immediately after `open()`
  - Messages are now queued during `CONNECTING` state and sent after connection is established
  - Ensures messages sent right after `open()` are not lost and are delivered in order
  - Updated status check to allow queueing during both `DISCONNECTED` and `CONNECTING` states

## Testing
- Added comprehensive WebSocket unit tests for send-after-open scenarios:
  - `SendImmediatelyAfterOpen_MessageQueuedAndSentAfterConnect` - Single message test
  - `SendImmediatelyAfterOpen_MultipleMessages` - Multiple messages queuing test
  - `SendImmediatelyAfterOpen_VerifyOrderPreserved` - Message ordering verification
  - `SendImmediatelyAfterOpen_LargeMessage` - Large message (5KB) queueing test

## Build System
- Added CMake installation rules for proper package export
- Created `slick_net-config.cmake.in` template for downstream projects
- Added version compatibility checking (SameMajorVersion policy)
- Improved CMake target exports with proper namespace (`slick::slick_net`)

## CI/CD
- Updated GitHub Actions CI to use GCC 14 on Linux for full C++20 coroutine support
- Removed GCC 13 compatibility workarounds for awaitable HTTP tests

# [v1.2.0] - 2025-01-18

## New Features
- **C++20 Coroutine Awaitable HTTP API**: Added modern async/await interface for all HTTP methods
  - `asio::awaitable<Response> async_get(url, headers)` - Awaitable GET request
  - `asio::awaitable<Response> async_post(url, data, headers)` - Awaitable POST request
  - `asio::awaitable<Response> async_put(url, data, headers)` - Awaitable PUT request
  - `asio::awaitable<Response> async_patch(url, data, headers)` - Awaitable PATCH request
  - `asio::awaitable<Response> async_del(url, data, headers)` - Awaitable DELETE request
  - Clean async/await syntax using `co_await` for sequential or parallel HTTP operations
  - Uses caller's executor context (no service thread management required)
  - Supports both HTTP and HTTPS protocols
  - **Note**: GCC 13 has a known compiler bug with coroutine lambdas. Awaitable tests are disabled on GCC 13. Use GCC 14+ or Clang for full awaitable support.

# [v1.1.2] - 2025-11-13
- Remove unnecessary slick_logger from slick_net link dependencies
- Update CMakeLists to link slick_logger with example executables

# [v1.1.1] - 2025-10-22
- Fix slick_queue header include
- Fix GitHub CI builds
- Change Version to 3 digits

# [v1.1.0.1] - 2025-10-21

- Update slick_queue to v1.1.0.2
- Change namespace from slick_net to slick::net
- Change include folder structure from include/slick_net to include/slick/net

# [v1.1.0.0] - 2025-10-19

- Added plain WebSocket (ws://) and plain Http (http://) protocol support
- Added Comprehensive test coverage for plain HTTP (non-SSL) client

## [v1.0.0] - 2025-10-11

- Initial release of slick_net
- HTTP/HTTPS client with full SSL/TLS support
  - Synchronous methods: GET, POST, PUT, PATCH, DELETE
  - Asynchronous methods with callback-based API
  - Custom header support
- WebSocket/WebSocket Secure (WSS) client
  - Built on Boost.Beast and Boost.Asio coroutines
  - Full SSL/TLS support for wss:// connections
  - Non-SSL support for ws:// connections
  - Event-driven callback API (onConnected, onDisconnected, onData, onError)
  - Binary and text message support
  - Thread-safe concurrent operations
- HTTP Streaming support (HttpStream)
  - Server-Sent Events (SSE) support
  - Chunked response streaming
  - Custom header support
- Comprehensive test suite using Google Test
  - HTTP client tests (sync and async)
  - WebSocket client tests (connection lifecycle, messaging, error handling)
  - HTTP streaming tests
- Example applications
  - websocket_client_example
  - http_client_example
  - http_stream_client_example