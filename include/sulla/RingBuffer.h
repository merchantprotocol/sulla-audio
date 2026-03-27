#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sulla {

/**
 * RingBuffer — lock-free single-producer single-consumer circular buffer.
 *
 * Pure utility. No I/O, no platform deps, no business logic.
 * Used by capture backends to hand audio to the controller thread
 * without blocking the real-time audio callback.
 *
 * Thread safety: one thread calls write(), another calls read().
 * Both can run concurrently without locks.
 */
class RingBuffer {
public:
    explicit RingBuffer(size_t capacityBytes)
        : buffer_(capacityBytes)
        , capacity_(capacityBytes)
    {}

    /** Total capacity in bytes. */
    size_t capacity() const { return capacity_; }

    /** Bytes available to read. */
    size_t availableRead() const {
        const size_t w = writePos_.load(std::memory_order_acquire);
        const size_t r = readPos_.load(std::memory_order_relaxed);
        return w - r;
    }

    /** Bytes available to write. */
    size_t availableWrite() const {
        return capacity_ - availableRead();
    }

    /**
     * Write bytes into the ring buffer.
     * Returns the number of bytes actually written (may be less than requested
     * if the buffer is full).
     */
    size_t write(const uint8_t* data, size_t bytes) {
        const size_t avail = availableWrite();
        const size_t toWrite = (bytes <= avail) ? bytes : avail;
        if (toWrite == 0) return 0;

        const size_t w = writePos_.load(std::memory_order_relaxed);
        const size_t offset = w % capacity_;

        // May need to wrap around
        const size_t firstChunk = std::min(toWrite, capacity_ - offset);
        std::memcpy(buffer_.data() + offset, data, firstChunk);
        if (firstChunk < toWrite) {
            std::memcpy(buffer_.data(), data + firstChunk, toWrite - firstChunk);
        }

        writePos_.store(w + toWrite, std::memory_order_release);
        return toWrite;
    }

    /**
     * Read bytes from the ring buffer.
     * Returns the number of bytes actually read (may be less than requested
     * if the buffer doesn't have enough data).
     */
    size_t read(uint8_t* dest, size_t bytes) {
        const size_t avail = availableRead();
        const size_t toRead = (bytes <= avail) ? bytes : avail;
        if (toRead == 0) return 0;

        const size_t r = readPos_.load(std::memory_order_relaxed);
        const size_t offset = r % capacity_;

        const size_t firstChunk = std::min(toRead, capacity_ - offset);
        std::memcpy(dest, buffer_.data() + offset, firstChunk);
        if (firstChunk < toRead) {
            std::memcpy(dest + firstChunk, buffer_.data(), toRead - firstChunk);
        }

        readPos_.store(r + toRead, std::memory_order_release);
        return toRead;
    }

    /** Discard all buffered data. Only safe when no concurrent read/write. */
    void reset() {
        readPos_.store(0, std::memory_order_relaxed);
        writePos_.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<uint8_t> buffer_;
    size_t               capacity_;
    std::atomic<size_t>  writePos_{0};
    std::atomic<size_t>  readPos_{0};
};

} // namespace sulla
