#pragma once

#include <string>
#include <memory>
#include "AudioDevice.h"
#include "IDeviceEnumerator.h"
#include "PlatformDetector.h"

namespace sulla {

/**
 * DeviceController — makes decisions about which audio device to use.
 *
 * Business decisions made here:
 *   - If user specified a preferred device, use it (or error if not found)
 *   - On Windows: use the default output device (all support loopback)
 *   - On macOS: find the first virtual audio device (only those support loopback)
 *   - If no loopback-capable device found, report error with install instructions
 *
 * Delegates all device enumeration to IDeviceEnumerator (the utility layer).
 */
class DeviceController {
public:
    struct SelectionResult {
        AudioDevice device;
        bool        found = false;
        std::string message;
    };

    explicit DeviceController(std::unique_ptr<IDeviceEnumerator> enumerator)
        : enumerator_(std::move(enumerator))
    {}

    /**
     * Select the best loopback capture device.
     *
     * @param preferredDeviceId  User-configured device override (empty = auto-select)
     * @return  The selected device, or an error message explaining why none was found.
     */
    SelectionResult selectDevice(const std::string& preferredDeviceId = "") {
        if (!enumerator_) {
            return {{}, false, "No device enumerator available for this platform"};
        }

        // Decision: if user specified a device, honor it
        if (!preferredDeviceId.empty()) {
            return selectPreferredDevice(preferredDeviceId);
        }

        // Decision: platform-specific auto-selection
        if constexpr (PlatformDetector::hasNativeLoopback()) {
            return selectDefaultOutputDevice();
        } else {
            return selectVirtualDevice();
        }
    }

    /** List all devices (for the config UI). */
    DeviceList listAllDevices() {
        if (!enumerator_) return {};
        return enumerator_->listOutputDevices();
    }

private:
    std::unique_ptr<IDeviceEnumerator> enumerator_;

    /** User explicitly picked a device — find it or fail. */
    SelectionResult selectPreferredDevice(const std::string& deviceId) {
        auto device = enumerator_->getDeviceById(deviceId);
        if (device.id.empty()) {
            return {{}, false,
                "Preferred device not found: " + deviceId
                + ". It may have been disconnected."
            };
        }
        if (!device.isLoopbackCapable) {
            return {{}, false,
                "Device '" + device.name + "' does not support loopback capture."
            };
        }
        return {device, true, "Using preferred device: " + device.name};
    }

    /** Windows: default output always works for loopback. */
    SelectionResult selectDefaultOutputDevice() {
        auto device = enumerator_->getDefaultOutputDevice();
        if (device.id.empty()) {
            return {{}, false, "No audio output device found."};
        }
        device.isLoopbackCapable = true;
        return {device, true, "Using default output: " + device.name};
    }

    /** macOS: scan for a virtual audio device. */
    SelectionResult selectVirtualDevice() {
        auto devices = enumerator_->listOutputDevices();
        for (const auto& dev : devices) {
            if (dev.isLoopbackCapable) {
                return {dev, true, "Found virtual audio device: " + dev.name};
            }
        }
        return {{}, false,
            "No virtual audio device found. "
            "Install SullaAudio driver or BlackHole for system audio capture. "
            "See: https://github.com/ExistentialAudio/BlackHole"
        };
    }
};

} // namespace sulla
