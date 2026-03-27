#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

namespace sulla {

/**
 * AudioChunk — a labeled, wire-ready audio packet.
 *
 * Pure data model. Carries raw audio bytes + metadata about the source.
 * This is what flows through the pipeline from capture → transport.
 *
 * Wire format for multi-channel (enterprise gateway protocol):
 *   Channel 0 (mic):     [raw audio bytes]           — no prefix
 *   Channel 1 (speaker): [0x01][0x01][raw audio bytes] — tagged
 *
 * The source label (mic/speaker) maps to a channel number for the wire format,
 * and is also used in local mode for Sulla Desktop to identify the stream.
 */
struct AudioChunk {
    enum class Source : uint8_t {
        Mic     = 0,   // Channel 0 — user's microphone
        Speaker = 1    // Channel 1 — system/speaker audio (loopback)
    };

    Source               source;
    std::vector<uint8_t> audio;      // Raw PCM bytes
    uint32_t             sampleRate; // Sample rate of the audio
    uint16_t             channels;   // Channel count (usually 1 after conversion)
    uint16_t             bitDepth;   // Bit depth (usually 16 after conversion)
    uint64_t             timestamp;  // Capture timestamp (ms since epoch)
    uint32_t             sequenceNum; // Monotonic chunk counter per source

    /** Channel number for wire format. */
    uint8_t channelNumber() const {
        return static_cast<uint8_t>(source);
    }

    /** Human-readable source name. */
    std::string sourceName() const {
        switch (source) {
            case Source::Mic:     return "mic";
            case Source::Speaker: return "speaker";
        }
        return "unknown";
    }

    /** Size in bytes. */
    size_t sizeBytes() const { return audio.size(); }

    /** Duration in milliseconds. */
    double durationMs() const {
        if (sampleRate == 0 || channels == 0 || bitDepth == 0) return 0;
        uint32_t bytesPerFrame = (bitDepth / 8) * channels;
        uint32_t frames = static_cast<uint32_t>(audio.size() / bytesPerFrame);
        return (static_cast<double>(frames) / sampleRate) * 1000.0;
    }

    /**
     * Encode for gateway wire format.
     * Channel 0: raw bytes (no prefix)
     * Channel 1+: [0x01][channel][raw bytes]
     */
    std::vector<uint8_t> toWireFormat() const {
        uint8_t ch = channelNumber();
        if (ch == 0) {
            return audio;
        }
        std::vector<uint8_t> wire(2 + audio.size());
        wire[0] = 0x01; // Magic byte
        wire[1] = ch;
        std::memcpy(wire.data() + 2, audio.data(), audio.size());
        return wire;
    }

    /** Log-safe summary string. */
    std::string toString() const {
        return "AudioChunk{source=" + sourceName()
             + " #" + std::to_string(sequenceNum)
             + " " + std::to_string(audio.size()) + "B"
             + " " + std::to_string(durationMs()) + "ms"
             + " " + std::to_string(sampleRate) + "Hz"
             + "}";
    }
};

} // namespace sulla
