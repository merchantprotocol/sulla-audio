/**
 * sulla-audio-driver — standalone system audio capture agent.
 *
 * Dual-mode operation:
 *   Gateway mode: Authenticates via email/password, streams mic + speaker
 *                 audio directly to the enterprise gateway.
 *   Local mode:   Streams via local IPC to Sulla Desktop (no auth needed).
 *
 * Usage:
 *   sulla-audio-driver                         # Uses saved config
 *   sulla-audio-driver --list-devices          # Lists available audio devices
 *   sulla-audio-driver --mode gateway          # Gateway mode (default)
 *   sulla-audio-driver --mode local            # Local mode (Sulla Desktop)
 *   sulla-audio-driver --backend-url URL       # Backend API URL
 *   sulla-audio-driver --email EMAIL           # Login email
 *   sulla-audio-driver --device ID             # Preferred audio device
 */

#include <sulla/DriverConfig.h>
#include <sulla/DeviceController.h>
#include <sulla/CaptureController.h>
#include <sulla/DriverController.h>
#include <sulla/IDeviceEnumerator.h>
#include <sulla/ICaptureBackend.h>
#include <sulla/IGatewayClient.h>
#include <sulla/IAuthClient.h>
#include <sulla/ILocalTransport.h>
#include <sulla/PlatformDetector.h>
#include <sulla/AudioFormat.h>
#include <sulla/Logger.h>

#include <iostream>
#include <fstream>
#include <string>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

static std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running.store(false);
}

// ─── Config persistence ──────────────────────────────────────

sulla::DriverConfig loadConfig() {
    std::string path = sulla::DriverConfig::configFilePath();
    std::ifstream file(path);
    if (!file.is_open()) return {};

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    return sulla::DriverConfig::deserialize(content);
}

bool saveConfig(const sulla::DriverConfig& config) {
    std::string dir = sulla::DriverConfig::configDir();

#ifdef _WIN32
    system(("mkdir \"" + dir + "\" 2>nul").c_str());
#else
    system(("mkdir -p '" + dir + "' 2>/dev/null").c_str());
#endif

    std::ofstream file(sulla::DriverConfig::configFilePath());
    if (!file.is_open()) return false;
    file << config.serialize();
    return true;
}

// ─── Password prompt ─────────────────────────────────────────

std::string promptPassword(const std::string& email) {
    std::cout << "Password for " << email << ": " << std::flush;

    // Disable echo for password input
#ifndef _WIN32
    system("stty -echo");
#endif

    std::string password;
    std::getline(std::cin, password);

#ifndef _WIN32
    system("stty echo");
#endif

    std::cout << "\n";
    return password;
}

// ─── Commands ────────────────────────────────────────────────

void listDevices() {
    auto enumerator = sulla::IDeviceEnumerator::create();
    if (!enumerator) {
        std::cerr << "Error: Platform not supported\n";
        return;
    }

    auto devices = enumerator->listOutputDevices();
    std::cout << "Audio output devices (" << devices.size() << "):\n\n";
    for (const auto& dev : devices) {
        std::cout << "  " << (dev.isDefault ? "* " : "  ")
                  << dev.name << "\n"
                  << "    ID: " << dev.id << "\n"
                  << "    Format: " << dev.nativeFormat.toString() << "\n"
                  << "    Loopback: " << (dev.isLoopbackCapable ? "yes" : "no") << "\n"
                  << "\n";
    }

    if (sulla::PlatformDetector::needsVirtualDevice()) {
        bool hasVirtual = false;
        for (const auto& dev : devices) {
            if (dev.isLoopbackCapable) { hasVirtual = true; break; }
        }
        if (!hasVirtual) {
            std::cout << "  No virtual audio device found.\n"
                      << "  Install SullaAudioDevice.driver or BlackHole for loopback capture.\n\n";
        }
    }
}

void configure(sulla::DriverConfig& config, int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            std::string mode = argv[++i];
            config.mode = (mode == "local")
                ? sulla::DriverConfig::Mode::Local
                : sulla::DriverConfig::Mode::Gateway;
        } else if (arg == "--backend-url" && i + 1 < argc) {
            config.backendUrl = argv[++i];
        } else if (arg == "--email" && i + 1 < argc) {
            config.email = argv[++i];
        } else if (arg == "--gateway-url" && i + 1 < argc) {
            config.gatewayUrl = argv[++i];
        } else if (arg == "--socket" && i + 1 < argc) {
            config.localSocketPath = argv[++i];
        } else if (arg == "--local-port" && i + 1 < argc) {
            config.localPort = static_cast<uint16_t>(std::stoul(argv[++i]));
        } else if (arg == "--device" && i + 1 < argc) {
            config.preferredDevice = argv[++i];
        } else if (arg == "--chunk-ms" && i + 1 < argc) {
            config.chunkIntervalMs = std::stoul(argv[++i]);
        } else if (arg == "--log-level" && i + 1 < argc) {
            config.logLevel = argv[++i];
        } else if (arg == "--no-mic") {
            config.captureMic = false;
        } else if (arg == "--no-auto-start") {
            config.autoStart = false;
        }
    }

    saveConfig(config);
}

int runDriver(sulla::DriverConfig& config) {
    std::cout << "sulla-audio-driver v0.1.0 (" << sulla::PlatformDetector::osName() << ")\n";
    std::cout << "Mode: " << (config.isGatewayMode() ? "gateway" : "local") << "\n";

    // Build the object graph
    auto enumerator = sulla::IDeviceEnumerator::create();
    auto speakerBackend = sulla::ICaptureBackend::create();

    if (!enumerator || !speakerBackend) {
        std::cerr << "Error: Failed to create platform components\n";
        return 1;
    }

    auto deviceCtrl = std::make_unique<sulla::DeviceController>(std::move(enumerator));

    sulla::CaptureController::Config speakerCaptureConfig;
    speakerCaptureConfig.targetFormat = sulla::AudioFormat::telephony();
    speakerCaptureConfig.chunkIntervalMs = config.chunkIntervalMs;
    speakerCaptureConfig.channel = 1; // Speaker = channel 1

    auto speakerCapture = std::make_unique<sulla::CaptureController>(
        std::move(speakerBackend), speakerCaptureConfig
    );

    // Mic capture (separate backend)
    std::unique_ptr<sulla::CaptureController> micCapture;
    if (config.captureMic) {
        // TODO: mic capture needs input device backend
        // For now, mic is provided by Sulla Desktop in local mode
    }

    auto gateway = sulla::IGatewayClient::create();
    auto authClient = sulla::IAuthClient::create();
    auto localTransport = sulla::ILocalTransport::create();

    auto driver = std::make_unique<sulla::DriverController>(
        std::move(deviceCtrl),
        std::move(speakerCapture),
        std::move(micCapture),
        std::move(gateway),
        std::move(authClient),
        std::move(localTransport)
    );

    driver->onStatus([](sulla::DriverController::DriverState state, const std::string& msg) {
        std::cout << "[" << static_cast<int>(state) << "] " << msg << "\n";
    });

    // Apply config (may auto-start in local mode)
    driver->applyConfig(config);

    // Gateway mode: login then start
    if (config.isGatewayMode()) {
        if (!config.hasGatewayConfig()) {
            std::cerr << "Error: Gateway mode requires --backend-url and --email\n";
            return 1;
        }

        std::string password = promptPassword(config.email);
        if (password.empty()) {
            std::cerr << "Error: Password is required\n";
            return 1;
        }

        driver->login(password);
    }

    // Run until signal
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nStopping...\n";
    driver->stop();

    return 0;
}

// ─── Entry point ─────────────────────────────────────────────

int main(int argc, char* argv[]) {
    auto config = loadConfig();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list-devices" || arg == "-l") {
            listDevices();
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "sulla-audio-driver — system audio capture agent\n\n"
                      << "Usage:\n"
                      << "  sulla-audio-driver                        Start streaming\n"
                      << "  sulla-audio-driver --list-devices         List audio devices\n"
                      << "  sulla-audio-driver --mode gateway|local   Set operating mode\n"
                      << "  sulla-audio-driver --backend-url URL      Backend API URL\n"
                      << "  sulla-audio-driver --email EMAIL          Login email\n"
                      << "  sulla-audio-driver --gateway-url URL      Override gateway WS URL\n"
                      << "  sulla-audio-driver --socket PATH          Local socket path\n"
                      << "  sulla-audio-driver --local-port PORT      Local TCP port\n"
                      << "  sulla-audio-driver --device ID            Preferred audio device\n"
                      << "  sulla-audio-driver --chunk-ms MS          Chunk interval (default: 200)\n"
                      << "  sulla-audio-driver --log-level LEVEL      trace|debug|info|warn|error\n"
                      << "  sulla-audio-driver --no-mic               Disable mic capture\n"
                      << "  sulla-audio-driver --no-auto-start        Don't auto-start\n"
                      << "\nConfig: " << sulla::DriverConfig::configFilePath() << "\n";
            return 0;
        }
    }

    configure(config, argc, argv);
    return runDriver(config);
}
