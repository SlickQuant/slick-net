#include <slick/net/detail/websocket_impl.hpp>

#include <boost/asio/executor_work_guard.hpp>

namespace {
    asio::io_context ioc_;
    std::thread service_thread_;
    std::atomic_bool init_service_thread_{false};
    std::atomic_bool run_{false};
    std::atomic_bool busy_poll_{false};
    std::atomic<std::uint64_t> service_loop_iterations_{0};
}

namespace slick::net::detail {

asio::io_context& websocket_ioc() noexcept {
    return ioc_;
}

bool websocket_running() noexcept {
    return run_.load(std::memory_order_relaxed);
}

void set_websocket_busy_poll(bool enable) noexcept {
    if (!enable) {
        // The polling loop switches back to a blocking run() on its next iteration
        busy_poll_.store(false, std::memory_order_release);
    }
    else if (!busy_poll_.exchange(true, std::memory_order_acq_rel)) {
        // Wake the service thread out of a blocking run() so it starts polling
        ioc_.stop();
    }
}

bool websocket_busy_poll() noexcept {
    return busy_poll_.load(std::memory_order_relaxed);
}

std::uint64_t websocket_service_loop_iterations() noexcept {
    return service_loop_iterations_.load(std::memory_order_acquire);
}

void start_websocket_service() {
    auto init_service = init_service_thread_.load(std::memory_order_relaxed);
    if (init_service_thread_.compare_exchange_strong(init_service, true,
                                                     std::memory_order_acq_rel) && !init_service) {
        run_.store(true, std::memory_order_release);
        service_thread_ = std::thread([]() {
            LOG_INFO("Websocket service thread started.");
            // Clear a stop() left by a previous shutdown() or busy-poll switch
            ioc_.restart();
            {
                // Outstanding work keeps run() blocked while idle; without it run()
                // returns at once when no I/O is pending and the loop spins a core
                auto work = asio::make_work_guard(ioc_);
                while (run_.load(std::memory_order_acquire)) {
                    // Single writer, so load + store needs no CAS
                    service_loop_iterations_.store(
                        service_loop_iterations_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
                    try {
                        if (busy_poll_.load(std::memory_order_relaxed)) {
                            if (ioc_.poll() == 0 && ioc_.stopped()) {
                                ioc_.restart();
                            }
                        }
                        else {
                            // With the work guard, run() only returns once stopped by
                            // shutdown() or a busy-poll switch. run_ is re-checked after
                            // the restart, so a shutdown() stop is never lost.
                            ioc_.run();
                            ioc_.restart();
                        }
                    }
                    catch(const std::exception& e) {
                        // A throwing handler does not stop the io_context; resume as is
                        LOG_ERROR("{}", e.what());
                    }
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
