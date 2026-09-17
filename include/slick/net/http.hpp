// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Slick Quant LLC
// https://github.com/SlickQuant/slick-net

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

namespace slick::net {

class Http {
public:
    struct Response {
        uint32_t result_code = 0;
        std::string reason;
        std::string result_text;

        bool is_ok() const noexcept {
            return result_code >= 200 && result_code < 300;
        }
    };

    static Response get(std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static Response post(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static Response put(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static Response patch(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static Response del(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});

    // Callback-based requests run their I/O on the shared service's I/O thread and never wait for a callback:
    // on_response runs on the service's callback threads (see set_callback_threads), so a slow callback holds
    // up other callbacks at most, never another request's I/O.
    static void async_get(std::function<void(Response&&)> on_response, std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_post(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_put(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_patch(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_del(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data = "", std::vector<std::pair<std::string, std::string>>&& headers = {});

    // Same, but on_response is posted to executor, e.g. an io_context the caller runs, a strand or a
    // boost::asio::thread_pool; the request's I/O still runs on the I/O thread. The request does not count as
    // work on executor's context, so keep that context running (e.g. hold a work guard on an io_context) and
    // alive until on_response has run, or until shutdown() abandons the request. A null executor selects the
    // callback threads.
    static void async_get(boost::asio::any_io_executor executor, std::function<void(Response&&)> on_response, std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_post(boost::asio::any_io_executor executor, std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_put(boost::asio::any_io_executor executor, std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_patch(boost::asio::any_io_executor executor, std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_del(boost::asio::any_io_executor executor, std::function<void(Response&&)> on_response, std::string_view url, std::string_view data = "", std::vector<std::pair<std::string, std::string>>&& headers = {});

    // Whether the shared service that runs the callback-based async_*() requests above is running. It is
    // started by the first such request and stopped by shutdown().
    static bool is_running() noexcept;

    // Stops that shared service and joins its threads, abandoning requests still in flight and callbacks not
    // yet run on the callback threads: their callbacks are never called, even if a later request restarts the
    // service, so an abandoned request never posts to the executor it was given. The next callback-based
    // async_*() request starts the service again. It runs automatically at normal program exit, so an in-flight
    // request can never use the service while the statics it runs on are being destroyed. It joins the service
    // threads, so never call it from a response callback or from a signal handler. A callback already posted to
    // a caller's executor still runs, and the awaitable overloads below run on the caller's executor and are
    // unaffected.
    static void shutdown();

    // Number of threads that run callbacks of requests made without an executor; default 1. With more threads
    // a blocked callback holds up only its own thread, and callbacks may run concurrently. 0 runs them inline
    // on the I/O thread: no thread hop, but a slow callback then delays every request. Lock-free; applies when
    // the service starts: at the first callback-based request and at the first one after shutdown().
    static void set_callback_threads(std::size_t count) noexcept;
    static std::size_t callback_threads() noexcept;

    static boost::asio::awaitable<Response> async_get(std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static boost::asio::awaitable<Response> async_post(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static boost::asio::awaitable<Response> async_put(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static boost::asio::awaitable<Response> async_patch(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static boost::asio::awaitable<Response> async_del(std::string_view url, std::string_view data = "", std::vector<std::pair<std::string, std::string>>&& headers = {});
};

} // namespace slick::net
