#include <slick/net/detail/websocket_impl.hpp>

#include <boost/asio/executor_work_guard.hpp>

namespace {
    asio::io_context ioc_;

    // Who may touch service_thread_. starting and stopping are owned states: a thread enters one only by
    // winning a CAS out of an unowned state (stopped, running, stop_requested) and is the only one that
    // leaves it again, so the object is inspected, joined, move-assigned and destroyed by that owner
    // alone. Without this, concurrent shutdown()s raced each other on the same std::thread - both read
    // get_id(), both found it joinable, and both joined it - and raced an open() move-assigning over it.
    enum class service_state : std::uint8_t {
        stopped,         // no thread left to join; service_thread_ is free to claim
        starting,        // an owner is creating the service thread
        running,         // the service thread is serving
        stop_requested,  // a shutdown() from a callback asked the thread it runs on to stop; that thread
                         // cannot join itself, so it winds down with the join still owed
        stopping,        // an owner is joining the thread, either to end the service or to reuse the object
    };
    std::atomic<service_state> service_state_{ service_state::stopped };
    std::thread service_thread_;
    // Set on the service thread itself, so a shutdown() from a callback recognizes the self-join it must
    // not attempt without reading service_thread_, which it does not own
    thread_local bool on_service_thread_ = false;
    std::atomic_bool run_{false};
    std::atomic_bool busy_poll_{false};
    std::atomic<std::uint64_t> service_loop_iterations_{0};
    std::atomic<std::uint64_t> write_wakeups_{0};

    // Asks the service loop to exit; the thread returns from run() and finishes once the callback it is in
    // returns. Idempotent, so both a shutdown() that only requests the stop and the one that later joins
    // the thread can call it.
    void request_service_stop() {
        if (run_.exchange(false, std::memory_order_acq_rel)) {
            LOG_DEBUG("Shutting down WebSocket service thread.");
            ioc_.stop();
        }
    }

    // Joins the service thread once it has finished. Only ever called while holding the stopping state,
    // and never from the service thread itself: a thread cannot join itself, and every caller checks
    // on_service_thread_ before claiming that state.
    void join_service_thread() {
        if (service_thread_.joinable()) {
            service_thread_.join();
        }
    }
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

void count_websocket_write_wakeup() noexcept {
    // Only the service thread counts, so load + store needs no CAS
    write_wakeups_.store(write_wakeups_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

std::uint64_t websocket_write_wakeups() noexcept {
    return write_wakeups_.load(std::memory_order_acquire);
}

void start_websocket_service() {
    auto state = service_state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == service_state::running || state == service_state::starting) {
            // The service is up, or another thread is bringing it up
            return;
        }
        if (on_service_thread_) {
            // Only a shutdown() from a callback leaves the service stopped with its thread still running,
            // and reusing the object means joining that thread first - which it cannot do to itself. The
            // restart is left to the next open() or shutdown() from another thread, or to program exit.
            return;
        }
        if (state == service_state::stopping) {
            // Another start or shutdown owns the object while it joins; it is free again shortly
            std::this_thread::yield();
            state = service_state_.load(std::memory_order_acquire);
            continue;
        }
        // stopped, or stop_requested with a thread still to be joined. Claiming stopping rather than
        // starting for that join is what keeps the thread being joined - which may run a shutdown() of its
        // own while it winds down - from waiting on a state whose owner is waiting on it.
        const auto claimed = state == service_state::stopped ? service_state::starting
                                                             : service_state::stopping;
        if (service_state_.compare_exchange_weak(state, claimed,
                                                 std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (claimed == service_state::stopping) {
                // A shutdown() from a callback cannot join the thread it runs on, so this object may still
                // hold that finished thread; move-assigning onto a joinable std::thread would terminate
                // the process. Past this join the old thread is gone, so the new one below can never find
                // the state its own shutdown() must not wait on.
                join_service_thread();
                service_state_.store(service_state::starting, std::memory_order_release);
            }
            break;
        }
    }

    // Only after the join: a previous thread still in its loop would take this for a restart and never exit
    run_.store(true, std::memory_order_release);
    service_thread_ = std::thread([]() {
        on_service_thread_ = true;
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
    });
    service_state_.store(service_state::running, std::memory_order_release);
}

void stop_websocket_service() {
    auto state = service_state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == service_state::stopped) {
            // Nothing is running and nothing is left to join
            return;
        }
        if (state == service_state::starting) {
            // The owner has only the thread to create, so it reaches running shortly. Waiting rather than
            // returning keeps a shutdown() that races an open() from leaving the service up. Safe from the
            // service thread too: the thread being started is the only one that can be here, and the owner
            // waiting on it has already finished its join.
            std::this_thread::yield();
            state = service_state_.load(std::memory_order_acquire);
            continue;
        }
        if (on_service_thread_) {
            // Callbacks run on the service thread, so a shutdown() from one would have the thread join
            // itself: that throws resource_deadlock_would_occur and leaves this object joinable, and a
            // joinable std::thread terminates the process when it is destroyed or assigned over.
            // Requesting the stop is enough - the loop exits once the callback returns, and the join
            // happens in a later shutdown() from another thread, before the next start_websocket_service()
            // reuses the object, or in the terminator below. Waiting out an owner of the stopping state is
            // never an option here: it is waiting on this very thread.
            if (state != service_state::running ||
                service_state_.compare_exchange_weak(state, service_state::stop_requested,
                                                     std::memory_order_acq_rel, std::memory_order_acquire)) {
                request_service_stop();
                return;
            }
            continue;
        }
        if (state == service_state::stopping) {
            // Another shutdown(), or an open() restarting the service, owns the join; wait it out so this
            // call still returns with the service thread stopped and joined
            std::this_thread::yield();
            state = service_state_.load(std::memory_order_acquire);
            continue;
        }
        // running, or a thread that a shutdown() from a callback could only ask to stop
        if (service_state_.compare_exchange_weak(state, service_state::stopping,
                                                 std::memory_order_acq_rel, std::memory_order_acquire)) {
            request_service_stop();
            join_service_thread();
            service_state_.store(service_state::stopped, std::memory_order_release);
            return;
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
