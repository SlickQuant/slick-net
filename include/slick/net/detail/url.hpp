#pragma once

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace slick::net::detail {

struct url_parts {
    std::string host;    // IPv6 literals are stored without their brackets
    std::string target;  // origin-form request target, at least "/"
    uint16_t port;
    bool use_ssl;
};

// Parses "[scheme://]authority[/path][?query][#fragment]" where authority is "host", "host:port",
// "[ipv6]" or "[ipv6]:port". A missing scheme means `secure_scheme`; a missing or empty port means
// 80 for `plain_scheme` and 443 otherwise. An unbracketed authority with several colons is taken
// as an IPv6 literal without a port. The fragment is dropped (it is never sent to the server).
// Throws std::invalid_argument for an unterminated bracket or a port that is not 1-65535.
inline url_parts parse_url_parts(std::string_view url, std::string_view plain_scheme, std::string_view secure_scheme) {
    std::string_view scheme = secure_scheme;
    if (const auto pos = url.find("://"); pos != std::string_view::npos) {
        scheme = url.substr(0, pos);
        url.remove_prefix(pos + 3);
    }
    url = url.substr(0, url.find('#'));

    const auto authority_end = url.find_first_of("/?");
    const std::string_view authority = url.substr(0, authority_end);
    const std::string_view target = authority_end == std::string_view::npos ? std::string_view{} : url.substr(authority_end);

    std::string_view host = authority;
    std::string_view port;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos ||
            (close + 1 < authority.size() && authority[close + 1] != ':')) {
            throw std::invalid_argument("Invalid IPv6 literal in URL authority: " + std::string(authority));
        }
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            port = authority.substr(close + 2);
        }
    }
    else if (const auto colon = authority.find(':');
             colon != std::string_view::npos && authority.find(':', colon + 1) == std::string_view::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }

    url_parts parts{std::string(host), {}, static_cast<uint16_t>(scheme == plain_scheme ? 80 : 443), scheme == secure_scheme};
    if (!port.empty()) {
        const auto [end, ec] = std::from_chars(port.data(), port.data() + port.size(), parts.port);
        if (ec != std::errc{} || end != port.data() + port.size() || parts.port == 0) {
            throw std::invalid_argument("Invalid port in URL authority: " + std::string(authority));
        }
    }

    if (target.empty() || target.front() != '/') {
        parts.target.reserve(target.size() + 1);
        parts.target.push_back('/');
    }
    parts.target.append(target);
    return parts;
}

// Host header value for a parsed host: restores the brackets around IPv6 literals and appends
// ":port" when `port` is non-zero.
inline std::string format_authority(std::string_view host, uint16_t port = 0) {
    const bool ipv6 = host.find(':') != std::string_view::npos;
    std::string authority;
    authority.reserve(host.size() + 8);
    if (ipv6) {
        authority.push_back('[');
    }
    authority.append(host);
    if (ipv6) {
        authority.push_back(']');
    }
    if (port) {
        char digits[5];
        authority.push_back(':');
        authority.append(digits, std::to_chars(digits, digits + sizeof(digits), port).ptr);
    }
    return authority;
}

} // namespace slick::net::detail
