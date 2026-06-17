// Tests for Websocket<slick::dynamic_buffer<T>> backends.
#include <slick/stream_buffer.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <slick/dynamic_buffer.hpp>
#include <slick/net/websocket.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace slick::net;

// ── helpers ───────────────────────────────────────────────────────────────────

static constexpr const char* kEchoUrl   = "wss://ws.postman-echo.com/raw";
static constexpr uint64_t    kCapacity  = 1u << 20;   // 1 MiB data ring
static constexpr uint32_t    kControl   = 1u << 10;   // 1024-record control ring

template<typename Pred>
static bool wait_for(Pred pred, std::chrono::seconds timeout = std::chrono::seconds(10)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

struct SlickBufferWebsocketTest : ::testing::Test {
    void TearDown() override {
        // All Websocket<X>::shutdown() share the same io_context.
        Websocket<boost::beast::flat_buffer>::shutdown();
    }
};

// ── 1. stream_buffer construction (no network) ───────────────────────────────

TEST_F(SlickBufferWebsocketTest, SlickStreamBufferConstruction) {
    auto sb = std::make_shared<slick::stream_buffer>(kCapacity, kControl);

    Websocket<slick::dynamic_buffer<slick::stream_buffer>> ws(
        kEchoUrl,
        []() {}, []() {},
        [](const char*, std::size_t) {},
        [](std::string&&) {},
        sb
    );

    EXPECT_EQ(ws.status(),
              Websocket<slick::dynamic_buffer<slick::stream_buffer>>::Status::DISCONNECTED);
}

// ── 2. producer_buffer construction (no network) ─────────────────────────────

TEST_F(SlickBufferWebsocketTest, SlickProducerBufferConstruction) {
    slick::stream_buffer_multiplexer mux(256);
    auto pb = mux.add_producer(0, kCapacity, kControl);

    using PBuf = slick::stream_buffer_multiplexer::producer_buffer;
    Websocket<slick::dynamic_buffer<PBuf>> ws(
        kEchoUrl,
        []() {}, []() {},
        [](const char*, std::size_t) {},
        [](std::string&&) {},
        pb
    );

    EXPECT_EQ(ws.status(),
              Websocket<slick::dynamic_buffer<PBuf>>::Status::DISCONNECTED);
}

// ── 3. producer_buffer data round-trip (live network) ────────────────────────

TEST_F(SlickBufferWebsocketTest, SlickProducerBufferDataCallback) {
    slick::stream_buffer_multiplexer mux(256);
    auto pb = mux.add_producer(0, kCapacity, kControl);

    using PBuf = slick::stream_buffer_multiplexer::producer_buffer;

    std::atomic<bool> connected{false};
    std::atomic<bool> got_data{false};
    std::string       received;

    Websocket<slick::dynamic_buffer<PBuf>> ws(
        kEchoUrl,
        [&]() { connected.store(true, std::memory_order_release); },
        []() {},
        [&](const char* data, std::size_t len) {
            received.assign(data, len);
            got_data.store(true, std::memory_order_release);
        },
        [](std::string&&) {},
        pb
    );

    ws.open();
    ASSERT_TRUE(wait_for([&] { return connected.load(std::memory_order_acquire); }))
        << "Timed out waiting for connection";

    const std::string msg = "hello-slick-producer-buffer";
    ws.send(msg.c_str(), msg.size());

    ASSERT_TRUE(wait_for([&] { return got_data.load(std::memory_order_acquire); }))
        << "Timed out waiting for echo";

    EXPECT_EQ(received, msg);
    ws.close();
}

// ── 4. producer_buffer reconnect reuses the same backend ─────────────────────

TEST_F(SlickBufferWebsocketTest, SlickProducerBufferReconnectSharesBuffer) {
    slick::stream_buffer_multiplexer mux(256);
    auto pb = mux.add_producer(0, kCapacity, kControl);

    using PBuf = slick::stream_buffer_multiplexer::producer_buffer;

    std::atomic<int> connect_count{0};
    std::atomic<int> data_count{0};

    Websocket<slick::dynamic_buffer<PBuf>> ws(
        kEchoUrl,
        [&]() { connect_count.fetch_add(1, std::memory_order_release); },
        []() {},
        [&](const char*, std::size_t) { data_count.fetch_add(1, std::memory_order_release); },
        [](std::string&&) {},
        pb
    );

    // Capture shared-queue cursor before any data is published.
    uint64_t cursor = mux.initial_reading_index();

    // ── session 1 ──
    ws.open();
    ASSERT_TRUE(wait_for([&] { return connect_count.load() >= 1; }))
        << "First connect timed out";

    ws.send("session-one", 11);
    ASSERT_TRUE(wait_for([&] { return data_count.load() >= 1; }))
        << "First echo timed out";

    ws.close();
    using WsType = Websocket<slick::dynamic_buffer<PBuf>>;
    ASSERT_TRUE(wait_for([&] { return ws.status() == WsType::Status::DISCONNECTED; }))
        << "First disconnect timed out";

    // ── session 2 (reconnect on same object) ──
    ws.open();
    ASSERT_TRUE(wait_for([&] { return connect_count.load() >= 2; }))
        << "Second connect timed out";

    ws.send("session-two", 11);
    ASSERT_TRUE(wait_for([&] { return data_count.load() >= 2; }))
        << "Second echo timed out";

    ws.close();

    // Both sessions' data is routed through the multiplexer's shared queue.
    // Walk from the cursor captured before open() and confirm at least 2 records.
    int records = 0;
    for (int i = 0; i < 10; ++i) {
        auto rec = mux.read(cursor);
        if (!rec) break;
        ++records;
    }
    EXPECT_GE(records, 2)
        << "Expected at least 2 records in shared multiplexer queue across both sessions";
}

// ── 5. stream_buffer data round-trip (live network) ─────────────────────────

TEST_F(SlickBufferWebsocketTest, SlickStreamBufferDataCallback) {
    auto sb = std::make_shared<slick::stream_buffer>(kCapacity, kControl);

    std::atomic<bool> connected{false};
    std::atomic<bool> got_data{false};
    std::string       received;

    Websocket<slick::dynamic_buffer<slick::stream_buffer>> ws(
        kEchoUrl,
        [&]() { connected.store(true, std::memory_order_release); },
        []() {},
        [&](const char* data, std::size_t len) {
            received.assign(data, len);
            got_data.store(true, std::memory_order_release);
        },
        [](std::string&&) {},
        sb
    );

    ws.open();
    ASSERT_TRUE(wait_for([&] { return connected.load(std::memory_order_acquire); }))
        << "Timed out waiting for connection";

    const std::string msg = "hello-slick-stream-buffer";
    ws.send(msg.c_str(), msg.size());

    ASSERT_TRUE(wait_for([&] { return got_data.load(std::memory_order_acquire); }))
        << "Timed out waiting for echo";

    EXPECT_EQ(received, msg);
    ws.close();
}

// ── 6. reconnect reuses the same stream_buffer backend ───────────────────────

TEST_F(SlickBufferWebsocketTest, SlickStreamBufferReconnectSharesBuffer) {
    auto sb = std::make_shared<slick::stream_buffer>(kCapacity, kControl);

    std::atomic<int> connect_count{0};
    std::atomic<int> data_count{0};

    Websocket<slick::dynamic_buffer<slick::stream_buffer>> ws(
        kEchoUrl,
        [&]() { connect_count.fetch_add(1, std::memory_order_release); },
        []() {},
        [&](const char*, std::size_t) { data_count.fetch_add(1, std::memory_order_release); },
        [](std::string&&) {},
        sb
    );

    // Capture the read cursor BEFORE any data is published so we can walk all records later.
    uint64_t cursor = sb->initial_reading_index();

    // ── session 1 ──
    ws.open();
    ASSERT_TRUE(wait_for([&] { return connect_count.load() >= 1; }))
        << "First connect timed out";

    ws.send("session-one", 11);
    ASSERT_TRUE(wait_for([&] { return data_count.load() >= 1; }))
        << "First echo timed out";

    ws.close();
    using WsType = Websocket<slick::dynamic_buffer<slick::stream_buffer>>;
    ASSERT_TRUE(wait_for([&] { return ws.status() == WsType::Status::DISCONNECTED; }))
        << "First disconnect timed out";

    // ── session 2 (reconnect on same object) ──
    ws.open();
    ASSERT_TRUE(wait_for([&] { return connect_count.load() >= 2; }))
        << "Second connect timed out";

    ws.send("session-two", 11);
    ASSERT_TRUE(wait_for([&] { return data_count.load() >= 2; }))
        << "Second echo timed out";

    ws.close();

    // Both sessions' data is in the same stream_buffer backend.
    // Walk from the cursor captured before open() and confirm at least 2 records.
    int records = 0;
    for (int i = 0; i < 10; ++i) {
        auto [ptr, len] = sb->read(cursor);
        if (!ptr || !len) break;
        ++records;
    }
    EXPECT_GE(records, 2)
        << "Expected at least 2 records in shared stream_buffer across both sessions";
}
