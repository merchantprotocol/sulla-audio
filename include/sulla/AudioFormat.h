#pragma once

#include <cstdint>
#include <string>

namespace sulla {

/**
 * AudioFormat — immutable value object describing PCM audio format.
 *
 * Pure data, no behavior beyond comparison and serialization.
 * Independently testable with no dependencies.
 */
struct AudioFormat {
    uint32_t sampleRate  = 48000;
    uint16_t channels    = 2;
    uint16_t bitDepth    = 32;
    bool     isFloat     = true;

    /** Bytes per single sample (one channel). */
    uint32_t bytesPerSample() const {
        return bitDepth / 8;
    }

    /** Bytes per frame (all channels for one sample instant). */
    uint32_t bytesPerFrame() const {
        return bytesPerSample() * channels;
    }

    /** Bytes per second of audio. */
    uint32_t bytesPerSecond() const {
        return bytesPerFrame() * sampleRate;
    }

    /** Duration in milliseconds for a given number of frames. */
    double framesToMs(uint32_t frames) const {
        if (sampleRate == 0) return 0.0;
        return (static_cast<double>(frames) / sampleRate) * 1000.0;
    }

    /** Number of frames for a given duration in milliseconds. */
    uint32_t msToFrames(double ms) const {
        return static_cast<uint32_t>((ms / 1000.0) * sampleRate);
    }

    bool operator==(const AudioFormat& other) const {
        return sampleRate == other.sampleRate
            && channels   == other.channels
            && bitDepth   == other.bitDepth
            && isFloat    == other.isFloat;
    }

    bool operator!=(const AudioFormat& other) const {
        return !(*this == other);
    }

    std::string toString() const {
        return std::to_string(sampleRate) + "Hz, "
             + std::to_string(bitDepth) + "-bit"
             + (isFloat ? " float" : " int")
             + ", " + std::to_string(channels) + "ch";
    }

    /** Common format presets. */
    static AudioFormat cd()       { return {44100, 2, 16, false}; }
    static AudioFormat telephony(){ return {16000, 1, 16, false}; }
    static AudioFormat wasapi()   { return {48000, 2, 32, true};  }
};

} // namespace sulla
