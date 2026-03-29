/**
 * audio-driver — standalone system audio capture agent.
 *
 * Dual-mode operation:
 *   Gateway mode: Authenticates via email/password, streams mic + speaker
 *                 audio directly to the enterprise gateway.
 *   Local mode:   Streams via local IPC to Sulla Desktop (no auth needed).
 *
 * Usage:
 *   audio-driver                         # Uses saved config
 *   audio-driver --list-devices          # Lists available audio devices
 *   audio-driver --mode gateway          # Gateway mode (default)
 *   audio-driver --mode local            # Local mode (Sulla Desktop)
 *   audio-driver --backend-url URL       # Backend API URL
 *   audio-driver --email EMAIL           # Login email
 *   audio-driver --device ID             # Preferred audio device
 */

#include <cstdio>
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

#ifndef AUDIO_DRIVER_VERSION
#define AUDIO_DRIVER_VERSION "0.1.0"
#endif
#ifndef AUDIO_DRIVER_COMMIT
#define AUDIO_DRIVER_COMMIT "unknown"
#endif
#ifndef AUDIO_DRIVER_BUILD_DATE
#define AUDIO_DRIVER_BUILD_DATE "unknown"
#endif

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
                      << "  Run the installer to build the SullaLoopback driver for loopback capture.\n\n";
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
    std::cout << "audio-driver v" << AUDIO_DRIVER_VERSION
              << " (" << AUDIO_DRIVER_COMMIT << ", " << sulla::PlatformDetector::osName() << ")\n";
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

    // Check if startup failed (e.g. transport bind error)
    if (driver->state() == sulla::DriverController::DriverState::Error) {
        std::cerr << "Error: Driver failed to start. Check logs above.\n";
        driver->stop();
        return 1;
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
    // Line-buffer stdout/stderr so logs flush immediately when redirected to a file (launchd)
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    // Handle info-only flags before loading config or touching any resources
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            std::cout << "audio-driver v" << AUDIO_DRIVER_VERSION
                      << " (" << AUDIO_DRIVER_COMMIT << ", " << AUDIO_DRIVER_BUILD_DATE << ")\n";
            return 0;
        }
        if (arg == "--list-devices" || arg == "-l") {
            listDevices();
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "audio-driver — system audio capture agent\n\n"
                      << "Usage:\n"
                      << "  audio-driver                        Start streaming\n"
                      << "  audio-driver --list-devices         List audio devices\n"
                      << "  audio-driver --mode gateway|local   Set operating mode\n"
                      << "  audio-driver --backend-url URL      Backend API URL\n"
                      << "  audio-driver --email EMAIL          Login email\n"
                      << "  audio-driver --gateway-url URL      Override gateway WS URL\n"
                      << "  audio-driver --socket PATH          Local socket path\n"
                      << "  audio-driver --local-port PORT      Local TCP port\n"
                      << "  audio-driver --device ID            Preferred audio device\n"
                      << "  audio-driver --chunk-ms MS          Chunk interval (default: 200)\n"
                      << "  audio-driver --log-level LEVEL      trace|debug|info|warn|error\n"
                      << "  audio-driver --no-mic               Disable mic capture\n"
                      << "  audio-driver --no-auto-start        Don't auto-start\n"
                      << "\nVolume control:\n"
                      << "  audio-driver --get-volume           Print current volume (0.0–1.0)\n"
                      << "  audio-driver --set-volume VALUE     Set volume (0.0–1.0)\n"
                      << "  audio-driver --volume-up            Increase volume by one notch\n"
                      << "  audio-driver --volume-down          Decrease volume by one notch\n"
                      << "  audio-driver --mute                 Mute the output\n"
                      << "  audio-driver --unmute               Unmute the output\n"
                      << "\nMirror control:\n"
                      << "  audio-driver --enable               Enable audio mirror (aggregate device)\n"
                      << "  audio-driver --disable              Disable mirror (restore normal volume)\n"
                      << "  audio-driver --status               Show mirror status\n"
                      << "\nConfig: " << sulla::DriverConfig::configFilePath() << "\n";
            return 0;
        }

#ifdef __APPLE__
        if (arg == "--enable") {
            sulla::AudioMirrorManager::enable();
            // Create the mirror immediately
            sulla::AudioMirrorManager mgr;
            if (mgr.start()) {
                std::cout << "Mirror enabled — audio capture active, volume keys disabled\n";
            } else {
                std::cout << "Mirror enabled — will activate when loopback driver is available\n";
            }
            // Detach so mirror stays alive (don't call stop/destructor)
            // Actually for CLI we just enable the flag; the daemon handles the rest
            mgr.stop();
            return 0;
        }
        if (arg == "--disable") {
            sulla::AudioMirrorManager::disable();
            std::cout << "Mirror disabled — volume keys restored\n";
            return 0;
        }
        if (arg == "--status") {
            bool enabled = sulla::AudioMirrorManager::isEnabled();
            AudioDeviceID mirror = sulla::AudioMirrorManager::findDeviceByUID(
                sulla::AudioMirrorManager::kMirrorUID);
            bool mirrorActive = (mirror != kAudioObjectUnknown);
            AudioDeviceID defOut = sulla::AudioMirrorManager::getDefaultOutputDevice();
            std::string defName = sulla::AudioMirrorManager::getDeviceName(defOut);

            std::cout << "Mirror:  " << (enabled ? "enabled" : "disabled") << "\n"
                      << "Active:  " << (mirrorActive ? "yes" : "no") << "\n"
                      << "Output:  " << defName << "\n";

            if (mirrorActive) {
                AudioDeviceID physical = sulla::AudioMirrorManager::getFirstNonBlackholeSubDevice(mirror);
                if (physical != kAudioObjectUnknown) {
                    std::cout << "Wrapped: " << sulla::AudioMirrorManager::getDeviceName(physical) << "\n";
                }
            }
            return 0;
        }

        // Volume CLI commands operate directly on the physical output device.
        // They do NOT create/destroy mirrors — safe to run while the driver daemon is active.
        if (arg == "--get-volume" || arg == "--set-volume" || arg == "--volume-up" ||
            arg == "--volume-down" || arg == "--mute" || arg == "--unmute") {

            // Find the physical output device:
            // If a mirror is active, extract the physical sub-device from it.
            // Otherwise, use the current default output.
            AudioDeviceID physicalDevice = kAudioObjectUnknown;
            AudioDeviceID defaultOut = sulla::AudioMirrorManager::getDefaultOutputDevice();
            std::string defaultUID = sulla::AudioMirrorManager::getDeviceUID(defaultOut);

            if (defaultUID == sulla::AudioMirrorManager::kMirrorUID) {
                // Mirror is active — find the physical sub-device inside it
                physicalDevice = sulla::AudioMirrorManager::getFirstNonBlackholeSubDevice(defaultOut);
            }
            if (physicalDevice == kAudioObjectUnknown) {
                physicalDevice = defaultOut;
            }

            if (physicalDevice == kAudioObjectUnknown) {
                std::cerr << "Error: No output device found\n";
                return 1;
            }

            // Helper lambdas that operate on the physical device directly
            auto getVol = [&]() -> float {
                Float32 volume = 0.0f;
                UInt32 size = sizeof(volume);
                AudioObjectPropertyAddress addr = {
                    kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
                    kAudioObjectPropertyScopeOutput,
                    kAudioObjectPropertyElementMain
                };
                if (AudioHardwareServiceGetPropertyData(physicalDevice, &addr, 0, nullptr, &size, &volume) == noErr)
                    return volume;
                addr.mSelector = kAudioDevicePropertyVolumeScalar;
                addr.mElement = 1;
                if (AudioObjectGetPropertyData(physicalDevice, &addr, 0, nullptr, &size, &volume) == noErr)
                    return volume;
                return -1.0f;
            };

            auto setVol = [&](float v) -> bool {
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                Float32 vol = v;
                AudioObjectPropertyAddress addr = {
                    kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
                    kAudioObjectPropertyScopeOutput,
                    kAudioObjectPropertyElementMain
                };
                return AudioHardwareServiceSetPropertyData(physicalDevice, &addr, 0, nullptr, sizeof(vol), &vol) == noErr;
            };

            if (arg == "--get-volume") {
                float vol = getVol();
                if (vol < 0.0f) { std::cerr << "Error: Could not read volume\n"; return 1; }
                std::cout << vol << "\n";
                return 0;
            }
            if (arg == "--set-volume" && i + 1 < argc) {
                float val = std::stof(argv[++i]);
                if (!setVol(val)) { std::cerr << "Error: Could not set volume\n"; return 1; }
                std::cout << "Volume set to " << val << "\n";
                return 0;
            }
            if (arg == "--volume-up") {
                float cur = getVol();
                if (cur < 0.0f) { std::cerr << "Error: Could not read volume\n"; return 1; }
                float newVol = std::min(cur + 0.0625f, 1.0f);
                if (!setVol(newVol)) { std::cerr << "Error: Could not set volume\n"; return 1; }
                std::cout << newVol << "\n";
                return 0;
            }
            if (arg == "--volume-down") {
                float cur = getVol();
                if (cur < 0.0f) { std::cerr << "Error: Could not read volume\n"; return 1; }
                float newVol = std::max(cur - 0.0625f, 0.0f);
                if (!setVol(newVol)) { std::cerr << "Error: Could not set volume\n"; return 1; }
                std::cout << newVol << "\n";
                return 0;
            }
            if (arg == "--mute") {
                AudioObjectPropertyAddress addr = { kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
                UInt32 muted = 1;
                if (AudioObjectSetPropertyData(physicalDevice, &addr, 0, nullptr, sizeof(muted), &muted) != noErr) {
                    std::cerr << "Error: Could not mute\n"; return 1;
                }
                std::cout << "Muted\n";
                return 0;
            }
            if (arg == "--unmute") {
                AudioObjectPropertyAddress addr = { kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
                UInt32 muted = 0;
                if (AudioObjectSetPropertyData(physicalDevice, &addr, 0, nullptr, sizeof(muted), &muted) != noErr) {
                    std::cerr << "Error: Could not unmute\n"; return 1;
                }
                std::cout << "Unmuted\n";
                return 0;
            }
        }
#endif
    }

    auto config = loadConfig();
    configure(config, argc, argv);
    return runDriver(config);
}
