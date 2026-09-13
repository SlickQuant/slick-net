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

struct net_queue_traits : slick::queue_traits {
    static constexpr bool enable_read_last = false;
    static constexpr bool enable_cpu_relax = false;
};

using buffer_multiplexer = slick::basic_stream_buffer_multiplexer<net_queue_traits>;
using fast_producer_buffer = buffer_multiplexer::producer_buffer;

template class Websocket<slick::dynamic_buffer<fast_producer_buffer>>;
template Websocket<slick::dynamic_buffer<fast_producer_buffer>>::Websocket(
    std::string,
    std::function<void()> &&,
    std::function<void()> &&,
    std::function<void(const char*, std::size_t)> &&,
    std::function<void(std::string &&err)> &&,
    std::shared_ptr<fast_producer_buffer>,
    uint32_t);


} // namespace slick::net
