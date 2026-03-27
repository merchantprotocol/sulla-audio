#include <gtest/gtest.h>
#include <sulla/RingBuffer.h>
#include <thread>
#include <vector>
#include <numeric>

using namespace sulla;

TEST(RingBuffer, InitialState) {
    RingBuffer rb(1024);
    EXPECT_EQ(rb.capacity(), 1024u);
    EXPECT_EQ(rb.availableRead(), 0u);
    EXPECT_EQ(rb.availableWrite(), 1024u);
}

TEST(RingBuffer, WriteAndRead) {
    RingBuffer rb(1024);
    uint8_t data[100];
    std::iota(data, data + 100, 0);

    size_t written = rb.write(data, 100);
    EXPECT_EQ(written, 100u);
    EXPECT_EQ(rb.availableRead(), 100u);
    EXPECT_EQ(rb.availableWrite(), 924u);

    uint8_t out[100];
    size_t bytesRead = rb.read(out, 100);
    EXPECT_EQ(bytesRead, 100u);
    EXPECT_EQ(rb.availableRead(), 0u);

    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(out[i], static_cast<uint8_t>(i));
    }
}

TEST(RingBuffer, PartialRead) {
    RingBuffer rb(1024);
    uint8_t data[100];
    std::fill_n(data, 100, 42);
    rb.write(data, 100);

    uint8_t out[50];
    size_t bytesRead = rb.read(out, 50);
    EXPECT_EQ(bytesRead, 50u);
    EXPECT_EQ(rb.availableRead(), 50u);

    // Read remaining
    bytesRead = rb.read(out, 50);
    EXPECT_EQ(bytesRead, 50u);
    EXPECT_EQ(rb.availableRead(), 0u);
}

TEST(RingBuffer, ReadFromEmpty) {
    RingBuffer rb(1024);
    uint8_t out[10];
    EXPECT_EQ(rb.read(out, 10), 0u);
}

TEST(RingBuffer, WriteToFull) {
    RingBuffer rb(64);
    uint8_t data[100];
    std::fill_n(data, 100, 1);

    size_t written = rb.write(data, 100);
    EXPECT_EQ(written, 64u); // Capped at capacity
    EXPECT_EQ(rb.availableWrite(), 0u);

    // Try writing more — should return 0
    written = rb.write(data, 10);
    EXPECT_EQ(written, 0u);
}

TEST(RingBuffer, Wraparound) {
    RingBuffer rb(64);
    uint8_t data[40];
    uint8_t out[40];

    // Fill 40 bytes
    std::fill_n(data, 40, 0xAA);
    rb.write(data, 40);

    // Read 40 (moves read pointer to 40)
    rb.read(out, 40);
    EXPECT_EQ(rb.availableRead(), 0u);

    // Write 40 more — wraps around the 64-byte boundary
    std::fill_n(data, 40, 0xBB);
    rb.write(data, 40);

    // Read them back
    rb.read(out, 40);
    for (int i = 0; i < 40; ++i) {
        EXPECT_EQ(out[i], 0xBB);
    }
}

TEST(RingBuffer, Reset) {
    RingBuffer rb(1024);
    uint8_t data[100];
    rb.write(data, 100);
    EXPECT_EQ(rb.availableRead(), 100u);

    rb.reset();
    EXPECT_EQ(rb.availableRead(), 0u);
    EXPECT_EQ(rb.availableWrite(), 1024u);
}

TEST(RingBuffer, ConcurrentWriteRead) {
    // Single producer, single consumer — should be safe
    RingBuffer rb(4096);
    const size_t totalBytes = 100000;
    std::atomic<size_t> totalWritten{0};
    std::atomic<size_t> totalRead{0};

    std::thread writer([&]() {
        uint8_t chunk[256];
        std::iota(chunk, chunk + 256, 0);
        while (totalWritten.load() < totalBytes) {
            size_t written = rb.write(chunk, 256);
            totalWritten.fetch_add(written);
            if (written == 0) std::this_thread::yield();
        }
    });

    std::thread reader([&]() {
        uint8_t out[256];
        while (totalRead.load() < totalBytes) {
            size_t bytesRead = rb.read(out, 256);
            totalRead.fetch_add(bytesRead);
            if (bytesRead == 0) std::this_thread::yield();
        }
    });

    writer.join();
    reader.join();

    EXPECT_GE(totalWritten.load(), totalBytes);
    EXPECT_GE(totalRead.load(), totalBytes);
}
