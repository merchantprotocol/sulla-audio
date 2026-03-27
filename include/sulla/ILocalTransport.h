#pragma once

#include <functional>
#include <memory>
#include <string>
#include "AudioChunk.h"

namespace sulla {

/**
 * ILocalTransport — interface for streaming audio to Sulla Desktop locally.
 *
 * When the driver and Sulla Desktop are on the same machine, audio goes
 * through this local transport instead of the gateway. No auth needed.
 *
 * Sulla Desktop's SecretaryMode connects to this transport and receives
 * labeled audio chunks (mic + speaker). SecretaryMode then sends both
 * channels through the gateway using its existing gateway streaming code.
 *
 * Transport options:
 *   - Unix domain socket (macOS/Linux)
 *   - Named pipe (Windows)
 *   - Local TCP WebSocket (fallback)
 *
 * Protocol: binary frames with a simple header:
 *   [1 byte: source (0=mic, 1=speaker)]
 *   [4 bytes: payload length, big-endian]
 *   [N bytes: raw PCM audio]
 */
class ILocalTransport {
public:
    using ChunkCallback = std::function<void(const AudioChunk& chunk)>;
    using StatusCallback = std::function<void(bool connected, const std::string& detail)>;

    virtual ~ILocalTransport() = default;

    // ── Server side (driver) ─────────────────────────────────

    /** Start listening for Sulla Desktop connections. */
    virtual bool startServer(const std::string& socketPath, uint16_t tcpPort = 0) = 0;

    /** Send a chunk to all connected Sulla Desktop clients. */
    virtual bool sendChunk(const AudioChunk& chunk) = 0;

    /** Stop the server and disconnect all clients. */
    virtual void stopServer() = 0;

    /** Number of connected clients. */
    virtual uint32_t clientCount() const = 0;

    // ── Client side (Sulla Desktop) ──────────────────────────

    /** Connect to the driver's local transport. */
    virtual bool connect(const std::string& socketPath, uint16_t tcpPort = 0) = 0;

    /** Register callback for incoming chunks (client side). */
    void onChunk(ChunkCallback cb) { chunkCallback_ = std::move(cb); }

    /** Disconnect from the driver. */
    virtual void disconnect() = 0;

    // ── Common ───────────────────────────────────────────────

    /** Register callback for connection status changes. */
    void onStatus(StatusCallback cb) { statusCallback_ = std::move(cb); }

    /** Factory. */
    static std::unique_ptr<ILocalTransport> create();

protected:
    ChunkCallback  chunkCallback_;
    StatusCallback statusCallback_;

    void emitChunk(const AudioChunk& chunk) {
        if (chunkCallback_) chunkCallback_(chunk);
    }

    void emitStatus(bool connected, const std::string& detail = "") {
        if (statusCallback_) statusCallback_(connected, detail);
    }
};

} // namespace sulla
