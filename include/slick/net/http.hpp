// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Slick Quant LLC
// https://github.com/SlickQuant/slick-net

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <boost/asio/awaitable.hpp>

namespace slick::net {

class Http {
public:
    struct Response {
        uint32_t result_code = 0;
        std::string reason;
        std::string result_text;

        bool is_ok() const noexcept {
            return result_code >= 200 && result_code < 300;
        }
    };

    static Response get(std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static Response post(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static Response put(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static Response patch(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static Response del(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});

    static void async_get(std::function<void(Response&&)> on_response, std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_post(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_put(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_patch(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static void async_del(std::function<void(Response&&)> on_response, std::string_view url, std::string_view data = "", std::vector<std::pair<std::string, std::string>>&& headers = {});

    static boost::asio::awaitable<Response> async_get(std::string_view url, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static boost::asio::awaitable<Response> async_post(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static boost::asio::awaitable<Response> async_put(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static boost::asio::awaitable<Response> async_patch(std::string_view url, std::string_view data, std::vector<std::pair<std::string, std::string>>&& headers = {});
    static boost::asio::awaitable<Response> async_del(std::string_view url, std::string_view data = "", std::vector<std::pair<std::string, std::string>>&& headers = {});
};

} // namespace slick::net
