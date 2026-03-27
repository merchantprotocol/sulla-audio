#pragma once

#ifdef __APPLE__

#include <sulla/IDeviceEnumerator.h>
#include <sulla/AudioDevice.h>
#include <sulla/AudioFormat.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <vector>
#include <string>

namespace sulla {

/**
 * CoreAudioDeviceEnumerator — lists audio devices via macOS CoreAudio.
 *
 * Thin wrapper around AudioObjectGetPropertyData.
 * On macOS, only virtual audio devices (BlackHole, AudioDriver) support
 * loopback capture — this enumerator marks them accordingly.
 */
class CoreAudioDeviceEnumerator : public IDeviceEnumerator {
public:
    DeviceList listOutputDevices() override {
        DeviceList result;

        AudioObjectPropertyAddress prop = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        UInt32 dataSize = 0;
        OSStatus status = AudioObjectGetPropertyDataSize(
            kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize
        );
        if (status != noErr) return result;

        const UInt32 deviceCount = dataSize / sizeof(AudioDeviceID);
        std::vector<AudioDeviceID> deviceIds(deviceCount);
        status = AudioObjectGetPropertyData(
            kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize, deviceIds.data()
        );
        if (status != noErr) return result;

        AudioDeviceID defaultId = getDefaultOutputDeviceId();

        for (AudioDeviceID id : deviceIds) {
            if (!hasOutputStreams(id)) continue;

            AudioDevice dev;
            dev.id = std::to_string(id);
            dev.name = getDeviceName(id);
            dev.type = AudioDevice::Type::Output;
            dev.isDefault = (id == defaultId);
            dev.nativeFormat = getDeviceFormat(id);
            dev.isLoopbackCapable = isVirtualDevice(dev.name);

            result.push_back(dev);
        }

        return result;
    }

    AudioDevice getDefaultOutputDevice() override {
        AudioDeviceID id = getDefaultOutputDeviceId();
        if (id == kAudioDeviceUnknown) return {};

        AudioDevice dev;
        dev.id = std::to_string(id);
        dev.name = getDeviceName(id);
        dev.type = AudioDevice::Type::Output;
        dev.isDefault = true;
        dev.nativeFormat = getDeviceFormat(id);
        dev.isLoopbackCapable = isVirtualDevice(dev.name);
        return dev;
    }

    AudioDevice getDeviceById(const std::string& deviceId) override {
        AudioDeviceID id = static_cast<AudioDeviceID>(std::stoul(deviceId));

        AudioDevice dev;
        dev.id = deviceId;
        dev.name = getDeviceName(id);
        dev.type = AudioDevice::Type::Output;
        dev.nativeFormat = getDeviceFormat(id);
        dev.isDefault = (id == getDefaultOutputDeviceId());
        dev.isLoopbackCapable = isVirtualDevice(dev.name);
        return dev;
    }

private:
    AudioDeviceID getDefaultOutputDeviceId() {
        AudioObjectPropertyAddress prop = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioDeviceID id = kAudioDeviceUnknown;
        UInt32 size = sizeof(id);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &size, &id);
        return id;
    }

    std::string getDeviceName(AudioDeviceID id) {
        AudioObjectPropertyAddress prop = {
            kAudioDevicePropertyDeviceNameCFString,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        CFStringRef nameRef = nullptr;
        UInt32 size = sizeof(nameRef);
        OSStatus status = AudioObjectGetPropertyData(id, &prop, 0, nullptr, &size, &nameRef);
        if (status != noErr || !nameRef) return "(unknown)";

        char buf[256];
        CFStringGetCString(nameRef, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(nameRef);
        return std::string(buf);
    }

    AudioFormat getDeviceFormat(AudioDeviceID id) {
        AudioObjectPropertyAddress prop = {
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        Float64 sampleRate = 48000.0;
        UInt32 size = sizeof(sampleRate);
        AudioObjectGetPropertyData(id, &prop, 0, nullptr, &size, &sampleRate);

        return AudioFormat{
            static_cast<uint32_t>(sampleRate),
            2, 32, true  // CoreAudio typically delivers float32 stereo
        };
    }

    bool hasOutputStreams(AudioDeviceID id) {
        AudioObjectPropertyAddress prop = {
            kAudioDevicePropertyStreams,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 size = 0;
        OSStatus status = AudioObjectGetPropertyDataSize(id, &prop, 0, nullptr, &size);
        return (status == noErr && size > 0);
    }

    /**
     * Check if a device name matches known virtual audio device patterns.
     * On macOS, only virtual devices support loopback capture.
     */
    bool isVirtualDevice(const std::string& name) {
        // Same patterns as SecretaryModeController's virtualPatterns
        static const char* patterns[] = {
            "BlackHole", "blackhole",
            "Loopback",
            "AudioDriver", "Audio Driver", "audio-driver",
            "Soundflower", "soundflower",
            "VB-Cable", "vb-cable",
            "ZoomAudioDevice", "Zoom Audio",
            "Virtual Audio",
        };
        for (const char* p : patterns) {
            if (name.find(p) != std::string::npos) return true;
        }
        return false;
    }
};

} // namespace sulla

#endif // __APPLE__
