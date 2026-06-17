#include <slick/net/detail/websocket_impl.hpp>
#include <slick/dynamic_buffer.hpp>
#include <slick/stream_buffer_multiplexer.hpp>

namespace slick::net {

using producer_buffer = slick::stream_buffer_multiplexer::producer_buffer;

template class Websocket<slick::dynamic_buffer<producer_buffer>>;
template Websocket<slick::dynamic_buffer<producer_buffer>>::Websocket(
    std::string,
    std::function<void()> &&,
    std::function<void()> &&,
    std::function<void(const char*, std::size_t)> &&,
    std::function<void(std::string &&err)> &&,
    std::shared_ptr<producer_buffer>,
    uint32_t);

} // namespace slick::net
