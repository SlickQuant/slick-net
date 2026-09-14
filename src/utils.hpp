#pragma once

#include <slick/net/detail/url.hpp>

#include <tuple>
#include <string>
#include <string_view>

namespace slick::net {

// Returns {host, target, port, use_ssl}; the host of an IPv6 literal has no brackets.
inline std::tuple<std::string, std::string, std::string, bool> parse_url(std::string_view url) {
    auto parts = detail::parse_url_parts(url, "http", "https");
    return {std::move(parts.host), std::move(parts.target), std::to_string(parts.port), parts.use_ssl};
}

}   // end namespace slick::net
