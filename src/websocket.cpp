#include <slick/net/detail/websocket_impl.hpp>

namespace {
    asio::io_context ioc_;
    ssl::context ctx_{ssl::context::tlsv12_client};
    std::thread service_thread_;
    std::atomic_bool init_service_thread_{false};
    std::atomic_bool run_{false};

    using signal_handler_t = void (*)(int);
    signal_handler_t previous_sigint = SIG_DFL;
    signal_handler_t previous_sigterm = SIG_DFL;
}

namespace slick::net::detail {

asio::io_context& websocket_ioc() noexcept {
    return ioc_;
}
ssl::context& websocket_ssl_context() noexcept {
    return ctx_;
}

bool websocket_running() noexcept {
    return run_.load(std::memory_order_relaxed);
}

extern "C" inline void __signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        Websocket<>::shutdown();
    }

    auto previous = signal == SIGINT ? previous_sigint : previous_sigterm;
    if (previous && previous != SIG_DFL && previous != SIG_IGN) {
        previous(signal);
    } else if (previous == SIG_DFL) {
        std::signal(signal, SIG_DFL);
        std::raise(signal);
    }
}

void install_signal_handlers() {
    previous_sigint = std::signal(SIGINT, __signal_handler);
    previous_sigterm = std::signal(SIGTERM, __signal_handler);
}

void start_websocket_service() {
    auto init_service = init_service_thread_.load(std::memory_order_relaxed);
    if (init_service_thread_.compare_exchange_strong(init_service, true,
                                                     std::memory_order_acq_rel) && !init_service) {
        install_signal_handlers();
        run_.store(true, std::memory_order_release);
        service_thread_ = std::thread([]() {
            LOG_INFO("Websocket service thread started.");
            while (run_.load(std::memory_order_relaxed)) {
                try {
                    if (ioc_.stopped()) {
                        ioc_.restart();
                    }
                    ioc_.run();
                }
                catch(const std::exception& e) {
                    ioc_.restart();
                    LOG_ERROR("{}", e.what());
                }
            }

            if (!ioc_.stopped()) [[unlikely]] {
                LOG_TRACE("call ioc_.stop at the end of run");
                ioc_.stop();
            }
            LOG_INFO("Websocket service thread exit");
            init_service_thread_.store(false, std::memory_order_release);
        });
    }
}

void stop_websocket_service() {
    if (run_.load(std::memory_order_relaxed)) {
        LOG_DEBUG("Shutting down WebSocket service thread.");
        run_.store(false, std::memory_order_release);
        ioc_.stop();
        if (service_thread_.joinable()) {
            service_thread_.join();
        }
    }
    previous_sigint = SIG_DFL;
    previous_sigterm = SIG_DFL;
}

struct WebsocketServiceTerminater {
    ~WebsocketServiceTerminater() {
        stop_websocket_service();
    }
};

WebsocketServiceTerminater s_websocket_service_terminater;

} // namespace slick::net::detail

namespace slick::net {

// Provide out-of-line definitions for the default buffer type so user TUs
// do not need to instantiate them.
template class Websocket<boost::beast::flat_buffer>;

} // namespace slick::net
