# [v2.0.4] - 2026-06-02

## Changed
- `LINK_STATICALLY` cmake option is now available on all platforms, not just MSVC.
  - Windows: sets `VCPKG_TARGET_TRIPLET` to `x64-windows-static`.
  - macOS: sets `VCPKG_TARGET_TRIPLET` to `arm64-osx-static` or `x64-osx-static` based on architecture.
  - Linux: no triplet change needed — `x64-linux` is already static by default.
- `OPENSSL_MSVC_STATIC_RT` is now only set on MSVC (was incorrectly set on all platforms when `LINK_STATICALLY` was on).
- Non-MSVC release builds skip `-march=native` when cross-compiling.

# [v2.0.3] - 2026-05-07

## Added
- `Websocket::reset_callbacks()` — public API to silence all callbacks (sets each to a no-op), useful when the caller wants to tear down a `Websocket` without receiving further events.

## Fixed
- `Websocket` destructor now explicitly calls `reset_callbacks()` followed by `close()` instead of defaulting, preventing spurious callbacks from firing into already-destroyed caller state during object destruction.

# [v2.0.2] - 2026-05-04

## Added
- Added reason in Http::Response

## Fixed
- HTTP response body is now always returned in `result_text` regardless of status code. Previously, non-2xx responses (e.g. 400 Bad Request) set `result_text` to the HTTP reason phrase instead of the response body, discarding any JSON error payload sent by the server.

# [v2.0.1] - 2026-04-30

## Chaged
- Updated SSE endpoint in excamples and tests
- Added timeout handling in HTTP stream sessions

## Fixed
- WebSocket `co_spawn` completion handler now guards `on_error_()` with a `run_` check, suppressing spurious error callbacks after shutdown.
- Fixed use-after-free in WebSocket tests where `[&]` lambda captures referenced destroyed stack variables when async callbacks fired via IOCP after the test function returned. Affected tests (`ConnectToEchoServer`, `InvalidHostnameError`, `MultipleErrorCallbacks`, `ReconnectAfterError`, `PlainWebsocket_UrlParsing`) now use `shared_ptr` captures to extend captured variable lifetimes.
- normalize CRLF in SSE chunk parsing

## Tests
- Added 10 comprehensive `Reconnect_*` tests verifying correct behavior when reconnecting by creating a new `Websocket` instance during normal operation (service thread always running):
  - Graceful close → new-object reconnect
  - Three consecutive reconnect cycles with data verification
  - Reconnect initiated from within `on_disconnected_` callback (deadlock check)
  - Reconnect initiated from within `on_error_` callback
  - Reconnect after closing from within `on_data_` callback
  - Reconnect after simulated server-side close
  - Cross-session data integrity (no bleed via shared `io_context`/SSL context)
  - Rapid successive cycles (service thread stability)
  - Callback ordering across sessions (`connected` → `data` → `disconnected`)
  - Concurrent dual-instance connect (shared SSL context)

# [v2.0.0] - 2026-02-15

## Changed
- **BREAKING:** Switched `slick::net` from header-only to static-library
  - Normalized header files to .hpp
  - Separated HttpStream to its own header `http_stream.hpp`
  - Added compiled sources under `src/` (`http_stream.cpp`, `http.cpp`, `websocket.cpp`, `websocket_session.cpp`, `logging.cpp`)
  - Reduced downstream compile-time pressure by moving heavy Boost/OpenSSL implementation out of public headers
  - Websocket is nolonger derived from std::enabled_shared_from_this
- Added PIMPL-based slim public headers for `Websocket`
- Added runtime logging hook API in `include/slick/net/logging.hpp`
  - `set_log_handler(LogHandler)`
  - `clear_log_handler()`
- Added `logging_tests` for runtime logging hook dispatch behavior

# [v1.2.4] - 2026-02-08

## Fixed
- WebSocket error handling now properly ignores SSL stream_truncated errors
  - Added `ssl::error::stream_truncated` to the list of benign errors in read/write/close handlers
  - Prevents spurious error callbacks when SSL connections are closed without proper shutdown handshake
  - Improved code formatting for better readability of error condition checks
- WebSocket read handler now uses `memory_order_release` when setting DISCONNECTED state for proper memory synchronization

## Changed
- Removed INTERFACE precompiled headers to improve downstream project build times
  - Downstream projects are no longer forced to precompile Boost.Beast/Asio and OpenSSL headers
  - Projects can now opt-in to their own PCH strategy if desired
  - Library functionality remains unchanged

# [v1.2.3] - 2026-01-29

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
