#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include "AudioFormat.h"

namespace sulla {

/**
 * AudioBuffer — owns a contiguous block of PCM audio data.
 *
 * Pure value type. No I/O, no side effects. Holds raw bytes with
 * associated format metadata so consumers know how to interpret the data.
 */
class AudioBuffer {
public:
    AudioBuffer() = default;

    AudioBuffer(const float* data, uint32_t frameCount, AudioFormat format)
        : format_(format)
        , frameCount_(frameCount)
    {
        const size_t bytes = frameCount * format.bytesPerFrame();
        data_.resize(bytes);
        std::memcpy(data_.data(), data, bytes);
    }

    AudioBuffer(const uint8_t* rawBytes, size_t byteCount, AudioFormat format)
        : format_(format)
    {
        data_.assign(rawBytes, rawBytes + byteCount);
        if (format.bytesPerFrame() > 0) {
            frameCount_ = static_cast<uint32_t>(byteCount / format.bytesPerFrame());
        }
    }

    /** Raw byte pointer for reading. */
    const uint8_t* data() const { return data_.data(); }

    /** Raw byte pointer for writing. */
    uint8_t* data() { return data_.data(); }

    /** Total size in bytes. */
    size_t sizeBytes() const { return data_.size(); }

    /** Number of audio frames. */
    uint32_t frameCount() const { return frameCount_; }

    /** Format describing how to interpret the bytes. */
    const AudioFormat& format() const { return format_; }

    /** Duration in milliseconds. */
    double durationMs() const { return format_.framesToMs(frameCount_); }

    /** True if the buffer contains no data. */
    bool empty() const { return data_.empty(); }

    /** Interpret data as float samples (only valid when format.isFloat && bitDepth==32). */
    const float* asFloat() const {
        return reinterpret_cast<const float*>(data_.data());
    }

    /** Interpret data as int16 samples (only valid when !format.isFloat && bitDepth==16). */
    const int16_t* asInt16() const {
        return reinterpret_cast<const int16_t*>(data_.data());
    }

    /** Reserve space for N frames without initializing data. */
    void allocate(uint32_t frameCount, AudioFormat format) {
        format_ = format;
        frameCount_ = frameCount;
        data_.resize(frameCount * format.bytesPerFrame(), 0);
    }

private:
    std::vector<uint8_t> data_;
    AudioFormat          format_;
    uint32_t             frameCount_ = 0;
};

} // namespace sulla
