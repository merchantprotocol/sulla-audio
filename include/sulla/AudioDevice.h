#pragma once

#include <string>
#include <vector>
#include "AudioFormat.h"

namespace sulla {

/**
 * AudioDevice — immutable value object describing an audio endpoint.
 *
 * Pure data. Platform backends populate these; controllers make decisions
 * about which device to use.
 */
struct AudioDevice {
    enum class Type {
        Output,  // Speakers, headphones — loopback source
        Input,   // Microphones — not used by this library
        Unknown
    };

    std::string id;                   // Platform-specific device ID
    std::string name;                 // Human-readable name
    Type        type       = Type::Unknown;
    bool        isDefault  = false;
    bool        isLoopbackCapable = false;
    AudioFormat nativeFormat;         // Device's preferred format

    bool operator==(const AudioDevice& other) const {
        return id == other.id;
    }

    bool operator!=(const AudioDevice& other) const {
        return !(*this == other);
    }

    std::string toString() const {
        return name + " [" + id.substr(0, 16) + "...]"
             + (isDefault ? " (default)" : "")
             + (isLoopbackCapable ? " (loopback)" : "")
             + " — " + nativeFormat.toString();
    }
};

/** Alias for a list of devices. */
using DeviceList = std::vector<AudioDevice>;

} // namespace sulla
