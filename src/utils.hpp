#pragma once

#include <slick/net/detail/url.hpp>

#include <tuple>
#include <string>
#include <string_view>

namespace slick::net {

// Returns {host, target, port, use_ssl, host_header}; the host of an IPv6 literal has no brackets, and
// host_header is the value of the Host header (see detail::format_host_header).
inline std::tuple<std::string, std::string, std::string, bool, std::string> parse_url(std::string_view url) {
    auto parts = detail::parse_url_parts(url, "http", "https");
    auto host_header = detail::format_host_header(parts);
    return {std::move(parts.host), std::move(parts.target), std::to_string(parts.port), parts.use_ssl, std::move(host_header)};
}

}   // end namespace slick::net
