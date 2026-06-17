#include <slick/net/detail/websocket_impl.hpp>
#include <slick/dynamic_buffer.hpp>
#include <slick/stream_buffer.hpp>

namespace slick::net {

template class Websocket<slick::dynamic_buffer<slick::stream_buffer>>;
template Websocket<slick::dynamic_buffer<slick::stream_buffer>>::Websocket(
    std::string,
    std::function<void()> &&,
    std::function<void()> &&,
    std::function<void(const char*, std::size_t)> &&,
    std::function<void(std::string &&err)> &&,
    std::shared_ptr<slick::stream_buffer>,
    uint32_t);

} // namespace slick::net
