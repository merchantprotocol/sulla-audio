#pragma once

#ifdef __APPLE__

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioServices.h>
#include <functional>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <fstream>
#include <sys/stat.h>
#include "Logger.h"

namespace sulla {

/**
 * AudioMirrorManager — dynamically manages a Multi-Output Device
 * that mirrors the user's active output to the loopback driver for capture.
 *
 * When a CSR switches from speakers to a headset (or any output change),
 * this class detects it via a CoreAudio property listener, tears down
 * the old Multi-Output Device, and creates a new one combining the
 * new output + loopback driver. Then sets it as default output so audio
 * continues flowing to both the user's device and the loopback driver.
 *
 * Lifecycle:
 *   start() — installs the listener, creates initial mirror
 *   stop()  — removes the listener, destroys the mirror, restores output
 */
class AudioMirrorManager {
public:
    static constexpr const char* kMirrorNameSuffix = " + LoopBack";
    static constexpr const char* kMirrorUID  = "SullaAudioMirror_UID";
    static constexpr const char* kLoopbackUIDs[] = {
        "BlackHole2ch_UID",       // Official BlackHole 2ch (signed, via Homebrew)
        "SullaLoopback2ch_UID",   // Custom build (when signed with Developer ID)
    };

    using DeviceChangedCallback = std::function<void(AudioDeviceID loopbackDeviceId)>;

    static constexpr int kWatchdogIntervalSeconds = 3;
    static constexpr int kSilenceThresholdChunks  = 8; // ~2s of silence before watchdog kicks in

    AudioMirrorManager() = default;
    ~AudioMirrorManager() { stop(); }

    // ─── Enable / Disable (flag file) ──────────────────────────

    /** Path to the flag file that controls whether the mirror is enabled. */
    static std::string enabledFlagPath() {
#if defined(__APPLE__)
        const char* home = std::getenv("HOME");
        return std::string(home ? home : "/tmp") + "/Library/Application Support/AudioDriver/mirror-enabled";
#else
        const char* home = std::getenv("HOME");
        return std::string(home ? home : "/tmp") + "/.config/audio-driver/mirror-enabled";
#endif
    }

    /** Check if the mirror is enabled (flag file exists). Enabled by default if no flag file. */
    static bool isEnabled() {
        struct stat st;
        std::string disabledPath = disabledFlagPath();
        // Mirror is enabled unless a "mirror-disabled" flag exists
        return stat(disabledPath.c_str(), &st) != 0;
    }

    /** Enable the mirror — remove the disabled flag and create/restore the aggregate. */
    static void enable() {
        ::unlink(disabledFlagPath().c_str());
        SULLA_LOG_INFO("Mirror", "Enabled — mirror will be created on next start/rebuild");
    }

    /** Disable the mirror — set the disabled flag and destroy any existing aggregate. */
    static void disable() {
        // Create config dir if needed
        std::string dir;
#if defined(__APPLE__)
        const char* home = std::getenv("HOME");
        dir = std::string(home ? home : "/tmp") + "/Library/Application Support/AudioDriver";
#else
        const char* home = std::getenv("HOME");
        dir = std::string(home ? home : "/tmp") + "/.config/audio-driver";
#endif
        ::mkdir(dir.c_str(), 0755);

        // Write disabled flag
        std::ofstream f(disabledFlagPath());
        f << "disabled\n";
        f.close();

        // Destroy any existing mirror aggregate so the physical device becomes default
        AudioDeviceID orphan = findDeviceByUID(kMirrorUID);
        if (orphan != kAudioObjectUnknown) {
            // Restore default output to the physical sub-device first
            AudioDeviceID physical = getFirstNonBlackholeSubDevice(orphan);
            if (physical != kAudioObjectUnknown) {
                setDefaultOutputDevice(physical);
                SULLA_LOG_INFO("Mirror", "Restored default output to: " + getDeviceName(physical));
            }
            AudioHardwareDestroyAggregateDevice(orphan);
            SULLA_LOG_INFO("Mirror", "Destroyed mirror aggregate");
        }

        SULLA_LOG_INFO("Mirror", "Disabled — volume keys will work normally");
    }

    /** Get the UID that was actually matched at startup. */
    const char* activeLoopbackUID() const { return activeLoopbackUID_; }

    /**
     * Start managing the audio mirror.
     * Finds the loopback driver, creates the initial Multi-Output Device,
     * and installs a listener for output device changes.
     */
    bool start() {
        std::lock_guard<std::mutex> lock(mutex_);

        // Find loopback driver — try SullaLoopback first, fall back to BlackHole
        for (const char* uid : kLoopbackUIDs) {
            loopbackId_ = findDeviceByUID(uid);
            if (loopbackId_ != kAudioObjectUnknown) {
                activeLoopbackUID_ = uid;
                break;
            }
        }
        if (loopbackId_ == kAudioObjectUnknown) {
            SULLA_LOG_ERROR("Mirror", "No loopback driver found (tried SullaLoopback2ch, BlackHole2ch) — cannot create audio mirror");
            return false;
        }
        SULLA_LOG_INFO("Mirror", "Found loopback driver '" + std::string(activeLoopbackUID_) + "' (ID: " + std::to_string(loopbackId_) + ")");

        // Create the initial mirror (retry — CoreAudio may not be ready at boot)
        bool mirrorOk = false;
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (rebuildMirror()) {
                mirrorOk = true;
                break;
            }
            if (attempt < 4) {
                SULLA_LOG_WARN("Mirror", "Mirror creation failed (attempt " + std::to_string(attempt + 1) + "/5), retrying in 2 seconds...");
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
        if (!mirrorOk) {
            SULLA_LOG_WARN("Mirror", "All mirror creation attempts failed — will retry on output change");
        }

        // Install listener for default output changes
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        OSStatus status = AudioObjectAddPropertyListener(
            kAudioObjectSystemObject, &addr, outputChangedCallback, this
        );
        if (status != noErr) {
            SULLA_LOG_ERROR("Mirror", "Failed to install output device listener: " + std::to_string(status));
            return false;
        }

        // Also listen for device list changes (connect/disconnect headsets, etc.)
        // When a sub-device disappears from our mirror, we need to rebuild it
        AudioObjectPropertyAddress devListAddr = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectAddPropertyListener(
            kAudioObjectSystemObject, &devListAddr, deviceListChangedCallback, this
        );

        listening_ = true;
        SULLA_LOG_INFO("Mirror", "Listening for output device changes");

        return true;
    }

    /**
     * Start the watchdog thread. Called by DriverController when a client
     * is connected and sustained silence is detected — indicates macOS
     * may have silently switched away from our private mirror.
     *
     * Safe to call multiple times; only starts one thread.
     */
    void startWatchdog() {
        if (watchdogRunning_) return;
        watchdogRunning_ = true;
        watchdogThread_ = std::thread([this]() {
            SULLA_LOG_INFO("Mirror", "Watchdog started — checking mirror every " + std::to_string(kWatchdogIntervalSeconds) + "s");
            while (watchdogRunning_) {
                std::this_thread::sleep_for(std::chrono::seconds(kWatchdogIntervalSeconds));
                if (!watchdogRunning_) break;
                checkMirrorHealth();
            }
            SULLA_LOG_DEBUG("Mirror", "Watchdog stopped");
        });
    }

    /**
     * Stop the watchdog thread. Called when audio is confirmed flowing
     * or all clients have disconnected.
     */
    void stopWatchdog() {
        if (!watchdogRunning_) return;
        watchdogRunning_ = false;
        if (watchdogThread_.joinable()) {
            watchdogThread_.join();
        }
        SULLA_LOG_DEBUG("Mirror", "Watchdog stopped by controller");
    }

    /**
     * Stop managing — remove listener, destroy mirror, restore original output.
     */
    void stop() {
        // Stop watchdog first (outside mutex — thread may be waiting on lock)
        stopWatchdog();

        std::lock_guard<std::mutex> lock(mutex_);

        if (listening_) {
            AudioObjectPropertyAddress addr = {
                kAudioHardwarePropertyDefaultOutputDevice,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            AudioObjectRemovePropertyListener(
                kAudioObjectSystemObject, &addr, outputChangedCallback, this
            );
            AudioObjectPropertyAddress devListAddr = {
                kAudioHardwarePropertyDevices,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            AudioObjectRemovePropertyListener(
                kAudioObjectSystemObject, &devListAddr, deviceListChangedCallback, this
            );
            listening_ = false;
        }

        destroyMirror();
    }

    /** Called when capture should re-initialize after a mirror rebuild. */
    void onDeviceChanged(DeviceChangedCallback cb) { deviceChangedCb_ = std::move(cb); }

    /** Get the loopback device ID (for capture backend to use). */
    AudioDeviceID loopbackDeviceId() const { return loopbackId_; }

    /** Get the current mirror device ID. */
    AudioDeviceID mirrorDeviceId() const { return mirrorId_; }

    /** Get the physical output device ID that the mirror wraps. */
    AudioDeviceID physicalOutputDeviceId() const { return lastPhysicalOutput_; }

    /**
     * Get the volume of the wrapped physical output device.
     * Returns a value between 0.0 (muted) and 1.0 (max), or -1.0 on error.
     *
     * macOS disables the volume slider for multi-output aggregate devices,
     * so this method reads volume directly from the physical sub-device.
     */
    float getVolume() const {
        AudioDeviceID device = lastPhysicalOutput_;
        if (device == kAudioObjectUnknown) {
            SULLA_LOG_WARN("Mirror", "getVolume: no physical output device");
            return -1.0f;
        }

        Float32 volume = 0.0f;
        UInt32 size = sizeof(volume);

        // Use AudioHardwareService API — this is how macOS controls volume
        // for built-in speakers. AudioObject API doesn't work for these devices.
        AudioObjectPropertyAddress addr = {
            kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        OSStatus status = AudioHardwareServiceGetPropertyData(device, &addr, 0, nullptr, &size, &volume);
        if (status == noErr) {
            SULLA_LOG_DEBUG("Mirror", "getVolume: " + std::to_string(volume) + " via Service API");
            return volume;
        }
        SULLA_LOG_DEBUG("Mirror", "getVolume: Service API failed (" + std::to_string(status) + "), trying HAL API");

        // Fall back to HAL API for external audio devices
        addr.mSelector = kAudioDevicePropertyVolumeScalar;
        for (UInt32 ch : {0u, 1u}) {
            addr.mElement = ch;
            status = AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &volume);
            if (status == noErr) {
                SULLA_LOG_DEBUG("Mirror", "getVolume: " + std::to_string(volume) + " via HAL ch" + std::to_string(ch));
                return volume;
            }
        }

        SULLA_LOG_WARN("Mirror", "getVolume: all methods failed on device "
            + std::to_string(device) + " (" + getDeviceName(device) + ")");
        return -1.0f;
    }

    /**
     * Set the volume of the wrapped physical output device.
     * @param volume Value between 0.0 (muted) and 1.0 (max).
     * @return true on success.
     */
    bool setVolume(float volume) {
        AudioDeviceID device = lastPhysicalOutput_;
        if (device == kAudioObjectUnknown) {
            SULLA_LOG_WARN("Mirror", "setVolume: no physical output device");
            return false;
        }
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 1.0f) volume = 1.0f;

        Float32 vol = volume;

        // Use AudioHardwareService API — this is what macOS volume slider uses
        AudioObjectPropertyAddress addr = {
            kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        OSStatus status = AudioHardwareServiceSetPropertyData(device, &addr, 0, nullptr, sizeof(vol), &vol);
        if (status == noErr) {
            SULLA_LOG_DEBUG("Mirror", "setVolume: " + std::to_string(volume) + " via Service API");
            return true;
        }
        SULLA_LOG_DEBUG("Mirror", "setVolume: Service API failed (" + std::to_string(status) + "), trying HAL API");

        // Fall back to HAL API per-channel for external devices
        addr.mSelector = kAudioDevicePropertyVolumeScalar;
        bool anySet = false;
        for (UInt32 ch = 0; ch <= 2; ++ch) {
            addr.mElement = ch;
            Boolean settable = false;
            if (AudioObjectHasProperty(device, &addr) &&
                AudioObjectIsPropertySettable(device, &addr, &settable) == noErr &&
                settable) {
                status = AudioObjectSetPropertyData(device, &addr, 0, nullptr, sizeof(vol), &vol);
                if (status == noErr) {
                    anySet = true;
                    SULLA_LOG_DEBUG("Mirror", "setVolume ch" + std::to_string(ch) + ": " + std::to_string(volume));
                }
            }
        }

        if (!anySet) {
            SULLA_LOG_WARN("Mirror", "setVolume: all methods failed on device "
                + std::to_string(device) + " (" + getDeviceName(device) + ")");
        }
        return anySet;
    }

    /**
     * Adjust volume by a relative amount (e.g., +0.0625 to increase, -0.0625 to decrease).
     * @return The new volume level, or -1.0 on error.
     */
    float adjustVolume(float delta) {
        float current = getVolume();
        if (current < 0.0f) {
            SULLA_LOG_WARN("Mirror", "adjustVolume: getVolume failed");
            return -1.0f;
        }
        float newVol = current + delta;
        if (newVol < 0.0f) newVol = 0.0f;
        if (newVol > 1.0f) newVol = 1.0f;
        SULLA_LOG_INFO("Mirror", "Volume: " + std::to_string(current) + " → " + std::to_string(newVol));
        if (setVolume(newVol)) return newVol;
        return -1.0f;
    }

    /**
     * Check if the wrapped physical device is muted.
     * @return 1 = muted, 0 = not muted, -1 = error/unknown.
     */
    int isMuted() const {
        AudioDeviceID device = lastPhysicalOutput_;
        if (device == kAudioObjectUnknown) return -1;

        AudioObjectPropertyAddress addr = {
            kAudioDevicePropertyMute,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 muted = 0;
        UInt32 size = sizeof(muted);
        OSStatus status = AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &muted);
        if (status != noErr) return -1;
        return (int)muted;
    }

    /**
     * Set the mute state of the wrapped physical device.
     */
    bool setMuted(bool mute) {
        AudioDeviceID device = lastPhysicalOutput_;
        if (device == kAudioObjectUnknown) return false;

        AudioObjectPropertyAddress addr = {
            kAudioDevicePropertyMute,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 muted = mute ? 1 : 0;
        OSStatus status = AudioObjectSetPropertyData(device, &addr, 0, nullptr, sizeof(muted), &muted);
        if (status == noErr) {
            SULLA_LOG_DEBUG("Mirror", std::string(mute ? "Muted" : "Unmuted") + " physical output");
        }
        return status == noErr;
    }

private:
    std::mutex mutex_;
    AudioDeviceID loopbackId_ = kAudioObjectUnknown;
    AudioDeviceID mirrorId_ = kAudioObjectUnknown;
    const char* activeLoopbackUID_ = nullptr;
    AudioDeviceID lastPhysicalOutput_ = kAudioObjectUnknown;
    bool listening_ = false;
    bool rebuilding_ = false;
    DeviceChangedCallback deviceChangedCb_;
    std::atomic<bool> watchdogRunning_{false};
    std::thread watchdogThread_;

    /** Check if a UID matches any known loopback driver. */
    static bool isLoopbackUID(const std::string& uid) {
        for (const char* known : kLoopbackUIDs) {
            if (uid == known) return true;
        }
        return false;
    }

    /**
     * CoreAudio callback — fires when the default output device changes.
     */
    static OSStatus outputChangedCallback(
        AudioObjectID /*inObjectID*/,
        UInt32 /*inNumberAddresses*/,
        const AudioObjectPropertyAddress* /*inAddresses*/,
        void* inClientData
    ) {
        auto* self = static_cast<AudioMirrorManager*>(inClientData);
        self->handleOutputChange();
        return noErr;
    }

    /**
     * CoreAudio callback — fires when devices are added or removed.
     * Handles headset disconnect where the mirror's sub-device disappears
     * but the default output doesn't change (it stays on the broken mirror).
     */
    static OSStatus deviceListChangedCallback(
        AudioObjectID /*inObjectID*/,
        UInt32 /*inNumberAddresses*/,
        const AudioObjectPropertyAddress* /*inAddresses*/,
        void* inClientData
    ) {
        auto* self = static_cast<AudioMirrorManager*>(inClientData);
        self->handleDeviceListChange();
        return noErr;
    }

    void handleOutputChange() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (rebuilding_) return; // Ignore changes we caused

        AudioDeviceID currentDefault = getDefaultOutputDevice();
        if (currentDefault == kAudioObjectUnknown) return;

        // If the default is already our mirror, nothing to do
        std::string currentUID = getDeviceUID(currentDefault);
        if (currentUID == kMirrorUID) return;

        // If it's the loopback driver itself, don't mirror (would create a loop)
        if (isLoopbackUID(currentUID)) {
            SULLA_LOG_WARN("Mirror", "Default output switched to loopback driver — skipping mirror rebuild");
            return;
        }

        // The user switched to a new physical device
        std::string deviceName = getDeviceName(currentDefault);
        SULLA_LOG_INFO("Mirror", "Output changed to: " + deviceName + " (ID: " + std::to_string(currentDefault) + ")");

        // Rebuild the mirror with the new device
        lastPhysicalOutput_ = currentDefault;
        rebuildMirror();
    }

    void handleDeviceListChange() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (rebuilding_) return;
        if (mirrorId_ == kAudioObjectUnknown) return;

        // Check if our mirror's physical sub-device still exists
        if (lastPhysicalOutput_ != kAudioObjectUnknown) {
            std::string uid = getDeviceUID(lastPhysicalOutput_);
            if (!uid.empty()) return; // Sub-device still exists, mirror is healthy
        }

        // The physical output device disappeared (headset disconnected, etc.)
        // Find a new physical output to use
        SULLA_LOG_WARN("Mirror", "Physical output device disappeared — rebuilding mirror");

        // Destroy the broken mirror first so macOS picks a real device as default
        destroyMirrorNoLock();

        // Give macOS a moment to settle the default output, then rebuild
        // The output change listener will fire and handle the rebuild
        // But if the default lands on the loopback driver, we need to force it
        AudioDeviceID newDefault = getDefaultOutputDevice();
        std::string newUID = getDeviceUID(newDefault);

        if (isLoopbackUID(newUID) || newUID == kMirrorUID || newUID.empty()) {
            // macOS fell back to loopback driver or nothing — find a real device
            AudioDeviceID fallback = findFallbackOutputDevice();
            if (fallback != kAudioObjectUnknown) {
                setDefaultOutputDevice(fallback);
                SULLA_LOG_INFO("Mirror", "Fell back to: " + getDeviceName(fallback));
                lastPhysicalOutput_ = fallback;
            }
        } else {
            lastPhysicalOutput_ = newDefault;
            SULLA_LOG_INFO("Mirror", "macOS switched to: " + getDeviceName(newDefault));
        }

        rebuildMirror();
    }

    /**
     * Watchdog health check — called periodically to catch cases where
     * macOS silently switches away from our private mirror without
     * firing a property change callback (sleep/wake, other apps, etc.).
     *
     * If the default output is no longer our mirror, treat the current
     * default as the user's intended device and rebuild the mirror around it.
     */
    void checkMirrorHealth() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (rebuilding_ || !listening_) return;

        AudioDeviceID currentDefault = getDefaultOutputDevice();
        if (currentDefault == kAudioObjectUnknown) return;

        std::string currentUID = getDeviceUID(currentDefault);

        // Mirror is still the default — all good
        if (currentUID == kMirrorUID) return;

        // Default switched to the loopback driver itself — skip (would loop)
        if (isLoopbackUID(currentUID)) return;

        // macOS silently switched away from the mirror.
        // Treat the current default as the user's chosen physical device.
        std::string deviceName = getDeviceName(currentDefault);
        SULLA_LOG_WARN("Mirror", "Watchdog: default output is no longer mirror — now: " + deviceName + " (ID: " + std::to_string(currentDefault) + "). Rebuilding.");

        lastPhysicalOutput_ = currentDefault;
        rebuildMirror();
    }

    /**
     * Destroy old mirror (if any), create a new one with the current
     * physical output + loopback driver, set it as default.
     */
    bool rebuildMirror() {
        // Check if mirror is disabled by user
        if (!isEnabled()) {
            SULLA_LOG_DEBUG("Mirror", "Mirror is disabled — skipping rebuild");
            rebuilding_ = false;
            return false;
        }

        rebuilding_ = true;

        // Determine the physical output device to mirror
        AudioDeviceID physicalOutput = lastPhysicalOutput_;
        if (physicalOutput == kAudioObjectUnknown) {
            physicalOutput = getDefaultOutputDevice();
            if (physicalOutput == kAudioObjectUnknown) {
                rebuilding_ = false;
                return false;
            }
            // Make sure it's not already our mirror or the loopback driver
            std::string uid = getDeviceUID(physicalOutput);
            if (uid == kMirrorUID) {
                // Default output is already our mirror — destroy it and rebuild
                // so we pick up any configuration changes (e.g. master sub-device).
                // Extract the physical sub-device first so we know what to mirror.
                AudioDeviceID existingMirror = findDeviceByUID(kMirrorUID);
                AudioDeviceID physicalSub = getFirstNonBlackholeSubDevice(existingMirror);
                if (physicalSub != kAudioObjectUnknown) {
                    lastPhysicalOutput_ = physicalSub;
                    physicalOutput = physicalSub;
                    SULLA_LOG_INFO("Mirror", "Existing mirror found — rebuilding with physical device: " + getDeviceName(physicalSub));
                } else {
                    // Can't determine physical sub-device, find a fallback
                    physicalOutput = findFallbackOutputDevice();
                    if (physicalOutput == kAudioObjectUnknown) {
                        SULLA_LOG_WARN("Mirror", "Cannot determine physical output — adopting existing mirror");
                        mirrorId_ = existingMirror;
                        rebuilding_ = false;
                        return true;
                    }
                    lastPhysicalOutput_ = physicalOutput;
                }
                // Fall through to destroy + recreate below
            } else if (isLoopbackUID(uid)) {
                rebuilding_ = false;
                return false;
            }
            lastPhysicalOutput_ = physicalOutput;
        }

        // Destroy existing mirror (including any stale one from a previous run)
        destroyMirrorNoLock();
        // Also check for orphaned mirror from installer or previous crash
        AudioDeviceID orphan = findDeviceByUID(kMirrorUID);
        if (orphan != kAudioObjectUnknown) {
            SULLA_LOG_INFO("Mirror", "Destroying orphaned mirror device (ID: " + std::to_string(orphan) + ")");
            AudioHardwareDestroyAggregateDevice(orphan);
        }

        std::string physicalUID = getDeviceUID(physicalOutput);
        std::string physicalName = getDeviceName(physicalOutput);

        if (physicalUID.empty()) {
            SULLA_LOG_ERROR("Mirror", "Could not get UID for output device " + std::to_string(physicalOutput));
            rebuilding_ = false;
            return false;
        }

        std::string mirrorName = physicalName + kMirrorNameSuffix;
        SULLA_LOG_INFO("Mirror", "Creating mirror: " + mirrorName);

        // Build sub-device list: physical output first (main), loopback second
        CFMutableArrayRef subDevices = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

        CFMutableDictionaryRef physDict = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFStringRef physUIDRef = CFStringCreateWithCString(kCFAllocatorDefault, physicalUID.c_str(), kCFStringEncodingUTF8);
        CFDictionarySetValue(physDict, CFSTR(kAudioSubDeviceUIDKey), physUIDRef);
        CFArrayAppendValue(subDevices, physDict);
        CFRelease(physUIDRef);
        CFRelease(physDict);

        CFMutableDictionaryRef bhDict = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFStringRef bhUIDRef = CFStringCreateWithCString(kCFAllocatorDefault, activeLoopbackUID_, kCFStringEncodingUTF8);
        CFDictionarySetValue(bhDict, CFSTR(kAudioSubDeviceUIDKey), bhUIDRef);
        CFArrayAppendValue(subDevices, bhDict);
        CFRelease(bhUIDRef);
        CFRelease(bhDict);

        // Build the aggregate device description
        CFMutableDictionaryRef desc = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

        CFStringRef nameRef = CFStringCreateWithCString(kCFAllocatorDefault, mirrorName.c_str(), kCFStringEncodingUTF8);
        CFStringRef uidRef = CFStringCreateWithCString(kCFAllocatorDefault, kMirrorUID, kCFStringEncodingUTF8);

        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), nameRef);
        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), uidRef);
        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), subDevices);

        // kAudioAggregateDeviceIsStackedKey = 1 → Multi-Output (stacked)
        int stacked = 1;
        CFNumberRef stackedRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &stacked);
        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsStackedKey), stackedRef);

        // NOTE: Do NOT set kAudioAggregateDeviceIsPrivateKey. macOS actively
        // overrides private aggregate devices as the default output, causing an
        // infinite rebuild loop. The mirror is visible in System Settings as
        // "Sulla Audio Mirror" — if the user selects a different device,
        // handleOutputChange() rebuilds the mirror wrapping their new choice.

        // Set the physical output as the master sub-device for volume control.
        // Without this, macOS disables the volume keys and slider because
        // it doesn't know which sub-device should handle volume.
        CFStringRef masterRef = CFStringCreateWithCString(kCFAllocatorDefault, physicalUID.c_str(), kCFStringEncodingUTF8);
        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceMasterSubDeviceKey), masterRef);
        CFRelease(masterRef);

        AudioDeviceID newMirror = kAudioObjectUnknown;
        OSStatus status = AudioHardwareCreateAggregateDevice(desc, &newMirror);

        CFRelease(stackedRef);
        CFRelease(uidRef);
        CFRelease(nameRef);
        CFRelease(subDevices);
        CFRelease(desc);

        if (status != noErr) {
            SULLA_LOG_ERROR("Mirror", "AudioHardwareCreateAggregateDevice failed: " + std::to_string(status));
            rebuilding_ = false;
            return false;
        }

        mirrorId_ = newMirror;
        SULLA_LOG_INFO("Mirror", "Created mirror device (ID: " + std::to_string(mirrorId_) + ")");

        // Set the mirror as default output
        if (!setDefaultOutputDevice(mirrorId_)) {
            SULLA_LOG_WARN("Mirror", "Failed to set mirror as default output");
        } else {
            SULLA_LOG_INFO("Mirror", "Set as default output");
        }

        rebuilding_ = false;

        // Notify that loopback driver should now have audio
        if (deviceChangedCb_) {
            deviceChangedCb_(loopbackId_);
        }

        return true;
    }

    void destroyMirror() {
        destroyMirrorNoLock();
    }

    void destroyMirrorNoLock() {
        if (mirrorId_ == kAudioObjectUnknown) return;

        // Restore default output to the physical device before destroying
        if (lastPhysicalOutput_ != kAudioObjectUnknown) {
            setDefaultOutputDevice(lastPhysicalOutput_);
        }

        OSStatus status = AudioHardwareDestroyAggregateDevice(mirrorId_);
        if (status != noErr) {
            SULLA_LOG_WARN("Mirror", "Failed to destroy mirror device: " + std::to_string(status));
        } else {
            SULLA_LOG_INFO("Mirror", "Destroyed mirror device");
        }
        mirrorId_ = kAudioObjectUnknown;
    }

public:
    // ─── CoreAudio helpers (public for CLI volume commands) ─────

    static AudioDeviceID getDefaultOutputDevice() {
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioDeviceID deviceId = kAudioObjectUnknown;
        UInt32 size = sizeof(deviceId);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nil, &size, &deviceId);
        return deviceId;
    }

    static std::string getDeviceUID(AudioDeviceID deviceId) {
        AudioObjectPropertyAddress addr = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        CFStringRef uid = nullptr;
        UInt32 size = sizeof(uid);
        OSStatus status = AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &uid);
        if (status != noErr || !uid) return "";
        char buf[256];
        CFStringGetCString(uid, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(uid);
        return std::string(buf);
    }

    static std::string getDeviceName(AudioDeviceID deviceId) {
        AudioObjectPropertyAddress addr = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        CFStringRef name = nullptr;
        UInt32 size = sizeof(name);
        OSStatus status = AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &name);
        if (status != noErr || !name) return "Unknown";
        char buf[256];
        CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(name);
        return std::string(buf);
    }

    /**
     * Given an aggregate device, find its first sub-device that isn't the loopback driver.
     * Used to recover the physical output device from an existing mirror.
     */
    static AudioDeviceID getFirstNonBlackholeSubDevice(AudioDeviceID aggregateDevice) {
        if (aggregateDevice == kAudioObjectUnknown) return kAudioObjectUnknown;

        AudioObjectPropertyAddress addr = {
            kAudioAggregateDevicePropertyActiveSubDeviceList,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 size = 0;
        OSStatus status = AudioObjectGetPropertyDataSize(aggregateDevice, &addr, 0, nullptr, &size);
        if (status != noErr || size == 0) return kAudioObjectUnknown;

        int count = size / sizeof(AudioDeviceID);
        std::vector<AudioDeviceID> subDevices(count);
        status = AudioObjectGetPropertyData(aggregateDevice, &addr, 0, nullptr, &size, subDevices.data());
        if (status != noErr) return kAudioObjectUnknown;

        for (auto sub : subDevices) {
            std::string uid = getDeviceUID(sub);
            if (!isLoopbackUID(uid) && uid != kMirrorUID && !uid.empty()) {
                return sub;
            }
        }
        return kAudioObjectUnknown;
    }

    static AudioDeviceID findDeviceByUID(const char* targetUID) {
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 size = 0;
        AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size);
        int count = size / sizeof(AudioDeviceID);
        std::vector<AudioDeviceID> devices(count);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, devices.data());

        for (auto dev : devices) {
            std::string uid = getDeviceUID(dev);
            if (uid == targetUID) return dev;
        }
        return kAudioObjectUnknown;
    }

    /**
     * Find a physical output device to fall back to when the current one disappears.
     * Skips loopback driver, our mirror, and any device without output channels.
     */
    static AudioDeviceID findFallbackOutputDevice() {
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 size = 0;
        AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size);
        int count = size / sizeof(AudioDeviceID);
        std::vector<AudioDeviceID> devices(count);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, devices.data());

        for (auto dev : devices) {
            std::string uid = getDeviceUID(dev);
            if (uid == kMirrorUID || isLoopbackUID(uid) || uid.empty()) continue;

            // Check it has output channels
            AudioObjectPropertyAddress outAddr = {
                kAudioDevicePropertyStreamConfiguration,
                kAudioObjectPropertyScopeOutput,
                kAudioObjectPropertyElementMain
            };
            UInt32 bufSize = 0;
            AudioObjectGetPropertyDataSize(dev, &outAddr, 0, nullptr, &bufSize);
            if (bufSize > sizeof(AudioBufferList)) {
                // Has output streams — check it's not a virtual/aggregate device
                std::string name = getDeviceName(dev);
                // Prefer built-in devices
                if (name.find("Speaker") != std::string::npos ||
                    name.find("Headphone") != std::string::npos ||
                    name.find("Built-in") != std::string::npos ||
                    name.find("MacBook") != std::string::npos) {
                    return dev;
                }
            }
        }

        // No preferred device found — return first device with output channels
        for (auto dev : devices) {
            std::string uid = getDeviceUID(dev);
            if (uid == kMirrorUID || isLoopbackUID(uid) || uid.empty()) continue;
            AudioObjectPropertyAddress outAddr = {
                kAudioDevicePropertyStreamConfiguration,
                kAudioObjectPropertyScopeOutput,
                kAudioObjectPropertyElementMain
            };
            UInt32 bufSize = 0;
            AudioObjectGetPropertyDataSize(dev, &outAddr, 0, nullptr, &bufSize);
            if (bufSize > sizeof(AudioBufferList)) return dev;
        }

        return kAudioObjectUnknown;
    }

    static bool setDefaultOutputDevice(AudioDeviceID deviceId) {
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        OSStatus status = AudioObjectSetPropertyData(
            kAudioObjectSystemObject, &addr, 0, nullptr,
            sizeof(deviceId), &deviceId
        );
        return status == noErr;
    }

private:
    static std::string disabledFlagPath() {
#if defined(__APPLE__)
        const char* home = std::getenv("HOME");
        return std::string(home ? home : "/tmp") + "/Library/Application Support/AudioDriver/mirror-disabled";
#else
        const char* home = std::getenv("HOME");
        return std::string(home ? home : "/tmp") + "/.config/audio-driver/mirror-disabled";
#endif
    }
};

} // namespace sulla

#endif // __APPLE__
