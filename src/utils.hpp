#pragma once

#include <tuple>
#include <string>
#include <string_view>

namespace slick::net {

inline std::tuple<std::string, std::string, std::string, bool> parse_url(std::string_view url) {
    std::string host;
    std::string target;
    uint_fast16_t port = (uint_fast16_t)-1;
    bool use_ssl = true;  // Default to SSL

    std::string protoco("https");
    auto pos = url.find("://");
    if (pos == std::string::npos)
    {
        pos = url.find("/");
        if (pos == std::string::npos)
        {
            host = std::string(url);
            target = "/";
        }
        else
        {
            host = std::string(url.substr(0, pos));
            target = std::string(url.substr(pos));
        }
    }
    else
    {
        protoco = std::string(url.substr(0, pos));
        auto host_begin = pos + 3;
        pos = url.find("/", host_begin);
        if (pos == std::string::npos)
        {
            host = std::string(url.substr(host_begin));
            target = "/";
        }
        else
        {
            host = std::string(url.substr(host_begin, pos - host_begin));
            target = std::string(url.substr(pos));
        }
    }

    pos = host.find(':');
    if (pos != 3 && pos != 4 && pos != std::string::npos)
    {
        port = std::stoi(std::string(host.substr(pos + 1)));
        host = std::string(host.substr(0, pos));
    }

    if (port == (uint_fast16_t)-1)
    {
        port = (protoco == "http") ? 80 : 443;
    }

    // Determine if SSL should be used
    use_ssl = (protoco == "https");

    return {host, target, std::to_string(port), use_ssl};
}

}   // end namespace slick::net