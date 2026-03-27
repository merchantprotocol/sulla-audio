#pragma once

#ifdef __APPLE__

#include <CoreAudio/CoreAudio.h>
#include <functional>
#include <string>
#include <atomic>
#include <mutex>
#include "Logger.h"

namespace sulla {

/**
 * AudioMirrorManager — dynamically manages a Multi-Output Device
 * that mirrors the user's active output to BlackHole for capture.
 *
 * When a CSR switches from speakers to a headset (or any output change),
 * this class detects it via a CoreAudio property listener, tears down
 * the old Multi-Output Device, and creates a new one combining the
 * new output + BlackHole. Then sets it as default output so audio
 * continues flowing to both the user's device and BlackHole.
 *
 * Lifecycle:
 *   start() — installs the listener, creates initial mirror
 *   stop()  — removes the listener, destroys the mirror, restores output
 */
class AudioMirrorManager {
public:
    static constexpr const char* kMirrorName = "Sulla Audio Mirror";
    static constexpr const char* kMirrorUID  = "SullaAudioMirror_UID";
    static constexpr const char* kBlackHoleUID = "BlackHole2ch_UID";

    using DeviceChangedCallback = std::function<void(AudioDeviceID blackholeDeviceId)>;

    AudioMirrorManager() = default;
    ~AudioMirrorManager() { stop(); }

    /**
     * Start managing the audio mirror.
     * Finds BlackHole, creates the initial Multi-Output Device,
     * and installs a listener for output device changes.
     */
    bool start() {
        std::lock_guard<std::mutex> lock(mutex_);

        // Find BlackHole
        blackholeId_ = findDeviceByUID(kBlackHoleUID);
        if (blackholeId_ == kAudioObjectUnknown) {
            SULLA_LOG_ERROR("Mirror", "BlackHole 2ch not found — cannot create audio mirror");
            return false;
        }
        SULLA_LOG_INFO("Mirror", "Found BlackHole 2ch (ID: " + std::to_string(blackholeId_) + ")");

        // Create the initial mirror
        if (!rebuildMirror()) {
            SULLA_LOG_WARN("Mirror", "Initial mirror creation failed — will retry on output change");
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
     * Stop managing — remove listener, destroy mirror, restore original output.
     */
    void stop() {
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

    /** Get the BlackHole device ID (for capture backend to use). */
    AudioDeviceID blackholeDeviceId() const { return blackholeId_; }

    /** Get the current mirror device ID. */
    AudioDeviceID mirrorDeviceId() const { return mirrorId_; }

private:
    std::mutex mutex_;
    AudioDeviceID blackholeId_ = kAudioObjectUnknown;
    AudioDeviceID mirrorId_ = kAudioObjectUnknown;
    AudioDeviceID lastPhysicalOutput_ = kAudioObjectUnknown;
    bool listening_ = false;
    bool rebuilding_ = false;
    DeviceChangedCallback deviceChangedCb_;

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

        // If it's BlackHole itself, don't mirror (would create a loop)
        if (currentUID == kBlackHoleUID) {
            SULLA_LOG_WARN("Mirror", "Default output switched to BlackHole — skipping mirror rebuild");
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
        // But if the default lands on BlackHole, we need to force it
        AudioDeviceID newDefault = getDefaultOutputDevice();
        std::string newUID = getDeviceUID(newDefault);

        if (newUID == kBlackHoleUID || newUID == kMirrorUID || newUID.empty()) {
            // macOS fell back to BlackHole or nothing — find a real device
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
     * Destroy old mirror (if any), create a new one with the current
     * physical output + BlackHole, set it as default.
     */
    bool rebuildMirror() {
        rebuilding_ = true;

        // Determine the physical output device to mirror
        AudioDeviceID physicalOutput = lastPhysicalOutput_;
        if (physicalOutput == kAudioObjectUnknown) {
            physicalOutput = getDefaultOutputDevice();
            if (physicalOutput == kAudioObjectUnknown) {
                rebuilding_ = false;
                return false;
            }
            // Make sure it's not already our mirror
            std::string uid = getDeviceUID(physicalOutput);
            if (uid == kMirrorUID) {
                // Mirror already exists and is default — adopt it
                AudioDeviceID existing = findDeviceByUID(kMirrorUID);
                if (existing != kAudioObjectUnknown) {
                    mirrorId_ = existing;
                    SULLA_LOG_INFO("Mirror", "Adopted existing mirror device (ID: " + std::to_string(mirrorId_) + ")");
                }
                rebuilding_ = false;
                return true;
            }
            if (uid == kBlackHoleUID) {
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

        SULLA_LOG_INFO("Mirror", "Creating mirror: " + physicalName + " + BlackHole 2ch");

        // Build sub-device list: physical output first (main), BlackHole second
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
        CFStringRef bhUIDRef = CFStringCreateWithCString(kCFAllocatorDefault, kBlackHoleUID, kCFStringEncodingUTF8);
        CFDictionarySetValue(bhDict, CFSTR(kAudioSubDeviceUIDKey), bhUIDRef);
        CFArrayAppendValue(subDevices, bhDict);
        CFRelease(bhUIDRef);
        CFRelease(bhDict);

        // Build the aggregate device description
        CFMutableDictionaryRef desc = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

        CFStringRef nameRef = CFStringCreateWithCString(kCFAllocatorDefault, kMirrorName, kCFStringEncodingUTF8);
        CFStringRef uidRef = CFStringCreateWithCString(kCFAllocatorDefault, kMirrorUID, kCFStringEncodingUTF8);

        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), nameRef);
        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), uidRef);
        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), subDevices);

        // kAudioAggregateDeviceIsStackedKey = 1 → Multi-Output (stacked)
        int stacked = 1;
        CFNumberRef stackedRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &stacked);
        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsStackedKey), stackedRef);

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

        // Notify that BlackHole should now have audio
        if (deviceChangedCb_) {
            deviceChangedCb_(blackholeId_);
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

    // ─── CoreAudio helpers ──────────────────────────────────────

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
     * Skips BlackHole, our mirror, and any device without output channels.
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
            if (uid == kMirrorUID || uid == kBlackHoleUID || uid.empty()) continue;

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
            if (uid == kMirrorUID || uid == kBlackHoleUID || uid.empty()) continue;
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
};

} // namespace sulla

#endif // __APPLE__
