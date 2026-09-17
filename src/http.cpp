#include <slick/net/http.hpp>
#include <slick/net/logging.hpp>
#include <slick/net/tls.hpp>
#include "utils.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <utility>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/executor_work_guard.hpp>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace asio = boost::asio;           // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl;       // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

namespace slick::net {
namespace {
        
    using Response = Http::Response;

    // Shared service that runs the callback-based async_*() requests: their I/O on async_ioc_'s thread, and the
    // callbacks of requests made without an executor on callback_ioc_'s threads, so a slow callback never holds
    // up a request's I/O. The statics of a translation unit are destroyed in reverse order of construction, so
    // declaring these before the terminator below has the threads joined while both contexts, and everything
    // the running requests use, are still alive.
    asio::io_context callback_ioc_;
    asio::io_context async_ioc_;

    enum class service_state : std::uint8_t {
        stopped,
        starting,   // start_service() is creating the threads
        running,
        stopping,   // shutdown() is joining the threads
    };
    std::atomic<service_state> service_state_{ service_state::stopped };
    std::atomic<std::size_t> callback_thread_count_{ 1 };
    // The running service has no callback threads and runs callbacks on the I/O thread; set before its threads start
    std::atomic<bool> inline_callbacks_{ false };
    // Bumped by every shutdown() that stops the service. A request, or a callback queued for the callback threads,
    // from an earlier epoch was abandoned, so a restarted service that resumes it drops its callback.
    std::atomic<std::uint64_t> service_epoch_{ 0 };
    // The I/O thread, then the callback threads. Touched only by the thread that moved service_state_ to starting
    // or stopping, so it needs no lock
    std::vector<std::thread> service_threads_;

    // Builds the request message; body is moved into it, so a large payload is never copied
    http::request<http::string_body> make_request(
        std::string_view host_header,
        std::string_view target,
        http::verb method,
        const std::vector<std::pair<std::string, std::string>>& headers,
        std::string&& body,
        int version)
    {
        http::request<http::string_body> req{ method, target, version };
        req.set(http::field::host, host_header);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        // Set headers
        for (const auto& [name, value] : headers) {
            req.set(name, value);
        }

        // Set request body if provided
        if (!body.empty()) {
            req.body() = std::move(body);
            req.prepare_payload();
        }
        return req;
    }

    // Builds the Response for a received message; its body is moved out instead of copied
    Response make_response(http::response<http::string_body>& res) {
        Response response;
        response.result_code = static_cast<uint32_t>(res.result_int());
        response.reason = std::string(res.reason());
        response.result_text = std::move(res.body());
        return response;
    }

    asio::awaitable<Http::Response> do_session_plain_awaitable(
        std::string host,
        std::string target,
        std::string port,
        std::string host_header,
        http::verb method,
        std::vector<std::pair<std::string, std::string>> headers,
        std::string body,
        int version)
    {
        auto executor = co_await asio::this_coro::executor;
        auto resolver = asio::ip::tcp::resolver{ executor };
        auto stream   = beast::tcp_stream{ executor };

        // Look up the domain name
        auto const results = co_await resolver.async_resolve(host, port);

        // Set the timeout.
        stream.expires_after(std::chrono::seconds(30));

        // Make the connection on the IP address we get from a lookup
        co_await stream.async_connect(results);

        // Set up an HTTP request message
        auto req = make_request(host_header, target, method, headers, std::move(body), version);

        // Set the timeout.
        stream.expires_after(std::chrono::seconds(30));

        // Send the HTTP request to the remote host
        co_await http::async_write(stream, req);

        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::string_body> res;

        // Receive the HTTP response
        co_await http::async_read(stream, buffer, res);

        auto response = make_response(res);

        // Set the timeout.
        stream.expires_after(std::chrono::seconds(30));

        // Gracefully close the socket
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        // not_connected happens sometimes, so don't bother reporting it.
        if(ec && ec != beast::errc::not_connected) {
            LOG_ERROR("Socket shutdown error: {}", ec.message());
        }

        co_return response;
    }

    asio::awaitable<Http::Response> do_session_ssl_awaitable(
        std::string host,
        std::string target,
        std::string port,
        std::string host_header,
        http::verb method,
        std::vector<std::pair<std::string, std::string>> headers,
        std::string body,
        int version)
    {
        auto executor = co_await asio::this_coro::executor;
        auto resolver = asio::ip::tcp::resolver{ executor };
        auto stream   = ssl::stream<beast::tcp_stream>{ executor, tls_context() };

        // Set SNI and the host name/IP the server certificate must match
        if (auto ec = detail::set_tls_peer_host(stream.native_handle(), host)) {
            co_return Http::Response{5000, std::format("Error setting TLS peer host: {}", ec.message())};
        }

        // Look up the domain name
        auto const results = co_await resolver.async_resolve(host, port);

        // Set the timeout.
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));

        // Make the connection on the IP address we get from a lookup
        co_await beast::get_lowest_layer(stream).async_connect(results);

        // Set the timeout.
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));

        // Perform the SSL handshake (verifies the certificate chain and peer host)
        if (auto [ec] = co_await stream.async_handshake(ssl::stream_base::client, asio::as_tuple); ec) {
            detail::throw_tls_handshake_error(stream.native_handle(), ec);
        }

        // Set up an HTTP request message
        auto req = make_request(host_header, target, method, headers, std::move(body), version);

        // Set the timeout.
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));

        // Send the HTTP request to the remote host
        co_await http::async_write(stream, req);

        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::string_body> res;

        // Receive the HTTP response
        co_await http::async_read(stream, buffer, res);

        auto response = make_response(res);

        // Set the timeout.
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));

        // Gracefully close the stream - do not threat every error as an exception!
        auto [ec] = co_await stream.async_shutdown(asio::as_tuple);

        // ssl::error::stream_truncated, also known as an SSL "short read",
        // indicates the peer closed the connection without performing the
        // required closing handshake (for example, Google does this to
        // improve performance). Generally this can be a security issue,
        // but if your communication protocol is self-terminated (as
        // it is with both HTTP and WebSocket) then you may simply
        // ignore the lack of close_notify.
        //
        // https://github.com/boostorg/beast/issues/38
        //
        // https://security.stackexchange.com/questions/91435/how-to-handle-a-malicious-ssl-tls-shutdown
        //
        // When a short read would cut off the end of an HTTP message,
        // Beast returns the error beast::http::error::partial_message.
        // Therefore, if we see a short read here, it has occurred
        // after the message has been completed, so it is safe to ignore it.

        if(ec && ec != asio::ssl::error::stream_truncated) {
            LOG_ERROR("SSL shutdown error: {}", ec.message());
        }

        co_return response;
    }

    asio::awaitable<Http::Response> do_session_awaitable(
        std::string url,
        http::verb method,
        std::vector<std::pair<std::string, std::string>> headers = {},
        std::string body = "",
        int version = 11)
    {
        // Parse inside the coroutine so a malformed URL is reported like any other request error
        auto [host, target, port, use_ssl, host_header] = parse_url(url);

        // Keep if/else: GCC evaluates both arms of a ?: operand of co_await, so the second
        // call would receive moved-from (empty) arguments and resolve an empty host.
        if (use_ssl) {
            co_return co_await do_session_ssl_awaitable(std::move(host), std::move(target), std::move(port), std::move(host_header),
                                                        method, std::move(headers), std::move(body), version);
        }
        co_return co_await do_session_plain_awaitable(std::move(host), std::move(target), std::move(port), std::move(host_header),
                                                      method, std::move(headers), std::move(body), version);
    }

    // The response a callback-based request reports when an exception replaced its response
    Response async_error_response(std::exception_ptr e) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            return Response{500, ex.what()};
        } catch (...) {
            return Response{500, "Unknown error"};
        }
    }

    // Delivers the response of a callback-based request: posts the callback to the caller's executor, or runs it on
    // the callback threads (inline on the I/O thread when there are none). A request shutdown() abandoned drops its
    // callback instead. Invoked at most once, on the I/O thread.
    class response_delivery {
    public:
        response_delivery(asio::any_io_executor executor, std::function<void(Response&&)>&& on_response)
            : on_response_(std::move(on_response))
            , epoch_(service_epoch_.load(std::memory_order_acquire))
            , has_executor_(static_cast<bool>(executor)) {
            // Untracked: an abandoned request never posts, and work it left counted would keep the context's run()
            // from returning and, with IOCP, its destructor waiting forever
            if (has_executor_) {
                new (&executor_) asio::any_io_executor(std::move(executor));
            }
        }

        response_delivery(response_delivery&& other) noexcept
            : on_response_(std::move(other.on_response_))
            , epoch_(other.epoch_)
            , has_executor_(std::exchange(other.has_executor_, false)) {
            if (has_executor_) {
                new (&executor_) asio::any_io_executor(std::move(other.executor_));
                // A moved-from executor is empty, so destroying it touches no context
                other.executor_.~any_io_executor();
            }
        }

        response_delivery(const response_delivery&) = delete;
        response_delivery& operator=(const response_delivery&) = delete;
        response_delivery& operator=(response_delivery&&) = delete;

        // An executor still held here belongs to a request that never posted its callback: shutdown() abandoned it,
        // and it is being destroyed with async_ioc_ at program exit or was dropped after a restart. The executor's
        // context may be gone by then, and destroying an executor such as a strand touches its context, so the
        // executor is deliberately leaked.
        ~response_delivery() {}

        void operator()(Response&& response) {
            if (epoch_ != service_epoch_.load(std::memory_order_acquire)) {
                return;
            }

            if (has_executor_) {
                asio::post(executor_, [on_response = std::move(on_response_), response = std::move(response)]() mutable {
                    on_response(std::move(response));
                });
                executor_.~any_io_executor();
                has_executor_ = false;
            }
            else if (inline_callbacks_.load(std::memory_order_acquire)) {
                on_response_(std::move(response));
            }
            else {
                asio::post(callback_ioc_, [on_response = std::move(on_response_), response = std::move(response), epoch = epoch_]() mutable {
                    // shutdown() abandons the callbacks it leaves queued
                    if (epoch == service_epoch_.load(std::memory_order_acquire)) {
                        on_response(std::move(response));
                    }
                });
            }
        }

    private:
        std::function<void(Response&&)> on_response_;
        std::uint64_t epoch_;   // service_epoch_ when the request was made
        bool has_executor_;
        // Constructed and destroyed by hand, so the destructor can leave an undelivered one alone
        union { asio::any_io_executor executor_; };
    };

    void run_service_thread(asio::io_context& ioc) {
        // Outstanding work keeps run() blocked while idle, so it returns only once shutdown() stops the
        // context. The thread never ends on its own, so it stays owned and joinable, and a request can
        // never race its restart() against a thread that is still winding down.
        auto work = asio::make_work_guard(ioc);
        for (;;) {
            try {
                ioc.run();
                return;
            }
            catch (const std::exception& ex) {
                // A throwing response callback does not stop the io_context; keep serving the other requests
                LOG_ERROR("Http service thread error: {}", ex.what());
            }
        }
    }

    // Starts the service threads unless the service is already starting or running. A request made while
    // shutdown() is stopping the service stays queued until a later request starts it again.
    void start_service() {
        auto expected = service_state::stopped;
        if (!service_state_.compare_exchange_strong(expected, service_state::starting,
                                                    std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }

        // Clear the stop() left by a previous shutdown()
        async_ioc_.restart();
        callback_ioc_.restart();
        const auto callback_count = callback_thread_count_.load(std::memory_order_acquire);
        inline_callbacks_.store(callback_count == 0, std::memory_order_release);
        service_threads_.reserve(callback_count + 1);
        service_threads_.emplace_back([] { run_service_thread(async_ioc_); });
        for (std::size_t i = 0; i < callback_count; ++i) {
            service_threads_.emplace_back([] { run_service_thread(callback_ioc_); });
        }
        service_state_.store(service_state::running, std::memory_order_release);
    }

    // A Terminator class to ensure Http::shutdown() is called at program exit
    struct HttpTerminater
    {
        ~HttpTerminater() {
            Http::shutdown();
        }
    };

    static HttpTerminater s_http_terminater;

    // Runs a callback-based request on the shared service, reporting the response, or the exception that
    // replaced it, through on_response. do_session_awaitable parses the URL inside the coroutine, so a
    // malformed URL reaches on_response instead of throwing out of async_*().
    void spawn_async_request(
        asio::any_io_executor executor,
        std::string url,
        http::verb method,
        std::function<void(Response&&)>&& on_response,
        std::vector<std::pair<std::string, std::string>>&& headers,
        std::string body = {})
    {
        start_service();
        asio::co_spawn(
            async_ioc_,
            do_session_awaitable(std::move(url), method, std::move(headers), std::move(body)),
            [delivery = response_delivery(std::move(executor), std::move(on_response))](std::exception_ptr e, Response&& response) mutable {
                if (e) {
                    delivery(async_error_response(e));
                } else {
                    delivery(std::move(response));
                }
            });
    }

    Response run_sync_request(
        asio::io_context& ioc,
        std::string_view url,
        http::verb method,
        std::vector<std::pair<std::string, std::string>>&& headers,
        std::string_view body)
    {
        Response res;
        ioc.restart();
        asio::co_spawn(
            ioc,
            do_session_awaitable(std::string(url), method, std::move(headers), std::string(body)),
            [&res](std::exception_ptr e, Response&& response) {
                if (!e) {
                    res = std::move(response);
                    return;
                }
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    res.result_code = 500;
                    res.result_text = ex.what();
                } catch (...) {
                    res.result_code = 500;
                    res.result_text = "Unknown error";
                }
            });
        ioc.run();
        return res;
    }

    // Idle io_contexts for synchronous calls. Every call runs a context of its own, so concurrent
    // (or nested) calls need no synchronization and run() returns as soon as that call's request is
    // done; parking contexts keeps their reactor, timer and resolver services alive across calls.
    // Never make the context thread_local: on Windows the destructor of an io_context that has used
    // a timer or resolver joins Asio's helper thread, which deadlocks under the loader lock that
    // thread_local destructors run with, so the calling thread could never exit.
    class sync_context_pool {
    public:
        ~sync_context_pool() {
            for (auto& slot : slots_) {
                delete slot.load(std::memory_order_relaxed);
            }
        }

        // Claims a parked context, or creates one when none is parked
        std::unique_ptr<asio::io_context> acquire() {
            for (auto& slot : slots_) {
                if (slot.load(std::memory_order_relaxed)) {
                    if (auto* ioc = slot.exchange(nullptr, std::memory_order_acq_rel)) {
                        return std::unique_ptr<asio::io_context>(ioc);
                    }
                }
            }
            return std::make_unique<asio::io_context>();
        }

        // Parks a context in a free slot, or destroys it when every slot is taken
        void release(std::unique_ptr<asio::io_context> ioc) {
            for (auto& slot : slots_) {
                asio::io_context* empty = nullptr;
                if (!slot.load(std::memory_order_relaxed) &&
                    slot.compare_exchange_strong(empty, ioc.get(), std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    ioc.release();
                    return;
                }
            }
        }

    private:
        std::array<std::atomic<asio::io_context*>, 16> slots_{};
    };
    sync_context_pool sync_contexts_;

    Response sync_request(
        std::string_view url,
        http::verb method,
        std::vector<std::pair<std::string, std::string>>&& headers,
        std::string_view body = {})
    {
        auto ioc = sync_contexts_.acquire();
        auto res = run_sync_request(*ioc, url, method, std::move(headers), body);
        sync_contexts_.release(std::move(ioc));
        return res;
    }

} // namespace

Http::Response Http::get(std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers) {
    return sync_request(url, http::verb::get, std::move(headers));
}

Http::Response Http::post(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers) {
    return sync_request(url, http::verb::post, std::move(headers), data);
}

Http::Response Http::put(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers) {
    return sync_request(url, http::verb::put, std::move(headers), data);
}

Http::Response Http::patch(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers) {
    return sync_request(url, http::verb::patch, std::move(headers), data);
}

Http::Response Http::del(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers) {
    return sync_request(url, http::verb::delete_, std::move(headers), data);
}

bool Http::is_running() noexcept {
    return service_state_.load(std::memory_order_relaxed) == service_state::running;
}

void Http::shutdown() {
    auto state = service_state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == service_state::starting) {
            // start_service() only creates the thread, so it finishes shortly
            std::this_thread::yield();
            state = service_state_.load(std::memory_order_acquire);
        }
        else if (state != service_state::running) {
            // Already stopped, or another shutdown() is stopping it
            return;
        }
        else if (service_state_.compare_exchange_weak(state, service_state::stopping,
                                                      std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }

    // Requests still in flight are abandoned rather than waited for, so a hung one cannot hold up program exit.
    // Bumping the epoch first drops their callbacks, and those left queued for the callback threads, should a
    // later request restart the service and resume them. The handlers left behind are destroyed with the
    // contexts, after these joins have ended the only threads using them.
    service_epoch_.fetch_add(1, std::memory_order_acq_rel);
    async_ioc_.stop();
    callback_ioc_.stop();
    for (auto& thread : service_threads_) {
        thread.join();
    }
    service_threads_.clear();
    service_state_.store(service_state::stopped, std::memory_order_release);
}

void Http::set_callback_threads(std::size_t count) noexcept {
    callback_thread_count_.store(count, std::memory_order_release);
}

std::size_t Http::callback_threads() noexcept {
    return callback_thread_count_.load(std::memory_order_acquire);
}

void Http::async_get(std::function<void(Response&&)> on_response, std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers) {
    async_get(asio::any_io_executor{}, std::move(on_response), url, std::move(headers));
}

void Http::async_post(
    std::function<void(Response&&)> on_response,
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    async_post(asio::any_io_executor{}, std::move(on_response), url, data, std::move(headers));
}

void Http::async_put(
    std::function<void(Response&&)> on_response,
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    async_put(asio::any_io_executor{}, std::move(on_response), url, data, std::move(headers));
}

void Http::async_patch(
    std::function<void(Response&&)> on_response,
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    async_patch(asio::any_io_executor{}, std::move(on_response), url, data, std::move(headers));
}

void Http::async_del(
    std::function<void(Response&&)> on_response,
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    async_del(asio::any_io_executor{}, std::move(on_response), url, data, std::move(headers));
}

void Http::async_get(
    boost::asio::any_io_executor executor,
    std::function<void(Response&&)> on_response,
    std::string_view url,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    spawn_async_request(std::move(executor), std::string(url), http::verb::get, std::move(on_response), std::move(headers));
}

void Http::async_post(
    boost::asio::any_io_executor executor,
    std::function<void(Response&&)> on_response,
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    spawn_async_request(std::move(executor), std::string(url), http::verb::post, std::move(on_response), std::move(headers), std::string(data));
}

void Http::async_put(
    boost::asio::any_io_executor executor,
    std::function<void(Response&&)> on_response,
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    spawn_async_request(std::move(executor), std::string(url), http::verb::put, std::move(on_response), std::move(headers), std::string(data));
}

void Http::async_patch(
    boost::asio::any_io_executor executor,
    std::function<void(Response&&)> on_response,
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    spawn_async_request(std::move(executor), std::string(url), http::verb::patch, std::move(on_response), std::move(headers), std::string(data));
}

void Http::async_del(
    boost::asio::any_io_executor executor,
    std::function<void(Response&&)> on_response,
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    spawn_async_request(std::move(executor), std::string(url), http::verb::delete_, std::move(on_response), std::move(headers), std::string(data));
}

boost::asio::awaitable<Http::Response> Http::async_get(std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers) {
    return do_session_awaitable(std::string(url), http::verb::get, std::move(headers), "", 11);
}

boost::asio::awaitable<Http::Response> Http::async_post(
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    return do_session_awaitable(std::string(url), http::verb::post, std::move(headers), std::string(data), 11);
}

boost::asio::awaitable<Http::Response> Http::async_put(
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    return do_session_awaitable(std::string(url), http::verb::put, std::move(headers), std::string(data), 11);
}

boost::asio::awaitable<Http::Response> Http::async_patch(
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    return do_session_awaitable(std::string(url), http::verb::patch, std::move(headers), std::string(data), 11);
}

boost::asio::awaitable<Http::Response> Http::async_del(
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    return do_session_awaitable(std::string(url), http::verb::delete_, std::move(headers), std::string(data), 11);
}


// ---------------------------------------------------- HttpStream Implementation ----------------------------------------------------

} // namespace slick::net

#undef LOG_ERROR
