#include <slick/net/http.hpp>
#include <slick/net/logging.hpp>
#include <slick/net/tls.hpp>
#include "utils.hpp"

#include <memory>
#include <utility>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/as_tuple.hpp>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace asio = boost::asio;           // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl;       // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

namespace slick::net {
namespace {
        
    using Response = Http::Response;
    asio::io_context async_ioc_;

    struct service_info
    {
        uint32_t async_requests_ = 0;
        bool service_running_ = false;
    };
    std::atomic<service_info> async_service_;

    asio::awaitable<Http::Response> do_session_plain_awaitable(
        std::string host,
        std::string target,
        std::string port,
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
        http::request<http::string_body> req{ method, target, version };
        req.set(http::field::host, detail::format_authority(host));
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        // Set headers
        for (auto &header_pair : headers) {
            req.set(header_pair.first, header_pair.second);
        }

        // Set request body if provided
        if (!body.empty()) {
            req.body() = body;
            req.prepare_payload();
        }

        // Set the timeout.
        stream.expires_after(std::chrono::seconds(30));

        // Send the HTTP request to the remote host
        co_await http::async_write(stream, req);

        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::dynamic_body> res;

        // Receive the HTTP response
        co_await http::async_read(stream, buffer, res);

        Http::Response response;
        response.result_code = static_cast<uint32_t>(res.result_int());
        response.result_text = beast::buffers_to_string(res.body().data());
        response.reason = std::string(res.reason());

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
        http::request<http::string_body> req{ method, target, version };
        req.set(http::field::host, detail::format_authority(host));
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        // Set headers
        for (auto &header_pair : headers) {
            req.set(header_pair.first, header_pair.second);
        }

        // Set request body if provided
        if (!body.empty()) {
            req.body() = body;
            req.prepare_payload();
        }

        // Set the timeout.
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));

        // Send the HTTP request to the remote host
        co_await http::async_write(stream, req);

        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::dynamic_body> res;

        // Receive the HTTP response
        co_await http::async_read(stream, buffer, res);

        Http::Response response;
        response.result_code = static_cast<uint32_t>(res.result_int());
        response.result_text = beast::buffers_to_string(res.body().data());

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
        auto [host, target, port, use_ssl] = parse_url(url);

        co_return co_await (use_ssl
            ? do_session_ssl_awaitable(std::move(host), std::move(target), std::move(port), method, std::move(headers), std::move(body), version)
            : do_session_plain_awaitable(std::move(host), std::move(target), std::move(port), method, std::move(headers), std::move(body), version));
    }

    asio::awaitable<void> do_session_ssl(
        std::string host,
        std::string target,
        std::string port,
        http::verb method,
        std::function<void(Response&&)> on_response,
        std::vector<std::pair<std::string, std::string>> headers,
        std::string body,
        int version)
    {
        auto response = co_await do_session_ssl_awaitable(host, target, port, method, headers, body, version);
        on_response(std::move(response));
        co_return;
    }

    asio::awaitable<void> do_session_plain(
        std::string host,
        std::string target,
        std::string port,
        http::verb method,
        std::function<void(Response&&)> on_response,
        std::vector<std::pair<std::string, std::string>> headers,
        std::string body,
        int version)
    {
        auto response = co_await do_session_plain_awaitable(host, target, port, method, headers, body, version);
        on_response(std::move(response));
        co_return;
    }

    asio::awaitable<void> do_session(
        std::string url,
        http::verb method,
        std::function<void(Http::Response&&)> on_response,
        std::vector<std::pair<std::string, std::string>> headers = {},
        std::string body = "",
        int version = 11)
    {
        // Parse inside the coroutine so a malformed URL reaches the completion handler instead of
        // throwing out of async_*() after ensure_service_thread() counted the request
        auto [host, target, port, use_ssl] = parse_url(url);

        co_await (use_ssl
            ? do_session_ssl(std::move(host), std::move(target), std::move(port), method, std::move(on_response), std::move(headers), std::move(body), version)
            : do_session_plain(std::move(host), std::move(target), std::move(port), method, std::move(on_response), std::move(headers), std::move(body), version));
    }

    void async_request_done() {
        auto svc_info = async_service_.load(std::memory_order_relaxed);
        service_info update;
        do {
            assert(svc_info.async_requests_ > 0);
            update = svc_info;
            --update.async_requests_;
        } while (!async_service_.compare_exchange_weak(svc_info, update, std::memory_order_acq_rel, std::memory_order_relaxed));
    }

    void ensure_service_thread() {
        auto svc_info = async_service_.load(std::memory_order_relaxed);
        service_info update;
        do {
            update = svc_info;
            ++update.async_requests_;
            update.service_running_ = true;
        } while (!async_service_.compare_exchange_weak(svc_info, update, std::memory_order_acq_rel, std::memory_order_relaxed));

        if (!svc_info.service_running_)
        {
            std::thread([](){
                bool run = true;
                while (run) {
                    try {
                        if (async_ioc_.stopped()) {
                            auto svc_info = async_service_.load(std::memory_order_relaxed);
                            service_info update;
                            do {
                                if (svc_info.async_requests_) {
                                    break;
                                }

                                update = svc_info;
                                update.service_running_ = false; 
                            }
                            while (!async_service_.compare_exchange_weak(svc_info, update, std::memory_order_acq_rel, std::memory_order_relaxed));
                            run = svc_info.async_requests_;
                            async_ioc_.restart();
                            continue;
                        }
                        async_ioc_.run();
                    }
                    catch(const std::exception& e) {
                        async_ioc_.restart();
                        LOG_ERROR("Http service thread error: {}", e.what());
                    }
                }
            }).detach();
        }
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

    Response sync_request(
        std::string_view url,
        http::verb method,
        std::vector<std::pair<std::string, std::string>>&& headers,
        std::string_view body = {})
    {
        // One io_context per calling thread: concurrent synchronous calls never share one, so they
        // need no synchronization, and run() returns as soon as this thread's own request is done.
        // Reusing it keeps the reactor, timer and resolver services alive across calls.
        thread_local asio::io_context ioc;
        if (ioc.get_executor().running_in_this_thread()) [[unlikely]] {
            // Nested call from code executing inside this thread's request (e.g. a log handler)
            asio::io_context nested_ioc;
            return run_sync_request(nested_ioc, url, method, std::move(headers), body);
        }
        return run_sync_request(ioc, url, method, std::move(headers), body);
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

void Http::async_get(std::function<void(Response&&)> on_response, std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers) {
    ensure_service_thread();
    asio::co_spawn(
        async_ioc_,
        do_session(std::string(url), http::verb::get,
            [on_response](Response&& response) mutable {
                on_response(std::move(response));
            },
            std::move(headers)),
        [on_response](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    on_response(Response{500, e.what()});
                }
            }
            async_request_done();
    });
}

void Http::async_post(
    std::function<void(Response&&)> on_response,
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    ensure_service_thread();
    asio::co_spawn(
        async_ioc_,
        do_session(std::string(url), http::verb::post,
            [on_response](Response&& response) mutable {
                on_response(std::move(response));
            },
            std::move(headers), std::string(data)),
        [on_response](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    on_response(Response{500, e.what()});
                }
            }
            async_request_done();
    });
}

void Http::async_put(
    std::function<void(Response&&)> on_response,
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    ensure_service_thread();
    asio::co_spawn(
        async_ioc_,
        do_session(std::string(url), http::verb::put,
            [on_response](Response&& response) mutable {
                on_response(std::move(response));
            },
            std::move(headers), std::string(data)),
        [on_response](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    on_response(Response{500, e.what()});
                }
            }
            async_request_done();
    });
}

void Http::async_patch(
    std::function<void(Response&&)> on_response,
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    ensure_service_thread();
    asio::co_spawn(
        async_ioc_,
        do_session(std::string(url), http::verb::patch,
            [on_response](Response&& response) mutable {
                on_response(std::move(response));
            },
            std::move(headers), std::string(data)),
        [on_response](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    on_response(Response{500, e.what()});
                }
            }
            async_request_done();
    });
}

void Http::async_del(
    std::function<void(Response&&)> on_response,
    std::string_view url,
    std::string_view data,
    std::vector<std::pair<std::string, std::string>>&& headers) {
    ensure_service_thread();
    asio::co_spawn(
        async_ioc_,
        do_session(std::string(url), http::verb::delete_,
            [on_response](Response&& response) mutable {
                on_response(std::move(response));
            },
            std::move(headers), std::string(data)),
        [on_response](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& e) {
                    on_response(Response{500, e.what()});
                }
            }
            async_request_done();
    });
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
