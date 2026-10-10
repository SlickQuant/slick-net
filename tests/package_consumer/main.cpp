// Consumes an installed slick-net through find_package(slick-net) alone: every header, include
// path and library below has to arrive through the slick::net target. Nothing here touches the
// network.
#include <slick/net/http.hpp>
#include <slick/net/http_stream.hpp>
#include <slick/net/tls.hpp>
#include <slick/net/websocket.hpp>
#include <slick/dynamic_buffer.hpp>
#include <slick/stream_buffer.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <cstdio>
#include <memory>
#include <string>

using namespace slick::net;

template<typename BufferT, typename BackendT>
static bool constructs_disconnected(BackendT backend) {
    Websocket<BufferT> ws(
        "wss://localhost",
        []() {}, []() {},
        [](const char*, std::size_t) {},
        [](std::string&&) {},
        std::move(backend)
    );
    return ws.status() == Websocket<BufferT>::Status::DISCONNECTED;
}

int main() {
    // Compiled into the library and backed by OpenSSL, so a missing transitive link fails here
    (void)tls_context();

    // The slick backends are explicit instantiations inside the installed library. They resolve
    // only if this translation unit selects the same slick::default_queue_traits the library
    // was built with.
    using PBuf = slick::stream_buffer_multiplexer::producer_buffer;
    slick::stream_buffer_multiplexer mux(256);

    bool ok = constructs_disconnected<slick::dynamic_buffer<slick::stream_buffer>>(
                  std::make_shared<slick::stream_buffer>(1u << 20, 1u << 10))
           && constructs_disconnected<slick::dynamic_buffer<PBuf>>(
                  mux.add_producer(0, 1u << 20, 1u << 10));

    Websocket<>::shutdown();

    std::puts(ok ? "slick-net package consumer: OK" : "slick-net package consumer: FAILED");
    return ok ? 0 : 1;
}
