#include <slick/net/http_stream.hpp>
#include <slick/net/logging.hpp>
#include <slick/net/tls.hpp>
#include "utils.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/strand.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace slick::net {

namespace {
    // Shared service that runs every stream constructed without an executor
    asio::io_context ioc_;

    enum class service_state : std::uint8_t {
        stopped,
        starting,   // start_service() is creating the threads
        running,
        stopping,   // shutdown() is joining the threads
    };
    std::atomic<service_state> service_state_{ service_state::stopped };
    std::atomic<std::size_t> service_thread_count_{ 1 };
    // Touched only by the thread that moved service_state_ to starting or stopping, so it needs no lock
    std::vector<std::thread> service_threads_;

    void run_service_thread() {
        // Outstanding work keeps run() blocked while idle, so it only returns once shutdown() stops the io_context
        auto work = asio::make_work_guard(ioc_);
        for (;;) {
            try {
                ioc_.run();
                return;
            }
            catch (const std::exception& ex) {
                // A throwing handler does not stop the io_context; keep serving the other streams
                LOG_ERROR("HttpStream service thread error: {}", ex.what());
            }
        }
    }

    // Starts the service threads unless the service is already starting or running. A stream opened while
    // shutdown() is stopping the service stays queued until a later open() starts it again.
    void start_service() {
        auto expected = service_state::stopped;
        if (!service_state_.compare_exchange_strong(expected, service_state::starting,
                                                    std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }

        // Clear the stop() left by a previous shutdown()
        ioc_.restart();
        const auto count = service_thread_count_.load(std::memory_order_acquire);
        service_threads_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            service_threads_.emplace_back(run_service_thread);
        }
        service_state_.store(service_state::running, std::memory_order_release);
    }

    // A Terminator class to ensure HttpStream::shutdown() is called at program exit
    struct HttpStreamTerminater
    {
        HttpStreamTerminater() {
        }

        ~HttpStreamTerminater() {
            HttpStream::shutdown();
        }
    };

    static HttpStreamTerminater s_http_stream_terminater;

    // HttpStream::state_ layout: the generation above the Status in the lowest two bits
    constexpr std::uint64_t make_state(std::uint64_t generation, HttpStream::Status status) noexcept {
        return (generation << 2) | static_cast<std::uint64_t>(status);
    }

    constexpr std::uint64_t generation_of(std::uint64_t state) noexcept {
        return state >> 2;
    }

    constexpr HttpStream::Status status_of(std::uint64_t state) noexcept {
        return static_cast<HttpStream::Status>(state & 3);
    }

    // The first open() starts generation 1, so 0 never names an open generation
    constexpr std::uint64_t any_generation = 0;

    // Runs a callback where nothing is left to report its exception to, logging the exception instead so it
    // cannot skip the bookkeeping that ends a session
    template <typename Callback, typename... Args>
    void invoke_logged(std::string_view name, Callback& callback, Args&&... args) {
        try {
            callback(std::forward<Args>(args)...);
        }
        catch (const std::exception& ex) {
            LOG_ERROR("HttpStream {} callback error: {}", name, ex.what());
        }
        catch (...) {
            LOG_ERROR("HttpStream {} callback error: unknown exception", name);
        }
    }
}   // end namespace

// The I/O close() cancels: the resolver looking up the host and the socket that connects, handshakes, sends the
// request and reads the response. Created, destroyed and used only on the stream's strand, which runs one session
// at a time, so it needs no synchronization.
struct HttpStream::session_io {
    session_io(HttpStream& owner, std::uint64_t generation, tcp::resolver& resolver, beast::tcp_stream& socket) noexcept
        : owner(owner)
        , generation(generation)
        , resolver(resolver)
        , socket(socket) {
        owner.session_io_ = this;
    }

    ~session_io() {
        owner.session_io_ = nullptr;
    }

    session_io(const session_io&) = delete;
    session_io& operator=(const session_io&) = delete;

    void cancel() {
        resolver.cancel();
        socket.cancel();
    }

    HttpStream& owner;
    const std::uint64_t generation;
    tcp::resolver& resolver;
    beast::tcp_stream& socket;
};

HttpStream::HttpStream(
    std::string url,
    std::function<void()> &&onConnectedCallback,
    std::function<void()> &&onDisconnectedCallback,
    std::function<void(const char*, std::size_t)> &&onDataCallback,
    std::function<void(std::string err)> &&onErrorCallback,
    std::vector<std::pair<std::string, std::string>>&& headers)
    : HttpStream(asio::any_io_executor{}, std::move(url), std::move(onConnectedCallback), std::move(onDisconnectedCallback),
                 std::move(onDataCallback), std::move(onErrorCallback), std::move(headers)) {
}

HttpStream::HttpStream(
    asio::any_io_executor executor,
    std::string url,
    std::function<void()> &&onConnectedCallback,
    std::function<void()> &&onDisconnectedCallback,
    std::function<void(const char*, std::size_t)> &&onDataCallback,
    std::function<void(std::string err)> &&onErrorCallback,
    std::vector<std::pair<std::string, std::string>>&& headers)
    : url_(std::move(url))
    , headers_(std::move(headers))
    , on_connected_(std::move(onConnectedCallback))
    , on_disconnected_(std::move(onDisconnectedCallback))
    , on_data_(std::move(onDataCallback))
    , on_error_(std::move(onErrorCallback))
    // The strand serializes this stream's sessions and close() even on a multi-threaded executor
    , executor_(executor ? asio::any_io_executor(asio::make_strand(executor)) : asio::any_io_executor(asio::make_strand(ioc_)))
    , use_service_(!executor) {
    auto [host, target, port, use_ssl] = parse_url(url_);
    host_ = std::move(host);
    target_ = std::move(target);
    port_ = std::move(port);
    use_ssl_ = use_ssl;
}

HttpStream::~HttpStream() = default;

bool HttpStream::is_running() noexcept {
    return service_state_.load(std::memory_order_relaxed) == service_state::running;
}

void HttpStream::set_service_threads(std::size_t count) noexcept {
    service_thread_count_.store(std::max<std::size_t>(count, 1), std::memory_order_release);
}

std::size_t HttpStream::service_threads() noexcept {
    return service_thread_count_.load(std::memory_order_acquire);
}

HttpStream::Status HttpStream::status() const noexcept {
    return status_of(state_.load(std::memory_order_relaxed));
}

void HttpStream::open()
{
    // Taken first, so a stream no shared_ptr owns throws before it is marked open
    auto self = shared_from_this();

    auto state = state_.load(std::memory_order_acquire);
    do {
        if (status_of(state) != Status::DISCONNECTED) {
            LOG_DEBUG("HTTP Stream {} is already open", url_);
            return;
        }
    } while (!state_.compare_exchange_weak(state, make_state(generation_of(state) + 1, Status::CONNECTING),
                                           std::memory_order_acq_rel, std::memory_order_acquire));

    LOG_INFO("Opening HTTP Stream {}", url_);

    // A stream on its own executor leaves the shared service alone
    if (use_service_) {
        start_service();
    }

    // Start the session on this stream's strand - the handler keeps the stream alive
    asio::post(executor_, [self = std::move(self)]() { self->serve(); });
}

void HttpStream::close()
{
    if (!end_generation(any_generation)) {
        return;
    }

    LOG_INFO("Closing HTTP Stream {}", url_);

    // The session checks the generation after every step, but a step waiting on a connect, a handshake or a
    // silent server may not finish for a long time, so cancel its I/O on the stream's strand, the only place that
    // touches it. A session that has not registered its I/O yet sees the ended generation once it does. A session
    // of a later open() may have registered by the time this runs; it is current, so it is left alone.
    asio::post(executor_, [weak_self = weak_from_this()]() {
        if (auto self = weak_self.lock(); self && self->session_io_ && !self->is_current(self->session_io_->generation)) {
            self->session_io_->cancel();
        }
    });
}

void HttpStream::shutdown() {
    auto state = service_state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == service_state::starting) {
            // start_service() only creates the threads, so it finishes shortly
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

    ioc_.stop();
    for (auto& thread : service_threads_) {
        thread.join();
    }
    service_threads_.clear();
    service_state_.store(service_state::stopped, std::memory_order_release);
}

// Whether generation is still open: neither close() nor the end of its session has ended it
bool HttpStream::is_current(std::uint64_t generation) const noexcept {
    const auto state = state_.load(std::memory_order_acquire);
    return generation_of(state) == generation && status_of(state) != Status::DISCONNECTED;
}

// Ends a session whose generation was ended while its last step still succeeded: a DNS lookup cannot be
// interrupted, and I/O can complete just before close() cancels it
void HttpStream::throw_if_closed(std::uint64_t generation) const {
    if (!is_current(generation)) {
        throw boost::system::system_error(asio::error::operation_aborted);
    }
}

// Moves the stream to DISCONNECTED if generation - or with any_generation, whichever one - is open. Returns
// whether this call ended it.
bool HttpStream::end_generation(std::uint64_t generation) noexcept {
    auto state = state_.load(std::memory_order_acquire);
    do {
        if (status_of(state) == Status::DISCONNECTED ||
            (generation != any_generation && generation_of(state) != generation)) {
            return false;
        }
    } while (!state_.compare_exchange_weak(state, make_state(generation_of(state), Status::DISCONNECTED),
                                           std::memory_order_acq_rel, std::memory_order_acquire));
    return true;
}

// Runs on the strand for every open() that started a session. Starts serve_sessions() unless it is running
// already, in which case it takes up the new generation once its current session ends.
void HttpStream::serve() {
    if (serving_ || generation_of(state_.load(std::memory_order_acquire)) == served_generation_) {
        return;
    }
    serving_ = true;
    asio::co_spawn(executor_, serve_sessions(), [self = shared_from_this()](std::exception_ptr e) {
        if (e) {
            // Sessions catch the std::exception their I/O or a callback throws, so little else gets here
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                LOG_ERROR("HttpStream session error: {}", ex.what());
            } catch (...) {
                LOG_ERROR("HttpStream session error: unknown exception");
            }
            // Let the next open() serve the stream again
            self->serving_ = false;
        }
    });
}

// Runs the sessions of this stream one after another, so no two ever run at once, until every open() is served
asio::awaitable<void> HttpStream::serve_sessions() {
    for (;;) {
        const auto generation = generation_of(state_.load(std::memory_order_acquire));
        if (generation == served_generation_) {
            // Cleared in the handler that found nothing left to serve, so the serve() of any later open() starts
            // the loop again
            serving_ = false;
            co_return;
        }

        // Each open() before the newest one was closed before its session could start, so it never gets one, but it is
        // still owed its onDisconnected
        while (++served_generation_ != generation) {
            invoke_logged("onDisconnected", on_disconnected_);
        }

        co_await run_session(generation);
    }
}

asio::awaitable<void> HttpStream::run_session(std::uint64_t generation) {
    try {
        if (use_ssl_) {
            co_await do_stream_session_ssl(generation);
        } else {
            co_await do_stream_session_plain(generation);
        }
    }
    catch (const std::exception& e) {
        // After close() this is the cancelled I/O, not a failure
        if (is_current(generation)) {
            LOG_ERROR("HttpStream exception: {}", e.what());
            invoke_logged("onError", on_error_, e.what());
        }
    }

    // A session that ended on its own ends its generation; after close() it is ended already
    end_generation(generation);
    invoke_logged("onDisconnected", on_disconnected_);
}

asio::awaitable<void> HttpStream::do_stream_session_ssl(std::uint64_t generation) {
    auto stream = ssl::stream<beast::tcp_stream>{ executor_, tls_context() };

    // Set SNI and the host name/IP the server certificate must match
    if (auto ec = detail::set_tls_peer_host(stream.native_handle(), host_)) {
        throw std::runtime_error("Error setting TLS peer host: " + ec.message());
    }

    if (co_await stream_response(generation, stream)) {
        // Graceful shutdown
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        auto [ec] = co_await stream.async_shutdown(asio::as_tuple);

        if(ec && ec != asio::ssl::error::stream_truncated) {
            LOG_WARN("SSL shutdown warning: {}", ec.message());
        }
    }
}

asio::awaitable<void> HttpStream::do_stream_session_plain(std::uint64_t generation) {
    auto stream = beast::tcp_stream{ executor_ };

    if (co_await stream_response(generation, stream)) {
        // Graceful shutdown
        stream.expires_after(std::chrono::seconds(5));
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        if(ec && ec != beast::errc::not_connected) {
            LOG_WARN("Socket shutdown warning: {}", ec.message());
        }
    }
}

// Connects stream to the server (with the TLS handshake for an ssl::stream), sends the streaming GET request and
// delivers the response body until the message ends, the generation is ended or the read fails. Returns false if
// the server rejected the request.
template <typename Stream>
asio::awaitable<bool> HttpStream::stream_response(std::uint64_t generation, Stream& stream) {
    auto& socket = beast::get_lowest_layer(stream);
    auto resolver = tcp::resolver{ executor_ };

    // Let close() cancel every step from the DNS lookup to the response I/O. Unregistered on return, before the
    // graceful shutdown, so a close() issued from a callback cannot cancel the shutdown.
    session_io io{ *this, generation, resolver, socket };

    // close() may have run before there was any I/O to cancel
    throw_if_closed(generation);

    // Look up the domain name
    auto const results = co_await resolver.async_resolve(host_, port_);
    throw_if_closed(generation);

    // Make the connection
    socket.expires_after(std::chrono::seconds(30));
    co_await socket.async_connect(results);
    throw_if_closed(generation);

    if constexpr (std::is_same_v<Stream, ssl::stream<beast::tcp_stream>>) {
        // Perform the SSL handshake (verifies the certificate chain and peer host)
        socket.expires_after(std::chrono::seconds(30));
        if (auto [ec] = co_await stream.async_handshake(ssl::stream_base::client, asio::as_tuple); ec) {
            detail::throw_tls_handshake_error(stream.native_handle(), ec);
        }
        throw_if_closed(generation);
    }

    // Set up an HTTP GET request for streaming
    http::request<http::string_body> req{ http::verb::get, target_, 11 };
    req.set(http::field::host, detail::format_authority(host_));
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::accept, "text/event-stream");
    req.set(http::field::cache_control, "no-cache");

    // Set custom headers
    for (auto &header_pair : headers_) {
        req.set(header_pair.first, header_pair.second);
    }

    // Disable timeout for streaming. A stream may stay silent for any length of time, and a Beast
    // timeout closes the socket, so a read timeout would drop idle streams; close() cancels instead.
    socket.expires_never();

    // Send the HTTP request
    co_await http::async_write(stream, req);
    throw_if_closed(generation);

    // Read response header first. The body is read through the same parser so the transfer coding
    // (chunk sizes, chunk extensions, trailers) is decoded; buffer_body has it write the decoded bytes
    // straight into body_buf instead of accumulating them in the message.
    beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(std::numeric_limits<std::uint64_t>::max());

    // Read just the header
    co_await http::async_read_header(stream, buffer, parser);
    throw_if_closed(generation);

    auto& res = parser.get();

    if (res.result() != http::status::ok) {
        LOG_ERROR("HTTP Stream failed with status: {}", static_cast<int>(res.result()));
        on_error_(std::format("HTTP error: {}", std::string(res.reason())));
        co_return false;
    }

    // Connection established successfully, unless close() ended the generation since the check above
    if (auto connecting = make_state(generation, Status::CONNECTING);
        !state_.compare_exchange_strong(connecting, make_state(generation, Status::CONNECTED),
                                        std::memory_order_acq_rel, std::memory_order_acquire)) {
        co_return true;
    }
    on_connected_();

    // Check if this is SSE format
    const bool is_sse = res[http::field::content_type].find("text/event-stream") != std::string::npos;

    // Drop any partial event an earlier response of this stream ended in, so it is not joined to this body
    sse_parser_.reset();

    // One read may complete several events; stop delivering them as soon as a callback calls close()
    const auto deliver_event = [this, generation](const char* data, std::size_t size) {
        if (is_current(generation)) {
            on_data_(data, size);
        }
    };

    // Body bytes async_read_header read past the header stay in buffer and are parsed by the first read
    std::array<char, 8192> body_buf;

    // Read body continuously
    while (!parser.is_done() && is_current(generation))
    {
        auto& body = res.body();
        body.data = body_buf.data();
        body.size = body_buf.size();

        auto [ec, bytes_transferred] = co_await http::async_read_some(
            stream, buffer, parser,
            asio::as_tuple(asio::use_awaitable)
        );

        if (!is_current(generation)) {
            // close() cancelled the read, or was called while the read completed; drop what it decoded
            break;
        }

        // Deliver what was decoded, even if the read then stopped with an error
        if (const auto decoded = body_buf.size() - body.size; decoded > 0) {
            if (is_sse) {
                sse_parser_.feed(body_buf.data(), decoded, deliver_event);
            } else {
                on_data_(body_buf.data(), decoded);
            }
        }

        if (!ec || ec == http::error::need_buffer) {
            // need_buffer: body_buf is full, the rest is delivered by the next read
            continue;
        }

        // Not reported once onData has called close()
        if (is_current(generation)) {
            LOG_ERROR("HTTP Stream read error: {}", ec.message());
            on_error_(ec.message());
        }
        break;
    }

    if (parser.is_done()) {
        // The server finished the response (final chunk, Content-Length reached or EOF-delimited body closed)
        LOG_INFO("HTTP Stream ended");
    }
    co_return true;
}

}   // end namespace slick::net
