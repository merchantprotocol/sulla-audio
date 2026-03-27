#pragma once

#include <string>

namespace sulla {

/**
 * CaptureState — enum representing the lifecycle of a capture session.
 *
 * Pure value. Controllers transition between states; models and utilities
 * never reference this directly.
 */
enum class CaptureState {
    Idle,          // No capture active
    Initializing,  // Device acquired, configuring audio client
    Capturing,     // Actively reading audio frames
    Stopping,      // Draining buffers, releasing resources
    Error          // Unrecoverable error, must re-initialize
};

inline std::string captureStateToString(CaptureState state) {
    switch (state) {
        case CaptureState::Idle:          return "Idle";
        case CaptureState::Initializing:  return "Initializing";
        case CaptureState::Capturing:     return "Capturing";
        case CaptureState::Stopping:      return "Stopping";
        case CaptureState::Error:         return "Error";
    }
    return "Unknown";
}

/**
 * CaptureError — structured error information from the capture pipeline.
 */
struct CaptureError {
    enum class Code {
        None,
        DeviceNotFound,
        DeviceLost,
        FormatNotSupported,
        InitializationFailed,
        CaptureStartFailed,
        BufferOverrun,
        PlatformError
    };

    Code        code    = Code::None;
    std::string message;
    int         platformCode = 0;  // HRESULT on Windows, OSStatus on macOS

    bool ok() const { return code == Code::None; }

    static CaptureError none() { return {}; }

    static CaptureError make(Code code, const std::string& msg, int platformCode = 0) {
        return {code, msg, platformCode};
    }
};

} // namespace sulla
