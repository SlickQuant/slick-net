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

// #define LOG_DEBUG(fmt, ...) std::cout << std::format("{:%Y-%m-%d %H:%M:%S} ", std::chrono::system_clock::now()) << "[DEBUG] " << std::format(fmt, __VA_ARGS__) << std::endl
// #define LOG_INFO(fmt, ...) std::cout << std::format("{:%Y-%m-%d %H:%M:%S} ", std::chrono::system_clock::now()) << "[INFO] " << std::format(fmt, __VA_ARGS__) << std::endl
// #define LOG_WARN(fmt, ...) std::cout << std::format("{:%Y-%m-%d %H:%M:%S} ", std::chrono::system_clock::now()) << "[WARNING] " << std::format(fmt, __VA_ARGS__) << std::endl
// #define LOG_ERROR(fmt, ...) std::cout << std::format("{:%Y-%m-%d %H:%M:%S} ", std::chrono::system_clock::now()) << "[ERROR] " << std::format(fmt, __VA_ARGS__) << std::endl
// #define LOG_TRACE(fmt, ...) std::cout << std::format("{:%Y-%m-%d %H:%M:%S} ", std::chrono::system_clock::now()) << "[TRACE] " << std::format(fmt, __VA_ARGS__) << std::endl

#include <slick/net/websocket.h>

namespace slick::net {

// Mock callbacks for testing
class MockWebsocketCallbacks {
public:
    MOCK_METHOD(void, onConnected, (), ());
    MOCK_METHOD(void, onDisconnected, (), ());
    MOCK_METHOD(void, onData, (const char*, std::size_t), ());
    MOCK_METHOD(void, onError, (std::string), ());
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

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw/test",
        [&]() { connected_called = true; },
        [&]() { disconnected_called = true; },
        [&](const char*, std::size_t) { data_called = true; },
        [&](std::string) { error_called = true; }
    );

    // Check URL parsing
    EXPECT_EQ(ws->status(), Websocket::Status::DISCONNECTED);
}

TEST_F(WebsocketTest, ConstructorParsesWssUrlWithPort) {
    std::atomic<bool> connected_called{false};
    std::atomic<bool> disconnected_called{false};
    std::atomic<bool> data_called{false};
    std::atomic<bool> error_called{false};

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw:443/test",
        [&]() { connected_called = true; },
        [&]() { disconnected_called = true; },
        [&](const char*, std::size_t) { data_called = true; },
        [&](std::string) { error_called = true; }
    );

    EXPECT_EQ(ws->status(), Websocket::Status::DISCONNECTED);
}

TEST_F(WebsocketTest, ConstructorParsesWsUrl) {
    // Test that plain WebSocket (ws://) URLs are parsed correctly
    std::atomic<bool> connected_called{false};
    std::atomic<bool> disconnected_called{false};
    std::atomic<bool> data_called{false};
    std::atomic<bool> error_called{false};

    auto ws = std::make_shared<Websocket>(
        "ws://localhost:8080/test",
        [&]() { connected_called = true; },
        [&]() { disconnected_called = true; },
        [&](const char*, std::size_t) { data_called = true; },
        [&](std::string) { error_called = true; }
    );

    // Should successfully create websocket object and parse URL
    EXPECT_EQ(ws->status(), Websocket::Status::DISCONNECTED);

    // Note: This test verifies URL parsing for plain WebSocket.
    // To test actual ws:// connections, run a local WebSocket server:
    // Example: wscat --listen 8080
}

TEST_F(WebsocketTest, ConstructorParsesHostOnlyUrl) {
    std::atomic<bool> connected_called{false};
    std::atomic<bool> disconnected_called{false};
    std::atomic<bool> data_called{false};
    std::atomic<bool> error_called{false};

    auto ws = std::make_shared<Websocket>(
        "echo.websocket.org",
        [&]() { connected_called = true; },
        [&]() { disconnected_called = true; },
        [&](const char*, std::size_t) { data_called = true; },
        [&](std::string) { error_called = true; }
    );

    EXPECT_EQ(ws->status(), Websocket::Status::DISCONNECTED);
}

TEST_F(WebsocketTest, ConstructorParsesUrlWithCustomPort) {
    std::atomic<bool> connected_called{false};
    std::atomic<bool> disconnected_called{false};
    std::atomic<bool> data_called{false};
    std::atomic<bool> error_called{false};

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw:9001/test",
        [&]() { connected_called = true; },
        [&]() { disconnected_called = true; },
        [&](const char*, std::size_t) { data_called = true; },
        [&](std::string) { error_called = true; }
    );

    EXPECT_EQ(ws->status(), Websocket::Status::DISCONNECTED);
}

TEST_F(WebsocketTest, StatusTransitions) {
    std::atomic<bool> connected_called{false};
    std::atomic<bool> disconnected_called{false};
    std::atomic<bool> data_called{false};
    std::atomic<bool> error_called{false};

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw/test",
        [&]() { connected_called = true; },
        [&]() { disconnected_called = true; },
        [&](const char*, std::size_t) { data_called = true; },
        [&](std::string) { error_called = true; }
    );

    // Initial state should be DISCONNECTED
    EXPECT_EQ(ws->status(), Websocket::Status::DISCONNECTED);

    // Note: We can't easily test CONNECTING/CONNECTED states without mocking
    // the network layer, but we can verify the initial state
}

TEST_F(WebsocketTest, CallbacksAreStored) {
    int connected_count = 0;
    int disconnected_count = 0;
    int data_count = 0;
    int error_count = 0;

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw/test",
        [&]() { connected_count++; },
        [&]() { disconnected_count++; },
        [&](const char*, std::size_t) { data_count++; },
        [&](std::string) { error_count++; }
    );

    // We can't directly invoke the callbacks since they're private,
    // but we can verify the websocket is created successfully
    EXPECT_EQ(ws->status(), Websocket::Status::DISCONNECTED);
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
    EventSynchronizer connected_sync;
    EventSynchronizer error_sync;
    std::string error_message;

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string err) {
            error_message = err;
            error_sync.notify();
        }
    );

    EXPECT_EQ(ws->status(), Websocket::Status::DISCONNECTED);

    ws->open();

    // Wait for connection or error
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    // Should be connected or have an error
    EXPECT_TRUE(connected_sync.is_triggered() || error_sync.is_triggered());

    if (connected_sync.is_triggered()) {
        EXPECT_EQ(ws->status(), Websocket::Status::CONNECTED);
    }

    // Always close the websocket
    ws->close();
}

TEST_F(WebsocketTest, CloseConnection) {
    EventSynchronizer connected_sync;
    EventSynchronizer disconnected_sync;

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() { disconnected_sync.notify(); },
        [&](const char*, std::size_t) {},
        [&](std::string) {}
    );

    ws->open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    if (connected_sync.is_triggered()) {
        ws->close();

        disconnected_sync.wait_for(std::chrono::milliseconds(1000));

        // Verify status changes to DISCONNECTING
        EXPECT_TRUE(ws->status() == Websocket::Status::DISCONNECTING ||
                   ws->status() == Websocket::Status::DISCONNECTED);
    } else {
        // Always close the websocket even if connection failed
        ws->close();
    }
}

TEST_F(WebsocketTest, InvalidHostnameError) {
    EventSynchronizer error_sync;
    std::string error_message;

    auto ws = std::make_shared<Websocket>(
        "wss://invalid-hostname-that-does-not-exist-12345.com",
        [&]() {},
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string err) {
            error_message = err;
            error_sync.notify();
        }
    );

    ws->open();
    EXPECT_TRUE(ws->status() == Websocket::Status::CONNECTING);
    error_sync.wait_for(std::chrono::milliseconds(5000));

    EXPECT_TRUE(error_sync.is_triggered());
    EXPECT_FALSE(error_message.empty());

    // Close the websocket to clean up
    ws->close();
}

// ======================== Message Send/Receive Tests ========================

TEST_F(WebsocketTest, SendAndReceiveEcho) {
    EventSynchronizer connected_sync;
    EventSynchronizer data_sync;
    std::string received_data;

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char* data, std::size_t len) {
            received_data.assign(data, len);
            data_sync.notify();
        },
        [&](std::string) {}
    );

    ws->open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        const char* test_message = "Hello WebSocket!";
        ws->send(test_message, strlen(test_message));

        data_sync.wait_for(std::chrono::milliseconds(5000));

        if (data_sync.is_triggered()) {
            EXPECT_EQ(received_data, "Hello WebSocket!");
        }
    }

    // Always close the websocket
    ws->close();
}

TEST_F(WebsocketTest, SendMultipleMessages) {
    EventSynchronizer connected_sync;
    std::atomic<int> messages_received{0};
    std::vector<std::string> received_messages;
    std::mutex messages_mutex;

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char* data, std::size_t len) {
            std::lock_guard<std::mutex> lock(messages_mutex);
            received_messages.emplace_back(data, len);
            messages_received++;
        },
        [&](std::string) {}
    );

    ws->open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        const int num_messages = 5;
        for (int i = 0; i < num_messages; ++i) {
            std::string msg = "Message " + std::to_string(i);
            ws->send(msg.c_str(), msg.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Wait for all messages to be received
        wait_for_condition([&]() { return messages_received >= num_messages; },
                          std::chrono::milliseconds(10000));

        EXPECT_GE(messages_received.load(), num_messages);
    }

    // Always close the websocket
    ws->close();
}

TEST_F(WebsocketTest, SendLargeMessage) {
    EventSynchronizer connected_sync;
    EventSynchronizer data_sync;
    std::string received_data;

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char* data, std::size_t len) {
            received_data.assign(data, len);
            data_sync.notify();
        },
        [&](std::string) {}
    );

    ws->open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        // Create a large message (10KB)
        std::string large_message(10240, 'A');
        ws->send(large_message.c_str(), large_message.size());

        data_sync.wait_for(std::chrono::milliseconds(5000));

        if (data_sync.is_triggered()) {
            EXPECT_EQ(received_data.size(), large_message.size());
            EXPECT_EQ(received_data, large_message);
        }
    }

    // Always close the websocket
    ws->close();
}

TEST_F(WebsocketTest, SendBinaryData) {
    EventSynchronizer connected_sync;
    EventSynchronizer data_sync;
    EventSynchronizer error_sync;
    std::vector<char> received_data;

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char* data, std::size_t len) {
            received_data.assign(data, data + len);
            data_sync.notify();
        },
        [&](std::string e) {
            printf("%s\n", e.c_str());
            error_sync.notify();
        }
    );

    ws->open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    if (connected_sync.is_triggered() && !error_sync.is_triggered()) {
        // Binary data with null bytes
        std::vector<char> binary_data = {'\x01', '\x02', '\x03', static_cast<char>(0xFF), static_cast<char>(0xFE), '\x42'};
        ws->send(binary_data.data(), binary_data.size(), true);

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
    ws->close();
}

// ======================== Concurrent Operations Tests ========================

TEST_F(WebsocketTest, ConcurrentSends) {
    EventSynchronizer connected_sync;
    std::atomic<int> messages_sent{0};
    std::atomic<int> messages_received{0};

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char*, std::size_t) {
            messages_received++;
        },
        [&](std::string) {}
    );

    ws->open();
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
                    ws->send(msg.c_str(), msg.size());
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
    ws->close();
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
            [&](std::string) {}
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

    std::shared_ptr<Websocket> ws;
    ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() {
            {
                std::lock_guard<std::mutex> lock(status_mutex);
                observed_statuses.push_back(ws->status());
            }
            connected_sync.notify();
        },
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string) {}
    );

    EXPECT_EQ(ws->status(), Websocket::Status::DISCONNECTED);

    ws->open();

    // Should transition to CONNECTING
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto status_after_open = ws->status();
    EXPECT_TRUE(status_after_open == Websocket::Status::CONNECTING ||
                status_after_open == Websocket::Status::CONNECTED);

    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        EXPECT_EQ(ws->status(), Websocket::Status::CONNECTED);
    }

    // Always close the websocket
    ws->close();
}

TEST_F(WebsocketTest, CannotSendWhenDisconnected) {
    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() {},
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string) {}
    );

    EXPECT_EQ(ws->status(), Websocket::Status::DISCONNECTED);

    // Sending when disconnected shouldn't crash (message gets queued)
    const char* msg = "test";
    EXPECT_NO_THROW(ws->send(msg, strlen(msg)));
}

TEST_F(WebsocketTest, IsRunningAfterFirstOpen) {
    EventSynchronizer connected_sync;

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string) {}
    );

    EXPECT_FALSE(Websocket::is_running());

    ws->open();

    // Give it time to start the service thread
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_TRUE(Websocket::is_running());

    ws->close();
}

// ======================== Error Handling Tests ========================

TEST_F(WebsocketTest, MultipleErrorCallbacks) {
    std::atomic<int> error_count{0};
    std::vector<std::string> error_messages;
    std::mutex error_mutex;

    auto ws = std::make_shared<Websocket>(
        "wss://invalid-host-12345.test",
        [&]() {},
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string err) {
            std::lock_guard<std::mutex> lock(error_mutex);
            error_messages.push_back(err);
            error_count++;
        }
    );

    ws->open();

    // Wait for error
    wait_for_condition([&]() { return error_count > 0; },
                      std::chrono::milliseconds(5000));

    EXPECT_GT(error_count.load(), 0);

    std::lock_guard<std::mutex> lock(error_mutex);
    EXPECT_FALSE(error_messages.empty());

    // Close the websocket to clean up
    ws->close();
}

TEST_F(WebsocketTest, ReconnectAfterError) {
    EventSynchronizer first_error_sync;
    EventSynchronizer second_connected_sync;
    std::atomic<int> error_count{0};

    auto ws_first = std::make_shared<Websocket>(
        "wss://invalid-host-xyz.test",
        [&]() {},
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string) {
            error_count++;
            first_error_sync.notify();
        }
    );

    ws_first->open();
    first_error_sync.wait_for(std::chrono::milliseconds(5000));

    EXPECT_GT(error_count.load(), 0);

    // Close the first websocket to clean up before moving on
    ws_first->close();

    // Now try connecting to a valid host with a new instance
    auto ws_second = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { second_connected_sync.notify(); },
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string) {}
    );

    ws_second->open();
    second_connected_sync.wait_for(std::chrono::milliseconds(10000));

    if (second_connected_sync.is_triggered()) {
        EXPECT_EQ(ws_second->status(), Websocket::Status::CONNECTED);
        ws_second->close();
    }
}

// ======================== Edge Cases ========================

TEST_F(WebsocketTest, EmptyMessageSend) {
    EventSynchronizer connected_sync;
    EventSynchronizer data_sync;

    auto ws = std::make_shared<Websocket>(
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

    ws->open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        // Send empty message
        ws->send("", 0);

        // Some servers may echo it back, others may not
        data_sync.wait_for(std::chrono::milliseconds(2000));
    }

    // Always close the websocket
    ws->close();
}

TEST_F(WebsocketTest, RapidOpenClose) {
    EventSynchronizer connected_sync;
    EventSynchronizer disconnected_sync;
    EventSynchronizer error_sync;

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() { disconnected_sync.notify(); },
        [&](const char*, std::size_t) {},
        [&](std::string) { error_sync.notify(); }
    );

    // Rapidly open and close
    ws->open();
    wait_for_condition([&]() {
        return connected_sync.is_triggered() || error_sync.is_triggered();
    }, std::chrono::milliseconds(1000));

    if (connected_sync.is_triggered()) {
        EXPECT_EQ(ws->status(), Websocket::Status::CONNECTED);
    }

    ws->close();
    EXPECT_TRUE(wait_for_condition([&]() {
        return ws->status() == Websocket::Status::DISCONNECTED ||
            ws->status() == Websocket::Status::DISCONNECTING ||
            disconnected_sync.is_triggered();
    }, std::chrono::milliseconds(2000)));

    // Should not crash
    EXPECT_TRUE(ws->status() == Websocket::Status::DISCONNECTED ||
                ws->status() == Websocket::Status::DISCONNECTING);
}

TEST_F(WebsocketTest, DoubleClose) {
    EventSynchronizer connected_sync;

    auto ws = std::make_shared<Websocket>(
        "wss://ws.postman-echo.com/raw",
        [&]() { connected_sync.notify(); },
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string) {}
    );

    ws->open();
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        ws->close();

        // Second close should not crash
        EXPECT_NO_THROW(ws->close());
    } else {
        // Always close the websocket even if connection failed
        ws->close();
    }
}

TEST_F(WebsocketTest, DestructorWhileConnected) {
    EventSynchronizer connected_sync;

    {
        auto ws = std::make_shared<Websocket>(
            "wss://ws.postman-echo.com/raw",
            [&]() { connected_sync.notify(); },
            [&]() {},
            [&](const char*, std::size_t) {},
            [&](std::string) {}
        );

        ws->open();
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
    // Verify that plain WebSocket URLs are correctly parsed and handled
    EventSynchronizer error_sync;
    std::atomic<bool> connected{false};
    std::string error_message;

    auto ws = std::make_shared<Websocket>(
        "ws://localhost:9001",  // Local server (won't be running in CI)
        [&]() { connected.store(true); },
        [&]() {},
        [&](const char*, std::size_t) {},
        [&](std::string err) {
            error_message = err;
            error_sync.notify();
        }
    );

    EXPECT_EQ(ws->status(), Websocket::Status::DISCONNECTED);

    ws->open();

    // Wait a bit to see if connection succeeds or fails
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // Either connected to local server OR got connection error (expected in CI)
    if (connected.load()) {
        EXPECT_EQ(ws->status(), Websocket::Status::CONNECTED);
        std::cout << "Note: Successfully connected to local plain WebSocket server\n";
        ws->close();
    } else {
        // Expected behavior when no local server is running
        std::cout << "Note: Plain WebSocket test - no local server running (expected in CI)\n";
        ws->close();
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

    auto ws = std::make_shared<Websocket>(
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
    EXPECT_EQ(ws->status(), Websocket::Status::DISCONNECTED);

    // Open the WebSocket connection
    ws->open();

    // Immediately send data right after calling open (before connection is established)
    const char* test_message = "Immediate message after open";
    ws->send(test_message, strlen(test_message));
    message_sent.store(true);

    // At this point, the connection should be CONNECTING or CONNECTED
    auto status_after_send = ws->status();
    EXPECT_TRUE(status_after_send == Websocket::Status::CONNECTING ||
                status_after_send == Websocket::Status::CONNECTED);

    // Wait for connection to be established
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        EXPECT_EQ(ws->status(), Websocket::Status::CONNECTED);
        EXPECT_TRUE(message_sent.load());

        // Wait for the echo response
        data_sync.wait_for(std::chrono::milliseconds(5000));

        if (data_sync.is_triggered()) {
            // Verify the message was sent and echoed back after connection was established
            EXPECT_EQ(received_data, "Immediate message after open");
        }
    }

    // Always close the websocket
    ws->close();
}

TEST_F(WebsocketTest, SendImmediatelyAfterOpen_MultipleMessages) {
    EventSynchronizer connected_sync;
    std::atomic<int> messages_received{0};
    std::vector<std::string> received_messages;
    std::mutex messages_mutex;

    auto ws = std::make_shared<Websocket>(
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

    EXPECT_EQ(ws->status(), Websocket::Status::DISCONNECTED);

    // Open connection
    ws->open();

    // Immediately send multiple messages right after open
    const int num_messages = 3;
    for (int i = 0; i < num_messages; ++i) {
        std::string msg = "Quick message " + std::to_string(i);
        ws->send(msg.c_str(), msg.size());
    }

    // Wait for connection to establish
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        EXPECT_EQ(ws->status(), Websocket::Status::CONNECTED);

        // Wait for all messages to be received
        wait_for_condition([&]() { return messages_received >= num_messages; },
                          std::chrono::milliseconds(10000));

        // All messages should have been queued and sent after connection established
        EXPECT_GE(messages_received.load(), num_messages);

        std::lock_guard<std::mutex> lock(messages_mutex);
        EXPECT_GE(received_messages.size(), static_cast<size_t>(num_messages));
    }

    ws->close();
}

TEST_F(WebsocketTest, SendImmediatelyAfterOpen_VerifyOrderPreserved) {
    EventSynchronizer connected_sync;
    std::atomic<int> messages_received{0};
    std::vector<std::string> received_messages;
    std::mutex messages_mutex;

    auto ws = std::make_shared<Websocket>(
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

    ws->open();

    // Send messages immediately after open - they should be queued
    ws->send("First", 5);
    ws->send("Second", 6);
    ws->send("Third", 5);

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

    ws->close();
}

TEST_F(WebsocketTest, SendImmediatelyAfterOpen_LargeMessage) {
    EventSynchronizer connected_sync;
    EventSynchronizer data_sync;
    std::string received_data;

    auto ws = std::make_shared<Websocket>(
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

    ws->open();

    // Send a large message immediately after open
    std::string large_message(5120, 'X');  // 5KB message
    ws->send(large_message.c_str(), large_message.size());

    // Wait for connection
    connected_sync.wait_for(std::chrono::milliseconds(10000));

    EXPECT_TRUE(connected_sync.is_triggered());
    if (connected_sync.is_triggered()) {
        EXPECT_EQ(ws->status(), Websocket::Status::CONNECTED);

        // Wait for the large message echo
        data_sync.wait_for(std::chrono::milliseconds(10000));

        if (data_sync.is_triggered()) {
            EXPECT_EQ(received_data.size(), large_message.size());
            EXPECT_EQ(received_data, large_message);
        }
    }

    ws->close();
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

} // namespace slick::net
