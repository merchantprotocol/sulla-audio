#pragma once

#include <memory>
#include <string>
#include <functional>
#include <atomic>

#include "DriverConfig.h"
#include "DeviceController.h"
#include "CaptureController.h"
#include "IGatewayClient.h"
#include "IAuthClient.h"
#include "ILocalTransport.h"
#include "AuthCredentials.h"
#include "GatewaySession.h"
#include "AudioChunk.h"
#include "CaptureState.h"
#include "PlatformDetector.h"
#include "Logger.h"

#ifdef __APPLE__
#include "AudioMirrorManager.h"
#include "MediaKeyListener.h"
#endif

namespace sulla {

/**
 * DriverController — top-level controller that wires everything together.
 *
 * Business decisions:
 *   - Mode selection: gateway (standalone + auth) vs local (Sulla Desktop IPC)
 *   - Auth lifecycle: login, token refresh, re-auth on expiry
 *   - Device selection delegation to DeviceController
 *   - Chunk routing: gateway WebSocket vs local transport vs both
 *   - Reconnection logic on disconnect
 *   - Logging: what to log, what to redact
 *
 * Gateway mode flow:
 *   1. Login (email + password → JWT token + userId)
 *   2. Derive gateway WebSocket URL from backend
 *   3. Select capture device (DeviceController)
 *   4. Create gateway session (POST /api/sessions with auth header)
 *   5. Connect audio WebSocket (auth in header, not URL)
 *   6. Capture mic + speaker → labeled chunks → gateway
 *
 * Local mode flow:
 *   1. No auth
 *   2. Start local transport server (Unix socket / named pipe)
 *   3. Select capture device (DeviceController)
 *   4. Capture mic + speaker → labeled chunks → local transport
 *   5. Sulla Desktop connects, receives chunks, routes through SecretaryMode
 */
class DriverController {
public:
    enum class DriverState {
        Unconfigured,
        Authenticating,
        Ready,
        Connecting,
        Streaming,
        Reconnecting,
        Error
    };

    using StatusCallback = std::function<void(DriverState state, const std::string& message)>;

    DriverController(
        std::unique_ptr<DeviceController> deviceCtrl,
        std::unique_ptr<CaptureController> speakerCapture,
        std::unique_ptr<CaptureController> micCapture,
        std::unique_ptr<IGatewayClient> gatewayClient,
        std::unique_ptr<IAuthClient> authClient,
        std::unique_ptr<ILocalTransport> localTransport
    )
        : deviceCtrl_(std::move(deviceCtrl))
        , speakerCapture_(std::move(speakerCapture))
        , micCapture_(std::move(micCapture))
        , gatewayClient_(std::move(gatewayClient))
        , authClient_(std::move(authClient))
        , localTransport_(std::move(localTransport))
    {}

    void onStatus(StatusCallback cb) { statusCallback_ = std::move(cb); }

    // ─── Configuration ───────────────────────────────────────

    /**
     * Apply config and auto-start if ready.
     * In gateway mode, auth must happen before streaming.
     */
    void applyConfig(const DriverConfig& config) {
        config_ = config;

        // Initialize logging
        auto& log = Logger::instance();
        log.setLogDir(DriverConfig::logDir());
        if (config.logLevel == "trace")     log.setLevel(Logger::Level::Trace);
        else if (config.logLevel == "debug") log.setLevel(Logger::Level::Debug);
        else if (config.logLevel == "warn")  log.setLevel(Logger::Level::Warn);
        else if (config.logLevel == "error") log.setLevel(Logger::Level::Error);
        else                                  log.setLevel(Logger::Level::Info);

        SULLA_LOG_INFO("Driver", "Config applied — mode: "
            + std::string(config.isGatewayMode() ? "gateway" : "local")
            + ", platform: " + PlatformDetector::osName());

        if (config.isGatewayMode()) {
            if (!config.hasGatewayConfig()) {
                setState(DriverState::Unconfigured,
                    "Gateway mode requires backend_url and email");
                return;
            }
            setState(DriverState::Ready, "Configured — login required before streaming");
        } else {
            if (!config.hasLocalConfig()) {
                config_.localSocketPath = DriverConfig::defaultLocalSocket();
                SULLA_LOG_INFO("Driver", "Using default local socket: " + config_.localSocketPath);
            }
            setState(DriverState::Ready, "Local mode — no auth required");

            if (config.autoStart) {
                startLocal();
            }
        }
    }

    // ─── Gateway mode ────────────────────────────────────────

    /**
     * Authenticate with the backend. Must be called before startGateway().
     * Password is provided at runtime, never persisted.
     */
    void login(const std::string& password) {
        if (!config_.isGatewayMode()) {
            SULLA_LOG_WARN("Driver", "login() called but not in gateway mode");
            return;
        }

        setState(DriverState::Authenticating, "Logging in as " + config_.email + "...");
        SULLA_LOG_INFO("Auth", "Authenticating: " + config_.email + " → " + config_.authEndpoint());

        if (!authClient_) {
            SULLA_LOG_ERROR("Auth", "Auth client not available (not implemented)");
            setState(DriverState::Error, "Auth client not available");
            return;
        }

        auto result = authClient_->login(config_.backendUrl, config_.email, password);

        if (!result.success) {
            SULLA_LOG_ERROR("Auth", "Login failed: " + result.error);
            setState(DriverState::Error, "Login failed: " + result.error);
            return;
        }

        auth_ = result.credentials;
        SULLA_LOG_INFO("Auth", "Login successful — " + auth_.redacted());

        // Derive gateway URL from backend if not explicitly set
        if (config_.gatewayUrl.empty()) {
            // Convert http(s)://host:port/api/ → ws(s)://host:port
            std::string url = config_.backendUrl;
            if (url.find("https://") == 0)    url = "wss://" + url.substr(8);
            else if (url.find("http://") == 0) url = "ws://" + url.substr(7);
            // Strip trailing /api/ or /api
            auto apiPos = url.rfind("/api");
            if (apiPos != std::string::npos) url = url.substr(0, apiPos);
            config_.gatewayUrl = url;
            SULLA_LOG_INFO("Auth", "Derived gateway URL: " + config_.gatewayUrl);
        }

        setState(DriverState::Ready, "Authenticated — ready to stream");

        if (config_.autoStart) {
            startGateway();
        }
    }

    /**
     * Start streaming to the gateway (requires prior login).
     */
    void startGateway() {
        if (!auth_.isAuthenticated()) {
            setState(DriverState::Error, "Not authenticated — call login() first");
            return;
        }

        setState(DriverState::Connecting, "Selecting audio device...");

#ifdef __APPLE__
        if (!mirrorManager_.start()) {
            SULLA_LOG_WARN("Mirror", "Audio mirror unavailable — speaker capture may be silent");
        }
        startMediaKeyListener();
#endif

        // Step 1: Device selection
        auto selection = deviceCtrl_->selectDevice(config_.preferredDevice);
        if (!selection.found) {
            SULLA_LOG_ERROR("Device", selection.message);
            setState(DriverState::Error, selection.message);
            return;
        }
        selectedDevice_ = selection.device;
        SULLA_LOG_INFO("Device", "Selected: " + selection.device.toString());

        // Step 2: Create gateway session
        if (!gatewayClient_) {
            SULLA_LOG_ERROR("Gateway", "Gateway client not available (not implemented)");
            setState(DriverState::Error, "Gateway client not available");
            return;
        }

        setState(DriverState::Connecting, "Creating gateway session...");
        auto channelMap = (config_.captureMic && config_.captureSpeaker)
            ? ChannelMap::dualChannel()
            : ChannelMap::singleChannel();

        session_ = gatewayClient_->createSession(
            config_.sessionEndpoint(), auth_, "Audio Driver", channelMap);

        if (!session_.isValid()) {
            SULLA_LOG_ERROR("Gateway", "Failed to create session");
            setState(DriverState::Error, "Gateway session creation failed");
            return;
        }
        SULLA_LOG_INFO("Gateway", "Session created: " + session_.sessionId);

        // Step 3: Connect audio WebSocket (token in header)
        if (!gatewayClient_->connectAudio(session_.audioUrl, auth_)) {
            SULLA_LOG_ERROR("Gateway", "Audio WebSocket connection failed");
            setState(DriverState::Error, "Audio WebSocket connection failed");
            return;
        }
        SULLA_LOG_INFO("Gateway", "Audio WebSocket connected");

        // Step 4: Wire captures to gateway
        wireCapturesToGateway();

        // Step 5: Start capture
        startCapture();
    }

    // ─── Local mode ──────────────────────────────────────────

    /**
     * Start local transport server and begin capturing.
     * Sulla Desktop connects and receives labeled chunks.
     */
    void startLocal() {
        setState(DriverState::Connecting, "Starting local transport...");

        if (!localTransport_) {
            SULLA_LOG_ERROR("Local", "Local transport not available (platform not implemented)");
            setState(DriverState::Error, "Local transport not available");
            return;
        }

        if (!localTransport_->startServer(config_.localSocketPath, config_.localPort)) {
            SULLA_LOG_ERROR("Local", "Failed to start local transport server");
            setState(DriverState::Error, "Local transport server failed to start");
            return;
        }
        SULLA_LOG_INFO("Local", "Local transport listening on " + config_.localSocketPath);

#ifdef __APPLE__
        // Start audio mirror — ensures system audio is routed to loopback driver
        // regardless of which output device the user selects
        if (!mirrorManager_.start()) {
            SULLA_LOG_WARN("Mirror", "Audio mirror unavailable — speaker capture may be silent");
        }
        startMediaKeyListener();

        // Track client connections to gate the mirror watchdog
        localTransport_->onStatus([this](bool connected, const std::string& detail) {
            clientConnected_ = connected;
            if (connected) {
                SULLA_LOG_INFO("Driver", "Client connected — monitoring audio for silence");
                silentChunkCount_ = 0;
            } else {
                SULLA_LOG_INFO("Driver", "All clients disconnected — stopping mirror watchdog");
                mirrorManager_.stopWatchdog();
                silentChunkCount_ = 0;
            }
        });
#endif

        // Device selection
        auto selection = deviceCtrl_->selectDevice(config_.preferredDevice);
        if (!selection.found) {
            SULLA_LOG_ERROR("Device", selection.message);
            setState(DriverState::Error, selection.message);
            return;
        }
        selectedDevice_ = selection.device;
        SULLA_LOG_INFO("Device", "Selected: " + selection.device.toString());

        // Wire captures to local transport
        wireCapturesToLocal();

        // Start capture
        startCapture();
    }

    // ─── Shared ──────────────────────────────────────────────

    void stop() {
        SULLA_LOG_INFO("Driver", "Stopping...");

        if (speakerCapture_) speakerCapture_->stop();
        if (micCapture_) micCapture_->stop();

#ifdef __APPLE__
        mediaKeyListener_.stop();
        mirrorManager_.stop();
#endif

        if (config_.isGatewayMode()) {
            if (gatewayClient_) gatewayClient_->disconnectAudio();
        } else {
            if (localTransport_) localTransport_->stopServer();
        }

        setState(DriverState::Ready, "Stopped");
        SULLA_LOG_INFO("Driver", "Stopped");
    }

    DriverState state() const { return state_; }
    const GatewaySession& session() const { return session_; }
    const AudioDevice& device() const { return selectedDevice_; }
    const DriverConfig& config() const { return config_; }
    const AuthCredentials& auth() const { return auth_; }

    // ─── Volume control ─────────────────────────────────────
    // These control the physical output device that the mirror wraps.
    // macOS disables volume UI for multi-output aggregate devices,
    // so these provide direct access to the underlying device's volume.

#ifdef __APPLE__
    /** Get volume (0.0–1.0) of the wrapped physical output, or -1 on error. */
    float getVolume() const { return mirrorManager_.getVolume(); }

    /** Set volume (0.0–1.0) of the wrapped physical output. */
    bool setVolume(float volume) { return mirrorManager_.setVolume(volume); }

    /** Adjust volume by delta (e.g. +0.0625 = one notch up). Returns new volume or -1. */
    float adjustVolume(float delta) { return mirrorManager_.adjustVolume(delta); }

    /** Check mute state: 1=muted, 0=unmuted, -1=error. */
    int isMuted() const { return mirrorManager_.isMuted(); }

    /** Set mute state. */
    bool setMuted(bool mute) { return mirrorManager_.setMuted(mute); }
#endif

private:
    std::unique_ptr<DeviceController>  deviceCtrl_;
    std::unique_ptr<CaptureController> speakerCapture_;
    std::unique_ptr<CaptureController> micCapture_;
    std::unique_ptr<IGatewayClient>    gatewayClient_;
    std::unique_ptr<IAuthClient>       authClient_;
    std::unique_ptr<ILocalTransport>   localTransport_;

    DriverConfig    config_;
    AuthCredentials auth_;
#ifdef __APPLE__
    AudioMirrorManager mirrorManager_;
    MediaKeyListener mediaKeyListener_;
#endif
    GatewaySession  session_;
    AudioDevice     selectedDevice_;
    DriverState     state_ = DriverState::Unconfigured;
    StatusCallback  statusCallback_;

    uint32_t speakerChunkCount_ = 0;
    uint32_t micChunkCount_ = 0;
#ifdef __APPLE__
    std::atomic<bool> clientConnected_{false};
    std::atomic<uint32_t> silentChunkCount_{0};
#endif

    /**
     * Check if a PCM chunk is silence (all samples below noise floor).
     * Chunks are 16-bit signed int PCM (the CaptureController's target format).
     */
    static bool isChunkSilent(const std::vector<uint8_t>& raw) {
        // 16-bit PCM: 2 bytes per sample. Threshold ~0.1% of full scale.
        static constexpr int16_t kSilenceThreshold = 32;
        const auto* samples = reinterpret_cast<const int16_t*>(raw.data());
        size_t count = raw.size() / sizeof(int16_t);
        for (size_t i = 0; i < count; ++i) {
            int16_t s = samples[i];
            if (s > kSilenceThreshold || s < -kSilenceThreshold) return false;
        }
        return true;
    }

#ifdef __APPLE__
    void startMediaKeyListener() {
        mediaKeyListener_.onVolumeAdjust([this](float delta) {
            float newVol = mirrorManager_.adjustVolume(delta);
            if (newVol >= 0.0f) {
                SULLA_LOG_DEBUG("Driver", "Volume key → " + std::to_string(newVol));
            }
        });
        mediaKeyListener_.onMuteToggle([this]() {
            int muted = mirrorManager_.isMuted();
            if (muted >= 0) {
                mirrorManager_.setMuted(!muted);
                SULLA_LOG_DEBUG("Driver", std::string("Mute key → ") + (muted ? "unmuted" : "muted"));
            }
        });
        if (!mediaKeyListener_.start()) {
            SULLA_LOG_WARN("Driver", "Media key listener unavailable — keyboard volume keys won't work");
        }
    }
#endif

    void wireCapturesToGateway() {
        speakerCapture_->onChunk([this](const std::vector<uint8_t>& raw) {
            AudioChunk chunk;
            chunk.source = AudioChunk::Source::Speaker;
            chunk.audio = raw;
            chunk.sequenceNum = ++speakerChunkCount_;

            if (config_.logAudioDiagnostics && (chunk.sequenceNum <= 5 || chunk.sequenceNum % 100 == 0)) {
                SULLA_LOG_DEBUG("Capture", chunk.toString());
            }

            gatewayClient_->sendChunk(chunk);
        });

        if (config_.captureMic && micCapture_) {
            micCapture_->onChunk([this](const std::vector<uint8_t>& raw) {
                AudioChunk chunk;
                chunk.source = AudioChunk::Source::Mic;
                chunk.audio = raw;
                chunk.sequenceNum = ++micChunkCount_;

                if (config_.logAudioDiagnostics && (chunk.sequenceNum <= 5 || chunk.sequenceNum % 100 == 0)) {
                    SULLA_LOG_DEBUG("Capture", chunk.toString());
                }

                gatewayClient_->sendChunk(chunk);
            });
        }
    }

    void wireCapturesToLocal() {
        speakerCapture_->onChunk([this](const std::vector<uint8_t>& raw) {
            AudioChunk chunk;
            chunk.source = AudioChunk::Source::Speaker;
            chunk.audio = raw;
            chunk.sequenceNum = ++speakerChunkCount_;

            if (config_.logAudioDiagnostics && (chunk.sequenceNum <= 5 || chunk.sequenceNum % 100 == 0)) {
                SULLA_LOG_DEBUG("Capture", chunk.toString());
            }

#ifdef __APPLE__
            // Gate the mirror watchdog based on audio silence + client presence.
            // Only burn CPU polling CoreAudio when a client needs audio and we're
            // delivering silence (meaning macOS probably switched away from mirror).
            if (clientConnected_) {
                bool silent = isChunkSilent(raw);
                if (silent) {
                    uint32_t count = ++silentChunkCount_;
                    if (count == AudioMirrorManager::kSilenceThresholdChunks) {
                        SULLA_LOG_WARN("Driver", "Sustained silence detected with client connected — starting mirror watchdog");
                        mirrorManager_.startWatchdog();
                    }
                } else {
                    if (silentChunkCount_ >= AudioMirrorManager::kSilenceThresholdChunks) {
                        SULLA_LOG_INFO("Driver", "Audio flowing again — stopping mirror watchdog");
                        mirrorManager_.stopWatchdog();
                    }
                    silentChunkCount_ = 0;
                }
            }
#endif

            localTransport_->sendChunk(chunk);
        });

        if (config_.captureMic && micCapture_) {
            micCapture_->onChunk([this](const std::vector<uint8_t>& raw) {
                AudioChunk chunk;
                chunk.source = AudioChunk::Source::Mic;
                chunk.audio = raw;
                chunk.sequenceNum = ++micChunkCount_;

                if (config_.logAudioDiagnostics && (chunk.sequenceNum <= 5 || chunk.sequenceNum % 100 == 0)) {
                    SULLA_LOG_DEBUG("Capture", chunk.toString());
                }

                localTransport_->sendChunk(chunk);
            });
        }
    }

    void startCapture() {
        speakerChunkCount_ = 0;
        micChunkCount_ = 0;

        // Start speaker capture
        auto err = speakerCapture_->start(selectedDevice_);
        if (!err.ok()) {
            SULLA_LOG_ERROR("Capture", "Speaker capture failed: " + err.message);
            setState(DriverState::Error, "Speaker capture failed: " + err.message);
            return;
        }
        SULLA_LOG_INFO("Capture", "Speaker capture started on " + selectedDevice_.name);

        // Start mic capture if configured
        // TODO: mic capture needs a separate device (input device, not output)
        // For now, mic is only captured if Sulla Desktop provides it (local mode)
        // or the gateway session was created with dual-channel (gateway mode with mic)

        setState(DriverState::Streaming,
            "Streaming from " + selectedDevice_.name
            + (config_.isGatewayMode()
                ? " → gateway session " + session_.sessionId
                : " → local transport"));
        SULLA_LOG_INFO("Driver", "Streaming active");
    }

    void setState(DriverState state, const std::string& message) {
        state_ = state;
        SULLA_LOG_INFO("Driver", "State → " + stateToString(state) + ": " + message);
        if (statusCallback_) statusCallback_(state, message);
    }

    static std::string stateToString(DriverState s) {
        switch (s) {
            case DriverState::Unconfigured:   return "Unconfigured";
            case DriverState::Authenticating: return "Authenticating";
            case DriverState::Ready:          return "Ready";
            case DriverState::Connecting:     return "Connecting";
            case DriverState::Streaming:      return "Streaming";
            case DriverState::Reconnecting:   return "Reconnecting";
            case DriverState::Error:          return "Error";
        }
        return "Unknown";
    }
};

} // namespace sulla
