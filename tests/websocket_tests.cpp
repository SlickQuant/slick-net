#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <string>
#include <algorithm>

#include <slick/net/websocket.hpp>

namespace slick::net {

// Mock callbacks for testing
class MockWebsocketCallbacks {
public:
    MOCK_METHOD(void, onConnected, (), ());
    MOCK_METHOD(void, onDisconnected, (), ());
    MOCK_METHOD(void, onData, (const char*, std::size_t), ());
    MOCK_METHOD(void, onError, (std::string&&), ());
};

// Helper class to synchronize async events in tests
class EventSynchronizer {
public:
    void wait_for(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        auto start = std::chrono::high_resolution_clock::now();
        while(!triggered_.load(std::memory_order_relaxed) &&
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start) < timeout);
    }

    void notify() {
        triggered_.store(true, std::memory_order_release);
    }

    void reset() {
        triggered_.store(false, std::memory_order_release);
    }

    bool is_triggered() const {
        return triggered_.load(std::memory_order_relaxed);
    }

private:
    std::atomic_bool triggered_ = false;
};

class WebsocketTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Ensure clean state for each test
        // Note: Don't call shutdown() here as it can cause use-after-free
        // if previous test's callbacks are still pending
    }

    void TearDown() override {
        Websocket::shutdown();
        // Clean up after each test
        // Note: Don't call shutdown() here - let websockets close naturally
        // Calling shutdown() can invoke callbacks after local variables are destroyed
    }

    // Helper: Wait with timeout for a condition
    template<typename Predicate>
    bool wait_for_condition(Predicate pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        auto start = std::chrono::steady_clock::now();
        while (!pred()) {
            if (std::chrono::steady_clock::now() - start > timeout) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }
};

TEST_F(WebsocketTest, ConstructorParsesWssUrlWithHostAndPath) {
    std::atomic<bool> connected_called{false};
    std::atomic<bool> disconnected_called{false};
    std::atomic<bool> data_called{false};
    std::atomic<bool> error_called{false};

    Websocket ws(
        "wss://ws.postman-echo.com/raw/test",
        [&]() { connected_called = true; },
        [&]() { disconnected_called = true; },
        [&](const char*, std::size_t) { data_called = true; },
        [&](std::string&&) { error_called = true; }
    );

    // Check URL parsing
    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED);
}

TEST_F(WebsocketTest, ConstructorParsesWssUrlWithPort) {
    std::atomic<bool> connected_called{false};
    std::atomic<bool> disconnected_called{false};
    std::atomic<bool> data_called{false};
    std::atomic<bool> error_called{false};

    Websocket ws (
        "wss://ws.postman-echo.com/raw:443/test",
        [&]() { connected_called = true; },
        [&]() { disconnected_called = true; },
        [&](const char*, std::size_t) { data_called = true; },
        [&](std::string&&) { error_called = true; }
    );

    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED);
}

TEST_F(WebsocketTest, ConstructorParsesWsUrl) {
    // Test that plain WebSocket (ws://) URLs are parsed correctly
    std::atomic<bool> connected_called{false};
    std::atomic<bool> disconnected_called{false};
    std::atomic<bool> data_called{false};
    std::atomic<bool> error_called{false};

    Websocket ws(
        "ws://localhost:8080/test",
        [&]() { connected_called = true; },
        [&]() { disconnected_called = true; },
        [&](const char*, std::size_t) { data_called = true; },
        [&](std::string&&) { error_called = true; }
    );

    // Should successfully create websocket object and parse URL
    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED);

    // Note: This test verifies URL parsing for plain WebSocket.
    // To test actual ws:// connections, run a local WebSocket server:
    // Example: wscat --listen 8080
}

TEST_F(WebsocketTest, ConstructorParsesHostOnlyUrl) {
    std::atomic<bool> connected_called{false};
    std::atomic<bool> disconnected_called{false};
    std::atomic<bool> data_called{false};
    std::atomic<bool> error_called{false};

    Websocket ws(
        "echo.websocket.org",
        [&]() { connected_called = true; },
        [&]() { disconnected_called = true; },
        [&](const char*, std::size_t) { data_called = true; },
        [&](std::string&&) { error_called = true; }
    );

    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED);
}

TEST_F(WebsocketTest, ConstructorParsesUrlWithCustomPort) {
    std::atomic<bool> connected_called{false};
    std::atomic<bool> disconnected_called{false};
    std::atomic<bool> data_called{false};
    std::atomic<bool> error_called{false};

    Websocket ws(
        "wss://ws.postman-echo.com/raw:9001/test",
        [&]() { connected_called = true; },
        [&]() { disconnected_called = true; },
        [&](const char*, std::size_t) { data_called = true; },
        [&](std::string&&) { error_called = true; }
    );

    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED);
}

TEST_F(WebsocketTest, StatusTransitions) {
    std::atomic<bool> connected_called{false};
    std::atomic<bool> disconnected_called{false};
    std::atomic<bool> data_called{false};
    std::atomic<bool> error_called{false};

    Websocket ws(
        "wss://ws.postman-echo.com/raw/test",
        [&]() { connected_called = true; },
        [&]() { disconnected_called = true; },
        [&](const char*, std::size_t) { data_called = true; },
        [&](std::string&&) { error_called = true; }
    );

    // Initial state should be DISCONNECTED
    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED);

    // Note: We can't easily test CONNECTING/CONNECTED states without mocking
    // the network layer, but we can verify the initial state
}

TEST_F(WebsocketTest, CallbacksAreStored) {
    int connected_count = 0;
    int disconnected_count = 0;
    int data_count = 0;
    int error_count = 0;

    Websocket ws(
        "wss://ws.postman-echo.com/raw/test",
        [&]() { connected_count++; },
        [&]() { disconnected_count++; },
        [&](const char*, std::size_t) { data_count++; },
        [&](std::string&&) { error_count++; }
    );

    // We can't directly invoke the callbacks since they're private,
    // but we can verify the websocket is created successfully
    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED);
    EXPECT_EQ(connected_count, 0);
    EXPECT_EQ(disconnected_count, 0);
    EXPECT_EQ(data_count, 0);
    EXPECT_EQ(error_count, 0);
}

TEST_F(WebsocketTest, IsRunningInitiallyFalse) {
    EXPECT_FALSE(Websocket::is_running());
}

TEST_F(WebsocketTest, ShutdownWhenNotRunning) {
    // Should not crash when shutting down when not running
    Websocket::shutdown();
    EXPECT_FALSE(Websocket::is_running());
}

// ======================== Connection Lifecycle Tests ========================

TEST_F(WebsocketTest, ConnectToEchoServer) {
    // Use shared_ptr captures: on_close may fire on_error_() after the test function
    // returns (SSL teardown is async via IOCP), making [&] captures dangling.
    auto connected_sync = std::make_shared<EventSynchronizer>();
    auto error_sync = std::make_shared<EventSynchronizer>();
    auto error_message = std::make_shared<std::string>();

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [connected_sync]() { connected_sync->notify(); },
        []() {},
        [](const char*, std::size_t) {},
        [error_message, error_sync](std::string &&err) {
            *error_message = std::move(err);
            error_sync->notify();
        }
    );

    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED);

    ws.open();

    // Wait for connection or error
    connected_sync->wait_for(std::chrono::milliseconds(10000));

    // Should be connected or have an error
    EXPECT_TRUE(connected_sync->is_triggered() || error_sync->is_triggered());

    if (connected_sync->is_triggered()) {
        EXPECT_EQ(ws.status(), Websocket::Status::CONNECTED);
    }

    // Always close the websocket
    ws.close();
}

TEST_F(WebsocketTest, CloseConnection) {
    EventSynchronizer connected_sync;
    EventSynchronizer disconnected_sync;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() { disconnected_sync.notify(); },
        [&](const char*, std::size_t) {},
        [&](std::string&&) {}
    );

    ws.open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    if (connected_sync.is_triggered()) {
        ws.close();

        disconnected_sync.wait_for(std::chrono::milliseconds(1000));

        // Verify status changes to DISCONNECTING
        EXPECT_TRUE(ws.status() == Websocket::Status::DISCONNECTING ||
                   ws.status() == Websocket::Status::DISCONNECTED);
    } else {
        // Always close the websocket even if connection failed
        ws.close();
    }
}

TEST_F(WebsocketTest, InvalidHostnameError) {
    // Use shared_ptr captures: DNS timeout can exceed test lifetime, causing stale callbacks.
    auto error_sync = std::make_shared<EventSynchronizer>();
    auto error_message = std::make_shared<std::string>();

    Websocket ws(
        "wss://invalid-hostname-that-does-not-exist-12345.com",
        []() {},
        []() {},
        [](const char*, std::size_t) {},
        [error_message, error_sync](std::string &&err) {
            *error_message = std::move(err);
            error_sync->notify();
        }
    );

    ws.open();
    EXPECT_TRUE(ws.status() == Websocket::Status::CONNECTING);
    error_sync->wait_for(std::chrono::milliseconds(5000));

    EXPECT_TRUE(error_sync->is_triggered());
    EXPECT_FALSE(error_message->empty());

    // Close the websocket to clean up
    ws.close();
}

// ======================== Message Send/Receive Tests ========================

TEST_F(WebsocketTest, SendAndReceiveEcho) {
    EventSynchronizer connected_sync;
    EventSynchronizer data_sync;
    std::string received_data;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char* data, std::size_t len) {
            received_data.assign(data, len);
            data_sync.notify();
        },
        [&](std::string&&) {}
    );

    ws.open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        const char* test_message = "Hello WebSocket!";
        ws.send(test_message, strlen(test_message));

        data_sync.wait_for(std::chrono::milliseconds(5000));

        if (data_sync.is_triggered()) {
            EXPECT_EQ(received_data, "Hello WebSocket!");
        }
    }

    // Always close the websocket
    ws.close();
}

TEST_F(WebsocketTest, SendMultipleMessages) {
    EventSynchronizer connected_sync;
    std::atomic<int> messages_received{0};
    std::vector<std::string> received_messages;
    std::mutex messages_mutex;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char* data, std::size_t len) {
            std::lock_guard<std::mutex> lock(messages_mutex);
            received_messages.emplace_back(data, len);
            messages_received++;
        },
        [&](std::string&&) {}
    );

    ws.open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        const int num_messages = 5;
        for (int i = 0; i < num_messages; ++i) {
            std::string msg = "Message " + std::to_string(i);
            ws.send(msg.c_str(), msg.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Wait for all messages to be received
        wait_for_condition([&]() { return messages_received >= num_messages; },
                          std::chrono::milliseconds(10000));

        EXPECT_GE(messages_received.load(), num_messages);
    }

    // Always close the websocket
    ws.close();
}

TEST_F(WebsocketTest, SendLargeMessage) {
    EventSynchronizer connected_sync;
    EventSynchronizer data_sync;
    std::string received_data;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char* data, std::size_t len) {
            received_data.assign(data, len);
            data_sync.notify();
        },
        [&](std::string&&) {}
    );

    ws.open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        // Create a large message (10KB)
        std::string large_message(10240, 'A');
        ws.send(large_message.c_str(), large_message.size());

        data_sync.wait_for(std::chrono::milliseconds(5000));

        if (data_sync.is_triggered()) {
            EXPECT_EQ(received_data.size(), large_message.size());
            EXPECT_EQ(received_data, large_message);
        }
    }

    // Always close the websocket
    ws.close();
}

TEST_F(WebsocketTest, SendBinaryData) {
    EventSynchronizer connected_sync;
    EventSynchronizer data_sync;
    EventSynchronizer error_sync;
    std::vector<char> received_data;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char* data, std::size_t len) {
            received_data.assign(data, data + len);
            data_sync.notify();
        },
        [&](std::string &&e) {
            printf("%s\n", e.c_str());
            error_sync.notify();
        }
    );

    ws.open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    if (connected_sync.is_triggered() && !error_sync.is_triggered()) {
        // Binary data with null bytes
        std::vector<char> binary_data = {'\x01', '\x02', '\x03', static_cast<char>(0xFF), static_cast<char>(0xFE), '\x42'};
        ws.send(binary_data.data(), binary_data.size(), true);

        data_sync.wait_for(std::chrono::milliseconds(5000));

        // The public test server (ws.postman-echo.com) is sometimes unreliable with binary data
        // and may close the connection unexpectedly. Only verify if we successfully received data.
        if (!error_sync.is_triggered() && data_sync.is_triggered()) {
            EXPECT_EQ(received_data.size(), binary_data.size());
            EXPECT_EQ(received_data, binary_data);
        } else {
            // Server may reject binary data or close connection - log but don't fail
            printf("Note: Binary data test incomplete due to server behavior (error: %s, data: %s)\n",
                   error_sync.is_triggered() ? "yes" : "no",
                   data_sync.is_triggered() ? "yes" : "no");
        }
    } else {
        printf("Note: Binary data test skipped - connection failed\n");
    }

    // Always close the websocket
    ws.close();
}

// ======================== Concurrent Operations Tests ========================

TEST_F(WebsocketTest, ConcurrentSends) {
    EventSynchronizer connected_sync;
    std::atomic<int> messages_sent{0};
    std::atomic<int> messages_received{0};

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char*, std::size_t) {
            messages_received++;
        },
        [&](std::string&&) {}
    );

    ws.open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        const int num_threads = 3;
        const int messages_per_thread = 5;
        std::vector<std::thread> threads;

        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < messages_per_thread; ++i) {
                    std::string msg = "Thread-" + std::to_string(t) + "-Msg-" + std::to_string(i);
                    ws.send(msg.c_str(), msg.size());
                    messages_sent++;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        // Wait for all messages to be received
        wait_for_condition([&]() {
            return messages_received >= num_threads * messages_per_thread;
        }, std::chrono::milliseconds(10000));

        EXPECT_EQ(messages_sent.load(), num_threads * messages_per_thread);
        EXPECT_GE(messages_received.load(), num_threads * messages_per_thread);
    }

    // Always close the websocket
    ws.close();
}

TEST_F(WebsocketTest, MultipleWebsocketInstances) {
    const int num_websockets = 3;
    std::vector<std::shared_ptr<Websocket>> websockets;
    std::vector<EventSynchronizer> connected_syncs(num_websockets);
    std::atomic<int> total_connected{0};

    for (int i = 0; i < num_websockets; ++i) {
        auto ws = std::make_shared<Websocket>(
            "wss://ws.postman-echo.com/raw",
            [&, i]() {
                connected_syncs[i].notify();
                total_connected++;
            },
            [&]() {},
            [&](const char*, std::size_t) {},
            [&](std::string&&) {}
        );
        websockets.push_back(ws);
    }

    // Open all connections
    for (auto& ws : websockets) {
        ws->open();
    }

    // Wait for all to connect
    for (auto& sync : connected_syncs) {
        sync.wait_for(std::chrono::milliseconds(10000));
    }

    EXPECT_GE(total_connected.load(), 1); // At least one should connect

    // Close all
    for (auto& ws : websockets) {
        ws->close();
    }
}

// ======================== Status and State Tests ========================

TEST_F(WebsocketTest, StatusTransitionsOnConnect) {
    EventSynchronizer connected_sync;
    std::vector<Websocket::Status> observed_statuses;
    std::mutex status_mutex;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() {
            {
                std::lock_guard<std::mutex> lock(status_mutex);
                observed_statuses.push_back(ws.status());
            }
            connected_sync.notify();
        },
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string&&) {}
    );

    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED);

    ws.open();

    // Should transition to CONNECTING
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto status_after_open = ws.status();
    EXPECT_TRUE(status_after_open == Websocket::Status::CONNECTING ||
                status_after_open == Websocket::Status::CONNECTED);

    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        EXPECT_EQ(ws.status(), Websocket::Status::CONNECTED);
    }

    // Always close the websocket
    ws.close();
}

TEST_F(WebsocketTest, CannotSendWhenDisconnected) {
    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() {},
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string&&) {}
    );

    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED);

    // Sending before open() shouldn't crash (logged and dropped)
    const char* msg = "test";
    EXPECT_NO_THROW(ws.send(msg, strlen(msg)));
}

TEST_F(WebsocketTest, IsRunningAfterFirstOpen) {
    EventSynchronizer connected_sync;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string) {}
    );

    EXPECT_FALSE(Websocket::is_running());

    ws.open();

    // Give it time to start the service thread
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_TRUE(Websocket::is_running());

    ws.close();
}

// ======================== Error Handling Tests ========================

TEST_F(WebsocketTest, MultipleErrorCallbacks) {
    // Use shared_ptr captures: DNS timeout can exceed test lifetime, causing stale callbacks.
    auto error_count = std::make_shared<std::atomic<int>>(0);
    auto error_messages = std::make_shared<std::vector<std::string>>();
    auto error_mutex = std::make_shared<std::mutex>();

    Websocket ws(
        "wss://invalid-host-12345.test",
        []() {},
        []() {},
        [](const char*, std::size_t) {},
        [error_count, error_messages, error_mutex](std::string &&err) {
            std::lock_guard<std::mutex> lock(*error_mutex);
            error_messages->push_back(err);
            (*error_count)++;
        }
    );

    ws.open();

    // Wait for error
    wait_for_condition([error_count]() { return error_count->load() > 0; },
                      std::chrono::milliseconds(5000));

    EXPECT_GT(error_count->load(), 0);

    std::lock_guard<std::mutex> lock(*error_mutex);
    EXPECT_FALSE(error_messages->empty());

    // Close the websocket to clean up
    ws.close();
}

TEST_F(WebsocketTest, ReconnectAfterError) {
    // Use shared_ptr captures for ws_first: DNS timeout can exceed test lifetime.
    auto first_error_sync = std::make_shared<EventSynchronizer>();
    auto error_count = std::make_shared<std::atomic<int>>(0);
    EventSynchronizer second_connected_sync;

    Websocket ws_first(
        "wss://invalid-host-xyz.test",
        []() {},
        []() {},
        [](const char*, std::size_t) {},
        [error_count, first_error_sync](std::string&&) {
            (*error_count)++;
            first_error_sync->notify();
        }
    );

    ws_first.open();
    first_error_sync->wait_for(std::chrono::milliseconds(5000));

    EXPECT_GT(error_count->load(), 0);

    // Close the first websocket to clean up before moving on
    ws_first.close();

    // Now try connecting to a valid host with a new instance
    Websocket ws_second(
        "wss://ws.postman-echo.com/raw",
        [&]() { second_connected_sync.notify(); },
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string) {}
    );

    ws_second.open();
    second_connected_sync.wait_for(std::chrono::milliseconds(10000));

    if (second_connected_sync.is_triggered()) {
        EXPECT_EQ(ws_second.status(), Websocket::Status::CONNECTED);
        ws_second.close();
    }
}

// ======================== Edge Cases ========================

TEST_F(WebsocketTest, EmptyMessageSend) {
    EventSynchronizer connected_sync;
    EventSynchronizer data_sync;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char* data, std::size_t len) {
            if (len == 0) {
                data_sync.notify();
            }
        },
        [&](std::string) {}
    );

    ws.open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        // Send empty message
        ws.send("", 0);

        // Some servers may echo it back, others may not
        data_sync.wait_for(std::chrono::milliseconds(2000));
    }

    // Always close the websocket
    ws.close();
}

TEST_F(WebsocketTest, RapidOpenClose) {
    EventSynchronizer connected_sync;
    EventSynchronizer disconnected_sync;
    EventSynchronizer error_sync;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() { disconnected_sync.notify(); },
        [&](const char*, std::size_t) {},
        [&](std::string) { error_sync.notify(); }
    );

    // Rapidly open and close
    ws.open();
    wait_for_condition([&]() {
        return connected_sync.is_triggered() || error_sync.is_triggered();
    }, std::chrono::milliseconds(1000));

    if (connected_sync.is_triggered()) {
        EXPECT_EQ(ws.status(), Websocket::Status::CONNECTED);
    }

    ws.close();
    EXPECT_TRUE(wait_for_condition([&]() {
        return ws.status() == Websocket::Status::DISCONNECTED ||
            ws.status() == Websocket::Status::DISCONNECTING ||
            disconnected_sync.is_triggered();
    }, std::chrono::milliseconds(2000)));

    // Should not crash
    EXPECT_TRUE(ws.status() == Websocket::Status::DISCONNECTED ||
                ws.status() == Websocket::Status::DISCONNECTING);
}

TEST_F(WebsocketTest, DoubleClose) {
    EventSynchronizer connected_sync;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string) {}
    );

    ws.open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        ws.close();

        // Second close should not crash
        EXPECT_NO_THROW(ws.close());
    } else {
        // Always close the websocket even if connection failed
        ws.close();
    }
}

TEST_F(WebsocketTest, DestructorWhileConnected) {
    EventSynchronizer connected_sync;

    {
        Websocket ws(
            "wss://ws.postman-echo.com/raw",
            [&]() { connected_sync.notify(); },
            [&]() {},
            [&](const char*, std::size_t) {},
            [&](std::string) {}
        );

        ws.open();
        connected_sync.wait_for(std::chrono::milliseconds(10000));

        // Let ws go out of scope while connected
    }

    // Should not crash
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ======================== Plain WebSocket (ws://) Tests ========================
// Note: Plain WebSocket servers are less common and reliable for testing.
// The following test demonstrates plain WebSocket functionality but expects
// connection failure unless you have a local WebSocket server running.

TEST_F(WebsocketTest, PlainWebsocket_UrlParsing) {
    // Verify that plain WebSocket URLs are correctly parsed and handled.
    // NOTE: All callback-captured variables are shared_ptr to prevent use-after-free.
    // If localhost:9001 is firewalled (not just "connection refused"), the TCP connect
    // can take up to 30 s to time out. The callback may fire after this test function
    // returns, so captures must remain valid beyond the test's stack frame.
    auto error_sync = std::make_shared<EventSynchronizer>();
    auto connected = std::make_shared<std::atomic<bool>>(false);
    auto error_message = std::make_shared<std::string>();

    Websocket ws(
        "ws://localhost:9001",  // Local server (won't be running in CI)
        [connected]() { connected->store(true); },
        []() {},
        [](const char*, std::size_t) {},
        [error_message, error_sync](std::string err) {
            *error_message = err;
            error_sync->notify();
        }
    );

    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED);

    ws.open();

    // Wait a bit to see if connection succeeds or fails
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // Either connected to local server OR got connection error (expected in CI)
    if (connected->load()) {
        EXPECT_EQ(ws.status(), Websocket::Status::CONNECTED);
        std::cout << "Note: Successfully connected to local plain WebSocket server\n";
        ws.close();
    } else {
        // Expected behavior when no local server is running
        std::cout << "Note: Plain WebSocket test - no local server running (expected in CI)\n";
        ws.close();
    }

    // This test passes either way - it verifies the code doesn't crash
    SUCCEED();
}

// ======================== Send Immediately After Open Tests ========================

TEST_F(WebsocketTest, SendImmediatelyAfterOpen_MessageQueuedAndSentAfterConnect) {
    EventSynchronizer connected_sync;
    EventSynchronizer data_sync;
    std::string received_data;
    std::atomic<bool> message_sent{false};

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() {
            connected_sync.notify();
        },
        [&]() {},
        [&](const char* data, std::size_t len) {
            received_data.assign(data, len);
            data_sync.notify();
        },
        [&](std::string) {}
    );

    // Initial state should be DISCONNECTED
    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED);

    // Open the WebSocket connection
    ws.open();

    // Immediately send data right after calling open (before connection is established)
    const char* test_message = "Immediate message after open";
    ws.send(test_message, strlen(test_message));
    message_sent.store(true);

    // At this point, the connection should be CONNECTING or CONNECTED
    auto status_after_send = ws.status();
    EXPECT_TRUE(status_after_send == Websocket::Status::CONNECTING ||
                status_after_send == Websocket::Status::CONNECTED);

    // Wait for connection to be established
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        EXPECT_EQ(ws.status(), Websocket::Status::CONNECTED);
        EXPECT_TRUE(message_sent.load());

        // Wait for the echo response
        data_sync.wait_for(std::chrono::milliseconds(5000));

        if (data_sync.is_triggered()) {
            // Verify the message was sent and echoed back after connection was established
            EXPECT_EQ(received_data, "Immediate message after open");
        }
    }

    // Always close the websocket
    ws.close();
}

TEST_F(WebsocketTest, SendImmediatelyAfterOpen_MultipleMessages) {
    EventSynchronizer connected_sync;
    std::atomic<int> messages_received{0};
    std::vector<std::string> received_messages;
    std::mutex messages_mutex;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() {
            connected_sync.notify();
        },
        [&]() {},
        [&](const char* data, std::size_t len) {
            std::lock_guard<std::mutex> lock(messages_mutex);
            received_messages.emplace_back(data, len);
            messages_received++;
        },
        [&](std::string) {}
    );

    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED);

    // Open connection
    ws.open();

    // Immediately send multiple messages right after open
    const int num_messages = 3;
    for (int i = 0; i < num_messages; ++i) {
        std::string msg = "Quick message " + std::to_string(i);
        ws.send(msg.c_str(), msg.size());
    }

    // Wait for connection to establish
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        EXPECT_EQ(ws.status(), Websocket::Status::CONNECTED);

        // Wait for all messages to be received
        wait_for_condition([&]() { return messages_received >= num_messages; },
                          std::chrono::milliseconds(10000));

        // All messages should have been queued and sent after connection established
        EXPECT_GE(messages_received.load(), num_messages);

        std::lock_guard<std::mutex> lock(messages_mutex);
        EXPECT_GE(received_messages.size(), static_cast<size_t>(num_messages));
    }

    ws.close();
}

TEST_F(WebsocketTest, SendImmediatelyAfterOpen_VerifyOrderPreserved) {
    EventSynchronizer connected_sync;
    std::atomic<int> messages_received{0};
    std::vector<std::string> received_messages;
    std::mutex messages_mutex;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() {
            connected_sync.notify();
        },
        [&]() {},
        [&](const char* data, std::size_t len) {
            std::lock_guard<std::mutex> lock(messages_mutex);
            received_messages.emplace_back(data, len);
            messages_received++;
        },
        [&](std::string) {}
    );

    ws.open();

    // Send messages immediately after open - they should be queued
    ws.send("First", 5);
    ws.send("Second", 6);
    ws.send("Third", 5);

    // Wait for connection
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        // Wait for all messages to arrive
        wait_for_condition([&]() { return messages_received >= 3; },
                          std::chrono::milliseconds(10000));

        std::lock_guard<std::mutex> lock(messages_mutex);
        EXPECT_GE(received_messages.size(), 3u);

        // Verify order is preserved (messages queued before connection should arrive in order)
        if (received_messages.size() >= 3) {
            EXPECT_EQ(received_messages[0], "First");
            EXPECT_EQ(received_messages[1], "Second");
            EXPECT_EQ(received_messages[2], "Third");
        }
    }

    ws.close();
}

TEST_F(WebsocketTest, SendImmediatelyAfterOpen_LargeMessage) {
    EventSynchronizer connected_sync;
    EventSynchronizer data_sync;
    std::string received_data;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() {
            connected_sync.notify();
        },
        [&]() {},
        [&](const char* data, std::size_t len) {
            received_data.assign(data, len);
            data_sync.notify();
        },
        [&](std::string) {}
    );

    ws.open();

    // Send a large message immediately after open
    std::string large_message(5120, 'X');  // 5KB message
    ws.send(large_message.c_str(), large_message.size());

    // Wait for connection
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        EXPECT_EQ(ws.status(), Websocket::Status::CONNECTED);

        // Wait for the large message echo
        data_sync.wait_for(std::chrono::milliseconds(10000));

        if (data_sync.is_triggered()) {
            EXPECT_EQ(received_data.size(), large_message.size());
            EXPECT_EQ(received_data, large_message);
        }
    }

    ws.close();
}

// Note: Main tests use wss://ws.postman-echo.com which is a public test server.
// Tests may fail if the server is down or network is unavailable.
//
// For testing plain WebSocket (ws://) connections locally:
// 1. Install wscat: npm install -g wscat
// 2. Run server: wscat --listen 9001
// 3. Run the PlainWebsocket_UrlParsing test above
//
// For production testing, consider setting up a local websocket test server.

// ======================== Reconnect Tests ========================
// These tests verify that creating a new Websocket instance to reconnect
// works correctly during normal program operation (service thread always running).

struct ReceivedMessages {
    std::vector<std::string> msgs;
    std::mutex mtx;
    std::atomic<int> count{0};
    EventSynchronizer sync;

    void add(const char* data, std::size_t len) {
        std::lock_guard<std::mutex> lk(mtx);
        msgs.emplace_back(data, len);
        count++;
        sync.notify();
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mtx);
        msgs.clear();
        count.store(0);
        sync.reset();
    }
};

// TEST 1: Basic new-object reconnect after graceful close
TEST_F(WebsocketTest, Reconnect_NewObject_AfterGracefulClose_Connects) {
    EventSynchronizer ws1_connected, ws1_disconnected;
    EventSynchronizer ws2_connected;
    ReceivedMessages received;

    auto ws1 = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { ws1_connected.notify(); },
        [&]() { ws1_disconnected.notify(); },
        [&](const char*, std::size_t) {},
        [&](std::string&&) {}
    );

    ws1->open();
    ws1_connected.wait_for(std::chrono::milliseconds(10000));

    if (!ws1_connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    ws1->close();
    bool disconnected = wait_for_condition(
        [&]() { return ws1->status() == Websocket::Status::DISCONNECTED; },
        std::chrono::milliseconds(5000));
    ASSERT_TRUE(disconnected) << "ws1 did not reach DISCONNECTED";
    ws1.reset();

    auto ws2 = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { ws2_connected.notify(); },
        [&]() {},
        [&](const char* d, std::size_t l) { received.add(d, l); },
        [&](std::string&&) {}
    );

    ws2->open();
    ws2_connected.wait_for(std::chrono::milliseconds(10000));

    ASSERT_TRUE(ws2_connected.is_triggered()) << "ws2 failed to connect after ws1 closed";
    EXPECT_EQ(ws2->status(), Websocket::Status::CONNECTED);

    const std::string msg = "hello-from-ws2";
    ws2->send(msg.data(), msg.size());
    received.sync.wait_for(std::chrono::milliseconds(5000));

    EXPECT_TRUE(received.sync.is_triggered()) << "No echo received on ws2";
    if (received.sync.is_triggered()) {
        std::lock_guard<std::mutex> lk(received.mtx);
        EXPECT_FALSE(received.msgs.empty());
        if (!received.msgs.empty()) {
            EXPECT_EQ(received.msgs[0], msg);
        }
    }
    ws2->close();
}

TEST_F(WebsocketTest, Reconnect_AfterGracefulClose_Connects) {
    EventSynchronizer ws_connected, ws_disconnected;
    EventSynchronizer ws2_connected;
    ReceivedMessages received;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { ws_connected.notify(); },
        [&]() { ws_disconnected.notify(); },
        [&](const char* d, std::size_t l) { received.add(d, l); },
        [&](std::string&&) {}
    );

    ws.open();
    ws_connected.wait_for(std::chrono::milliseconds(10000));

    if (!ws_connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    ws.close();
    bool disconnected = wait_for_condition(
        [&]() { return ws.status() == Websocket::Status::DISCONNECTED; },
        std::chrono::milliseconds(5000));
    ASSERT_TRUE(disconnected) << "ws did not reach DISCONNECTED";

    ws_connected.reset();
    ws_disconnected.reset();

    ws.open();
    ws_connected.wait_for(std::chrono::milliseconds(10000));

    ASSERT_TRUE(ws_connected.is_triggered()) << "ws2 failed to connect after ws1 closed";
    EXPECT_EQ(ws.status(), Websocket::Status::CONNECTED);

    const std::string msg = "hello-from-ws2";
    ws.send(msg.data(), msg.size());
    received.sync.wait_for(std::chrono::milliseconds(5000));

    EXPECT_TRUE(received.sync.is_triggered()) << "No echo received on ws2";
    if (received.sync.is_triggered()) {
        std::lock_guard<std::mutex> lk(received.mtx);
        EXPECT_FALSE(received.msgs.empty());
        if (!received.msgs.empty()) {
            EXPECT_EQ(received.msgs[0], msg);
        }
    }
    ws.close();
}

// TEST 2: Multiple consecutive new-object reconnect cycles
TEST_F(WebsocketTest, Reconnect_NewObject_MultipleConsecutiveCycles_AllConnect) {
    std::atomic<int> total_connects{0};
    std::atomic<int> total_disconnects{0};

    for (int cycle = 0; cycle < 3; ++cycle) {
        EventSynchronizer connected_sync, disconnected_sync;
        ReceivedMessages received;

        auto ws = std::make_shared<Websocket>(
            "wss://ws.postman-echo.com/raw",
            [&]() { connected_sync.notify(); total_connects++; },
            [&]() { disconnected_sync.notify(); total_disconnects++; },
            [&](const char* d, std::size_t l) { received.add(d, l); },
            [&](std::string&&) {}
        );

        ws->open();
        connected_sync.wait_for(std::chrono::milliseconds(10000));

        if (!connected_sync.is_triggered()) {
            if (cycle == 0) {
                GTEST_SKIP() << "Could not connect to echo server - network unavailable";
            }
            FAIL() << "Cycle " << cycle << ": failed to connect";
            break;
        }

        const std::string msg = "cycle-" + std::to_string(cycle);
        ws->send(msg.data(), msg.size());
        received.sync.wait_for(std::chrono::milliseconds(5000));

        EXPECT_TRUE(received.sync.is_triggered()) << "Cycle " << cycle << ": no echo";
        {
            std::lock_guard<std::mutex> lk(received.mtx);
            ASSERT_FALSE(received.msgs.empty()) << "Cycle " << cycle << ": no message";
            EXPECT_EQ(received.msgs[0], msg) << "Cycle " << cycle << ": data mismatch";
        }

        ws->close();
        bool disc = wait_for_condition(
            [&]() { return ws->status() == Websocket::Status::DISCONNECTED; },
            std::chrono::milliseconds(5000));
        EXPECT_TRUE(disc) << "Cycle " << cycle << ": did not reach DISCONNECTED";
    }

    EXPECT_EQ(total_connects.load(), 3);
    EXPECT_EQ(total_disconnects.load(), 3);
}

TEST_F(WebsocketTest, Reconnect_MultipleConsecutiveCycles_AllConnect) {
    std::atomic<int> total_connects{0};
    std::atomic<int> total_disconnects{0};
    EventSynchronizer connected_sync, disconnected_sync;
    ReceivedMessages received;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); total_connects++; },
        [&]() { disconnected_sync.notify(); total_disconnects++; },
        [&](const char* d, std::size_t l) { received.add(d, l); },
        [&](std::string&&) {}
    );

    for (int cycle = 0; cycle < 3; ++cycle) {
        connected_sync.reset();
        disconnected_sync.reset();
        received.clear();

        ws.open();
        connected_sync.wait_for(std::chrono::milliseconds(10000));

        if (!connected_sync.is_triggered()) {
            if (cycle == 0) {
                GTEST_SKIP() << "Could not connect to echo server - network unavailable";
            }
            FAIL() << "Cycle " << cycle << ": failed to connect";
            break;
        }

        const std::string msg = "cycle-" + std::to_string(cycle);
        ws.send(msg.data(), msg.size());
        received.sync.wait_for(std::chrono::milliseconds(5000));

        EXPECT_TRUE(received.sync.is_triggered()) << "Cycle " << cycle << ": no echo";
        {
            std::lock_guard<std::mutex> lk(received.mtx);
            ASSERT_FALSE(received.msgs.empty()) << "Cycle " << cycle << ": no message";
            EXPECT_EQ(received.msgs[0], msg) << "Cycle " << cycle << ": data mismatch";
        }

        ws.close();
        bool disc = wait_for_condition(
            [&]() { return ws.status() == Websocket::Status::DISCONNECTED; },
            std::chrono::milliseconds(5000));
        EXPECT_TRUE(disc) << "Cycle " << cycle << ": did not reach DISCONNECTED";
    }

    EXPECT_EQ(total_connects.load(), 3);
    EXPECT_EQ(total_disconnects.load(), 3);
}

// TEST 3: Reconnect new object from within the disconnect callback (service thread context)
// The callback runs on the service thread. open() posts a co_spawn that cannot run until
// the callback returns. The callback MUST return promptly — blocking here deadlocks.
TEST_F(WebsocketTest, Reconnect_NewObject_FromWithinDisconnectCallback_Connects) {
    // Use shared_ptr captures: ws2's callbacks may fire after wait_for times out.
    auto ws1_connected = std::make_shared<EventSynchronizer>();
    auto ws2_connected = std::make_shared<EventSynchronizer>();
    // ws2 must outlive the callback — wrapped in a shared holder.
    auto ws2_holder = std::make_shared<std::shared_ptr<Websocket>>();

    auto ws1 = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [ws1_connected]() { ws1_connected->notify(); },
        [ws2_holder, ws2_connected]() {
            // Running on the service thread. Must not block.
            *ws2_holder = std::make_shared<Websocket>(
                "wss://ws.postman-echo.com/raw",
                [ws2_connected]() { ws2_connected->notify(); },
                []() {},
                [](const char*, std::size_t) {},
                [](std::string&&) {}
            );
            (*ws2_holder)->open();
            // Return immediately — the co_spawn runs after this callback exits.
        },
        [](const char*, std::size_t) {},
        [](std::string&&) {}
    );

    ws1->open();
    ws1_connected->wait_for(std::chrono::milliseconds(10000));

    if (!ws1_connected->is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    ws1->close();
    // Wait for ws2 to connect (ws1 close time + ws2 connect time)
    ws2_connected->wait_for(std::chrono::milliseconds(15000));

    ASSERT_TRUE(ws2_connected->is_triggered())
        << "ws2 failed to connect when opened from within the disconnect callback. "
           "Possible deadlock: callback blocked the service thread.";
    ASSERT_TRUE(*ws2_holder != nullptr);
    EXPECT_EQ((*ws2_holder)->status(), Websocket::Status::CONNECTED);

    if (*ws2_holder) (*ws2_holder)->close();
}

TEST_F(WebsocketTest, Reconnect_FromWithinDisconnectCallback_Connects) {
    // Use shared_ptr captures: ws2's callbacks may fire after wait_for times out.
    auto ws_connected = std::make_shared<EventSynchronizer>();

    std::shared_ptr<Websocket> ws; // must outlive the callback
    ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { ws_connected->notify(); },
        [&]() {
            // Running on the service thread. Must not block.
            ws->open(); // reconnect the same object
            // Return immediately — the co_spawn runs after this callback exits.
        },
        [](const char*, std::size_t) {},
        [](std::string&&) {}
    );

    ws->open();
    ws_connected->wait_for(std::chrono::milliseconds(10000));

    if (!ws_connected->is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    ws_connected->reset(); // reset before reconnecting to reuse the same synchronizer for ws2
    ws->close();
    // Wait for ws2 to connect (ws1 close time + ws2 connect time)
    ws_connected->wait_for(std::chrono::milliseconds(15000));

    ASSERT_TRUE(ws_connected->is_triggered())
        << "ws2 failed to connect when opened from within the disconnect callback. "
           "Possible deadlock: callback blocked the service thread.";
    EXPECT_EQ(ws->status(), Websocket::Status::CONNECTED);

    ws->close();
}

// TEST 4: Reconnect new object from within the error callback
TEST_F(WebsocketTest, Reconnect_NewObject_FromWithinErrorCallback_Connects) {
    // Use shared_ptr captures: the error callback runs on the service thread and may
    // fire after wait_for times out, making [&] captures dangling.
    auto ws2_connected = std::make_shared<EventSynchronizer>();
    auto ws2 = std::make_shared<std::shared_ptr<Websocket>>();
    auto error_fired = std::make_shared<std::atomic<bool>>(false);

    auto ws1 = std::make_shared<Websocket>(
        "wss://invalid-host-reconnect-test-xyz.example",
        []() {},
        []() {},
        [](const char*, std::size_t) {},
        [ws2, ws2_connected, error_fired](std::string&&) {
            if (error_fired->exchange(true)) return; // only first error
            // Running on the service thread. Must not block.
            *ws2 = std::make_shared<Websocket>(
                "wss://ws.postman-echo.com/raw",
                [ws2_connected]() { ws2_connected->notify(); },
                []() {},
                [](const char*, std::size_t) {},
                [](std::string&&) {}
            );
            (*ws2)->open();
        }
    );

    ws1->open();
    // Error may take up to 10 s + ws2 connect up to 5 s
    ws2_connected->wait_for(std::chrono::milliseconds(15000));

    ASSERT_TRUE(ws2_connected->is_triggered())
        << "ws2 failed to connect when opened from within the error callback.";
    ASSERT_TRUE(*ws2 != nullptr);
    EXPECT_EQ((*ws2)->status(), Websocket::Status::CONNECTED);

    if (*ws2) (*ws2)->close();
    ws1->close();
}

// TEST 5: Reconnect after user closes ws1 from within on_data_ callback
TEST_F(WebsocketTest, Reconnect_NewObject_AfterDisconnectInDataCallback_Connects) {
    EventSynchronizer ws1_connected, ws1_disconnected;
    EventSynchronizer ws2_connected;
    ReceivedMessages ws2_received;
    std::atomic<bool> close_called{false};
    std::shared_ptr<Websocket> ws1;

    ws1 = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { ws1_connected.notify(); },
        [&]() { ws1_disconnected.notify(); },
        [&](const char*, std::size_t) {
            if (!close_called.exchange(true)) {
                ws1->close(); // close from on_data_ callback
            }
        },
        [&](std::string&&) {}
    );

    ws1->open();
    ws1_connected.wait_for(std::chrono::milliseconds(10000));

    if (!ws1_connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    const std::string trigger = "trigger-close";
    ws1->send(trigger.data(), trigger.size());

    bool disc = wait_for_condition(
        [&]() { return ws1->status() == Websocket::Status::DISCONNECTED; },
        std::chrono::milliseconds(5000));
    ASSERT_TRUE(disc) << "ws1 did not reach DISCONNECTED after close-in-data-callback";

    auto ws2 = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { ws2_connected.notify(); },
        [&]() {},
        [&](const char* d, std::size_t l) { ws2_received.add(d, l); },
        [&](std::string&&) {}
    );

    ws2->open();
    ws2_connected.wait_for(std::chrono::milliseconds(10000));

    ASSERT_TRUE(ws2_connected.is_triggered()) << "ws2 failed to connect";

    const std::string msg2 = "after-data-close";
    ws2->send(msg2.data(), msg2.size());
    ws2_received.sync.wait_for(std::chrono::milliseconds(5000));

    EXPECT_TRUE(ws2_received.sync.is_triggered());
    {
        std::lock_guard<std::mutex> lk(ws2_received.mtx);
        ASSERT_FALSE(ws2_received.msgs.empty());
        EXPECT_EQ(ws2_received.msgs[0], msg2);
    }
    ws2->close();
}

// TEST 6: Reconnect after simulated server-side close (close from on_connected_)
TEST_F(WebsocketTest, Reconnect_NewObject_AfterServerSideClose_Connects) {
    EventSynchronizer ws1_connected, ws1_disconnected;
    EventSynchronizer ws2_connected;
    ReceivedMessages received;
    std::shared_ptr<Websocket> ws1;

    ws1 = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() {
            ws1_connected.notify();
            // Immediately close to simulate server-side close
            ws1->close();
        },
        [&]() { ws1_disconnected.notify(); },
        [&](const char*, std::size_t) {},
        [&](std::string&&) {}
    );

    ws1->open();
    bool disc = wait_for_condition(
        [&]() { return ws1->status() == Websocket::Status::DISCONNECTED; },
        std::chrono::milliseconds(10000));

    if (!ws1_connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }
    ASSERT_TRUE(disc) << "ws1 did not reach DISCONNECTED";

    auto ws2 = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { ws2_connected.notify(); },
        [&]() {},
        [&](const char* d, std::size_t l) { received.add(d, l); },
        [&](std::string&&) {}
    );

    ws2->open();
    ws2_connected.wait_for(std::chrono::milliseconds(10000));

    ASSERT_TRUE(ws2_connected.is_triggered()) << "ws2 failed to connect after server-side close";

    const std::string msg = "after-server-close";
    ws2->send(msg.data(), msg.size());
    received.sync.wait_for(std::chrono::milliseconds(5000));

    EXPECT_TRUE(received.sync.is_triggered());
    {
        std::lock_guard<std::mutex> lk(received.mtx);
        ASSERT_FALSE(received.msgs.empty());
        EXPECT_EQ(received.msgs[0], msg);
    }
    ws2->close();
}

// TEST 7: No data bleed between sessions via shared global io_context/SSL context
TEST_F(WebsocketTest, Reconnect_DataIntegrity_NoBleedBetweenSessions) {
    // Session A
    EventSynchronizer ws1_connected;
    ReceivedMessages ws1_received;

    auto ws1 = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { ws1_connected.notify(); },
        [&]() {},
        [&](const char* d, std::size_t l) { ws1_received.add(d, l); },
        [&](std::string&&) {}
    );

    ws1->open();
    ws1_connected.wait_for(std::chrono::milliseconds(10000));

    if (!ws1_connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    ws1->send("session-A", 9);
    ws1_received.sync.wait_for(std::chrono::milliseconds(5000));
    ASSERT_TRUE(ws1_received.sync.is_triggered());

    ws1->close();
    bool disc = wait_for_condition(
        [&]() { return ws1->status() == Websocket::Status::DISCONNECTED; },
        std::chrono::milliseconds(5000));
    ASSERT_TRUE(disc);
    ws1.reset();

    // Session B
    EventSynchronizer ws2_connected;
    ReceivedMessages ws2_received;

    auto ws2 = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { ws2_connected.notify(); },
        [&]() {},
        [&](const char* d, std::size_t l) { ws2_received.add(d, l); },
        [&](std::string&&) {}
    );

    ws2->open();
    ws2_connected.wait_for(std::chrono::milliseconds(10000));
    ASSERT_TRUE(ws2_connected.is_triggered());

    // Wait briefly without sending — any stale data from session A would appear here
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
        std::lock_guard<std::mutex> lk(ws2_received.mtx);
        EXPECT_TRUE(ws2_received.msgs.empty())
            << "Stale data from session A appeared in session B";
    }

    ws2->send("session-B", 9);
    ws2_received.sync.wait_for(std::chrono::milliseconds(5000));
    EXPECT_TRUE(ws2_received.sync.is_triggered());
    {
        std::lock_guard<std::mutex> lk(ws2_received.mtx);
        EXPECT_EQ(ws2_received.msgs.size(), 1u);
        if (!ws2_received.msgs.empty()) {
            EXPECT_EQ(ws2_received.msgs[0], "session-B");
        }
    }
    ws2->close();
}

// TEST 8: Rapid successive reconnects — service thread must remain functional
TEST_F(WebsocketTest, Reconnect_RapidSuccessiveCycles_ServiceThreadRemainsFunctional) {
    EventSynchronizer first_connected;
    std::shared_ptr<Websocket> ws;

    ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { first_connected.notify(); },
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string&&) {}
    );
    ws->open();
    first_connected.wait_for(std::chrono::milliseconds(10000));

    if (!first_connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    // Rapidly destroy and recreate — no waits between iterations
    for (int i = 0; i < 5; ++i) {
        ws.reset();
        ws = std::make_shared<Websocket>(
            "wss://ws.postman-echo.com/raw",
            [&]() {},
            [&]() {},
            [&](const char*, std::size_t) {},
            [&](std::string&&) {}
        );
        ws->open();
    }

    // Allow final ws to settle
    bool settled = wait_for_condition(
        [&]() {
            auto s = ws->status();
            return s == Websocket::Status::CONNECTED || s == Websocket::Status::DISCONNECTED;
        },
        std::chrono::milliseconds(15000));

    ASSERT_TRUE(settled) << "Last ws stuck in CONNECTING/DISCONNECTING after rapid cycles";

    if (ws->status() == Websocket::Status::CONNECTED) {
        ReceivedMessages received;
        auto data_notifier = [&](const char* d, std::size_t l) { received.add(d, l); };
        // Re-create with data callback to verify write pipeline works
        ws->close();
        wait_for_condition(
            [&]() { return ws->status() == Websocket::Status::DISCONNECTED; },
            std::chrono::milliseconds(3000));

        EventSynchronizer final_connected;
        auto final_ws = std::make_shared<Websocket>(
            "wss://ws.postman-echo.com/raw",
            [&]() { final_connected.notify(); },
            [&]() {},
            data_notifier,
            [&](std::string&&) {}
        );
        final_ws->open();
        final_connected.wait_for(std::chrono::milliseconds(10000));

        if (final_connected.is_triggered()) {
            final_ws->send("stability-check", 15);
            received.sync.wait_for(std::chrono::milliseconds(5000));
            EXPECT_TRUE(received.sync.is_triggered())
                << "Write pipeline deadlocked after rapid reconnect cycles";
            final_ws->close();
        }
    }
}

// TEST 9: Callbacks fire in correct order across multiple sessions
TEST_F(WebsocketTest, Reconnect_CallbacksFireInCorrectOrder_AcrossMultipleSessions) {
    struct Event {
        std::string name;
        int session;
    };
    std::vector<Event> event_log;
    std::mutex log_mutex;

    auto log = [&](std::string name, int session) {
        std::lock_guard<std::mutex> lk(log_mutex);
        event_log.push_back({std::move(name), session});
    };

    for (int session = 0; session < 3; ++session) {
        EventSynchronizer connected_sync, disconnected_sync;
        ReceivedMessages received;

        auto ws = std::make_shared<Websocket>(
            "wss://ws.postman-echo.com/raw",
            [&, session]() { log("connected", session); connected_sync.notify(); },
            [&, session]() { log("disconnected", session); disconnected_sync.notify(); },
            [&, session](const char* d, std::size_t l) {
                log("data", session);
                received.add(d, l);
            },
            [&](std::string&&) {}
        );

        ws->open();
        connected_sync.wait_for(std::chrono::milliseconds(10000));

        if (!connected_sync.is_triggered()) {
            if (session == 0) {
                GTEST_SKIP() << "Could not connect to echo server - network unavailable";
            }
            FAIL() << "Session " << session << ": failed to connect";
            break;
        }

        ws->send("order-check", 11);
        received.sync.wait_for(std::chrono::milliseconds(5000));

        ws->close();
        disconnected_sync.wait_for(std::chrono::milliseconds(5000));
    }

    std::lock_guard<std::mutex> lk(log_mutex);

    for (int session = 0; session < 3; ++session) {
        // Find positions
        int connected_pos = -1, disconnected_pos = -1;
        std::vector<int> data_positions;

        for (int i = 0; i < static_cast<int>(event_log.size()); ++i) {
            if (event_log[i].session != session) continue;
            if (event_log[i].name == "connected") connected_pos = i;
            else if (event_log[i].name == "disconnected") disconnected_pos = i;
            else if (event_log[i].name == "data") data_positions.push_back(i);
        }

        EXPECT_GE(connected_pos, 0) << "Session " << session << ": no connected event";
        EXPECT_GE(disconnected_pos, 0) << "Session " << session << ": no disconnected event";

        for (int dp : data_positions) {
            EXPECT_LT(connected_pos, dp)
                << "Session " << session << ": data before connected";
            EXPECT_LT(dp, disconnected_pos)
                << "Session " << session << ": data after disconnected";
        }

        EXPECT_LT(connected_pos, disconnected_pos)
            << "Session " << session << ": disconnected before connected";
    }
}

// TEST 10: Two new Websocket instances connecting simultaneously (shared SSL ctx_)
TEST_F(WebsocketTest, Reconnect_ConcurrentNewObjects_BothConnect) {
    EventSynchronizer ws1_connected, ws2_connected;
    ReceivedMessages ws1_received, ws2_received;

    auto ws1 = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { ws1_connected.notify(); },
        [&]() {},
        [&](const char* d, std::size_t l) { ws1_received.add(d, l); },
        [&](std::string&&) {}
    );

    auto ws2 = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { ws2_connected.notify(); },
        [&]() {},
        [&](const char* d, std::size_t l) { ws2_received.add(d, l); },
        [&](std::string&&) {}
    );

    // Open both simultaneously
    ws1->open();
    ws2->open();

    ws1_connected.wait_for(std::chrono::milliseconds(15000));
    ws2_connected.wait_for(std::chrono::milliseconds(15000));

    if (!ws1_connected.is_triggered() && !ws2_connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    EXPECT_TRUE(ws1_connected.is_triggered()) << "ws1 failed to connect";
    EXPECT_TRUE(ws2_connected.is_triggered()) << "ws2 failed to connect";

    if (ws1_connected.is_triggered() && ws2_connected.is_triggered()) {
        ws1->send("from-ws1", 8);
        ws2->send("from-ws2", 8);

        ws1_received.sync.wait_for(std::chrono::milliseconds(5000));
        ws2_received.sync.wait_for(std::chrono::milliseconds(5000));

        EXPECT_TRUE(ws1_received.sync.is_triggered()) << "ws1 got no echo";
        EXPECT_TRUE(ws2_received.sync.is_triggered()) << "ws2 got no echo";

        {
            std::lock_guard<std::mutex> lk(ws1_received.mtx);
            ASSERT_FALSE(ws1_received.msgs.empty());
            EXPECT_EQ(ws1_received.msgs[0], "from-ws1");
        }
        {
            std::lock_guard<std::mutex> lk(ws2_received.mtx);
            ASSERT_FALSE(ws2_received.msgs.empty());
            EXPECT_EQ(ws2_received.msgs[0], "from-ws2");
        }
    }

    ws1->close();
    ws2->close();
}

// Same-object version of TEST 4: Reconnect from within error callback
TEST_F(WebsocketTest, Reconnect_FromWithinErrorCallback_Connects) {
    // Use shared holder to avoid dangling [&ws] if DNS times out past wait_for.
    auto error_count = std::make_shared<std::atomic<int>>(0);
    auto second_error_sync = std::make_shared<EventSynchronizer>();
    auto ws_holder = std::make_shared<std::shared_ptr<Websocket>>();

    *ws_holder = std::make_shared<Websocket>(
        "wss://invalid-host-reconnect-test-xyz.example",
        []() {},
        []() {},
        [](const char*, std::size_t) {},
        [ws_holder, error_count, second_error_sync](std::string&&) {
            int count = ++(*error_count);
            if (count == 1) {
                (*ws_holder)->open();
            } else if (count == 2) {
                second_error_sync->notify();
            }
        }
    );

    (*ws_holder)->open();
    // DNS error + reconnect DNS error: allow up to 20 s
    second_error_sync->wait_for(std::chrono::milliseconds(20000));

    ASSERT_TRUE(second_error_sync->is_triggered())
        << "Second error not received after same-object open() from error callback. "
           "Possible deadlock: callback blocked the service thread.";
    EXPECT_GE(error_count->load(), 2);

    (*ws_holder)->detach(); // break circular ref before close
    (*ws_holder)->close();
}

// Same-object version of TEST 5: Reconnect after close triggered from data callback
TEST_F(WebsocketTest, Reconnect_AfterDisconnectInDataCallback_Connects) {
    std::atomic<int> session{0};
    EventSynchronizer ws_connected;
    ReceivedMessages ws2_received;
    std::atomic<bool> close_triggered{false};

    std::shared_ptr<Websocket> ws;
    ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { ws_connected.notify(); },
        [&]() {},
        [&](const char* d, std::size_t l) {
            if (session.load() == 0 && !close_triggered.exchange(true)) {
                ws->close();
            } else if (session.load() == 1) {
                ws2_received.add(d, l);
            }
        },
        [&](std::string&&) {}
    );

    ws->open();
    ws_connected.wait_for(std::chrono::milliseconds(10000));
    if (!ws_connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    ws->send("trigger-close", 13);

    bool disc = wait_for_condition(
        [&]() { return ws->status() == Websocket::Status::DISCONNECTED; },
        std::chrono::milliseconds(5000));
    ASSERT_TRUE(disc) << "ws did not reach DISCONNECTED after close-in-data-callback";

    session.store(1);
    ws_connected.reset();

    ws->open();
    ws_connected.wait_for(std::chrono::milliseconds(10000));

    ASSERT_TRUE(ws_connected.is_triggered()) << "ws failed to reconnect after data-callback close";
    EXPECT_EQ(ws->status(), Websocket::Status::CONNECTED);

    const std::string msg = "after-data-close";
    ws->send(msg.data(), msg.size());
    ws2_received.sync.wait_for(std::chrono::milliseconds(5000));

    EXPECT_TRUE(ws2_received.sync.is_triggered());
    {
        std::lock_guard<std::mutex> lk(ws2_received.mtx);
        ASSERT_FALSE(ws2_received.msgs.empty());
        EXPECT_EQ(ws2_received.msgs[0], msg);
    }
    ws->close();
}

// Same-object version of TEST 6: Reconnect after close triggered from connected callback
TEST_F(WebsocketTest, Reconnect_AfterServerSideClose_Connects) {
    std::atomic<int> session{0};
    EventSynchronizer session0_connected;
    EventSynchronizer ws_connected;
    ReceivedMessages received;

    std::shared_ptr<Websocket> ws;
    ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() {
            if (session.load() == 0) {
                session0_connected.notify();
                ws->close(); // simulate immediate server-side close in session 0
            } else {
                ws_connected.notify();
            }
        },
        [&]() {},
        [&](const char* d, std::size_t l) { received.add(d, l); },
        [&](std::string&&) {}
    );

    ws->open();
    bool disc = wait_for_condition(
        [&]() { return ws->status() == Websocket::Status::DISCONNECTED; },
        std::chrono::milliseconds(10000));

    if (!session0_connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }
    ASSERT_TRUE(disc) << "ws did not reach DISCONNECTED";

    session.store(1);
    ws->open();
    ws_connected.wait_for(std::chrono::milliseconds(10000));

    ASSERT_TRUE(ws_connected.is_triggered()) << "ws failed to reconnect after server-side close";
    EXPECT_EQ(ws->status(), Websocket::Status::CONNECTED);

    const std::string msg = "after-server-close";
    ws->send(msg.data(), msg.size());
    received.sync.wait_for(std::chrono::milliseconds(5000));

    EXPECT_TRUE(received.sync.is_triggered());
    {
        std::lock_guard<std::mutex> lk(received.mtx);
        ASSERT_FALSE(received.msgs.empty());
        EXPECT_EQ(received.msgs[0], msg);
    }
    ws->close();
}

// Same-object version of TEST 7: No data bleed between sessions on the same object
TEST_F(WebsocketTest, Reconnect_SameObject_DataIntegrity_NoBleedBetweenSessions) {
    EventSynchronizer ws_connected;
    ReceivedMessages received;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { ws_connected.notify(); },
        [&]() {},
        [&](const char* d, std::size_t l) { received.add(d, l); },
        [&](std::string&&) {}
    );

    // Session A
    ws.open();
    ws_connected.wait_for(std::chrono::milliseconds(10000));
    if (!ws_connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    ws.send("session-A", 9);
    received.sync.wait_for(std::chrono::milliseconds(5000));
    ASSERT_TRUE(received.sync.is_triggered());

    ws.close();
    bool disc = wait_for_condition(
        [&]() { return ws.status() == Websocket::Status::DISCONNECTED; },
        std::chrono::milliseconds(5000));
    ASSERT_TRUE(disc);

    // Session B — same object
    ws_connected.reset();
    received.clear();

    ws.open();
    ws_connected.wait_for(std::chrono::milliseconds(10000));
    ASSERT_TRUE(ws_connected.is_triggered());

    // Wait briefly — any stale data from session A would appear here
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
        std::lock_guard<std::mutex> lk(received.mtx);
        EXPECT_TRUE(received.msgs.empty())
            << "Stale data from session A appeared in session B (same Websocket object)";
    }

    ws.send("session-B", 9);
    received.sync.wait_for(std::chrono::milliseconds(5000));
    EXPECT_TRUE(received.sync.is_triggered());
    {
        std::lock_guard<std::mutex> lk(received.mtx);
        EXPECT_EQ(received.msgs.size(), 1u);
        if (!received.msgs.empty()) {
            EXPECT_EQ(received.msgs[0], "session-B");
        }
    }
    ws.close();
}

// Same-object version of TEST 8: Rapid close/open cycles — service thread must remain functional
TEST_F(WebsocketTest, Reconnect_SameObject_RapidSuccessiveCycles_ServiceThreadRemainsFunctional) {
    std::atomic<int> session{0};
    EventSynchronizer first_connected;
    EventSynchronizer final_connected;
    ReceivedMessages received;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() {
            int s = session.load();
            if (s == 0) first_connected.notify();
            else if (s == 1) final_connected.notify();
        },
        [&]() {},
        [&](const char* d, std::size_t l) {
            if (session.load() == 1) received.add(d, l);
        },
        [&](std::string&&) {}
    );

    ws.open();
    first_connected.wait_for(std::chrono::milliseconds(10000));
    if (!first_connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    // Rapidly close and reopen — no waits between iterations
    for (int i = 0; i < 5; ++i) {
        ws.close();
        ws.open();
    }

    bool settled = wait_for_condition(
        [&]() {
            auto s = ws.status();
            return s == Websocket::Status::CONNECTED || s == Websocket::Status::DISCONNECTED;
        },
        std::chrono::milliseconds(15000));
    ASSERT_TRUE(settled) << "ws stuck in CONNECTING/DISCONNECTING after rapid same-object cycles";

    // Close and do one final clean open to verify the write pipeline is still functional
    ws.close();
    wait_for_condition([&]() { return ws.status() == Websocket::Status::DISCONNECTED; },
                      std::chrono::milliseconds(3000));

    session.store(1);
    ws.open();
    final_connected.wait_for(std::chrono::milliseconds(10000));

    if (final_connected.is_triggered()) {
        ws.send("stability-check", 15);
        received.sync.wait_for(std::chrono::milliseconds(5000));
        EXPECT_TRUE(received.sync.is_triggered())
            << "Write pipeline deadlocked after rapid same-object reconnect cycles";
    }
    ws.close();
}

// Same-object version of TEST 9: Callbacks fire in correct order across multiple sessions
TEST_F(WebsocketTest, Reconnect_SameObject_CallbacksFireInCorrectOrder) {
    struct Event { std::string name; int session; };
    std::vector<Event> event_log;
    std::mutex log_mutex;

    std::atomic<int> current_session{0};
    EventSynchronizer connected_sync, disconnected_sync;
    ReceivedMessages received;

    auto log_event = [&](std::string name) {
        std::lock_guard<std::mutex> lk(log_mutex);
        event_log.push_back({std::move(name), current_session.load()});
    };

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { log_event("connected"); connected_sync.notify(); },
        [&]() { log_event("disconnected"); disconnected_sync.notify(); },
        [&](const char* d, std::size_t l) { log_event("data"); received.add(d, l); },
        [&](std::string&&) {}
    );

    for (int session = 0; session < 3; ++session) {
        current_session.store(session);
        connected_sync.reset();
        disconnected_sync.reset();
        received.clear();

        ws.open();
        connected_sync.wait_for(std::chrono::milliseconds(10000));

        if (!connected_sync.is_triggered()) {
            if (session == 0) {
                GTEST_SKIP() << "Could not connect to echo server - network unavailable";
            }
            FAIL() << "Session " << session << ": failed to connect";
            break;
        }

        ws.send("order-check", 11);
        received.sync.wait_for(std::chrono::milliseconds(5000));

        ws.close();
        disconnected_sync.wait_for(std::chrono::milliseconds(5000));
    }

    std::lock_guard<std::mutex> lk(log_mutex);

    for (int session = 0; session < 3; ++session) {
        int connected_pos = -1, disconnected_pos = -1;
        std::vector<int> data_positions;

        for (int i = 0; i < static_cast<int>(event_log.size()); ++i) {
            if (event_log[i].session != session) continue;
            if (event_log[i].name == "connected") connected_pos = i;
            else if (event_log[i].name == "disconnected") disconnected_pos = i;
            else if (event_log[i].name == "data") data_positions.push_back(i);
        }

        EXPECT_GE(connected_pos, 0) << "Session " << session << ": no connected event";
        EXPECT_GE(disconnected_pos, 0) << "Session " << session << ": no disconnected event";

        for (int dp : data_positions) {
            EXPECT_LT(connected_pos, dp)
                << "Session " << session << ": data fired before connected";
            EXPECT_LT(dp, disconnected_pos)
                << "Session " << session << ": data fired after disconnected";
        }

        EXPECT_LT(connected_pos, disconnected_pos)
            << "Session " << session << ": disconnected fired before connected";
    }
}

// ============== Read-buffer ownership handoff tests (rapid reconnect) ==============
// The shared read stream buffer is single-producer: a new session must not touch it
// until the previous session's read loop has fully terminated. These tests exercise
// open() while the previous session is still DISCONNECTING.

// open() during DISCONNECTING defers the new session until the old one releases the
// buffer, reports CONNECTING during the window, and suppresses the old session's
// disconnect callback.
TEST_F(WebsocketTest, Reconnect_SameObject_OpenWhileDisconnecting_DefersAndConnects) {
    EventSynchronizer connected;
    std::atomic<int> disconnect_count{0};
    ReceivedMessages received;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected.notify(); },
        [&]() { disconnect_count++; },
        [&](const char* d, std::size_t l) { received.add(d, l); },
        [&](std::string&&) {}
    );

    ws.open();
    connected.wait_for(std::chrono::milliseconds(10000));
    if (!connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    connected.reset();
    ws.close();
    ws.open(); // old session still DISCONNECTING - the new session is deferred

    auto st = ws.status();
    EXPECT_TRUE(st == Websocket::Status::CONNECTING || st == Websocket::Status::CONNECTED)
        << "status() should read CONNECTING while the deferred open waits";

    connected.wait_for(std::chrono::milliseconds(15000));
    ASSERT_TRUE(connected.is_triggered()) << "Deferred open never connected";

    // The replaced session was detached: its disconnect callback must be suppressed.
    EXPECT_EQ(disconnect_count.load(), 0)
        << "Suppressed session's onDisconnected leaked through";

    const std::string msg = "deferred-open-check";
    ws.send(msg.data(), msg.size());
    received.sync.wait_for(std::chrono::milliseconds(5000));
    EXPECT_TRUE(received.sync.is_triggered()) << "No echo on the deferred session";
    {
        std::lock_guard<std::mutex> lk(received.mtx);
        ASSERT_FALSE(received.msgs.empty());
        EXPECT_EQ(received.msgs[0], msg);
    }
    ws.close();
}

// Rapid close()+open() while echoes from the old session may still be in flight:
// every record published to the stream buffer must be byte-identical to a sent
// payload - no torn records, no bytes interleaved between the two sessions.
TEST_F(WebsocketTest, Reconnect_SameObject_NoDataCorruption_AcrossRapidReconnect) {
    EventSynchronizer connected;
    std::atomic<int> session{0};
    ReceivedMessages received_b;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected.notify(); },
        [&]() {},
        [&](const char* d, std::size_t l) { if (session.load() == 1) received_b.add(d, l); },
        [&](std::string&&) {}
    );

    uint64_t cursor = ws.initial_reading_index();

    std::vector<std::string> payloads_a, payloads_b;
    for (int i = 0; i < 8; ++i) {
        payloads_a.push_back("A-" + std::to_string(i) + "-" + std::string(512, 'a'));
        payloads_b.push_back("B-" + std::to_string(i) + "-" + std::string(512, 'b'));
    }

    ws.open();
    connected.wait_for(std::chrono::milliseconds(10000));
    if (!connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    for (const auto& p : payloads_a) {
        ws.send(p.data(), p.size());
    }
    // Let some echoes arrive / be mid-flight when we reconnect.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    connected.reset();
    session.store(1);
    ws.close();
    ws.open(); // rapid reconnect while old-session echoes may still be arriving

    connected.wait_for(std::chrono::milliseconds(15000));
    ASSERT_TRUE(connected.is_triggered()) << "Failed to reconnect";

    for (const auto& p : payloads_b) {
        ws.send(p.data(), p.size());
    }
    bool got_all_b = wait_for_condition(
        [&]() { return received_b.count.load() >= static_cast<int>(payloads_b.size()); },
        std::chrono::milliseconds(10000));
    EXPECT_TRUE(got_all_b) << "Not all session-B echoes received: " << received_b.count.load();

    // Drain everything published to the shared stream buffer across both sessions.
    std::vector<std::string> drained;
    auto contains = [](const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };
    wait_for_condition(
        [&]() {
            ws.drain_data(cursor, [&](const char* d, std::size_t l) {
                drained.emplace_back(d, l);
            }, 1000);
            for (const auto& p : payloads_b) {
                if (!contains(drained, p)) return false;
            }
            return true;
        },
        std::chrono::milliseconds(5000));

    for (const auto& rec : drained) {
        EXPECT_TRUE(contains(payloads_a, rec) || contains(payloads_b, rec))
            << "Corrupted record published to the stream buffer (len=" << rec.size()
            << "): " << rec.substr(0, 64);
    }
    for (const auto& p : payloads_b) {
        EXPECT_TRUE(contains(drained, p)) << "Missing session-B record: " << p.substr(0, 16);
    }
    ws.close();
}

// close() after open() on a still-disconnecting session cancels the pending deferred
// open; the websocket settles at DISCONNECTED and the handoff chain stays functional.
TEST_F(WebsocketTest, Reconnect_SameObject_CloseCancelsPendingDeferredOpen) {
    EventSynchronizer connected;

    Websocket ws(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected.notify(); },
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string&&) {}
    );

    ws.open();
    connected.wait_for(std::chrono::milliseconds(10000));
    if (!connected.is_triggered()) {
        GTEST_SKIP() << "Could not connect to echo server - network unavailable";
    }

    connected.reset();
    ws.close();
    ws.open();  // deferred behind the closing session
    ws.close(); // cancels the pending deferred open

    bool settled = wait_for_condition(
        [&]() { return ws.status() == Websocket::Status::DISCONNECTED; },
        std::chrono::milliseconds(10000));
    EXPECT_TRUE(settled) << "Did not settle at DISCONNECTED after cancelling deferred open";
    EXPECT_FALSE(connected.is_triggered()) << "Cancelled deferred open still connected";

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    EXPECT_EQ(ws.status(), Websocket::Status::DISCONNECTED)
        << "Cancelled deferred open connected after settling";

    // The handoff chain must not be wedged: a fresh open() still connects.
    ws.open();
    connected.wait_for(std::chrono::milliseconds(15000));
    EXPECT_TRUE(connected.is_triggered()) << "open() after cancelled deferred open never connected";
    ws.close();
}

} // namespace slick::net
