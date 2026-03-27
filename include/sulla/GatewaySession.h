#pragma once

#include <string>
#include <map>
#include <cstdint>

namespace sulla {

/**
 * GatewaySession — model representing an active audio session on the gateway.
 *
 * Pure data returned from POST /api/sessions. No I/O.
 */
struct GatewaySession {
    std::string sessionId;
    std::string callId;
    std::string audioUrl;    // Full WebSocket URL: ws://gateway/audio/{sessionId}

    bool isValid() const {
        return !sessionId.empty() && !audioUrl.empty();
    }
};

/**
 * ChannelMap — describes what each audio channel represents.
 *
 * Sent to the gateway at session creation so the STT pipeline
 * knows how to label each channel's speaker.
 */
struct ChannelMap {
    struct Channel {
        std::string label;   // e.g. "User", "Caller"
        std::string source;  // e.g. "mic", "system_audio"
    };

    std::map<uint8_t, Channel> channels;

    /** Default single-channel (mic only). */
    static ChannelMap singleChannel() {
        ChannelMap m;
        m.channels[0] = {"User", "mic"};
        return m;
    }

    /** Dual-channel (mic + system audio). */
    static ChannelMap dualChannel() {
        ChannelMap m;
        m.channels[0] = {"User", "mic"};
        m.channels[1] = {"Caller", "system_audio"};
        return m;
    }

    bool isMultiChannel() const {
        return channels.size() > 1;
    }
};

/**
 * AudioChunkHeader — the wire format prefix for multi-channel audio.
 *
 * Wire format: [0x01 magic][channel uint8][audio bytes...]
 * Channel 0 is sent raw (no prefix) for backward compatibility.
 */
namespace AudioChunkHeader {

    static constexpr uint8_t MAGIC_BYTE = 0x01;

    /** Prepend channel header to raw audio bytes. Returns tagged buffer. */
    inline std::vector<uint8_t> tag(const uint8_t* audio, size_t audioBytes, uint8_t channel) {
        if (channel == 0) {
            // Channel 0: no prefix, raw audio
            return std::vector<uint8_t>(audio, audio + audioBytes);
        }
        // Channel 1+: [magic][channel][audio...]
        std::vector<uint8_t> tagged(2 + audioBytes);
        tagged[0] = MAGIC_BYTE;
        tagged[1] = channel;
        std::memcpy(tagged.data() + 2, audio, audioBytes);
        return tagged;
    }

    /** Parse channel from a tagged buffer. Returns channel and audio offset. */
    struct ParseResult {
        uint8_t  channel;
        size_t   audioOffset;  // Byte offset where audio data starts
    };

    inline ParseResult parse(const uint8_t* data, size_t length) {
        if (length >= 2 && data[0] == MAGIC_BYTE) {
            return {data[1], 2};
        }
        return {0, 0};  // Channel 0, audio starts at offset 0
    }

} // namespace AudioChunkHeader
} // namespace sulla
