#include <slick/net/tls.hpp>
#include <slick/net/logging.hpp>

#include <atomic>
#include <cstdlib>
#include <format>
#include <memory>

#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/system/system_error.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
// wincrypt.h defines macros (X509_NAME, ...) that clash with OpenSSL, so it must
// come after the OpenSSL headers.
#include <windows.h>
#include <wincrypt.h>
#else
#include <filesystem>
#endif

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;

namespace slick::net {

namespace {

std::atomic<ssl::context*> tls_context_{nullptr};

bool env_is_set(const char* name) noexcept {
#ifdef _MSC_VER
    std::size_t len = 0;
    return getenv_s(&len, nullptr, 0, name) == 0 && len > 0;
#else
    return std::getenv(name) != nullptr;
#endif
}

#ifdef _WIN32
// OpenSSL does not read the Windows certificate store, so import its trusted roots.
void load_platform_trust_roots(ssl::context& ctx) {
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store) {
        LOG_WARN("Unable to open the Windows ROOT certificate store (error {})", GetLastError());
        return;
    }

    X509_STORE* x509_store = SSL_CTX_get_cert_store(ctx.native_handle());
    PCCERT_CONTEXT cert = nullptr;
    while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
        const unsigned char* der = cert->pbCertEncoded;
        if (X509* x509 = d2i_X509(nullptr, &der, static_cast<long>(cert->cbCertEncoded))) {
            X509_STORE_add_cert(x509_store, x509);
            X509_free(x509);
        }
    }
    CertCloseStore(store, 0);
    ERR_clear_error();
}
#else
void load_platform_trust_roots(ssl::context& ctx) {
    // set_default_verify_paths() already loaded OpenSSL's compiled-in bundle when it exists.
    std::error_code fs_ec;
    if (std::filesystem::is_regular_file(X509_get_default_cert_file(), fs_ec)) {
        return;
    }

    // Otherwise (e.g. a vcpkg-built OpenSSL whose OPENSSLDIR has no bundle) use the OS bundle.
    static constexpr const char* ca_bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch, Gentoo
        "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                            // openSUSE
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS, RHEL 7
        "/etc/ssl/cert.pem",                                 // macOS, Alpine, FreeBSD
    };
    boost::system::error_code ec;
    for (const char* path : ca_bundles) {
        ctx.load_verify_file(path, ec);
        if (!ec) {
            return;
        }
    }
    LOG_WARN("No system CA bundle found; set SSL_CERT_FILE or call slick::net::tls_context().load_verify_file()");
}
#endif

std::unique_ptr<ssl::context> make_tls_context() {
    auto ctx = std::make_unique<ssl::context>(ssl::context::tlsv12_client);
    ctx->set_verify_mode(ssl::verify_peer);

    boost::system::error_code ec;
    ctx->set_default_verify_paths(ec);  // also honors SSL_CERT_FILE / SSL_CERT_DIR
    if (ec) {
        LOG_WARN("Failed to load OpenSSL default verify paths: {}", ec.message());
    }

    // An explicit SSL_CERT_FILE / SSL_CERT_DIR defines the complete trust set.
    if (!env_is_set(X509_get_default_cert_file_env()) && !env_is_set(X509_get_default_cert_dir_env())) {
        load_platform_trust_roots(*ctx);
    }
    return ctx;
}

boost::system::error_code last_ssl_error() noexcept {
    const auto err = ::ERR_get_error();
    if (err == 0) {
        return asio::error::invalid_argument;
    }
    return {static_cast<int>(err), asio::error::get_ssl_category()};
}

} // namespace

ssl::context& tls_context() {
    ssl::context* ctx = tls_context_.load(std::memory_order_acquire);
    if (ctx) [[likely]] {
        return *ctx;
    }

    // Lock-free one-time init: racing callers each build a context and all but the CAS
    // winner discard theirs. The context is intentionally never destroyed so sessions
    // torn down during static destruction can still reference it.
    auto created = make_tls_context();
    if (tls_context_.compare_exchange_strong(ctx, created.get(),
                                             std::memory_order_acq_rel, std::memory_order_acquire)) {
        return *created.release();
    }
    return *ctx;
}

namespace detail {

boost::system::error_code set_tls_peer_host(SSL* ssl, const std::string& host) noexcept {
    // An empty host would leave no expected identity and silently skip the check.
    if (host.empty()) {
        return asio::error::invalid_argument;
    }

    ::ERR_clear_error();
    X509_VERIFY_PARAM* param = SSL_get0_param(ssl);

    boost::system::error_code addr_ec;
    asio::ip::make_address(host, addr_ec);
    if (!addr_ec) {
        // IP literals are matched against iPAddress SANs and are not valid SNI (RFC 6066).
        if (!X509_VERIFY_PARAM_set1_ip_asc(param, host.c_str())) {
            return last_ssl_error();
        }
        return {};
    }

    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (!SSL_set_tlsext_host_name(ssl, host.c_str()) ||
        !X509_VERIFY_PARAM_set1_host(param, host.c_str(), host.size())) {
        return last_ssl_error();
    }
    return {};
}

void throw_tls_handshake_error(SSL* ssl, const boost::system::error_code& ec) {
    const long verify_result = SSL_get_verify_result(ssl);
    if (verify_result != X509_V_OK) {
        throw boost::system::system_error(
            ec, std::format("TLS handshake failed ({})", X509_verify_cert_error_string(verify_result)));
    }
    throw boost::system::system_error(ec, "TLS handshake failed");
}

} // namespace detail

} // namespace slick::net
