#pragma once

#include <functional>
#include <memory>
#include <string>
#include <cstdint>
#include "GatewaySession.h"
#include "DriverConfig.h"
#include "AudioChunk.h"
#include "AuthCredentials.h"

namespace sulla {

/**
 * IGatewayClient — interface for communicating with the enterprise gateway.
 *
 * Two responsibilities:
 *   1. REST: POST /api/sessions to create a session
 *   2. WebSocket: Connect to /audio/{sessionId} and stream labeled audio chunks
 *
 * Security:
 *   - Token goes in Authorization header: "Authorization: Bearer {jwt}"
 *   - Token is NEVER in the URL or query params
 *   - All connections use TLS when gatewayUrl starts with wss://
 *
 * No business decisions. The controller tells it when to connect/disconnect
 * and what data to send.
 */
class IGatewayClient {
public:
    enum class ConnectionState {
        Disconnected,
        Connecting,
        Connected,
        Reconnecting,
        Error
    };

    using StateCallback = std::function<void(ConnectionState state, const std::string& reason)>;
    using EventCallback = std::function<void(const std::string& eventJson)>;

    virtual ~IGatewayClient() = default;

    /**
     * Create a new audio session on the gateway.
     *
     * REST call: POST /api/sessions
     * Headers:  Authorization: Bearer {token}
     * Body:     { callerName, channels }
     *
     * Returns session info including the audio WebSocket URL.
     */
    virtual GatewaySession createSession(
        const std::string& sessionEndpointUrl,
        const AuthCredentials& auth,
        const std::string& callerName,
        const ChannelMap& channels
    ) = 0;

    /**
     * Open a WebSocket connection to /audio/{sessionId}.
     *
     * The token is sent as an HTTP header during the WebSocket upgrade:
     *   Authorization: Bearer {token}
     *
     * NOT as a query parameter (security: headers are encrypted, URLs may be logged).
     */
    virtual bool connectAudio(const std::string& audioUrl,
                               const AuthCredentials& auth) = 0;

    /** Close the audio WebSocket. */
    virtual void disconnectAudio() = 0;

    /**
     * Send a labeled audio chunk over the WebSocket.
     * The chunk's wire format is used (channel tagging via 0x01 magic byte).
     */
    virtual bool sendChunk(const AudioChunk& chunk) = 0;

    /** Current connection state. */
    virtual ConnectionState state() const = 0;

    /** Register callback for connection state changes. */
    void onStateChange(StateCallback cb) { stateCallback_ = std::move(cb); }

    /** Register callback for incoming gateway events (transcripts, etc). */
    void onEvent(EventCallback cb) { eventCallback_ = std::move(cb); }

    /** Factory. */
    static std::unique_ptr<IGatewayClient> create();

protected:
    StateCallback stateCallback_;
    EventCallback eventCallback_;

    void emitState(ConnectionState state, const std::string& reason = "") {
        if (stateCallback_) stateCallback_(state, reason);
    }

    void emitEvent(const std::string& eventJson) {
        if (eventCallback_) eventCallback_(eventJson);
    }
};

} // namespace sulla
