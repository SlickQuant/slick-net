// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Slick Quant LLC
// https://github.com/SlickQuant/slick-net

#pragma once

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <boost/asio/ssl/context.hpp>
#include <boost/system/error_code.hpp>

namespace slick::net {

/**
 * @brief TLS client context shared by `Http`, `HttpStream` and `Websocket` for
 *        `https://` and `wss://` connections.
 *
 * Created on first use with:
 *  - TLS 1.2
 *  - peer certificate verification (`boost::asio::ssl::verify_peer`); every
 *    connection additionally requires the certificate to match the URL's host
 *    name or IP address
 *  - the system trust roots: exactly `SSL_CERT_FILE` / `SSL_CERT_DIR` when either
 *    is set, otherwise OpenSSL's default verify paths plus the Windows `ROOT`
 *    certificate store (Windows) or the platform CA bundle (Linux/macOS)
 *
 * Customize it before opening connections; it must not be modified while
 * handshakes are in progress. For example, to trust a private CA:
 * @code
 *   slick::net::tls_context().load_verify_file("corp-root-ca.pem");
 * @endcode
 *
 * Setting `boost::asio::ssl::verify_none` disables both certificate and host name
 * verification and exposes connections to man-in-the-middle interception.
 */
boost::asio::ssl::context& tls_context();

namespace detail {

// Sets SNI (DNS names only) and binds the expected peer identity to a client SSL
// handle so the handshake fails unless the certificate matches `host`.
boost::system::error_code set_tls_peer_host(SSL* ssl, const std::string& host) noexcept;

// Throws boost::system::system_error for a failed client handshake, naming the
// certificate verification failure (e.g. "hostname mismatch") when there is one.
[[noreturn]] void throw_tls_handshake_error(SSL* ssl, const boost::system::error_code& ec);

} // namespace detail

} // namespace slick::net
