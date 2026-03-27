#pragma once

#include <memory>
#include "AudioDevice.h"
#include "CaptureState.h"

namespace sulla {

/**
 * IDeviceEnumerator — interface for listing audio devices.
 *
 * Platform backends implement this. No business decisions —
 * just asks the OS "what devices exist?" and returns the answer.
 */
class IDeviceEnumerator {
public:
    virtual ~IDeviceEnumerator() = default;

    /** List all output (render) devices capable of loopback capture. */
    virtual DeviceList listOutputDevices() = 0;

    /** Get the current default output device. */
    virtual AudioDevice getDefaultOutputDevice() = 0;

    /** Find a device by its platform ID. */
    virtual AudioDevice getDeviceById(const std::string& deviceId) = 0;

    /** Factory: create the platform-appropriate enumerator. */
    static std::unique_ptr<IDeviceEnumerator> create();
};

} // namespace sulla
