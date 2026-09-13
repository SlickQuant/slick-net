// Regression tests for TLS peer verification: https:// and wss:// connections must
// reject untrusted certificates and certificates issued for a different host.
// A local TLS server with runtime-generated certificates keeps these hermetic.

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <slick/net/http.hpp>
#include <slick/net/http_stream.hpp>
#include <slick/net/tls.hpp>
#include <slick/net/websocket.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;
using ::testing::HasSubstr;

struct openssl_deleter {
    void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); }
    void operator()(X509* p) const noexcept { X509_free(p); }
    void operator()(X509_EXTENSION* p) const noexcept { X509_EXTENSION_free(p); }
    void operator()(BIO* p) const noexcept { BIO_free(p); }
    void operator()(SSL* p) const noexcept { SSL_free(p); }
};

template<typename T>
using openssl_ptr = std::unique_ptr<T, openssl_deleter>;

struct test_certificate {
    std::string cert_pem;
    std::string key_pem;
};

std::string bio_to_string(BIO* bio) {
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio, &mem);
    return std::string(mem->data, mem->length);
}

// Self-signed P-256 certificate. `subject_alt_name` uses OpenSSL config syntax, e.g. "DNS:localhost".
test_certificate make_self_signed_certificate(const char* common_name, const char* subject_alt_name) {
    openssl_ptr<EVP_PKEY> key{EVP_EC_gen("P-256")};
    openssl_ptr<X509> cert{X509_new()};
    if (!key || !cert) {
        throw std::runtime_error("failed to allocate test certificate");
    }

    X509_set_version(cert.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 24 * 3600);
    X509_set_pubkey(cert.get(), key.get());

    auto* name = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0);
    X509_set_issuer_name(cert.get(), name);

    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, cert.get(), cert.get(), nullptr, nullptr, 0);
    openssl_ptr<X509_EXTENSION> san{X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name, subject_alt_name)};
    if (!san || !X509_add_ext(cert.get(), san.get(), -1) || !X509_sign(cert.get(), key.get(), EVP_sha256())) {
        throw std::runtime_error("failed to build test certificate");
    }

    openssl_ptr<BIO> cert_bio{BIO_new(BIO_s_mem())};
    openssl_ptr<BIO> key_bio{BIO_new(BIO_s_mem())};
    PEM_write_bio_X509(cert_bio.get(), cert.get());
    PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr);
    return {bio_to_string(cert_bio.get()), bio_to_string(key_bio.get())};
}

// Loopback TLS server: answers HTTP requests with 200 "ok" and accepts WebSocket upgrades.
class local_tls_server {
public:
    explicit local_tls_server(const test_certificate& cert)
        : ssl_ctx_(ssl::context::tls_server)
        , acceptor_v4_(ioc_)
        , acceptor_v6_(ioc_) {
        ssl_ctx_.use_certificate_chain(asio::buffer(cert.cert_pem));
        ssl_ctx_.use_private_key(asio::buffer(cert.key_pem), ssl::context::pem);

        if (!listen(acceptor_v4_, tcp::endpoint{asio::ip::address_v4::loopback(), 0})) {
            throw std::runtime_error("failed to listen on 127.0.0.1");
        }
        port_ = acceptor_v4_.local_endpoint().port();
        // "localhost" may resolve to ::1 first; listen there too so clients don't stall
        // on a refused IPv6 connect. Best effort: IPv6 may be unavailable.
        listen(acceptor_v6_, tcp::endpoint{asio::ip::address_v6::loopback(), port_});

        thread_ = std::thread([this] { ioc_.run(); });
    }

    ~local_tls_server() {
        ioc_.stop();
        thread_.join();
    }

    uint16_t port() const noexcept { return port_; }
    int handshakes() const noexcept { return handshakes_.load(std::memory_order_acquire); }

private:
    bool listen(tcp::acceptor& acceptor, const tcp::endpoint& endpoint) {
        boost::system::error_code ec;
        acceptor.open(endpoint.protocol(), ec);
        if (!ec && endpoint.address().is_v6()) {
            acceptor.set_option(asio::ip::v6_only(true), ec);
        }
        if (!ec) {
            acceptor.bind(endpoint, ec);
        }
        if (!ec) {
            acceptor.listen(asio::socket_base::max_listen_connections, ec);
        }
        if (ec) {
            acceptor.close(ec);
            return false;
        }
        asio::co_spawn(ioc_, accept_loop(acceptor), asio::detached);
        return true;
    }

    asio::awaitable<void> accept_loop(tcp::acceptor& acceptor) {
        for (;;) {
            auto [ec, socket] = co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                co_return;
            }
            asio::co_spawn(ioc_, serve(std::move(socket)), asio::detached);
        }
    }

    asio::awaitable<void> serve(tcp::socket socket) {
        const auto token = asio::as_tuple(asio::use_awaitable);
        ssl::stream<tcp::socket> stream{std::move(socket), ssl_ctx_};
        if (auto [ec] = co_await stream.async_handshake(ssl::stream_base::server, token); ec) {
            co_return;  // the client rejected the certificate
        }
        handshakes_.fetch_add(1, std::memory_order_release);

        beast::flat_buffer buffer;
        http::request<http::string_body> req;
        if (auto [ec, n] = co_await http::async_read(stream, buffer, req, token); ec) {
            co_return;
        }

        if (websocket::is_upgrade(req)) {
            websocket::stream<ssl::stream<tcp::socket>> ws{std::move(stream)};
            if (auto [ec] = co_await ws.async_accept(req, token); ec) {
                co_return;
            }
            beast::flat_buffer ws_buffer;
            for (;;) {  // hold the session open until the client closes it
                auto [ec, n] = co_await ws.async_read(ws_buffer, token);
                if (ec) {
                    co_return;
                }
                ws_buffer.consume(ws_buffer.size());
            }
        }

        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::content_type, "text/plain");
        res.keep_alive(false);
        res.body() = "ok";
        res.prepare_payload();
        co_await http::async_write(stream, res, token);
        co_await stream.async_shutdown(token);
    }

    ssl::context ssl_ctx_;
    asio::io_context ioc_;
    tcp::acceptor acceptor_v4_;
    tcp::acceptor acceptor_v6_;
    std::thread thread_;
    uint16_t port_ = 0;
    std::atomic_int handshakes_{0};
};

// Callback state shared with the service threads; `error` is written before `errored`
// and `disconnected` are set.
struct session_events {
    std::atomic_bool connected{false};
    std::atomic_bool disconnected{false};
    std::atomic_bool errored{false};
    std::string error;
};

std::string make_url(const char* scheme, const char* host, uint16_t port) {
    return std::format("{}://{}:{}/", scheme, host, port);
}

template<typename Predicate>
bool wait_for(Predicate pred, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

std::shared_ptr<slick::net::HttpStream> make_http_stream(const std::string& url,
                                                         const std::shared_ptr<session_events>& events) {
    return std::make_shared<slick::net::HttpStream>(
        url,
        [events]() { events->connected.store(true, std::memory_order_release); },
        [events]() { events->disconnected.store(true, std::memory_order_release); },
        [](const char*, std::size_t) {},
        [events](std::string err) {
            events->error = std::move(err);
            events->errored.store(true, std::memory_order_release);
        });
}

std::unique_ptr<slick::net::Websocket<>> make_websocket(const std::string& url,
                                                        const std::shared_ptr<session_events>& events) {
    return std::make_unique<slick::net::Websocket<>>(
        url,
        [events]() { events->connected.store(true, std::memory_order_release); },
        [events]() { events->disconnected.store(true, std::memory_order_release); },
        [](const char*, std::size_t) {},
        [events](std::string&& err) {
            events->error = std::move(err);
            events->errored.store(true, std::memory_order_release);
        });
}

} // namespace

namespace slick::net {

// ======================== Shared context configuration ========================

TEST(TlsContextTest, VerifiesPeerByDefault) {
    EXPECT_NE(SSL_CTX_get_verify_mode(tls_context().native_handle()) & SSL_VERIFY_PEER, 0);
}

TEST(TlsContextTest, ReturnsSameContext) {
    EXPECT_EQ(&tls_context(), &tls_context());
}

TEST(TlsContextTest, PeerHostSetsSniAndExpectedHostName) {
    openssl_ptr<SSL> ssl{SSL_new(tls_context().native_handle())};
    ASSERT_TRUE(ssl);

    EXPECT_FALSE(detail::set_tls_peer_host(ssl.get(), "example.com"));
    EXPECT_STREQ(SSL_get_servername(ssl.get(), TLSEXT_NAMETYPE_host_name), "example.com");
    EXPECT_STREQ(X509_VERIFY_PARAM_get0_host(SSL_get0_param(ssl.get()), 0), "example.com");
}

TEST(TlsContextTest, PeerHostMatchesIpAddressWithoutSni) {
    openssl_ptr<SSL> ssl{SSL_new(tls_context().native_handle())};
    ASSERT_TRUE(ssl);

    EXPECT_FALSE(detail::set_tls_peer_host(ssl.get(), "127.0.0.1"));
    EXPECT_EQ(SSL_get_servername(ssl.get(), TLSEXT_NAMETYPE_host_name), nullptr);
    EXPECT_EQ(X509_VERIFY_PARAM_get0_host(SSL_get0_param(ssl.get()), 0), nullptr);
    char* ip = X509_VERIFY_PARAM_get1_ip_asc(SSL_get0_param(ssl.get()));
    EXPECT_STREQ(ip, "127.0.0.1");
    OPENSSL_free(ip);
}

TEST(TlsContextTest, PeerHostRejectsEmptyHost) {
    openssl_ptr<SSL> ssl{SSL_new(tls_context().native_handle())};
    ASSERT_TRUE(ssl);

    EXPECT_TRUE(detail::set_tls_peer_host(ssl.get(), ""));
}

// ======================== Handshake verification ========================

class TlsVerificationTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Both are trusted: `trusted_` is issued for localhost, `wrong_host_` for another name.
        trusted_ = std::make_unique<test_certificate>(
            make_self_signed_certificate("slick-net trusted test", "DNS:localhost"));
        wrong_host_ = std::make_unique<test_certificate>(
            make_self_signed_certificate("slick-net wrong host test", "DNS:wrong-host.example"));
        untrusted_ = std::make_unique<test_certificate>(
            make_self_signed_certificate("slick-net untrusted test", "DNS:localhost"));

        tls_context().add_certificate_authority(asio::buffer(trusted_->cert_pem));
        tls_context().add_certificate_authority(asio::buffer(wrong_host_->cert_pem));
    }

    void TearDown() override {
        Websocket<>::shutdown();
    }

    inline static std::unique_ptr<test_certificate> trusted_;
    inline static std::unique_ptr<test_certificate> wrong_host_;
    inline static std::unique_ptr<test_certificate> untrusted_;
};

TEST_F(TlsVerificationTest, HttpAcceptsTrustedCertificate) {
    local_tls_server server{*trusted_};

    auto response = Http::get(make_url("https", "localhost", server.port()));

    EXPECT_EQ(response.result_code, 200u) << response.result_text;
    EXPECT_EQ(response.result_text, "ok");
    EXPECT_EQ(server.handshakes(), 1);
}

TEST_F(TlsVerificationTest, HttpRejectsUntrustedCertificate) {
    local_tls_server server{*untrusted_};

    auto response = Http::get(make_url("https", "localhost", server.port()));

    EXPECT_EQ(response.result_code, 500u);
    EXPECT_THAT(response.result_text, HasSubstr("TLS handshake failed"));
    EXPECT_EQ(server.handshakes(), 0);
}

TEST_F(TlsVerificationTest, HttpRejectsHostNameMismatch) {
    local_tls_server server{*wrong_host_};

    auto response = Http::get(make_url("https", "localhost", server.port()));

    EXPECT_EQ(response.result_code, 500u);
    EXPECT_THAT(response.result_text, HasSubstr("hostname mismatch"));
    EXPECT_EQ(server.handshakes(), 0);
}

TEST_F(TlsVerificationTest, HttpRejectsIpAddressMismatch) {
    local_tls_server server{*trusted_};  // valid for DNS:localhost only

    auto response = Http::get(make_url("https", "127.0.0.1", server.port()));

    EXPECT_EQ(response.result_code, 500u);
    EXPECT_THAT(response.result_text, HasSubstr("IP address mismatch"));
    EXPECT_EQ(server.handshakes(), 0);
}

TEST_F(TlsVerificationTest, HttpAwaitableRejectsUntrustedCertificate) {
    local_tls_server server{*untrusted_};

    asio::io_context ioc;
    Http::Response response;
    asio::co_spawn(ioc, Http::async_get(make_url("https", "localhost", server.port())),
        [&response](std::exception_ptr e, Http::Response rsp) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    response = Http::Response{500, "", ex.what()};
                }
                return;
            }
            response = std::move(rsp);
        });
    ioc.run();

    EXPECT_EQ(response.result_code, 500u);
    EXPECT_THAT(response.result_text, HasSubstr("TLS handshake failed"));
    EXPECT_EQ(server.handshakes(), 0);
}

TEST_F(TlsVerificationTest, HttpStreamAcceptsTrustedCertificate) {
    local_tls_server server{*trusted_};
    auto events = std::make_shared<session_events>();
    auto stream = make_http_stream(make_url("https", "localhost", server.port()), events);

    stream->open();

    ASSERT_TRUE(wait_for([&] { return events->disconnected.load(std::memory_order_acquire); }));
    EXPECT_TRUE(events->connected.load(std::memory_order_acquire));
    EXPECT_FALSE(events->errored.load(std::memory_order_acquire)) << events->error;
    EXPECT_EQ(server.handshakes(), 1);
}

TEST_F(TlsVerificationTest, HttpStreamRejectsUntrustedCertificate) {
    local_tls_server server{*untrusted_};
    auto events = std::make_shared<session_events>();
    auto stream = make_http_stream(make_url("https", "localhost", server.port()), events);

    stream->open();

    ASSERT_TRUE(wait_for([&] { return events->disconnected.load(std::memory_order_acquire); }));
    EXPECT_FALSE(events->connected.load(std::memory_order_acquire));
    ASSERT_TRUE(events->errored.load(std::memory_order_acquire));
    EXPECT_THAT(events->error, HasSubstr("TLS handshake failed"));
    EXPECT_EQ(server.handshakes(), 0);
}

TEST_F(TlsVerificationTest, WebsocketAcceptsTrustedCertificate) {
    local_tls_server server{*trusted_};
    auto events = std::make_shared<session_events>();
    auto ws = make_websocket(make_url("wss", "localhost", server.port()), events);

    ws->open();

    ASSERT_TRUE(wait_for([&] { return events->connected.load(std::memory_order_acquire); })) << events->error;
    EXPECT_EQ(server.handshakes(), 1);

    ws->close();
    EXPECT_TRUE(wait_for([&] { return events->disconnected.load(std::memory_order_acquire); }));
}

TEST_F(TlsVerificationTest, WebsocketRejectsUntrustedCertificate) {
    local_tls_server server{*untrusted_};
    auto events = std::make_shared<session_events>();
    auto ws = make_websocket(make_url("wss", "localhost", server.port()), events);

    ws->open();

    ASSERT_TRUE(wait_for([&] { return events->disconnected.load(std::memory_order_acquire); }));
    EXPECT_FALSE(events->connected.load(std::memory_order_acquire));
    ASSERT_TRUE(events->errored.load(std::memory_order_acquire));
    EXPECT_THAT(events->error, HasSubstr("TLS handshake failed"));
    EXPECT_EQ(server.handshakes(), 0);
}

TEST_F(TlsVerificationTest, WebsocketRejectsHostNameMismatch) {
    local_tls_server server{*wrong_host_};
    auto events = std::make_shared<session_events>();
    auto ws = make_websocket(make_url("wss", "localhost", server.port()), events);

    ws->open();

    ASSERT_TRUE(wait_for([&] { return events->disconnected.load(std::memory_order_acquire); }));
    EXPECT_FALSE(events->connected.load(std::memory_order_acquire));
    ASSERT_TRUE(events->errored.load(std::memory_order_acquire));
    EXPECT_THAT(events->error, HasSubstr("hostname mismatch"));
    EXPECT_EQ(server.handshakes(), 0);
}

} // namespace slick::net
