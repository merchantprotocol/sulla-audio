#pragma once

#include <string>
#include <cstdint>
#include <fstream>
#include <sstream>

namespace sulla {

/**
 * DriverConfig — persistent configuration for the audio driver.
 *
 * Pure data + serialization. No business decisions.
 *
 * Two operating modes:
 *   - Gateway mode:  Authenticates via email/password, streams directly to enterprise gateway
 *   - Local mode:    Sulla Desktop on same machine, streams via local IPC (no auth needed)
 *
 * Persisted as a simple key=value file at a platform-appropriate path:
 *   macOS:   ~/Library/Application Support/AudioDriver/config.ini
 *   Windows: %APPDATA%\AudioDriver\config.ini
 *
 * Security: passwords and tokens are NEVER serialized to the config file.
 * The user enters credentials at runtime (via CLI prompt or config UI).
 * Only the email is persisted for convenience.
 */
struct DriverConfig {
    // ── Mode ─────────────────────────────────────────────────
    enum class Mode {
        Gateway,  // Standalone: auth + stream directly to gateway
        Local     // Desktop app colocated: stream via local IPC, no auth
    };
    Mode mode = Mode::Local;

    // ── Auth (gateway mode only) ─────────────────────────────
    std::string backendUrl;      // REST API base, e.g. "https://api.example.com"
    std::string email;           // Persisted for convenience
    // password: NEVER persisted — entered at runtime
    // token: NEVER persisted — obtained from auth endpoint

    // ── Gateway connection ───────────────────────────────────
    std::string gatewayUrl;      // WebSocket base, e.g. "wss://gateway.example.com"
    // Derived at runtime from auth response or backendUrl

    // ── Local connection (local mode) ────────────────────────
    std::string localSocketPath; // Unix socket or named pipe for Sulla Desktop IPC
    uint16_t    localPort = 0;   // Alternative: TCP port for local WebSocket (0 = use socket)

    // ── Audio settings ───────────────────────────────────────
    std::string preferredDevice; // Device ID override (empty = auto-select)
    uint32_t    chunkIntervalMs = 200;   // How often to send audio chunks (ms)
    uint32_t    targetSampleRate = 16000; // Target sample rate for output
    uint16_t    targetBitDepth   = 16;    // Target bit depth for output
    uint16_t    targetChannels   = 1;     // Target channel count per source
    bool        captureMic       = true;  // Also capture microphone (both modes)
    bool        captureSpeaker   = true;  // Capture system audio (always true)
    bool        autoStart        = true;  // Start capture on driver launch

    // ── Logging ──────────────────────────────────────────────
    std::string logLevel = "info";        // trace, debug, info, warn, error
    bool        logAudioDiagnostics = true; // Log chunk counts, sizes, device info

    // ── Derived ──────────────────────────────────────────────

    bool isGatewayMode() const { return mode == Mode::Gateway; }
    bool isLocalMode() const { return mode == Mode::Local; }

    /** Whether gateway mode config is complete enough to attempt login. */
    bool hasGatewayConfig() const {
        return !backendUrl.empty() && !email.empty();
    }

    /** Whether local mode config is usable. */
    bool hasLocalConfig() const {
        return !localSocketPath.empty() || localPort > 0;
    }

    /** REST auth endpoint. */
    std::string authEndpoint() const {
        return backendUrl + "/auth/login";
    }

    /** REST session creation endpoint (on gateway). */
    std::string sessionEndpoint() const {
        // Convert ws(s):// to http(s)://
        std::string url = gatewayUrl;
        if (url.find("wss://") == 0)     url = "https://" + url.substr(6);
        else if (url.find("ws://") == 0)  url = "http://" + url.substr(5);
        return url + "/api/sessions";
    }

    /** WebSocket audio URL template (sessionId appended at runtime). */
    std::string audioEndpointBase() const {
        return gatewayUrl + "/audio/";
    }

    /** Default local socket path for this platform. */
    static std::string defaultLocalSocket() {
#if defined(__APPLE__) || defined(__linux__)
        return "/tmp/audio-driver.sock";
#elif defined(_WIN32)
        return "\\\\.\\pipe\\audio-driver";
#endif
        return "";
    }

    // ── Serialization ────────────────────────────────────────
    // Security: password, token, and apiKey are NEVER written to disk.

    std::string serialize() const {
        std::ostringstream ss;
        ss << "[mode]\n";
        ss << "mode=" << (mode == Mode::Gateway ? "gateway" : "local") << "\n";
        ss << "\n[auth]\n";
        ss << "backend_url=" << backendUrl << "\n";
        ss << "email=" << email << "\n";
        ss << "# password is never saved — enter at runtime\n";
        ss << "\n[gateway]\n";
        ss << "url=" << gatewayUrl << "\n";
        ss << "\n[local]\n";
        ss << "socket_path=" << localSocketPath << "\n";
        ss << "port=" << localPort << "\n";
        ss << "\n[audio]\n";
        ss << "preferred_device=" << preferredDevice << "\n";
        ss << "chunk_interval_ms=" << chunkIntervalMs << "\n";
        ss << "target_sample_rate=" << targetSampleRate << "\n";
        ss << "target_bit_depth=" << targetBitDepth << "\n";
        ss << "target_channels=" << targetChannels << "\n";
        ss << "capture_mic=" << (captureMic ? "true" : "false") << "\n";
        ss << "capture_speaker=" << (captureSpeaker ? "true" : "false") << "\n";
        ss << "auto_start=" << (autoStart ? "true" : "false") << "\n";
        ss << "\n[logging]\n";
        ss << "log_level=" << logLevel << "\n";
        ss << "log_audio_diagnostics=" << (logAudioDiagnostics ? "true" : "false") << "\n";
        return ss.str();
    }

    static DriverConfig deserialize(const std::string& content) {
        DriverConfig cfg;
        std::istringstream stream(content);
        std::string line;

        while (std::getline(stream, line)) {
            if (line.empty() || line[0] == '#' || line[0] == '[') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            while (!key.empty() && key.back() == ' ') key.pop_back();
            while (!val.empty() && val.front() == ' ') val.erase(val.begin());

            if      (key == "mode")                cfg.mode = (val == "local" ? Mode::Local : Mode::Gateway);
            else if (key == "backend_url")         cfg.backendUrl = val;
            else if (key == "email")               cfg.email = val;
            else if (key == "url")                 cfg.gatewayUrl = val;
            else if (key == "socket_path")         cfg.localSocketPath = val;
            else if (key == "port")                cfg.localPort = static_cast<uint16_t>(std::stoul(val));
            else if (key == "preferred_device")    cfg.preferredDevice = val;
            else if (key == "chunk_interval_ms")   cfg.chunkIntervalMs = std::stoul(val);
            else if (key == "target_sample_rate")  cfg.targetSampleRate = std::stoul(val);
            else if (key == "target_bit_depth")    cfg.targetBitDepth = static_cast<uint16_t>(std::stoul(val));
            else if (key == "target_channels")     cfg.targetChannels = static_cast<uint16_t>(std::stoul(val));
            else if (key == "capture_mic")         cfg.captureMic = (val == "true");
            else if (key == "capture_speaker")     cfg.captureSpeaker = (val == "true");
            else if (key == "auto_start")          cfg.autoStart = (val == "true");
            else if (key == "log_level")           cfg.logLevel = val;
            else if (key == "log_audio_diagnostics") cfg.logAudioDiagnostics = (val == "true");
        }

        return cfg;
    }

    static std::string configDir() {
#if defined(__APPLE__)
        const char* home = std::getenv("HOME");
        return std::string(home ? home : "/tmp") + "/Library/Application Support/AudioDriver";
#elif defined(_WIN32)
        const char* appdata = std::getenv("APPDATA");
        return std::string(appdata ? appdata : "C:\\") + "\\AudioDriver";
#else
        const char* home = std::getenv("HOME");
        return std::string(home ? home : "/tmp") + "/.config/audio-driver";
#endif
    }

    static std::string configFilePath() {
        return configDir() + "/config.ini";
    }

    static std::string logDir() {
        return configDir() + "/logs";
    }
};

} // namespace sulla
