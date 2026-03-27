#pragma once

#include <string>

namespace sulla {

/**
 * PlatformDetector — compile-time and runtime OS detection.
 *
 * Pure utility. No side effects.
 */
namespace PlatformDetector {

    enum class OS {
        Windows,
        macOS,
        Linux,
        Unknown
    };

    /** Compile-time OS detection. */
    constexpr OS currentOS() {
#if defined(_WIN32) || defined(_WIN64)
        return OS::Windows;
#elif defined(__APPLE__) && defined(__MACH__)
        return OS::macOS;
#elif defined(__linux__)
        return OS::Linux;
#else
        return OS::Unknown;
#endif
    }

    inline std::string osName() {
        switch (currentOS()) {
            case OS::Windows: return "Windows";
            case OS::macOS:   return "macOS";
            case OS::Linux:   return "Linux";
            case OS::Unknown: return "Unknown";
        }
        return "Unknown";
    }

    /** Whether this platform supports native loopback (no driver install). */
    constexpr bool hasNativeLoopback() {
        return currentOS() == OS::Windows;
    }

    /** Whether this platform needs a virtual audio device for loopback. */
    constexpr bool needsVirtualDevice() {
        return currentOS() == OS::macOS;
    }

} // namespace PlatformDetector
} // namespace sulla
