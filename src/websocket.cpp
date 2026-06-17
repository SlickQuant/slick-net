#include <slick/net/detail/websocket_impl.hpp>

namespace slick::net::detail {

asio::io_context ioc_;
ssl::context ctx_{ssl::context::tlsv12_client};
std::thread service_thread_;
std::atomic_bool init_service_thread_{ false };
std::atomic_bool run_;

extern "C" inline void __signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        Websocket<>::shutdown();
    }
}

void install_signal_handlers() {
    std::signal(SIGINT, __signal_handler);
    std::signal(SIGTERM, __signal_handler);
}

struct WebsocketServiceTerminater {
    ~WebsocketServiceTerminater() {
        Websocket<>::shutdown();
    }
};

WebsocketServiceTerminater s_websocket_service_terminater;

} // namespace slick::net::detail

namespace slick::net {

// Provide out-of-line definitions for the default buffer type so user TUs
// do not need to instantiate them.
template class Websocket<boost::beast::flat_buffer>;

} // namespace slick::net
